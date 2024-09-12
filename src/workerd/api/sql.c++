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
    jsg::Lock& js, kj::String querySql, jsg::Arguments<BindingValue> bindings) {
  SqliteDatabase::Regulator& regulator = *this;
  return jsg::alloc<Cursor>(getDb(js), regulator, querySql, kj::mv(bindings));
}

SqlStorage::IngestResult SqlStorage::ingest(jsg::Lock& js, kj::String querySql) {
  SqliteDatabase::Regulator& regulator = *this;
  auto result = getDb(js).ingestSql(regulator, querySql);
  return IngestResult(
      kj::str(result.remainder), result.rowsRead, result.rowsWritten, result.statementCount);
}

jsg::Ref<SqlStorage::Statement> SqlStorage::prepare(jsg::Lock& js, kj::String query) {
  return jsg::alloc<Statement>(getDb(js).prepare(*this, query));
}

jsg::JsArray SqlStorage::batch(jsg::Lock& js,
    kj::Array<kj::OneOf<jsg::Ref<SqlStorage::Statement>, jsg::Ref<SqlStorage::BoundStatement>>>
        statements) {
  // Batch needs to be run as a transaction, but we can't use `BEGIN TRANSACTION` because we may
  // already be in a transaction (especially an implicit transaction). That's OK, we can use
  // savepoints.
  auto& db = getDb(js);
  execMemoized(db, beginBatch, "SAVEPOINT _cf_batch");
  bool success = false;
  KJ_DEFER({
    if (!success) {
      execMemoized(db, rollbackBatch, "ROLLBACK TO _cf_batch");
    }
  });

  v8::LocalVector<v8::Value> results(js.v8Isolate);

  for (auto& statement: statements) {
    KJ_SWITCH_ONEOF(statement) {
      KJ_CASE_ONEOF(unbound, jsg::Ref<SqlStorage::Statement>) {
        // If the user put an unbound statement in the batch, assume that the statement simply
        // doesn't have any arguments.
        results.push_back(unbound->run(kj::Array<BindingValue>(nullptr))
            ->getResults(js));
      }
      KJ_CASE_ONEOF(bound, jsg::Ref<SqlStorage::BoundStatement>) {
        results.push_back(bound->run()->getResults(js));
      }
    }
  };

  execMemoized(db, commitBatch, "RELEASE _cf_batch");
  success = true;

  return jsg::JsArray(v8::Array::New(js.v8Isolate, results.data(), results.size()));
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

void SqlStorage::onError(kj::StringPtr message) const {
  JSG_ASSERT(false, Error, message);
}

bool SqlStorage::allowTransactions() const {
  if (IoContext::hasCurrent()) {
    IoContext::current().logWarningOnce(
        "To execute a transaction, please use the state.storage.transaction() API instead of the "
        "SQL BEGIN TRANSACTION or SAVEPOINT statements. The JavaScript API is safer because it "
        "will automatically roll back on exceptions, and because it interacts correctly with "
        "Durable Objects' automatic atomic write coalescing.");
  }
  return false;
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

SqlStorage::Cursor::State::State(kj::RefcountedWrapper<SqliteDatabase::Statement>& statement,
    kj::Array<BindingValue> bindingsParam)
    : dependency(statement.addWrappedRef()),
      bindings(kj::mv(bindingsParam)),
      query(statement.getWrapped().run(mapBindings(bindings).asPtr())) {}

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

void SqlStorage::Cursor::CachedColumnNames::ensureInitialized(
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

jsg::JsArray SqlStorage::Cursor::getResults(jsg::Lock& js) {
  KJ_IF_SOME(s, state) {
    cachedColumnNames.ensureInitialized(js, s->query);
  }

  // Wrap in handle scope mostly for the benefit of `batch()`, whose implementation calls this
  // many times.
  return jsg::JsArray(js.withinHandleScope([&]() {
    v8::LocalVector<v8::Value> results(js.v8Isolate);
    jsg::Ref<Cursor> self = JSG_THIS;
    for (;;) {
      KJ_IF_SOME(result, rowIteratorNext(js, self)) {
        results.push_back(wrapSqlRow(js, kj::mv(result)));
      } else {
        break;
      }
    }
    return v8::Array::New(js.v8Isolate, results.data(), results.size());
  }));
}

jsg::Ref<SqlStorage::Cursor::Meta> SqlStorage::Cursor::getMeta() {
  return jsg::alloc<Meta>(JSG_THIS);
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
jsg::JsArray SqlStorage::Cursor::getColumnNames(jsg::Lock& js) {
  KJ_IF_SOME(s, state) {
    cachedColumnNames.ensureInitialized(js, s->query);
    v8::LocalVector<v8::Value> results(js.v8Isolate);
    for (auto& name: cachedColumnNames.get()) {
      results.push_back(name.getHandle(js));
    }
    return jsg::JsArray(v8::Array::New(js.v8Isolate, results.data(), results.size()));
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

SqlStorage::Statement::Statement(SqliteDatabase::Statement&& statement)
    : statement(IoContext::current().addObject(
          kj::refcountedWrapper<SqliteDatabase::Statement>(kj::mv(statement)))) {}

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

jsg::Ref<SqlStorage::Cursor> SqlStorage::Statement::run(jsg::Arguments<BindingValue> bindings) {
  auto& statementRef = *statement;  // validate we're in the right IoContext

  KJ_IF_SOME(c, currentCursor) {
    // Invalidate previous cursor if it's still running. We have to do this because SQLite only
    // allows one execution of a statement at a time.
    //
    // If this is a problem, we could consider a scheme where we dynamically instantiate copies of
    // the statement as needed. However, that risks wasting memory if the app commonly leaves
    // cursors open and the GC doesn't run proactively enough.
    KJ_IF_SOME(s, c.state) {
      c.canceled = !s->query.isDone();
      c.state = kj::none;
    }
    c.selfRef = kj::none;
    c.statement = kj::none;
    currentCursor = kj::none;
  }

  auto result = jsg::alloc<Cursor>(cachedColumnNames, statementRef, kj::mv(bindings));
  result->statement = JSG_THIS;

  result->selfRef = currentCursor;
  currentCursor = *result;

  return result;
}

jsg::Ref<SqlStorage::BoundStatement> SqlStorage::Statement::bind(
    jsg::Arguments<BindingValue> bindings) {
  return jsg::alloc<BoundStatement>(JSG_THIS, kj::mv(bindings));
}

jsg::JsArray SqlStorage::Statement::raw(
    const v8::FunctionCallbackInfo<v8::Value>& args,
    const jsg::TypeHandler<BindingValue>& bindingTypeHandler,
    const jsg::TypeHandler<RawOptions>& optionsTypeHandler) {
  jsg::Lock& js = jsg::Lock::from(args.GetIsolate());

  uint bindingCount = statement->getWrapped().bindingCount();

  uint argc = args.Length();

  RawOptions options;
  if (argc > bindingCount) {
    // More args than there are bindings, so assume the arg after the last binding is the options
    // struct.
    options = JSG_REQUIRE_NONNULL(optionsTypeHandler.tryUnwrap(js, args[bindingCount]), TypeError,
        "Failed to execute SQL statement: argument ", bindingCount, " is not a valid options "
        "object.");
  }

  uint n = kj::min(bindingCount, argc);
  auto bindings = kj::heapArrayBuilder<BindingValue>(n);
  for (uint i: kj::zeroTo(n)) {
    bindings.add(JSG_REQUIRE_NONNULL(bindingTypeHandler.tryUnwrap(js, args[i]), TypeError,
        "Failed to execute SQL statement: argument ", i, " is not a valid SQL argument type."));
  }

  return rawImpl(js, bindings.finish(), kj::mv(options));
}

jsg::JsArray SqlStorage::Statement::rawImpl(
    jsg::Lock& js, jsg::Arguments<BindingValue> bindings,
    const Statement::RawOptions& options) {
  auto cursor = run(kj::mv(bindings));

  KJ_IF_SOME(s, cursor->state) {
    cursor->cachedColumnNames.ensureInitialized(js, s->query);
  }

  v8::LocalVector<v8::Value> results(js.v8Isolate);

  if (options.columnNames) {
    results.push_back(cursor->getColumnNames(js));
  }

  for (;;) {
    KJ_IF_SOME(result, Cursor::rawIteratorNext(js, cursor)) {
      results.push_back(wrapSqlRowRaw(js, kj::mv(result)));
    } else {
      break;
    }
  }

  return jsg::JsArray(v8::Array::New(js.v8Isolate, results.data(), results.size()));
}

jsg::JsValue SqlStorage::Statement::first(
    const v8::FunctionCallbackInfo<v8::Value>& args,
    const jsg::TypeHandler<BindingValue>& bindingTypeHandler) {
  jsg::Lock& js = jsg::Lock::from(args.GetIsolate());

  uint bindingCount = statement->getWrapped().bindingCount();

  uint argc = args.Length();

  kj::Maybe<jsg::JsString> columnName;
  if (argc > bindingCount) {
    // More args than there are bindings, so assume the arg after the last binding is the column
    // name.
    columnName = jsg::JsValue(args[bindingCount]).toJsString(js);
  }

  uint n = kj::min(bindingCount, argc);
  auto bindings = kj::heapArrayBuilder<BindingValue>(n);
  for (uint i: kj::zeroTo(n)) {
    bindings.add(JSG_REQUIRE_NONNULL(bindingTypeHandler.tryUnwrap(js, args[i]), TypeError,
        "Failed to execute SQL statement: argument ", i, " is not a valid SQL argument type."));
  }

  return firstImpl(js, bindings.finish(), kj::mv(columnName));
}

jsg::JsValue SqlStorage::Statement::firstImpl(
    jsg::Lock& js, jsg::Arguments<BindingValue> bindings, jsg::Optional<jsg::JsString> column) {
  // Note that because the cursor is never returned to JS, it is never subject to GC, and will be
  // deterministically destroyed when this function returns.
  auto cursor = run(kj::mv(bindings));

  auto result = KJ_UNWRAP_OR(Cursor::rowIteratorNext(js, cursor), {
    // No results.
    return js.null();
  });

  KJ_IF_SOME(c, column) {
    for (auto& field: result.fields) {
      if (field.name == c) {
        return jsg::JsValue(wrapSqlValue(js, kj::mv(field.value)));
      }
    }
    JSG_FAIL_REQUIRE(TypeError, "SQL query results have no column named \"", c, "\".");
  } else {
    // return whole row
    return jsg::JsValue(wrapSqlRow(js, kj::mv(result)));
  }
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

jsg::Ref<SqlStorage::Cursor> SqlStorage::BoundStatement::run() {
  return statement->run(cloneBindings());
}

jsg::JsArray SqlStorage::BoundStatement::raw(jsg::Lock& js,
    jsg::Optional<Statement::RawOptions> options) {
  return statement->rawImpl(js, cloneBindings(), options.orDefault({}));
}

jsg::JsValue SqlStorage::BoundStatement::first(
    jsg::Lock& js, jsg::Optional<jsg::JsString> column) {
  return statement->firstImpl(js, cloneBindings(), column);
}

jsg::Arguments<SqlStorage::BindingValue> SqlStorage::BoundStatement::cloneBindings() {
  // Annoyingly the bound statement could theoretically be executed multiple times and so we need
  // to clone the bindings each time in order to pass an owned copy to the query while also keeping
  // a copy for future calls. In theory with some refactoring we could avoid this but this is only
  // a compatibility shim anyway so we're not optimizing for it.

  return KJ_MAP(binding, bindings) -> BindingValue {
    KJ_IF_SOME(b, binding) {
      KJ_SWITCH_ONEOF(b) {
        KJ_CASE_ONEOF(bytes, kj::Array<const byte>) {
          return NonNullBindingValue(kj::Array<const byte>(kj::heapArray(bytes.asPtr())));
        }
        KJ_CASE_ONEOF(text, kj::String) {
          return NonNullBindingValue(kj::heapString(text));
        }
        KJ_CASE_ONEOF(number, double) {
          return NonNullBindingValue(number);
        }
      }
      KJ_UNREACHABLE;
    } else {
      return kj::none;
    }
  };
}

}  // namespace workerd::api
