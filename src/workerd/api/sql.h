// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/util/sqlite.h>
#include <workerd/io/compatibility-date.capnp.h>
#include <workerd/io/io-context.h>

namespace workerd::api {

class DurableObjectStorage;

typedef kj::OneOf<kj::Array<const byte>, kj::String, double> SqlBindingValue;
class SqlDatabase;
class SqlPreparedStatement;

class SqlResult final: public jsg::Object {
  class CachedColumnNames;
public:
  template <typename... Params>
  SqlResult(Params&&... params)
      : state(IoContext::current().addObject(kj::heap<State>(kj::fwd<Params>(params)...))),
        ownCachedColumnNames(nullptr),  // silence bogus Clang warning on next line
        cachedColumnNames(ownCachedColumnNames.emplace()) {}

  template <typename... Params>
  SqlResult(CachedColumnNames& cachedColumnNames, Params&&... params)
      : state(IoContext::current().addObject(kj::heap<State>(kj::fwd<Params>(params)...))),
        cachedColumnNames(cachedColumnNames) {}
  ~SqlResult() noexcept(false);

  JSG_RESOURCE_TYPE(SqlResult, CompatibilityFlags::Reader flags) {
    JSG_ITERABLE(rows);
    JSG_METHOD(raw);
  }

  using Value = kj::Maybe<kj::OneOf<kj::Array<byte>, kj::StringPtr, double>>;
  // One value returned from SQL. Note that we intentionally return StringPtr instead of String
  // because we know that the underlying buffer returned by SQLite will be valid long enough to be
  // converted by JSG into a V8 string. For byte arrays, on the other hand, we pass ownership to
  // JSG, which does not need to make a copy.

  using RowDict = jsg::Dict<Value, v8::Local<v8::String>>;
  JSG_ITERATOR(RowIterator, rows, RowDict, jsg::Ref<SqlResult>, rowIteratorNext);
  JSG_ITERATOR(RawIterator, raw, kj::Array<Value>, jsg::Ref<SqlResult>, rawIteratorNext);

private:
  class CachedColumnNames {
    // Helper class to cache column names for a query so that we don't have to recreate the V8
    // strings for every row.
    //
    // TODO(perf): Can we further cache the V8 object layout information for a row?
  public:
    kj::ArrayPtr<jsg::V8Ref<v8::String>> get() { return KJ_REQUIRE_NONNULL(names); }
    // Get the cached names. ensureInitialized() must have been called previously.

    void ensureInitialized(jsg::Lock& js, SqliteDatabase::Query& source);

  private:
    kj::Maybe<kj::Array<jsg::V8Ref<v8::String>>> names;
  };

  struct State {
    kj::Own<void> dependency;
    // Refcount on the SqliteDatabase::Statement underlying the query, if any.

    kj::Array<SqlBindingValue> bindings;
    // The bindings that were used to construct `query`. We have to keep these alive until the query
    // is done since it might contain pointers into strings and blobs.

    SqliteDatabase::Query query;

    bool isFirst = true;

    State(kj::RefcountedWrapper<SqliteDatabase::Statement>& statement,
          kj::Array<SqlBindingValue> bindings);
    State(SqliteDatabase& db, SqliteDatabase::Regulator& regulator,
          kj::StringPtr sqlCode, kj::Array<SqlBindingValue> bindings);
  };

  kj::Maybe<IoOwn<State>> state;
  // Nulled out when query is done or canceled.

  bool canceled = false;
  // True if the cursor was canceled by a new call to the same statement. This is used only to
  // flag an error if the application tries to reuse the cursor.

  kj::Maybe<kj::Maybe<SqlResult&>&> selfRef;
  // Reference to a weak reference that might point back to this object. If so, null it out at
  // destruction. Used by SqlPreparedStatement to invalidate past cursors when the statement is
  // executed again.

  kj::Maybe<CachedColumnNames> ownCachedColumnNames;
  CachedColumnNames& cachedColumnNames;

  static kj::Array<const SqliteDatabase::Query::ValuePtr> mapBindings(
      kj::ArrayPtr<SqlBindingValue> values);

  static kj::Maybe<RowDict> rowIteratorNext(jsg::Lock& js, jsg::Ref<SqlResult>& obj);
  static kj::Maybe<kj::Array<Value>> rawIteratorNext(jsg::Lock& js, jsg::Ref<SqlResult>& obj);
  template <typename Func>
  static auto iteratorImpl(jsg::Lock& js, jsg::Ref<SqlResult>& obj, Func&& func)
      -> kj::Maybe<kj::Array<
          decltype(func(kj::instance<State&>(), uint(), kj::instance<Value&&>()))>>;

  friend class SqlPreparedStatement;
};

class SqlPreparedStatement final: public jsg::Object {
public:
  SqlPreparedStatement(SqliteDatabase::Statement&& statement);

  jsg::Ref<SqlResult> run(jsg::Arguments<SqlBindingValue> bindings);

  JSG_RESOURCE_TYPE(SqlPreparedStatement, CompatibilityFlags::Reader flags) {
    JSG_CALLABLE(run);
  }

private:
  IoOwn<kj::RefcountedWrapper<SqliteDatabase::Statement>> statement;

  kj::Maybe<SqlResult&> currentCursor;
  // Weak reference to the SqlResult that is currently using this statement.

  SqlResult::CachedColumnNames cachedColumnNames;
  // All queries from the same prepared statement have the same column names, so we can cache them
  // on the statement.

  friend class SqlResult;
};

class SqlDatabase final: public jsg::Object, private SqliteDatabase::Regulator {
public:
  SqlDatabase(SqliteDatabase& sqlite, jsg::Ref<DurableObjectStorage> storage);
  ~SqlDatabase();

  jsg::Ref<SqlResult> exec(jsg::Lock& js, kj::String query, jsg::Arguments<SqlBindingValue> bindings);

  jsg::Ref<SqlPreparedStatement> prepare(jsg::Lock& js, kj::String query);

  JSG_RESOURCE_TYPE(SqlDatabase, CompatibilityFlags::Reader flags) {
    JSG_METHOD(exec);
    JSG_METHOD(prepare);
  }

private:
  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(storage);
  }

  bool isAllowedName(kj::StringPtr name) override;
  bool isAllowedTrigger(kj::StringPtr name) override;
  void onError(kj::StringPtr message) override;

  IoPtr<SqliteDatabase> sqlite;
  jsg::Ref<DurableObjectStorage> storage;

  friend class SqlPreparedStatement;
  friend SqlResult;
};

#define EW_SQL_ISOLATE_TYPES                \
  api::SqlDatabase,                         \
  api::SqlPreparedStatement,                \
  api::SqlResult,                           \
  api::SqlResult::RowIterator,              \
  api::SqlResult::RowIterator::Next,        \
  api::SqlResult::RawIterator,              \
  api::SqlResult::RawIterator::Next
// The list of sql.h types that are added to worker.c++'s JSG_DECLARE_ISOLATE_TYPE

}  // namespace workerd::api
