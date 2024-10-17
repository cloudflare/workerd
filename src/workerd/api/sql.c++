// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "sql.h"

#include "actor-state.h"

#include <workerd/io/io-context.h>

namespace workerd::api {

SqlStorage::SqlStorage(jsg::Ref<DurableObjectStorage> storage): storage(kj::mv(storage)) {}

SqlStorage::~SqlStorage() {}

jsg::Ref<SqlStorage::Cursor> SqlStorage::exec(
    jsg::Lock& js, jsg::JsString querySql, jsg::Arguments<BindingValue> bindings) {
  SqliteDatabase::Regulator& regulator = *this;
  return jsg::alloc<Cursor>(getDb(js), regulator, js.toString(querySql), kj::mv(bindings));
}

SqlStorage::IngestResult SqlStorage::ingest(jsg::Lock& js, kj::String querySql) {
  SqliteDatabase::Regulator& regulator = *this;
  auto result = getDb(js).ingestSql(regulator, querySql);
  return IngestResult(
      kj::str(result.remainder), result.rowsRead, result.rowsWritten, result.statementCount);
}

jsg::Ref<SqlStorage::Statement> SqlStorage::prepare(jsg::Lock& js, jsg::JsString query) {
  return jsg::alloc<Statement>(js, JSG_THIS, query);
}

double SqlStorage::getDatabaseSize(jsg::Lock& js) {
  auto& db = getDb(js);
  int64_t pages = execMemoized(db, pragmaPageCount,
      "select (select * from pragma_page_count) - (select * from pragma_freelist_count);")
                      .getInt64(0);
  return pages * getPageSize(db);
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

jsg::JsObject SqlStorage::wrapSqlRow(jsg::Lock& js, SqlRow row) {
  return jsg::JsObject(js.withinHandleScope([&]() -> v8::Local<v8::Object> {
    jsg::JsObject result = js.obj();
    for (auto& field: row.fields) {
      result.set(js, field.name, wrapSqlValue(js, kj::mv(field.value)));
    }
    return result;
  }));
}

jsg::JsArray SqlStorage::wrapSqlRowRaw(jsg::Lock& js, kj::Array<SqlValue> row) {
  return jsg::JsArray(js.withinHandleScope([&]() -> v8::Local<v8::Array> {
    v8::LocalVector<v8::Value> values(js.v8Isolate);
    for (auto& field: row) {
      values.push_back(wrapSqlValue(js, kj::mv(field)));
    }
    return v8::Array::New(js.v8Isolate, values.data(), values.size());
  }));
}

SqlStorage::Cursor::State::State(SqliteDatabase& db,
    SqliteDatabase::Regulator& regulator,
    kj::StringPtr sqlCode,
    kj::Array<BindingValue> bindingsParam)
    : bindings(kj::mv(bindingsParam)),
      query(db.run(regulator, sqlCode, mapBindings(bindings).asPtr())) {}

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

void SqlStorage::CachedColumnNames::ensureInitialized(
    jsg::Lock& js, SqliteDatabase::Query& source) {
  if (names == kj::none) {
    js.withinHandleScope([&] {
      auto builder = kj::heapArrayBuilder<jsg::JsRef<jsg::JsString>>(source.columnCount());
      for (auto i: kj::zeroTo(builder.capacity())) {
        builder.add(js, js.str(source.getColumnName(i)));
      }
      names = builder.finish();
    });
  }
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
  KJ_IF_SOME(s, state) {
    cachedColumnNames.ensureInitialized(js, s->query);
  }

  auto self = JSG_THIS;
  auto maybeRow = rowIteratorNext(js, self);
  bool done = maybeRow == kj::none;
  return {
    .done = done,
    .value = kj::mv(maybeRow),
  };
}

jsg::JsArray SqlStorage::Cursor::toArray(jsg::Lock& js) {
  KJ_IF_SOME(s, state) {
    cachedColumnNames.ensureInitialized(js, s->query);
  }

  auto self = JSG_THIS;
  v8::LocalVector<v8::Value> results(js.v8Isolate);
  for (;;) {
    auto maybeRow = rowIteratorNext(js, self);
    KJ_IF_SOME(row, maybeRow) {
      results.push_back(wrapSqlRow(js, kj::mv(row)));
    } else {
      break;
    }
  }

  return jsg::JsArray(v8::Array::New(js.v8Isolate, results.data(), results.size()));
}

jsg::JsValue SqlStorage::Cursor::one(jsg::Lock& js) {
  KJ_IF_SOME(s, state) {
    cachedColumnNames.ensureInitialized(js, s->query);
  }

  auto self = JSG_THIS;
  auto row = JSG_REQUIRE_NONNULL(rowIteratorNext(js, self), Error,
      "Expected exactly one result from SQL query, but got no results.");
  auto result = wrapSqlRow(js, kj::mv(row));

  // Manually grab the query, check that there are no more results, and clean it up.
  auto& query = KJ_ASSERT_NONNULL(state)->query;
  query.nextRow();
  bool hadOneResult = query.isDone();

  // Save off row counts before the query goes away, just like iteratorImpl() would do when done.
  rowsRead = query.getRowsRead();
  rowsWritten = query.getRowsWritten();

  // End the query even if it wasn't done.
  state = kj::none;

  JSG_REQUIRE(
      hadOneResult, Error, "Expected exactly one result from SQL query, but got multiple results.");

  return result;
}

jsg::Ref<SqlStorage::Cursor::RowIterator> SqlStorage::Cursor::rows(jsg::Lock& js) {
  KJ_IF_SOME(s, state) {
    cachedColumnNames.ensureInitialized(js, s->query);
  }
  return jsg::alloc<RowIterator>(JSG_THIS);
}

kj::Maybe<SqlStorage::SqlRow> SqlStorage::Cursor::rowIteratorNext(
    jsg::Lock& js, jsg::Ref<Cursor>& obj) {
  auto names = obj->cachedColumnNames.get();
  return iteratorImpl(js, obj, [&](State& state, uint i, SqlValue&& value) {
    return SqlRow::Field{
      // A little trick here: We know there are no HandleScopes on the stack between JSG and here,
      // so we can return a dict keyed by local handles, which avoids constructing new V8Refs here
      // which would be relatively slower.
      .name = names[i].getHandle(js),
      .value = kj::mv(value)};
  }).map([&](kj::Array<SqlRow::Field>&& fields) { return SqlRow{.fields = kj::mv(fields)}; });
}

jsg::Ref<SqlStorage::Cursor::RawIterator> SqlStorage::Cursor::raw(jsg::Lock&) {
  return jsg::alloc<RawIterator>(JSG_THIS);
}

// Returns the set of column names for the current Cursor. An exception will be thrown if the
// iterator has already been fully consumed. The resulting columns may contain duplicate entries,
// for instance a `SELECT *` across a join of two tables that share a column name.
kj::Array<jsg::JsRef<jsg::JsString>> SqlStorage::Cursor::getColumnNames(jsg::Lock& js) {
  KJ_IF_SOME(s, state) {
    cachedColumnNames.ensureInitialized(js, s->query);
    return KJ_MAP(name, this->cachedColumnNames.get()) { return name.addRef(js); };
  } else {
    JSG_FAIL_REQUIRE(Error, "Cannot call .getColumnNames after Cursor iterator has been consumed.");
  }
}

kj::Maybe<kj::Array<SqlStorage::SqlValue>> SqlStorage::Cursor::rawIteratorNext(
    jsg::Lock& js, jsg::Ref<Cursor>& obj) {
  return iteratorImpl(
      js, obj, [&](State& state, uint i, SqlValue&& value) { return kj::mv(value); });
}

template <typename Func>
auto SqlStorage::Cursor::iteratorImpl(jsg::Lock& js, jsg::Ref<Cursor>& obj, Func&& func)
    -> kj::Maybe<
        kj::Array<decltype(func(kj::instance<State&>(), uint(), kj::instance<SqlValue&&>()))>> {
  using Element = decltype(func(kj::instance<State&>(), uint(), kj::instance<SqlValue&&>()));

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

  if (state.isFirst) {
    // Little hack: We don't want to call query.nextRow() at the end of this method because it
    // may invalidate the backing buffers of StringPtrs that we haven't returned to JS yet.
    state.isFirst = false;
  } else {
    state.query.nextRow();
  }

  auto& query = state.query;

  if (query.isDone()) {
    // Save off row counts before the query goes away.
    obj->rowsRead = query.getRowsRead();
    obj->rowsWritten = query.getRowsWritten();
    // Clean up the query proactively.
    obj->state = kj::none;
    return kj::none;
  }

  auto results = kj::heapArrayBuilder<Element>(query.columnCount());
  for (auto i: kj::zeroTo(results.capacity())) {
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
    results.add(func(state, i, kj::mv(value)));
  }
  return results.finish();
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
