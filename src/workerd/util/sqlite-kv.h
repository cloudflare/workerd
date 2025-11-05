// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "sqlite.h"

#include <kj/debug.h>
#include <kj/exception.h>

namespace workerd {

// Small class which is used to customize certain aspects of the underlying sql operations
// In this case we just customize the error reporting to emit JSG user visible errors instead
// of KJ exceptions which become internal errors.
class SqliteKvRegulator: public SqliteDatabase::Regulator {
  void onError(kj::Maybe<int> sqliteErrorCode, kj::StringPtr message) const override;

  // We bill for KV operations as rows read/written.
  virtual bool shouldAddQueryStats() const override {
    return true;
  }
};

// Class which implements KV storage on top of SQLite. This is intended to be used for Durable
// Object storage.
//
// The table is named `_cf_KV`. The naming is designed so that if the application is allowed to
// perform direct SQL queries, we can block it from accessing any table prefixed with `_cf_`.
// (Ideally this class would allow configuring the table name, but this would require a somewhat
// obnoxious amount of string allocation.)
class SqliteKv: private SqliteDatabase::ResetListener {
 public:
  explicit SqliteKv(SqliteDatabase& db);
  ~SqliteKv() noexcept(false);

  using KeyPtr = kj::StringPtr;
  using ValuePtr = kj::ArrayPtr<const kj::byte>;

  // Search for a match for the given key. Calls the callback function with the result (a ValuePtr)
  // if found. This is intended to avoid the need to copy the bytes, if the caller would just parse
  // them and drop them immediately anyway. Returns true if there was a match, false if not.
  template <typename Func>
  bool get(KeyPtr key, Func&& callback);

  enum Order { FORWARD, REVERSE };

  // Search for all known keys and values in a range, calling the callback (with KeyPtr and
  // ValuePtr parameters) for each one seen. `end` and `limit` can be null to request no constraint
  // be enforced.
  template <typename Func>
  uint list(
      KeyPtr begin, kj::Maybe<KeyPtr> end, kj::Maybe<uint> limit, Order order, Func&& callback);

  // List returning a cursor which can be iterated one at a time.
  class ListCursor;
  kj::Own<ListCursor> list(KeyPtr begin, kj::Maybe<KeyPtr> end, kj::Maybe<uint> limit, Order order);

  struct WriteOptions {
    bool allowUnconfirmed = false;
  };

  // Store a value into the table.
  void put(KeyPtr key, ValuePtr value);
  void put(KeyPtr key, ValuePtr value, WriteOptions options);

  // Atomically store multiple values into the table.
  //
  // ArrayOfKeyValuePair should be a type that allows iteration of a struct that has two members,
  // key and value, that can be coerced into KeyPtr and ValuePtr, respectively.  I'm using a
  // template so that we don't have to transform (by copy) the values passed in from higher levels
  // while also preventing this module from taking a dependency on types from higher levels.
  template <typename ArrayOfKeyValuePair>
  void put(ArrayOfKeyValuePair& pairs, WriteOptions options);

  // Delete the key and return whether it was matched.
  bool delete_(KeyPtr key);
  bool delete_(KeyPtr key, WriteOptions options);

  uint deleteAll();

  // TODO(perf): Should we provide multi-get, multi-put, and multi-delete? It's a bit tricky to
  //   implement them as single SQL queries, while still using prepared statements. The c-array
  //   extension might help here, though it can only support arrays of NUL-terminated strings, not
  //   byte blobs or strings containing NUL bytes.

 private:
  struct Uninitialized {};

  struct Initialized {
    // This reference is redundant but storing it here makes the prepared statement code below
    // easier to manage.
    SqliteDatabase& db;

    SqliteKvRegulator regulator;

    SqliteDatabase::Statement stmtGet = db.prepare(regulator, R"(
      SELECT value FROM _cf_KV WHERE key = ?
    )");
    SqliteDatabase::Statement stmtPut = db.prepare(regulator, R"(
      INSERT INTO _cf_KV VALUES(?, ?)
        ON CONFLICT DO UPDATE SET value = excluded.value;
    )");
    SqliteDatabase::Statement stmtDelete = db.prepare(regulator, R"(
      DELETE FROM _cf_KV WHERE key = ?
    )");
    SqliteDatabase::Statement stmtList = db.prepare(regulator, R"(
      SELECT * FROM _cf_KV
      WHERE key >= ?
      ORDER BY key
    )");
    SqliteDatabase::Statement stmtListEnd = db.prepare(regulator, R"(
      SELECT * FROM _cf_KV
      WHERE key >= ? AND key < ?
      ORDER BY key
    )");
    SqliteDatabase::Statement stmtListLimit = db.prepare(regulator, R"(
      SELECT * FROM _cf_KV
      WHERE key >= ?
      ORDER BY key
      LIMIT ?
    )");
    SqliteDatabase::Statement stmtListEndLimit = db.prepare(regulator, R"(
      SELECT * FROM _cf_KV
      WHERE key >= ? AND key < ?
      ORDER BY key
      LIMIT ?
    )");
    SqliteDatabase::Statement stmtListReverse = db.prepare(regulator, R"(
      SELECT * FROM _cf_KV
      WHERE key >= ?
      ORDER BY key DESC
    )");
    SqliteDatabase::Statement stmtListEndReverse = db.prepare(regulator, R"(
      SELECT * FROM _cf_KV
      WHERE key >= ? AND key < ?
      ORDER BY key DESC
    )");
    SqliteDatabase::Statement stmtListLimitReverse = db.prepare(regulator, R"(
      SELECT * FROM _cf_KV
      WHERE key >= ?
      ORDER BY key DESC
      LIMIT ?
    )");
    SqliteDatabase::Statement stmtListEndLimitReverse = db.prepare(regulator, R"(
      SELECT * FROM _cf_KV
      WHERE key >= ? AND key < ?
      ORDER BY key DESC
      LIMIT ?
    )");
    SqliteDatabase::Statement stmtCountKeys = db.prepare(regulator, R"(
      SELECT count(*) FROM _cf_KV
    )");
    SqliteDatabase::Statement stmtMultiPutSavepoint = db.prepare(regulator, R"(
      SAVEPOINT _cf_put_multiple_savepoint
    )");
    SqliteDatabase::Statement stmtMultiPutRelease = db.prepare(regulator, R"(
      RELEASE _cf_put_multiple_savepoint
    )");

