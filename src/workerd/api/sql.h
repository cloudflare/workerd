// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/util/sqlite.h>
#include <workerd/io/compatibility-date.capnp.h>
#include <workerd/io/io-context.h>
#include <workerd/api/actor-state.h>

namespace workerd::api {

class SqlStorage final: public jsg::Object, private SqliteDatabase::Regulator {
public:
  SqlStorage(jsg::Ref<DurableObjectStorage> storage);
  ~SqlStorage();

  using NonNullBindingValue = kj::OneOf<kj::Array<const byte>, kj::String, double>;
  using BindingValue = kj::Maybe<NonNullBindingValue>;

  class Cursor;
  class Statement;
  class BoundStatement;
  struct IngestResult;

  // One value returned from SQL. Note that we intentionally return StringPtr instead of String
  // because we know that the underlying buffer returned by SQLite will be valid long enough to be
  // converted by JSG into a V8 string. For byte arrays, on the other hand, we pass ownership to
  // JSG, which does not need to make a copy.
  using SqlValue = kj::Maybe<kj::OneOf<kj::Array<byte>, kj::StringPtr, double>>;

  // One row of a SQL query result. This is an Object whose properties correspond to columns.
  using SqlRow = jsg::Dict<SqlValue, jsg::JsString>;

  jsg::Ref<Cursor> exec(jsg::Lock& js, kj::String query, jsg::Arguments<BindingValue> bindings);
  IngestResult ingest(jsg::Lock& js, kj::String query);

  jsg::Ref<Statement> prepare(jsg::Lock& js, kj::String query);

  // Backwards-compatibility API for D1. There's no reason to use this in DO.
  //
  // Return value is an array of arrays of SqlRows which have already been converted to JS values.
  // This conversion has to be done as we go because the StringPtrs in each row are invalidated
  // as soon as we proceed to the next row.
  jsg::JsArray batch(jsg::Lock& js,
      kj::Array<kj::OneOf<jsg::Ref<Statement>, jsg::Ref<BoundStatement>>> statements);

  double getDatabaseSize(jsg::Lock& js);

  JSG_RESOURCE_TYPE(SqlStorage, CompatibilityFlags::Reader flags) {
    JSG_METHOD(exec);
    JSG_METHOD(prepare);
    JSG_METHOD(batch);

    // Make sure that the 'ingest' function is still experimental-only if and when
    // the SQL API becomes publicly available.
    if (flags.getWorkerdExperimental()) {
      JSG_METHOD(ingest);
    }

    JSG_READONLY_PROTOTYPE_PROPERTY(databaseSize, getDatabaseSize);

    JSG_NESTED_TYPE(Cursor);
    JSG_NESTED_TYPE(Statement);
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const;

private:
  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(storage);
  }

  bool isAllowedName(kj::StringPtr name) const override;
  bool isAllowedTrigger(kj::StringPtr name) const override;
  void onError(kj::StringPtr message) const override;
  bool allowTransactions() const override;

  SqliteDatabase& getDb(jsg::Lock& js) {
    return storage->getSqliteDb(js);
  }

  jsg::Ref<DurableObjectStorage> storage;

  kj::Maybe<uint> pageSize;
  kj::Maybe<IoOwn<SqliteDatabase::Statement>> pragmaPageCount;
  kj::Maybe<IoOwn<SqliteDatabase::Statement>> pragmaGetMaxPageCount;
  kj::Maybe<IoOwn<SqliteDatabase::Statement>> beginBatch;
  kj::Maybe<IoOwn<SqliteDatabase::Statement>> commitBatch;
  kj::Maybe<IoOwn<SqliteDatabase::Statement>> rollbackBatch;

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
  class CachedColumnNames;

public:
  template <typename... Params>
  Cursor(Params&&... params)
      : state(IoContext::current().addObject(kj::heap<State>(kj::fwd<Params>(params)...))),
        ownCachedColumnNames(kj::none),  // silence bogus Clang warning on next line
        cachedColumnNames(ownCachedColumnNames.emplace()) {}

  template <typename... Params>
  Cursor(CachedColumnNames& cachedColumnNames, Params&&... params)
      : state(IoContext::current().addObject(kj::heap<State>(kj::fwd<Params>(params)...))),
        cachedColumnNames(cachedColumnNames) {}
  ~Cursor() noexcept(false);

  double getRowsRead();
  double getRowsWritten();

  // Get all results as an array and consume the cursor.
  //
  // Returns an array of SqlRows. However, each row must be converted into JS as we go because
  // it could contain StringPtrs that are invalidated as soon as we proceed to the next row.
  jsg::JsArray getResults(jsg::Lock& js);

  // `meta` property, for compatibility with D1 API.
  class Meta final: public jsg::Object {
  public:
    Meta(jsg::Ref<Cursor> cursor): cursor(kj::mv(cursor)) {}

    // We can't actually provide duration for Spectre reasons, so we always return zero.
    double getDuration() { return 0; }

    double getRowsRead() { return cursor->getRowsRead(); }
    double getRowsWritten() { return cursor->getRowsWritten(); }

