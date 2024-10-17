// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/api/actor-state.h>
#include <workerd/io/compatibility-date.capnp.h>
#include <workerd/io/io-context.h>
#include <workerd/jsg/jsg.h>
#include <workerd/util/sqlite.h>

namespace workerd::api {

class SqlStorage final: public jsg::Object, private SqliteDatabase::Regulator {
public:
  SqlStorage(jsg::Ref<DurableObjectStorage> storage);
  ~SqlStorage();

  using BindingValue = kj::Maybe<kj::OneOf<kj::Array<const byte>, kj::String, double>>;

  class Cursor;
  class Statement;
  struct IngestResult;

  // One value returned from SQL. Note that we intentionally return StringPtr instead of String
  // because we know that the underlying buffer returned by SQLite will be valid long enough to be
  // converted by JSG into a V8 string. For byte arrays, on the other hand, we pass ownership to
  // JSG, which does not need to make a copy.
  using SqlValue = kj::Maybe<kj::OneOf<kj::Array<byte>, kj::StringPtr, double>>;

  // One row of a SQL query result. This is an Object whose properties correspond to columns.
  using SqlRow = jsg::Dict<SqlValue, jsg::JsString>;

  jsg::Ref<Cursor> exec(jsg::Lock& js, jsg::JsString query, jsg::Arguments<BindingValue> bindings);
  IngestResult ingest(jsg::Lock& js, kj::String query);

  jsg::Ref<Statement> prepare(jsg::Lock& js, jsg::JsString query);

  double getDatabaseSize(jsg::Lock& js);

  JSG_RESOURCE_TYPE(SqlStorage, CompatibilityFlags::Reader flags) {
    JSG_METHOD(exec);

    if (flags.getWorkerdExperimental()) {
      // Prepared statement API is experimental-only and deprecated. exec() will automatically
      // handle caching prepared statements, so apps don't need to worry about it.
      JSG_METHOD(prepare);

      // 'ingest' functionality is still experimental-only
      JSG_METHOD(ingest);
    }

    JSG_READONLY_PROTOTYPE_PROPERTY(databaseSize, getDatabaseSize);

    JSG_NESTED_TYPE(Cursor);
    JSG_NESTED_TYPE(Statement);

    JSG_TS_OVERRIDE({
      exec<T extends Record<string, SqlStorageValue>>(query: string, ...bindings: any[]): SqlStorageCursor<T>
    });
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const;

private:
  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(storage);
  }

  bool isAllowedName(kj::StringPtr name) const override;
  bool isAllowedTrigger(kj::StringPtr name) const override;
  void onError(kj::Maybe<int> sqliteErrorCode, kj::StringPtr message) const override;
  bool allowTransactions() const override;

  SqliteDatabase& getDb(jsg::Lock& js) {
    return storage->getSqliteDb(js);
  }

  jsg::Ref<DurableObjectStorage> storage;

  kj::Maybe<uint> pageSize;
  kj::Maybe<IoOwn<SqliteDatabase::Statement>> pragmaPageCount;
  kj::Maybe<IoOwn<SqliteDatabase::Statement>> pragmaGetMaxPageCount;

  // Helper class to cache column names for a query so that we don't have to recreate the V8
  // strings for every row.
  class CachedColumnNames {
    // TODO(perf): Can we further cache the V8 object layout information for a row?
  public:
    // Get the cached names. ensureInitialized() must have been called previously.
    kj::ArrayPtr<jsg::JsRef<jsg::JsString>> get() {
      return KJ_REQUIRE_NONNULL(names);
    }

    void ensureInitialized(jsg::Lock& js, SqliteDatabase::Query& source);

    JSG_MEMORY_INFO(CachedColumnNames) {
      KJ_IF_SOME(list, names) {
        for (const auto& name: list) {
          tracker.trackField(nullptr, name);
        }
      }
    }

  private:
    kj::Maybe<kj::Array<jsg::JsRef<jsg::JsString>>> names;
  };

  // A statement in the statement cache.
  struct CachedStatement: public kj::Refcounted {
    jsg::HashableV8Ref<v8::String> query;
    size_t statementSize;
    SqliteDatabase::Statement statement;
    CachedColumnNames cachedColumnNames;
    kj::ListLink<CachedStatement> lruLink;

    CachedStatement(jsg::Lock& js,
        SqlStorage& sqlStorage,
        SqliteDatabase& db,
        jsg::JsString jsQuery,
        kj::String kjQuery)
        : query(js.v8Isolate, jsQuery),
          statementSize(kjQuery.size()),
          statement(db.prepareMulti(sqlStorage, kj::mv(kjQuery))) {}
  };

  class StatementCacheCallbacks {
  public:
    inline const jsg::HashableV8Ref<v8::String>& keyForRow(
        const kj::Rc<CachedStatement>& entry) const {
      return entry->query;
    }

