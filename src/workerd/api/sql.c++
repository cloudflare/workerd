// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "sql.h"

#include "actor-state.h"

#include <workerd/io/io-context.h>

#include <cmath>
#include <limits>

namespace workerd::api {

// Maximum total size of all cached statements (measured in size of the SQL code). If cached
// statements exceed this, we remove the LRU statement(s).
//
// Hopefully most apps don't ever hit this, but it's important to have a limit in case of
// queries containing dynamic content or excessively large one-off queries.
static constexpr uint SQL_STATEMENT_CACHE_MAX_SIZE = 1024 * 1024;

SqlStorage::SqlStorage(jsg::Ref<DurableObjectStorage> storage)
    : storage(kj::mv(storage)),
      statementCache(IoContext::current().addObject(kj::heap<StatementCache>())) {}

SqlStorage::~SqlStorage() {}

jsg::Ref<SqlStorage::Cursor> SqlStorage::exec(
    jsg::Lock& js, jsg::JsString querySql, jsg::Arguments<BindingValue> bindings) {
  auto& context = IoContext::current();
  auto span = context.makeTraceSpan("durable_object_storage_exec"_kjc);
  auto userSpan = context.makeUserTraceSpan("durable_object_storage_exec"_kjc);
  userSpan.setTag("db.system.name"_kjc, "cloudflare-durable-object-sql"_kjc);
  userSpan.setTag("db.operation.name"_kjc, "exec"_kjc);
  userSpan.setTag("db.query.text"_kjc, kj::str(querySql));
  userSpan.setTag(
      "cloudflare.durable_object.query.bindings"_kjc, static_cast<int64_t>(bindings.size()));

  // Internalize the string, so that the cache can be keyed by string identity rather than content.
  // Any string we put into the cache is expected to live there for a while anyway, so even if it
  // is a one-off, internalizing it (which moves it to the old generation) shouldn't hurt.
  querySql = querySql.internalize(js);

  auto& db = getDb(js);
  auto& statementCache = *this->statementCache;

  kj::Rc<CachedStatement>& slot = statementCache.map.findOrCreate(querySql, [&]() {
    auto result = kj::rc<CachedStatement>(js, *this, db, querySql, js.toString(querySql));
    statementCache.totalSize += result->statementSize;
    return result;
  });

  // Move cached statement to end of LRU queue.
  if (slot->lruLink.isLinked()) {
    statementCache.lru.remove(*slot.get());
  }
  statementCache.lru.add(*slot.get());

  // In order to get accurate statistics, we have to keep the spans around until the query is
  // actually done, which for read queries that iterate over a cursor won't be until later.
  kj::Maybe<kj::Function<void(Cursor&)>> doneCallback;
  if (span.isObserved() || userSpan.isObserved()) {
    doneCallback = [span = kj::mv(span), userSpan = kj::mv(userSpan)](Cursor& cursor) mutable {
      int64_t rowsRead = cursor.getRowsRead();
      int64_t rowsWritten = cursor.getRowsWritten();
      span.setTag("rows_read"_kjc, rowsRead);
      span.setTag("rows_written"_kjc, rowsWritten);
      userSpan.setTag("cloudflare.durable_object.response.rows_read"_kjc, rowsRead);
      userSpan.setTag("cloudflare.durable_object.response.rows_written"_kjc, rowsWritten);
    };
  }

  if (slot->isShared()) {
    // Oops, this CachedStatement is currently in-use (presumably by a Cursor).
    //
    // SQLite only allows one instance of a statement to run at a time, so we will have to compile
    // the statement again as a one-off.
    //
    // In theory we could try to cache multiple copies of the statement, but as this is probably
    // exceedingly rare, it is not worth the added code complexity.
    SqliteDatabase::Regulator& regulator = *this;
    return js.alloc<Cursor>(
        js, kj::mv(doneCallback), db, regulator, js.toString(querySql), kj::mv(bindings));
  }

  auto result = js.alloc<Cursor>(js, kj::mv(doneCallback), slot.addRef(), kj::mv(bindings));

  // If the statement cache grew too big, drop the least-recently-used entry.
  while (statementCache.totalSize > SQL_STATEMENT_CACHE_MAX_SIZE) {
    auto& toRemove = *statementCache.lru.begin();
    auto oldQuery = jsg::JsString(toRemove.query.getHandle(js));
    statementCache.totalSize -= toRemove.statementSize;
    statementCache.lru.remove(toRemove);
    KJ_ASSERT(statementCache.map.eraseMatch(oldQuery));
  }

  return result;
}

SqlStorage::IngestResult SqlStorage::ingest(jsg::Lock& js, kj::String querySql) {
  auto& context = IoContext::current();
  auto span = context.makeTraceSpan("durable_object_storage_ingest"_kjc);
  auto userSpan = context.makeUserTraceSpan("durable_object_storage_ingest"_kjc);
  SqliteDatabase::Regulator& regulator = *this;
  auto result = getDb(js).ingestSql(regulator, querySql);

  span.setTag("rows_read"_kjc, static_cast<int64_t>(result.rowsRead));
  span.setTag("rows_written"_kjc, static_cast<int64_t>(result.rowsWritten));
  span.setTag("statement_count"_kjc, static_cast<int64_t>(result.statementCount));
  userSpan.setTag(
      "cloudflare.durable_object.response.rows_read"_kjc, static_cast<int64_t>(result.rowsRead));
  userSpan.setTag("cloudflare.durable_object.response.rows_written"_kjc,
      static_cast<int64_t>(result.rowsWritten));
  userSpan.setTag("cloudflare.durable_object.response.statement_count"_kjc,
      static_cast<int64_t>(result.statementCount));

  return IngestResult(
      kj::str(result.remainder), result.rowsRead, result.rowsWritten, result.statementCount);
}

void SqlStorage::setMaxPageCountForTest(jsg::Lock& js, int count) {
  auto& db = getDb(js);
  db.run({.regulator = SqliteDatabase::TRUSTED}, kj::str("PRAGMA max_page_count = ", count));
}

jsg::Ref<SqlStorage::Statement> SqlStorage::prepare(jsg::Lock& js, jsg::JsString query) {
  return js.alloc<Statement>(js, JSG_THIS, query);
}

double SqlStorage::getDatabaseSize(jsg::Lock& js) {
  auto userSpan =
      IoContext::current().makeUserTraceSpan("durable_object_storage_getDatabaseSize"_kjc);
  userSpan.setTag("db.operation.name"_kjc, "getDatabaseSize"_kjc);
  auto& db = getDb(js);
  int64_t pages = execMemoized(db, pragmaPageCount,
      "select (select * from pragma_page_count) - (select * from pragma_freelist_count);")
                      .getInt64(0);
  auto dbSize = pages * getPageSize(db);
  userSpan.setTag("cloudflare.durable_object.response.db_size"_kjc, static_cast<int64_t>(dbSize));
  return dbSize;
}

void SqlStorage::createFunction(jsg::Lock& js,
    kj::String name,
    jsg::Function<jsg::Value(jsg::Arguments<jsg::Value>)> callback) {
  // Validate function name
  JSG_REQUIRE(name.size() > 0, TypeError, "Function name cannot be empty.");
  JSG_REQUIRE(name.size() <= 255, TypeError, "Function name is too long (max 255 bytes).");

  // Store the JS callback by creating the structure with a proper constructor
  auto jsFunc = kj::heap<RegisteredJsFunction>(kj::str(name), kj::mv(callback));

  // Get a raw pointer before moving into the map (we need it for the C++ callback)
  auto* jsFuncPtr = jsFunc.get();

  // Store in our map (takes ownership)
  registeredJsFunctions.insert(jsFuncPtr->name.asPtr(), kj::mv(jsFunc));

  // Create a C++ callback that wraps the JS function
  auto& db = getDb(js);
  db.registerScalarFunction(jsFuncPtr->name.asPtr(), -1,  // -1 = variadic
      [jsFuncPtr](
          kj::ArrayPtr<const SqliteDatabase::UdfArgValue> args) -> SqliteDatabase::UdfResultValue {
    // Get the current jsg::Lock from the IoContext
    // This is safe because SQL queries are always executed while holding the JS lock
    auto& workerLock = IoContext::current().getCurrentLock();
    jsg::Lock& js = workerLock;

    // Convert SQLite args to JS values
    auto jsArgsBuilder = kj::heapArrayBuilder<jsg::Value>(args.size());
    for (const auto& arg: args) {
      KJ_SWITCH_ONEOF(arg) {
        KJ_CASE_ONEOF(intVal, int64_t) {
          jsArgsBuilder.add(
              jsg::Value(js.v8Isolate, v8::Number::New(js.v8Isolate, static_cast<double>(intVal))));
        }
        KJ_CASE_ONEOF(doubleVal, double) {
          jsArgsBuilder.add(jsg::Value(js.v8Isolate, v8::Number::New(js.v8Isolate, doubleVal)));
        }
        KJ_CASE_ONEOF(strVal, kj::StringPtr) {
          jsArgsBuilder.add(jsg::Value(js.v8Isolate, jsg::v8Str(js.v8Isolate, strVal)));
        }
        KJ_CASE_ONEOF(blobVal, kj::ArrayPtr<const kj::byte>) {
          auto copy = kj::heapArray<kj::byte>(blobVal.size());
          memcpy(copy.begin(), blobVal.begin(), blobVal.size());
          jsArgsBuilder.add(jsg::Value(js.v8Isolate, js.wrapBytes(kj::mv(copy))));
        }
        KJ_CASE_ONEOF(nullVal, decltype(nullptr)) {
          jsArgsBuilder.add(jsg::Value(js.v8Isolate, v8::Null(js.v8Isolate)));
        }
      }
    }

    // Call the JS function with the arguments, catching any exceptions
    jsg::Arguments<jsg::Value> jsgArgs(jsArgsBuilder.finish());

    // Use v8::TryCatch to capture JavaScript exceptions with their message
    v8::TryCatch tryCatch(js.v8Isolate);
    jsg::Value result(js.v8Isolate, v8::Undefined(js.v8Isolate));
    try {
      result = jsFuncPtr->callback(js, kj::mv(jsgArgs));
    } catch (jsg::JsExceptionThrown&) {
      // The JS exception is in the TryCatch - extract the message and rethrow as kj::Exception
      if (tryCatch.HasCaught()) {
        v8::Local<v8::Value> exception = tryCatch.Exception();
        v8::Local<v8::String> message;
        if (exception->IsObject()) {
          auto obj = exception.As<v8::Object>();
          auto context = js.v8Context();
          auto msgValue = obj->Get(context, jsg::v8StrIntern(js.v8Isolate, "message"));
          if (!msgValue.IsEmpty() && msgValue.ToLocalChecked()->IsString()) {
            message = msgValue.ToLocalChecked().As<v8::String>();
          }
        }
        if (message.IsEmpty()) {
          message = exception->ToString(js.v8Context()).ToLocalChecked();
        }
        v8::String::Utf8Value utf8(js.v8Isolate, message);
        kj::throwFatalException(KJ_EXCEPTION(FAILED, kj::str(*utf8)));
      }
      throw;  // Re-throw if we couldn't extract the message
    }

    // Convert JS result back to SQLite UdfResultValue (owning)
    auto handle = result.getHandle(js);
    if (handle->IsNull() || handle->IsUndefined()) {
      return nullptr;
    } else if (handle->IsNumber()) {
      double num = handle.As<v8::Number>()->Value();
      // Check if it's an integer - use a simpler check
      double intPart;
      if (std::modf(num, &intPart) == 0.0 &&
          num >= static_cast<double>(std::numeric_limits<int64_t>::min()) &&
          num <= static_cast<double>(std::numeric_limits<int64_t>::max())) {
        return static_cast<int64_t>(num);
      }
      return num;
    } else if (handle->IsString()) {
      // Return owning kj::String
      return kj::str(js.toString(jsg::JsValue(handle)));
    } else if (handle->IsArrayBuffer() || handle->IsArrayBufferView()) {
      // Return owning kj::Array<byte>
      jsg::BufferSource buffer(js, handle);
      auto data = buffer.asArrayPtr();
      auto copy = kj::heapArray<kj::byte>(data.size());
      memcpy(copy.begin(), data.begin(), data.size());
      return kj::mv(copy);
    } else {
      // For other types, convert to string (owning)
      return kj::str(js.toString(jsg::JsValue(handle)));
    }
  });
}

bool SqlStorage::isAllowedName(kj::StringPtr name) const {
  return !name.startsWith("_cf_");
}

bool SqlStorage::isAllowedTrigger(kj::StringPtr name) const {
  return true;
}

void SqlStorage::onError(kj::Maybe<int> sqliteErrorCode, kj::StringPtr message) const {
  JSG_ASSERT(false, Error, message);
}

bool SqlStorage::allowTransactions() const {
  JSG_FAIL_REQUIRE(Error,
      "To execute a transaction, please use the state.storage.transaction() or "
      "state.storage.transactionSync() APIs instead of the SQL BEGIN TRANSACTION or SAVEPOINT "
      "statements. The JavaScript API is safer because it will automatically roll back on "
      "exceptions, and because it interacts correctly with Durable Objects' automatic atomic "
      "write coalescing.");
}

bool SqlStorage::shouldAddQueryStats() const {
  // Bill for queries executed from JavaScript.
  return true;
}

SqlStorage::StatementCache::~StatementCache() noexcept(false) {
  for (auto& entry: lru) {
    lru.remove(entry);
  }
}

jsg::JsValue SqlStorage::wrapSqlValue(jsg::Lock& js, SqlValue value) {
  KJ_IF_SOME(v, value) {
    KJ_SWITCH_ONEOF(v) {
      KJ_CASE_ONEOF(bytes, kj::Array<byte>) {
        return jsg::JsValue(js.wrapBytes(kj::mv(bytes)));
      }
      KJ_CASE_ONEOF(text, kj::StringPtr) {
        return js.str(text);
      }
      KJ_CASE_ONEOF(number, double) {
        return js.num(number);
      }
    }
    KJ_UNREACHABLE;
  } else {
    return js.null();
  }
}

SqlStorage::Cursor::State::State(SqliteDatabase& db,
    SqliteDatabase::Regulator& regulator,
    kj::StringPtr sqlCode,
    kj::Array<BindingValue> bindingsParam)
    : bindings(kj::mv(bindingsParam)),
      query(db.run({.regulator = regulator}, sqlCode, mapBindings(bindings).asPtr())) {}

SqlStorage::Cursor::State::State(
    kj::Rc<CachedStatement> cachedStatementParam, kj::Array<BindingValue> bindingsParam)
    : bindings(kj::mv(bindingsParam)),
      query(cachedStatement.emplace(kj::mv(cachedStatementParam))
                ->statement.run(mapBindings(bindings).asPtr())) {}

SqlStorage::Cursor::~Cursor() noexcept(false) {
  // If this Cursor was created from a Statement, clear the Statement's currentCursor weak ref.
  KJ_IF_SOME(s, selfRef) {
    KJ_IF_SOME(p, s) {
      if (&p == this) {
        s = kj::none;
      }
    }
  }
}

void SqlStorage::Cursor::initColumnNames(jsg::Lock& js, State& stateRef) {
  KJ_IF_SOME(cached, stateRef.cachedStatement) {
    reusedCachedQuery = cached->useCount++ > 0;
  }

  js.withinHandleScope([&]() {
    v8::LocalVector<v8::Value> vec(js.v8Isolate);
    for (auto i: kj::zeroTo(stateRef.query.columnCount())) {
      vec.push_back(js.str(stateRef.query.getColumnName(i)));
    }
    auto array = jsg::JsArray(v8::Array::New(js.v8Isolate, vec.data(), vec.size()));
    columnNames = jsg::JsRef<jsg::JsArray>(js, array);
  });
}

double SqlStorage::Cursor::getRowsRead() {
  KJ_IF_SOME(st, state) {
    return static_cast<double>(st->query.getRowsRead());
  } else {
    return static_cast<double>(rowsRead);
  }
}

double SqlStorage::Cursor::getRowsWritten() {
  KJ_IF_SOME(st, state) {
    return static_cast<double>(st->query.getRowsWritten());
  } else {
    return static_cast<double>(rowsWritten);
  }
}

SqlStorage::Cursor::RowIterator::Next SqlStorage::Cursor::next(jsg::Lock& js) {
  auto self = JSG_THIS;
  auto maybeRow = rowIteratorNext(js, self);
  bool done = maybeRow == kj::none;
  return {
    .done = done,
    .value = kj::mv(maybeRow),
  };
}

jsg::JsArray SqlStorage::Cursor::toArray(jsg::Lock& js) {
  auto self = JSG_THIS;
  v8::LocalVector<v8::Value> results(js.v8Isolate);
  for (;;) {
    auto maybeRow = rowIteratorNext(js, self);
    KJ_IF_SOME(row, maybeRow) {
      results.push_back(row);
    } else {
      break;
    }
  }

  return jsg::JsArray(v8::Array::New(js.v8Isolate, results.data(), results.size()));
}

jsg::JsValue SqlStorage::Cursor::one(jsg::Lock& js) {
  auto self = JSG_THIS;
  auto result = JSG_REQUIRE_NONNULL(rowIteratorNext(js, self), Error,
      "Expected exactly one result from SQL query, but got no results.");

  KJ_IF_SOME(s, state) {
    // It appears that the query had more results, otherwise we would have set `state` to `none`
    // inside `iteratorImpl()`.
    endQuery(*s);
    JSG_FAIL_REQUIRE(
        Error, "Expected exactly one result from SQL query, but got multiple results.");
  }

  return result;
}

jsg::Ref<SqlStorage::Cursor::RowIterator> SqlStorage::Cursor::rows(jsg::Lock& js) {
  return js.alloc<RowIterator>(JSG_THIS);
}

kj::Maybe<jsg::JsObject> SqlStorage::Cursor::rowIteratorNext(jsg::Lock& js, jsg::Ref<Cursor>& obj) {
  KJ_IF_SOME(values, iteratorImpl(js, obj)) {
    auto names = obj->columnNames.getHandle(js);
    jsg::JsObject result = js.obj();
    KJ_ASSERT(names.size() == values.size());
    for (auto i: kj::zeroTo(names.size())) {
      result.set(js, names.get(js, i), jsg::JsValue(values[i]));
    }
    return result;
  } else {
    return kj::none;
  }
}

jsg::Ref<SqlStorage::Cursor::RawIterator> SqlStorage::Cursor::raw(jsg::Lock& js) {
  return js.alloc<RawIterator>(JSG_THIS);
}

// Returns the set of column names for the current Cursor. An exception will be thrown if the
// iterator has already been fully consumed. The resulting columns may contain duplicate entries,
// for instance a `SELECT *` across a join of two tables that share a column name.
jsg::JsArray SqlStorage::Cursor::getColumnNames(jsg::Lock& js) {
  return columnNames.getHandle(js);
}

kj::Maybe<jsg::JsArray> SqlStorage::Cursor::rawIteratorNext(jsg::Lock& js, jsg::Ref<Cursor>& obj) {
  KJ_IF_SOME(values, iteratorImpl(js, obj)) {
    return jsg::JsArray(v8::Array::New(js.v8Isolate, values.data(), values.size()));
  } else {
    return kj::none;
  }
}

kj::Maybe<v8::LocalVector<v8::Value>> SqlStorage::Cursor::iteratorImpl(
    jsg::Lock& js, jsg::Ref<Cursor>& obj) {
  auto& state = *KJ_UNWRAP_OR(obj->state, {
    if (obj->canceled) {
      JSG_FAIL_REQUIRE(Error,
          "SQL cursor was closed because the same statement was executed again. If you need to "
          "run multiple copies of the same statement concurrently, you must create multiple "
          "prepared statement objects.");
    } else {
      // Query already done.
      return kj::none;
    }
  });

  auto& query = state.query;

  if (query.isDone()) {
    obj->endQuery(state);
    return kj::none;
  }

  auto n = query.columnCount();
  v8::LocalVector<v8::Value> results(js.v8Isolate);
  results.reserve(n);
  for (auto i: kj::zeroTo(n)) {
    SqlValue value;
    KJ_SWITCH_ONEOF(query.getValue(i)) {
      KJ_CASE_ONEOF(data, kj::ArrayPtr<const byte>) {
        value.emplace(kj::heapArray(data));
      }
      KJ_CASE_ONEOF(text, kj::StringPtr) {
        value.emplace(text);
      }
      KJ_CASE_ONEOF(i, int64_t) {
        // int64 will become BigInt, but most applications won't want all their integers to be
        // BigInt. We will coerce to a double here.
        // TODO(someday): Allow applications to request that certain columns use BigInt.
        value.emplace(static_cast<double>(i));
      }
      KJ_CASE_ONEOF(d, double) {
        value.emplace(d);
      }
      KJ_CASE_ONEOF(_, decltype(nullptr)) {
        // leave value null
      }
    }
    results.push_back(wrapSqlValue(js, kj::mv(value)));
  }

  // Proactively iterate to the next row and, if it turns out the query is done, discard it. This
  // is an optimization to make sure that the statement can be returned to the statement cache once
  // the application has iterated over all results, even if the application fails to call next()
  // one last time to get `{done: true}`. A common case where this could happen is if the app is
  // expecting zero or one results, so it calls `exec(...).next()`. In the case that one result
  // was returned, the application may not bother calling `next()` again. If we hadn't proactively
  // iterated ahead by one, then the statement would not be returned to the cache until it was
  // GC'ed, which might prevent the cache from being effective in the meantime.
  //
  // Unfortunately, this does not help with the case where the application stops iterating with
  // results still available from the cursor. There's not much we can do about that case since
  // there's no way to know if the app might come back and try to use the cursor again later.
  query.nextRow();
  if (query.isDone()) {
    obj->endQuery(state);
  }

  return kj::mv(results);
}

void SqlStorage::Cursor::endQuery(State& stateRef) {
  // Save off row counts before the query goes away.
  rowsRead = stateRef.query.getRowsRead();
  rowsWritten = stateRef.query.getRowsWritten();

  KJ_IF_SOME(cb, doneCallback) {
    cb(*this);
    doneCallback = kj::none;
  }

  // Clean up the query proactively.
  state = kj::none;
}

kj::Array<const SqliteDatabase::Query::ValuePtr> SqlStorage::Cursor::mapBindings(
    kj::ArrayPtr<BindingValue> values) {
  return KJ_MAP(value, values) -> SqliteDatabase::Query::ValuePtr {
    KJ_IF_SOME(v, value) {
      KJ_SWITCH_ONEOF(v) {
        KJ_CASE_ONEOF(data, kj::Array<const byte>) {
          return data.asPtr();
        }
        KJ_CASE_ONEOF(text, kj::String) {
          return text.asPtr();
        }
        KJ_CASE_ONEOF(d, double) {
          return d;
        }
      }
    } else {
      return nullptr;
    }
    KJ_UNREACHABLE;
  };
}

jsg::Ref<SqlStorage::Cursor> SqlStorage::Statement::run(
    jsg::Lock& js, jsg::Arguments<BindingValue> bindings) {
  return sqlStorage->exec(js, jsg::JsString(query.getHandle(js)), kj::mv(bindings));
}

void SqlStorage::visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
  tracker.trackField("storage", storage);
  tracker.trackFieldWithSize("IoPtr<SqliteDatabase>", sizeof(IoPtr<SqliteDatabase>));
  if (pragmaPageCount != kj::none) {
    tracker.trackFieldWithSize(
        "IoPtr<SqllitDatabase::Statement>", sizeof(IoPtr<SqliteDatabase::Statement>));
  }
  if (pragmaGetMaxPageCount != kj::none) {
    tracker.trackFieldWithSize(
        "IoPtr<SqllitDatabase::Statement>", sizeof(IoPtr<SqliteDatabase::Statement>));
  }
}

}  // namespace workerd::api
