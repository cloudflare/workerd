// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "sql.h"

#include "actor-state.h"

#include <workerd/io/io-context.h>
#include <workerd/util/autogate.h>
#include <workerd/util/sentry.h>

#if _WIN32
#define strncasecmp _strnicmp
#else
#include <strings.h>
#endif

namespace workerd::api {

// Maximum total size of all cached statements (measured in size of the SQL code). If cached
// statements exceed this, we remove the LRU statement(s).
//
// Hopefully most apps don't ever hit this, but it's important to have a limit in case of
// queries containing dynamic content or excessively large one-off queries.
static constexpr uint SQL_STATEMENT_CACHE_MAX_SIZE = 1024 * 1024;

namespace {

// While a SQL query is executing, points to a slot in which a JavaScript exception thrown by an
// application-defined SQL function can be stashed. Exceptions cannot propagate through SQLite's
// stack frames, so the function wrapper converts the exception to a kj::Exception for the
// unwind; this slot additionally carries the original JavaScript exception object across that
// unwind so that the query call site can rethrow it with its identity and stack intact, rather
// than throwing a copy that round-tripped through the kj::Exception. (Compare `vfsErrorListener`
// in sqlite.c++, which uses the same pattern.)
thread_local kj::Maybe<jsg::Value>* currentUserFunctionError = nullptr;

// Calls func(), rethrowing the original JavaScript exception object if an application-defined
// SQL function threw during the call.
template <typename Func>
auto rethrowingUserFunctionErrors(jsg::Lock& js, Func&& func) -> decltype(func()) {
  kj::Maybe<jsg::Value> error;
  auto prev = currentUserFunctionError;
  currentUserFunctionError = &error;
  KJ_DEFER(currentUserFunctionError = prev);
  try {
    return func();
  } catch (...) {
    KJ_IF_SOME(e, error) {
      js.throwException(kj::mv(e));
    }
    throw;
  }
}

}  // namespace

SqlStorage::SqlStorage(jsg::Ref<DurableObjectStorage> storage)
    : storage(kj::mv(storage)),
      statementCache(IoContext::current().addObject(kj::heap<StatementCache>())) {}

SqlStorage::~SqlStorage() {}

jsg::Ref<SqlStorage::Cursor> SqlStorage::exec(
    jsg::Lock& js, jsg::JsString querySql, jsg::Arguments<BindingValue> bindings) {
  auto& context = IoContext::current();
  TraceContext traceContext = context.makeUserTraceSpan("durable_object_storage_exec"_kjc);
  traceContext.setTag("db.system.name"_kjc, "cloudflare-durable-object-sql"_kjc);
  traceContext.setTag("db.operation.name"_kjc, "exec"_kjc);
  traceContext.setTag("db.query.text"_kjc, kj::str(querySql));
  traceContext.setTag(
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
  if (traceContext.isObserved()) {
    doneCallback = [traceContext = context.addObject(kj::heap(kj::mv(traceContext)))](
                       Cursor& cursor) mutable {
      int64_t rowsRead = cursor.getRowsRead();
      int64_t rowsWritten = cursor.getRowsWritten();
      traceContext->setTag("cloudflare.durable_object.response.rows_read"_kjc, rowsRead);
      traceContext->setTag("cloudflare.durable_object.response.rows_written"_kjc, rowsWritten);
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
    return rethrowingUserFunctionErrors(js, [&]() {
      return js.alloc<Cursor>(js, kj::mv(doneCallback), db,
          SqliteDatabase::StaticRegulator(regulator), js.toString(querySql), kj::mv(bindings));
    });
  }

  auto result = rethrowingUserFunctionErrors(js, [&]() {
    return js.alloc<Cursor>(js, kj::mv(doneCallback), slot.addRef(), kj::mv(bindings));
  });

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

jsg::Value SqlStorage::wrapFunctionArgument(
    jsg::Lock& js, const SqliteDatabase::ValuePtr& arg, bool useBigIntArguments) {
  KJ_SWITCH_ONEOF(arg) {
    KJ_CASE_ONEOF(blob, kj::ArrayPtr<const byte>) {
      return js.v8Ref(v8::Local<v8::Value>(jsg::JsValue(js.wrapBytes(kj::heapArray(blob)))));
    }
    KJ_CASE_ONEOF(text, kj::StringPtr) {
      return js.v8Ref(v8::Local<v8::Value>(js.str(text)));
    }
    KJ_CASE_ONEOF(i, int64_t) {
      if (useBigIntArguments) {
        return js.v8Ref(v8::Local<v8::Value>(js.bigInt(i)));
      }
      // Matching node:sqlite, we'd rather fail loudly than silently lose precision.
      JSG_REQUIRE(i >= -9007199254740991 && i <= 9007199254740991, RangeError,
          "Integer argument to SQL function is too large to be represented as a JavaScript "
          "number: ",
          i, ". Pass the option `useBigIntArguments: true` to receive integers as BigInts.");
      return js.v8Ref(v8::Local<v8::Value>(js.num(static_cast<double>(i))));
    }
    KJ_CASE_ONEOF(d, double) {
      return js.v8Ref(v8::Local<v8::Value>(js.num(d)));
    }
    KJ_CASE_ONEOF(_, decltype(nullptr)) {
      return js.v8Ref(v8::Local<v8::Value>(js.null()));
    }
  }
  KJ_UNREACHABLE;
}

SqliteDatabase::Value SqlStorage::unwrapFunctionResult(jsg::Lock& js, const jsg::Value& result) {
  auto handle = result.getHandle(js);

  // Matching node:sqlite, only types with an unambiguous SQL representation are accepted;
  // notably, there is no implicit coercion of booleans or arbitrary objects.
  if (handle->IsNullOrUndefined()) {
    return nullptr;
  } else if (handle->IsNumber()) {
    return handle.As<v8::Number>()->Value();
  } else if (handle->IsBigInt()) {
    bool lossless = false;
    int64_t value = handle.As<v8::BigInt>()->Int64Value(&lossless);
    JSG_REQUIRE(lossless, RangeError,
        "BigInt returned from a SQL function is too large to be stored as a SQL integer.");
    return value;
  } else if (handle->IsString()) {
    return js.toString(jsg::JsString(handle.As<v8::String>()));
  } else if (handle->IsArrayBuffer()) {
    return kj::Array<const byte>(jsg::asBytes(handle.As<v8::ArrayBuffer>()));
  } else if (handle->IsArrayBufferView()) {
    return kj::Array<const byte>(jsg::asBytes(handle.As<v8::ArrayBufferView>()));
  } else if (handle->IsPromise()) {
    JSG_FAIL_REQUIRE(TypeError,
        "SQL functions must return synchronously: the function returned a Promise. A function may "
        "schedule asynchronous work, but cannot return or await its result.");
  } else {
    JSG_FAIL_REQUIRE(TypeError,
        "SQL functions must return a number, BigInt, string, ArrayBuffer (or view), null, or "
        "undefined.");
  }
}

void SqlStorage::stashAndTunnelFunctionError(jsg::Lock& js, jsg::Value exception) {
  // Stash the original JavaScript exception object so that the query call site can rethrow
  // it as-is, then unwind through SQLite with a tunneled kj::Exception. (The tunneled copy
  // also serves as a fallback in case no stash slot is active.)
  auto tunneled = js.exceptionToKj(exception.addRef(js));
  if (currentUserFunctionError != nullptr && *currentUserFunctionError == kj::none) {
    *currentUserFunctionError = kj::mv(exception);
  }
  kj::throwFatalException(kj::mv(tunneled));
}

namespace {

// Returns the declared parameter count (the `length` property) of the JavaScript function
// underlying `callback`, used as the SQL function's required argument count when the `varargs`
// option is false.
uint getFunctionLength(jsg::Lock& js, SqlStorage::FunctionCallback& callback) {
  auto handle = KJ_ASSERT_NONNULL(callback.tryGetHandle(js.v8Isolate));
  auto lengthValue = jsg::check(handle->Get(js.v8Context(), js.strIntern("length"_kj)));
  double length = jsg::check(lengthValue->NumberValue(js.v8Context()));
  if (!(length > 0)) return 0;  // negative or NaN
  // Out-of-range counts are rejected with a friendly error by the SQLite layer; just avoid
  // overflowing the conversion here.
  return static_cast<uint>(kj::min(length, 1000.0));
}

}  // namespace

void SqlStorage::registerFunction(jsg::Lock& js,
    kj::String name,
    kj::OneOf<FunctionCallback, FunctionOptions> optionsOrCallback,
    jsg::Optional<FunctionCallback> maybeCallback) {
  FunctionOptions options;
  kj::Maybe<FunctionCallback> callbackFromArgs;
  KJ_SWITCH_ONEOF(optionsOrCallback) {
    KJ_CASE_ONEOF(cb, FunctionCallback) {
      JSG_REQUIRE(maybeCallback == kj::none, TypeError,
          "function() must be called as function(name, callback) or "
          "function(name, options, callback).");
      callbackFromArgs = kj::mv(cb);
    }
    KJ_CASE_ONEOF(opts, FunctionOptions) {
      options = kj::mv(opts);
      callbackFromArgs =
          kj::mv(JSG_REQUIRE_NONNULL(maybeCallback, TypeError, "function() requires a callback."));
    }
  }
  auto callback = KJ_ASSERT_NONNULL(kj::mv(callbackFromArgs));

  bool useBigIntArguments = options.useBigIntArguments.orDefault(false);
  kj::Maybe<uint> argCount;
  if (!options.varargs.orDefault(false)) {
    argCount = getFunctionLength(js, callback);
  }

  auto& db = getDb(js);
  db.registerFunction(regulator, name, argCount,
      [callback = kj::mv(callback), useBigIntArguments, isolate = js.v8Isolate](
          kj::ArrayPtr<const SqliteDatabase::ValuePtr> args) mutable -> SqliteDatabase::Value {
    // We can only get here while a query is executing, and queries on this database are only
    // executed from the JavaScript API, so the current thread must hold the isolate lock.
    auto& js = jsg::Lock::from(isolate);

    return js.tryCatch([&]() -> SqliteDatabase::Value {
      return js.withinHandleScope([&]() -> SqliteDatabase::Value {
        auto jsArgs = kj::heapArrayBuilder<jsg::Value>(args.size());
        for (auto& arg: args) {
          jsArgs.add(wrapFunctionArgument(js, arg, useBigIntArguments));
        }

        auto result = callback(js, jsg::Arguments<jsg::Value>(jsArgs.finish()));
        return unwrapFunctionResult(js, result);
      });
    }, [&](jsg::Value exception) -> SqliteDatabase::Value {
      stashAndTunnelFunctionError(js, kj::mv(exception));
    });
  });
}

namespace {

// Accumulator state for an application-defined aggregate function: an arbitrary JavaScript
// value.
struct JsAggregateState final: public SqliteDatabase::AggregateState {
  jsg::Value value;

  JsAggregateState(jsg::Value value): value(kj::mv(value)) {}
};

// Produces the initial accumulator from the aggregate's `start` option: the option's value, or
// the result of calling it if it is a function (so that mutable accumulators get a fresh value
// per aggregation group), or undefined if absent.
jsg::Value resolveStartValue(jsg::Lock& js, kj::Maybe<jsg::JsRef<jsg::JsValue>>& start) {
  KJ_IF_SOME(s, start) {
    v8::Local<v8::Value> handle = s.getHandle(js);
    if (handle->IsFunction()) {
      auto fn = handle.As<v8::Function>();
      return js.v8Ref(
          jsg::check(fn->Call(js.v8Context(), v8::Undefined(js.v8Isolate), 0, nullptr)));
    }
    return js.v8Ref(handle);
  }
  return js.v8Ref(v8::Local<v8::Value>(js.undefined()));
}

// Returns the current accumulator value: the accumulated state if present, otherwise a fresh
// initial value.
jsg::Value aggregateAccumulator(
    jsg::Lock& js, kj::Maybe<jsg::Value> stateValue, kj::Maybe<jsg::JsRef<jsg::JsValue>>& start) {
  KJ_IF_SOME(s, stateValue) {
    return kj::mv(s);
  }
  return resolveStartValue(js, start);
}

}  // namespace

void SqlStorage::registerAggregate(jsg::Lock& js, kj::String name, AggregateOptions options) {
  auto& db = getDb(js);
  auto isolate = js.v8Isolate;

  bool isWindow = options.inverse != kj::none;
  bool useBigIntArguments = options.useBigIntArguments.orDefault(false);

  kj::Maybe<uint> argCount;
  if (!options.varargs.orDefault(false)) {
    // The step callback's first parameter is the accumulator, not a SQL argument.
    uint stepLength = getFunctionLength(js, options.step);
    argCount = stepLength > 0 ? stepLength - 1 : 0;
  }

  // The step and result wrappers (and, for window functions, the value wrapper) each need
  // their own reference to the `start` option, and the value wrapper shares the result
  // callback.
  kj::Maybe<jsg::JsRef<jsg::JsValue>> startForStep;
  kj::Maybe<jsg::JsRef<jsg::JsValue>> startForFinal;
  kj::Maybe<jsg::JsRef<jsg::JsValue>> startForValue;
  KJ_IF_SOME(s, options.start) {
    startForStep = s.addRef(js);
    startForFinal = s.addRef(js);
    if (isWindow) startForValue = s.addRef(js);
  }
  kj::Maybe<jsg::Function<jsg::Value(jsg::Value)>> resultForValue;
  KJ_IF_SOME(r, options.result) {
    if (isWindow) resultForValue = r.addRef(js);
  }

  SqliteDatabase::AggregateCallbacks callbacks{
    .step = [stepFn = kj::mv(options.step), start = kj::mv(startForStep), useBigIntArguments,
                isolate](kj::Maybe<kj::Own<SqliteDatabase::AggregateState>> state,
                kj::ArrayPtr<const SqliteDatabase::ValuePtr> args) mutable
    -> kj::Own<SqliteDatabase::AggregateState> {
    auto& js = jsg::Lock::from(isolate);
    return js.tryCatch([&]() -> kj::Own<SqliteDatabase::AggregateState> {
      return js.withinHandleScope([&]() -> kj::Own<SqliteDatabase::AggregateState> {
        auto jsArgs = kj::heapArrayBuilder<jsg::Value>(args.size() + 1);
        jsArgs.add(
            aggregateAccumulator(js, state.map([](kj::Own<SqliteDatabase::AggregateState>& s) {
          return kj::mv(kj::downcast<JsAggregateState>(*s).value);
        }),
                start));
        for (auto& arg: args) {
          jsArgs.add(wrapFunctionArgument(js, arg, useBigIntArguments));
        }

        auto result = stepFn(js, jsg::Arguments<jsg::Value>(jsArgs.finish()));
        return kj::heap<JsAggregateState>(kj::mv(result));
      });
    }, [&](jsg::Value exception) -> kj::Own<SqliteDatabase::AggregateState> {
      stashAndTunnelFunctionError(js, kj::mv(exception));
    });
  },
    .finalize = [resultFn = kj::mv(options.result), start = kj::mv(startForFinal), isolate](
                    kj::Maybe<kj::Own<SqliteDatabase::AggregateState>> state) mutable
    -> SqliteDatabase::Value {
    if (v8::Isolate::TryGetCurrent() != isolate) {
      // We are being invoked to clean up an abandoned query from outside the isolate lock (e.g.
      // an unconsumed cursor is being destroyed from the I/O thread), so we cannot call into
      // JavaScript -- and the result would be discarded anyway. Note that dropping `state` here
      // without the lock is safe: V8 handle destruction is deferred in that case.
      return nullptr;
    }
    auto& js = jsg::Lock::from(isolate);
    return js.tryCatch([&]() -> SqliteDatabase::Value {
      return js.withinHandleScope([&]() -> SqliteDatabase::Value {
        auto acc =
            aggregateAccumulator(js, state.map([](kj::Own<SqliteDatabase::AggregateState>& s) {
          return kj::mv(kj::downcast<JsAggregateState>(*s).value);
        }),
                start);

        KJ_IF_SOME(f, resultFn) {
          auto result = f(js, kj::mv(acc));
          return unwrapFunctionResult(js, result);
        } else {
          // No result callback: the result is the accumulator itself.
          return unwrapFunctionResult(js, acc);
        }
      });
    }, [&](jsg::Value exception) -> SqliteDatabase::Value {
      stashAndTunnelFunctionError(js, kj::mv(exception));
    });
  },
  };

  if (isWindow) {
    callbacks.value.emplace(
        [resultFn = kj::mv(resultForValue), start = kj::mv(startForValue), isolate](
            kj::Maybe<SqliteDatabase::AggregateState&> state) mutable -> SqliteDatabase::Value {
      auto& js = jsg::Lock::from(isolate);
      return js.tryCatch([&]() -> SqliteDatabase::Value {
        return js.withinHandleScope([&]() -> SqliteDatabase::Value {
          // Unlike the final result, this must not consume the state, since SQLite will keep
          // updating the same window.
          auto acc = aggregateAccumulator(js, state.map([&js](SqliteDatabase::AggregateState& s) {
            return kj::downcast<JsAggregateState>(s).value.addRef(js);
          }),
              start);

          KJ_IF_SOME(f, resultFn) {
            auto result = f(js, kj::mv(acc));
            return unwrapFunctionResult(js, result);
          } else {
            return unwrapFunctionResult(js, acc);
          }
        });
      }, [&](jsg::Value exception) -> SqliteDatabase::Value {
        stashAndTunnelFunctionError(js, kj::mv(exception));
      });
    });

    callbacks.inverse.emplace(
        [inverseFn = kj::mv(KJ_ASSERT_NONNULL(options.inverse)), useBigIntArguments, isolate](
            kj::Own<SqliteDatabase::AggregateState> state,
            kj::ArrayPtr<const SqliteDatabase::ValuePtr> args) mutable
        -> kj::Own<SqliteDatabase::AggregateState> {
      auto& js = jsg::Lock::from(isolate);
      return js.tryCatch([&]() -> kj::Own<SqliteDatabase::AggregateState> {
        return js.withinHandleScope([&]() -> kj::Own<SqliteDatabase::AggregateState> {
          auto jsArgs = kj::heapArrayBuilder<jsg::Value>(args.size() + 1);
          jsArgs.add(kj::mv(kj::downcast<JsAggregateState>(*state).value));
          for (auto& arg: args) {
            jsArgs.add(wrapFunctionArgument(js, arg, useBigIntArguments));
          }

          auto result = inverseFn(js, jsg::Arguments<jsg::Value>(jsArgs.finish()));
          return kj::heap<JsAggregateState>(kj::mv(result));
        });
      }, [&](jsg::Value exception) -> kj::Own<SqliteDatabase::AggregateState> {
        stashAndTunnelFunctionError(js, kj::mv(exception));
      });
    });
  }

  db.registerAggregateFunction(regulator, name, argCount, kj::mv(callbacks));
}

SqlStorage::IngestResult SqlStorage::ingest(jsg::Lock& js, kj::String querySql) {
  auto& context = IoContext::current();
  TraceContext traceContext = context.makeUserTraceSpan("durable_object_storage_ingest"_kjc);
  auto result =
      rethrowingUserFunctionErrors(js, [&]() { return getDb(js).ingestSql(regulator, querySql); });

  traceContext.setTag(
      "cloudflare.durable_object.response.rows_read"_kjc, static_cast<int64_t>(result.rowsRead));
  traceContext.setTag("cloudflare.durable_object.response.rows_written"_kjc,
      static_cast<int64_t>(result.rowsWritten));
  traceContext.setTag("cloudflare.durable_object.response.statement_count"_kjc,
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
  auto& context = IoContext::current();
  TraceContext traceContext =
      context.makeUserTraceSpan("durable_object_storage_getDatabaseSize"_kjc);
  traceContext.setTag("db.operation.name"_kjc, "getDatabaseSize"_kjc);
  auto& db = getDb(js);
  int64_t pages = execMemoized(db, pragmaPageCount,
      "select (select * from pragma_page_count) - (select * from pragma_freelist_count);")
                      .getInt64(0);
  auto dbSize = pages * getPageSize(db);
  traceContext.setTag(
      "cloudflare.durable_object.response.db_size"_kjc, static_cast<int64_t>(dbSize));
  return dbSize;
}

bool SqlStorageRegulator::isAllowedName(kj::StringPtr name) const {
  if (util::Autogate::isEnabled(util::AutogateKey::SQL_RESTRICT_RESERVED_NAMES)) {
    return strncasecmp(name.begin(), "_cf_", 4) != 0;
  }
  if (name.size() >= 4 && strncasecmp(name.begin(), "_cf_", 4) == 0) {
    LOG_WARNING_PERIODICALLY("SQL identifier matches reserved _cf_ prefix case-insensitively");
  }
  return !name.startsWith("_cf_");
}

bool SqlStorageRegulator::isAllowedTrigger(kj::StringPtr name) const {
  return true;
}

void SqlStorageRegulator::onError(kj::Maybe<int> sqliteErrorCode, kj::StringPtr message) const {
  JSG_ASSERT(false, Error, message);
}

bool SqlStorageRegulator::allowTransactions() const {
  JSG_FAIL_REQUIRE(Error,
      "To execute a transaction, please use the state.storage.transaction() or "
      "state.storage.transactionSync() APIs instead of the SQL BEGIN TRANSACTION or SAVEPOINT "
      "statements. The JavaScript API is safer because it will automatically roll back on "
      "exceptions, and because it interacts correctly with Durable Objects' automatic atomic "
      "write coalescing.");
}

bool SqlStorageRegulator::shouldAddQueryStats() const {
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
    SqliteDatabase::StaticRegulator regulator,
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
  rethrowingUserFunctionErrors(js, [&]() { query.nextRow(); });
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