    inline bool matches(const kj::Rc<CachedStatement>& entry, jsg::JsString key) const {
      return entry->query == key;
    }
    inline bool matches(
        const kj::Rc<CachedStatement>& entry, const jsg::HashableV8Ref<v8::String>& key) const {
      return entry->query == key;
    }

    inline auto hashCode(jsg::JsString key) const {
      return key.hashCode();
    }
    inline auto hashCode(const jsg::HashableV8Ref<v8::String>& key) const {
      return key.hashCode();
    }
  };

  // We can't quite just use kj::HashMap here because we want the table key to be
  // `CachedStatement::query`, which is a member of the refcounted object.
  using StatementMap = kj::Table<kj::Rc<CachedStatement>, kj::HashIndex<StatementCacheCallbacks>>;

  struct StatementCache {
    StatementMap map;
    kj::List<CachedStatement, &CachedStatement::lruLink> lru;
    size_t totalSize = 0;

    ~StatementCache() noexcept(false);
  };
  IoOwn<StatementCache> statementCache;

  template <size_t size, typename... Params>
  SqliteDatabase::Query execMemoized(SqliteDatabase& db,
      kj::Maybe<IoOwn<SqliteDatabase::Statement>>& slot,
      const char (&sqlCode)[size],
      Params&&... params) {
    // Run a (trusted) statement, preparing it on the first call and reusing the prepared version
    // for future calls.

    SqliteDatabase::Statement* stmt;
    KJ_IF_SOME(s, slot) {
      stmt = &*s;
    } else {
      stmt = &*slot.emplace(IoContext::current().addObject(kj::heap(db.prepare(sqlCode))));
    }
    return stmt->run(kj::fwd<Params>(params)...);
  }

  uint64_t getPageSize(SqliteDatabase& db) {
    KJ_IF_SOME(p, pageSize) {
      return p;
    } else {
      return pageSize.emplace(db.run("PRAGMA page_size;").getInt64(0));
    }
  }

  // Utility functions to convert SqlValue, SqlRow, and Array<SqlValue> to JS values. In some
  // cases we end up having to do this conversion before actually returning, so we can't have
  // JSG do it. We can't use jsg::TypeHandler because SqlValue contains StringPtr, which doesn't
  // support unwrapping. We don't actually ever use unwrapping, but requesting a TypeHandler forces
  // JSG to try to generate the code for unwrapping, leading to compiler errors.
  //
  // TODO(cleanup): Think hard about how to make JSG support this better. Part of the problem is
  //   that we're being too clever with optimizations to avoid copying strings when we don't need
  //   to.
  static jsg::JsValue wrapSqlValue(jsg::Lock& js, SqlValue value);
  static jsg::JsObject wrapSqlRow(jsg::Lock& js, SqlRow row);
  static jsg::JsArray wrapSqlRowRaw(jsg::Lock& js, kj::Array<SqlValue> row);
};

class SqlStorage::Cursor final: public jsg::Object {
public:
  template <typename... Params>
  Cursor(Params&&... params)
      : state(IoContext::current().addObject(kj::heap<State>(kj::fwd<Params>(params)...))),
        ownCachedColumnNames(kj::none),  // silence bogus Clang warning on next line
        cachedColumnNames(ownCachedColumnNames.emplace()) {}
  ~Cursor() noexcept(false);

  double getRowsRead();
  double getRowsWritten();

  kj::Array<jsg::JsRef<jsg::JsString>> getColumnNames(jsg::Lock& js);
  JSG_RESOURCE_TYPE(Cursor) {
    JSG_METHOD(next);
    JSG_METHOD(toArray);
    JSG_METHOD(one);

    JSG_ITERABLE(rows);
    JSG_METHOD(raw);
    JSG_READONLY_PROTOTYPE_PROPERTY(columnNames, getColumnNames);
    JSG_READONLY_PROTOTYPE_PROPERTY(rowsRead, getRowsRead);
    JSG_READONLY_PROTOTYPE_PROPERTY(rowsWritten, getRowsWritten);

    JSG_TS_DEFINE(type SqlStorageValue = ArrayBuffer | string | number | null);
    JSG_TS_OVERRIDE(<T extends Record<string, SqlStorageValue>> {
      [Symbol.iterator](): IterableIterator<T>;
      raw<U extends SqlStorageValue[]>(): IterableIterator<U>;
      next(): { done?: false, value: T } | { done: true, value?: never };
      toArray(): T[];
      one(): T;
    });
  }

  JSG_ITERATOR(RowIterator, rows, SqlRow, jsg::Ref<Cursor>, rowIteratorNext);
  JSG_ITERATOR(RawIterator, raw, kj::Array<SqlValue>, jsg::Ref<Cursor>, rawIteratorNext);