    JSG_RESOURCE_TYPE(Meta) {
      // Instance properties so that if you log the whole thing you actually see the values.
      JSG_READONLY_INSTANCE_PROPERTY(duration, getDuration);
      JSG_READONLY_INSTANCE_PROPERTY(rowsRead, getRowsRead);
      JSG_READONLY_INSTANCE_PROPERTY(rowsWritten, getRowsWritten);

      // Unfortunately, D1 named these properties with underscores, so fake it out.
      JSG_READONLY_PROTOTYPE_PROPERTY(rows_read, getRowsRead);
      JSG_READONLY_PROTOTYPE_PROPERTY(rows_written, getRowsWritten);

      JSG_TS_OVERRIDE({
        rows_read: never;
        rows_written: never;
      });
    }

    void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
      tracker.trackField("cursor", cursor);
    }

  private:
    jsg::Ref<Cursor> cursor;

    void visitForGc(jsg::GcVisitor& visitor) {
      visitor.visit(cursor);
    }
  };

  jsg::Ref<Meta> getMeta();

  // Returns array of strings. We don't return KJ types because for performance we already have
  // the column names in V8 strings so we just need this to build a V8 array from them.
  jsg::JsArray getColumnNames(jsg::Lock& js);

  JSG_RESOURCE_TYPE(Cursor) {
    JSG_ITERABLE(rows);
    JSG_METHOD(raw);
    JSG_READONLY_PROTOTYPE_PROPERTY(columnNames, getColumnNames);
    JSG_READONLY_PROTOTYPE_PROPERTY(rowsRead, getRowsRead);
    JSG_READONLY_PROTOTYPE_PROPERTY(rowsWritten, getRowsWritten);

    // Reading this property consumes the cursor. We make it a lazy instance property, rather than
    // a regular getter property, so that if you read it twice you don't get an empty array the
    // second time.
    JSG_LAZY_INSTANCE_PROPERTY(results, getResults);

    JSG_READONLY_PROTOTYPE_PROPERTY(meta, getMeta);

    JSG_TS_DEFINE(type SqlStorageResultValue = ArrayBuffer | string | number | null);
    JSG_TS_OVERRIDE(<T = Record<string, SqlStorageResultValue>> {
      get columnNames(): string[];
      get results(): T[];
      [Symbol.iterator](): IterableIterator<T>;
      raw(): IterableIterator<SqlStorageResultValue[]>;
    });
  }

  JSG_ITERATOR(RowIterator, rows, SqlRow, jsg::Ref<Cursor>, rowIteratorNext);
  JSG_ITERATOR(RawIterator, raw, kj::Array<SqlValue>, jsg::Ref<Cursor>, rawIteratorNext);

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    if (state != kj::none) {
      tracker.trackFieldWithSize("IoOwn<State>", sizeof(IoOwn<State>));
    }
    tracker.trackField("statement", statement);
    tracker.trackField("cachedColumnNames", ownCachedColumnNames);
  }

private:
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

    JSG_MEMORY_INFO(cachedColumnNames) {
      KJ_IF_SOME(list, names) {
        for (const auto& name: list) {
          tracker.trackField(nullptr, name);
        }
      }
    }

  private:
    kj::Maybe<kj::Array<jsg::JsRef<jsg::JsString>>> names;
  };

  struct State {
    // Refcount on the SqliteDatabase::Statement underlying the query, if any.
    kj::Own<void> dependency;

    // The bindings that were used to construct `query`. We have to keep these alive until the query
    // is done since it might contain pointers into strings and blobs.
    kj::Array<BindingValue> bindings;

    SqliteDatabase::Query query;

    bool isFirst = true;

    State(kj::RefcountedWrapper<SqliteDatabase::Statement>& statement,
        kj::Array<BindingValue> bindings);
    State(SqliteDatabase& db,
        SqliteDatabase::Regulator& regulator,
        kj::StringPtr sqlCode,
        kj::Array<BindingValue> bindings);
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

  // If this cursor was created from a prepared statement, this keeps the statement object alive.
  kj::Maybe<jsg::Ref<Statement>> statement;

  // Row IO counts. These are updated as the query runs. We keep these outside the State so they
  // remain available even after the query is done or canceled.
  uint64_t rowsRead = 0;
  // Row IO counts. These are updated as the query runs. We keep these outside the State so they
  // remain available even after the query is done or canceled.
  uint64_t rowsWritten = 0;

  kj::Maybe<CachedColumnNames> ownCachedColumnNames;
  CachedColumnNames& cachedColumnNames;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(statement);
  }

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

class SqlStorage::Statement final: public jsg::Object {
public:
  Statement(SqliteDatabase::Statement&& statement);

  jsg::Ref<Cursor> run(jsg::Arguments<BindingValue> bindings);
  jsg::Ref<BoundStatement> bind(jsg::Arguments<BindingValue> bindings);

  struct RawOptions {
    bool columnNames = false;

    JSG_STRUCT(columnNames);
    JSG_STRUCT_TS_OVERRIDE(type RawOptions = never);
  };