    Initialized(SqliteDatabase& db): db(db) {}
  };

  kj::OneOf<Uninitialized, Initialized> state;

  // Has the _cf_KV table been created? This is separate from Uninitialized/Initialized since it
  // has to be repeated after a reset, whereas the statements do not need to be recreated.
  bool tableCreated = false;

  kj::Maybe<ListCursor&> currentCursor;

  void cancelCurrentCursor();

  Initialized& ensureInitialized(bool allowUnconfirmed);
  // Make sure the KV table is created and prepared statements are ready. Not called until the
  // first write.

  void beforeSqliteReset() override;

  // Helper function that rolls back a multi-put statement and swallows any exceptions that may
  // occur during the rollback.
  void rollbackMultiPut(Initialized& stmts, WriteOptions options);
};

// Iterator over list results.
class SqliteKv::ListCursor {
 public:
  template <typename... Params>
  ListCursor(kj::Badge<SqliteKv>, SqliteKv& parent, Params&&... params) {
    parent.cancelCurrentCursor();
    state.emplace(parent, kj::fwd<Params>(params)...);
    parent.currentCursor = *this;
  }
  ListCursor(decltype(nullptr)) {}

  template <typename Func>
  uint forEach(Func&& callback) {
    auto& query = KJ_UNWRAP_OR(state, return 0).query;
    size_t count = 0;
    while (!query.isDone()) {
      callback(query.getText(0), query.getBlob(1));
      query.nextRow();
      ++count;
    }
    return count;
  };

  struct KeyValuePair {
    kj::StringPtr key;
    kj::ArrayPtr<const byte> value;
  };
  kj::Maybe<KeyValuePair> next();

  // If true, the cursor was canceled due to a new list() operation starting. Only one list() is
  // allowed at a time.
  bool wasCanceled() {
    return canceled;
  }

 private:
  struct State {
    SqliteKv& parent;
    SqliteDatabase::Query query;

    template <typename... Params>
    State(SqliteKv& parent, SqliteDatabase::Statement& stmt, Params&&... params)
        : parent(parent),
          query(stmt.run(kj::fwd<Params>(params)...)) {}
    ~State() noexcept(false) {
      parent.currentCursor = kj::none;
    }
  };

  kj::Maybe<State> state;

  // Are we at the beginning of the list?
  bool first = true;

  bool canceled = false;

  friend class SqliteKv;
};

// =======================================================================================
// inline implementation details
//
// We define these two methods as templates rather than use kj::Function since they're not too
// complicated and avoiding the virtual call is nice. Plus in list()'s case, the actual call sites
// pass constants for `order` so the `order ==` branch can be eliminated.

template <typename Func>
bool SqliteKv::get(KeyPtr key, Func&& callback) {
  if (!tableCreated) return false;
  auto& stmts = KJ_UNWRAP_OR(state.tryGet<Initialized>(), return false);

  auto query = stmts.stmtGet.run(key);

  if (query.isDone()) {
    return false;
  } else {
    callback(query.getBlob(0));
    return true;
  }
}

template <typename Func>
uint SqliteKv::list(
    KeyPtr begin, kj::Maybe<KeyPtr> end, kj::Maybe<uint> limit, Order order, Func&& callback) {
  return list(begin, end, limit, order)->forEach(kj::fwd<Func>(callback));
}

template <typename ArrayOfKeyValuePair>
void SqliteKv::put(ArrayOfKeyValuePair& pairs, WriteOptions options) {
  // TODO(cleanup): This code is very similar to DurableObjectStorage::transactionSync.  Perhaps the
  // general structure can be shared somehow?
  auto& stmts = ensureInitialized(options.allowUnconfirmed);
  stmts.stmtMultiPutSavepoint.run({.allowUnconfirmed = options.allowUnconfirmed});

  {
    // If any of the puts throw an exception, rollback the transaction and re-throw the exception
    // from the put that failed.
    KJ_ON_SCOPE_FAILURE(rollbackMultiPut(stmts, options));
    for (const auto& pair: pairs) {
      put(pair.key, pair.value, {.allowUnconfirmed = options.allowUnconfirmed});
    }
  }
  stmts.stmtMultiPutRelease.run({.allowUnconfirmed = options.allowUnconfirmed});
}

}  // namespace workerd