  RowIterator::Next next(jsg::Lock& js);
  jsg::JsArray toArray(jsg::Lock& js);
  jsg::JsValue one(jsg::Lock& js);

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    if (state != kj::none) {
      tracker.trackFieldWithSize("IoOwn<State>", sizeof(IoOwn<State>));
    }
    tracker.trackField("cachedColumnNames", cachedColumnNames);
  }

private:
  struct State {
    kj::Maybe<kj::Rc<CachedStatement>> cachedStatement;

    // The bindings that were used to construct `query`. We have to keep these alive until the query
    // is done since it might contain pointers into strings and blobs.
    kj::Array<BindingValue> bindings;

    SqliteDatabase::Query query;

    bool isFirst = true;

    State(SqliteDatabase& db,
        SqliteDatabase::Regulator& regulator,
        kj::StringPtr sqlCode,
        kj::Array<BindingValue> bindings);

    State(kj::Rc<CachedStatement> cachedStatement, kj::Array<BindingValue> bindings);
  };

  // Nulled out when query is done or canceled.
  kj::Maybe<IoOwn<State>> state;

  // True if the cursor was canceled by a new call to the same statement. This is used only to
  // flag an error if the application tries to reuse the cursor.
  bool canceled = false;

  // Reference to a weak reference that might point back to this object. If so, null it out at
  // destruction. Used by Statement to invalidate past cursors when the statement is
  // executed again.
  kj::Maybe<kj::Maybe<Cursor&>&> selfRef;

  // Row IO counts. These are updated as the query runs. We keep these outside the State so they
  // remain available even after the query is done or canceled.
  uint64_t rowsRead = 0;
  // Row IO counts. These are updated as the query runs. We keep these outside the State so they
  // remain available even after the query is done or canceled.
  uint64_t rowsWritten = 0;

  kj::Maybe<CachedColumnNames> ownCachedColumnNames;
  CachedColumnNames& cachedColumnNames;

  static kj::Array<const SqliteDatabase::Query::ValuePtr> mapBindings(
      kj::ArrayPtr<BindingValue> values);

  static kj::Maybe<SqlRow> rowIteratorNext(jsg::Lock& js, jsg::Ref<Cursor>& obj);
  static kj::Maybe<kj::Array<SqlValue>> rawIteratorNext(jsg::Lock& js, jsg::Ref<Cursor>& obj);
  template <typename Func>
  static auto iteratorImpl(jsg::Lock& js, jsg::Ref<Cursor>& obj, Func&& func)
      -> kj::Maybe<
          kj::Array<decltype(func(kj::instance<State&>(), uint(), kj::instance<SqlValue&&>()))>>;

  friend class Statement;
};

// The prepared statement API is supported only for backwards compatibility for certain early
// internal users of SQLite-backed DOs. This API was not released because we chose instead to
// implement automatic prepared statement caching via the simple `exec()` API. Since this is
// a compatibility shim only, to simplify things, it is acutally just a wrapper around `exec()`.
class SqlStorage::Statement final: public jsg::Object {
public:
  Statement(jsg::Lock& js, jsg::Ref<SqlStorage> sqlStorage, jsg::JsString query)
      : sqlStorage(kj::mv(sqlStorage)),
        query(js.v8Isolate, query) {}

  jsg::Ref<Cursor> run(jsg::Lock& js, jsg::Arguments<BindingValue> bindings);

  JSG_RESOURCE_TYPE(Statement) {
    JSG_CALLABLE(run);
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("sqlStorage", sqlStorage);
    tracker.trackField("query", query);
  }

private:
  jsg::Ref<SqlStorage> sqlStorage;
  jsg::V8Ref<v8::String> query;

  friend class Cursor;
};

struct SqlStorage::IngestResult {
  IngestResult(kj::String remainder, double rowsRead, double rowsWritten, double statementCount)
      : remainder(kj::mv(remainder)),
        rowsRead(rowsRead),
        rowsWritten(rowsWritten),
        statementCount(statementCount) {}

  kj::String remainder;
  double rowsRead;
  double rowsWritten;
  double statementCount;

  JSG_STRUCT(remainder, rowsRead, rowsWritten, statementCount);
};

#define EW_SQL_ISOLATE_TYPES                                                                       \
  api::SqlStorage, api::SqlStorage::Statement, api::SqlStorage::Cursor,                            \
      api::SqlStorage::IngestResult, api::SqlStorage::Cursor::RowIterator,                         \
      api::SqlStorage::Cursor::RowIterator::Next, api::SqlStorage::Cursor::RawIterator,            \
      api::SqlStorage::Cursor::RawIterator::Next
// The list of sql.h types that are added to worker.c++'s JSG_DECLARE_ISOLATE_TYPE

}  // namespace workerd::api