  // D1 compatibility methods: raw() and first(). These get a little hacky because they both take
  // optional additional options, which must appear after the argument bindings, but the number of
  // bindings depends on the query, so we can't have JSG unpack this for us.

  // Returns an array of rows, where each row is represented as an array of values, not SqlRow.
  // Like Cursor::getResults(), raw() must translate each result row to JS as it iterates.
  jsg::JsArray raw(
      const v8::FunctionCallbackInfo<v8::Value>& args,
      const jsg::TypeHandler<BindingValue>& bindingTypeHandler,
      const jsg::TypeHandler<RawOptions>& optionsTypeHandler);

  // Returns either one SqlRow or one SqlValue depending on options.
  // This returns JsValue mainly because `kj::Maybe<kj::OneOf<SqlRow, SqlValue>>` expands to
  // too many nested layers of Maybe and OneOf for JSG to handle and it ends up barfing up 3MB
  // worth of compiler errors.
  jsg::JsValue first(
      const v8::FunctionCallbackInfo<v8::Value>& args,
      const jsg::TypeHandler<BindingValue>& bindingTypeHandler);

  JSG_RESOURCE_TYPE(Statement) {
    JSG_CALLABLE(run);
    JSG_METHOD(run);
    JSG_METHOD_NAMED(all, run);

    // D1 compatibility APIs.
    JSG_METHOD(bind);
    JSG_METHOD(raw);
    JSG_METHOD(first);

    JSG_TS_OVERRIDE({
      bind(...values: unknown[]): SqlStorageBoundStatement;
      run<T = Record<string, unknown>>(...values: unknown[]): SqlStorageCursor<T>;
      all<T = Record<string, unknown>>(...values: unknown[]): SqlStorageCursor<T>;
      raw<T = unknown[]>(options: {columnNames: true}): [string[], ...T[]];
      raw<T = unknown[]>(options?: {columnNames?: false}): T[];
      first<T = unknown>(colName: string): T | null;
      first<T = Record<string, unknown>>(): T | null;
    });
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackFieldWithSize("IoOwn<kj::RefcountedWrapper<SqliteDatabase::Statement>>",
        sizeof(IoOwn<kj::RefcountedWrapper<SqliteDatabase::Statement>>));
    tracker.trackField("cachedColumnNames", cachedColumnNames);
  }

private:
  IoOwn<kj::RefcountedWrapper<SqliteDatabase::Statement>> statement;

  // Weak reference to the Cursor that is currently using this statement.
  kj::Maybe<Cursor&> currentCursor;

  // All queries from the same prepared statement have the same column names, so we can cache them
  // on the statement.
  Cursor::CachedColumnNames cachedColumnNames;

  jsg::JsArray rawImpl(jsg::Lock& js, jsg::Arguments<BindingValue> bindings,
      const Statement::RawOptions& options);
  jsg::JsValue firstImpl(
      jsg::Lock& js, jsg::Arguments<BindingValue> bindings, jsg::Optional<jsg::JsString> column);

  friend class Cursor;
  friend class BoundStatement;
};

// Compatibility shim to emulate the D1 API, which uses `.bind(...).run()` instead of `.run(...)`.
class SqlStorage::BoundStatement final: public jsg::Object {
public:
  BoundStatement(jsg::Ref<Statement> statement, jsg::Arguments<BindingValue> bindings)
      : statement(kj::mv(statement)), bindings(kj::mv(bindings)) {}

  jsg::Ref<Cursor> run();

  // Like the methods of `Statement` but this time we don't have to deal with bindings.
  jsg::JsArray raw(jsg::Lock& js, jsg::Optional<Statement::RawOptions> options);
  jsg::JsValue first(jsg::Lock& js, jsg::Optional<jsg::JsString> column);

  JSG_RESOURCE_TYPE(BoundStatement) {
    JSG_METHOD(run);
    JSG_METHOD_NAMED(all, run);
    JSG_METHOD(raw);
    JSG_METHOD(first);

    JSG_TS_OVERRIDE({
      run<T = Record<string, unknown>>(): SqlStorageCursor<T>;
      all<T = Record<string, unknown>>(): SqlStorageCursor<T>;
      raw<T = unknown[]>(options: {columnNames: true}): [string[], ...T[]];
      raw<T = unknown[]>(options?: {columnNames?: false}): T[];
      first<T = unknown>(colName: string): T | null;
      first<T = Record<string, unknown>>(): T | null;
    });
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("statement", statement);
  }

private:
  jsg::Ref<Statement> statement;
  jsg::Arguments<BindingValue> bindings;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(statement);
  }

  jsg::Arguments<BindingValue> cloneBindings();

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
      api::SqlStorage::Cursor::RawIterator::Next, api::SqlStorage::BoundStatement,                 \
      api::SqlStorage::Cursor::Meta, api::SqlStorage::Statement::RawOptions
// The list of sql.h types that are added to worker.c++'s JSG_DECLARE_ISOLATE_TYPE

}  // namespace workerd::api
