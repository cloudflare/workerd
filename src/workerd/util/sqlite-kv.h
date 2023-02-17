// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "sqlite.h"

namespace workerd {

class SqliteKv {
  // Class which implements KV storage on top of SQLite. This is intended to be used for Durable
  // Object storage.
  //
  // The table is named `_cf_KV`. The naming is designed so that if the application is allowed to
  // perform direct SQL queries, we can block it from accessing any table prefixed with `_cf_`.
  // (Ideally this class would allow configuring the table name, but this would require a somewhat
  // obnoxious about of string allocation.)

public:
  explicit SqliteKv(SqliteDatabase& db): SqliteKv(ensureInitialized(db), true) {}

  typedef kj::StringPtr KeyPtr;
  typedef kj::ArrayPtr<const kj::byte> ValuePtr;

  template <typename Func>
  bool get(KeyPtr key, Func&& callback);
  // Search for a match for the given key. Calls the callback function with the result (a ValuePtr)
  // if found. This is intended to avoid the need to copy the bytes, if the caller would just parse
  // them and drop them immediately anyway. Returns true if there was a match, false if not.

  enum Order {
    FORWARD,
    REVERSE
  };

  template <typename Func>
  uint list(KeyPtr begin, kj::Maybe<KeyPtr> end, kj::Maybe<uint> limit, Order order,
            Func&& callback);
  // Search for all knows keys and values in a range, calling the callback (with KeyPtr and
  // ValuePtr parameters) for each one seen. `end` and `limit` can be null to request no constraint
  // be enforced.

  void put(KeyPtr key, ValuePtr value);
  // Store a value into the table.

  bool delete_(KeyPtr key);
  // Delete the key and return whether it was matched.

  void deleteAll();

  // TODO(perf): Should we provide multi-get, multi-put, and multi-delete? It's a bit tricky to
  //   implement them as single SQL queries, while still using prepared statements. The c-array
  //   extension might help here, though it can only support arrays of NUL-terminated strings, not
  //   byte blobs or strings containing NUL bytes.

private:
  SqliteDatabase& db;

  SqliteDatabase::Statement stmtGet = db.prepare(R"(
    SELECT value FROM _cf_KV WHERE key = ?
  )");
  SqliteDatabase::Statement stmtPut = db.prepare(R"(
    INSERT INTO _cf_KV VALUES(?, ?)
      ON CONFLICT DO UPDATE SET value = excluded.value;
  )");
  SqliteDatabase::Statement stmtDelete = db.prepare(R"(
    DELETE FROM _cf_KV WHERE key = ?
  )");
  SqliteDatabase::Statement stmtList = db.prepare(R"(
    SELECT * FROM _cf_KV
    WHERE key >= ?
    ORDER BY key
  )");
  SqliteDatabase::Statement stmtListEnd = db.prepare(R"(
    SELECT * FROM _cf_KV
    WHERE key >= ? AND key < ?
    ORDER BY key
  )");
  SqliteDatabase::Statement stmtListLimit = db.prepare(R"(
    SELECT * FROM _cf_KV
    WHERE key >= ?
    ORDER BY key
    LIMIT ?
  )");
  SqliteDatabase::Statement stmtListEndLimit = db.prepare(R"(
    SELECT * FROM _cf_KV
    WHERE key >= ? AND key < ?
    ORDER BY key
    LIMIT ?
  )");
  SqliteDatabase::Statement stmtListReverse = db.prepare(R"(
    SELECT * FROM _cf_KV
    WHERE key >= ?
    ORDER BY key DESC
  )");
  SqliteDatabase::Statement stmtListEndReverse = db.prepare(R"(
    SELECT * FROM _cf_KV
    WHERE key >= ? AND key < ?
    ORDER BY key DESC
  )");
  SqliteDatabase::Statement stmtListLimitReverse = db.prepare(R"(
    SELECT * FROM _cf_KV
    WHERE key >= ?
    ORDER BY key DESC
    LIMIT ?
  )");
  SqliteDatabase::Statement stmtListEndLimitReverse = db.prepare(R"(
    SELECT * FROM _cf_KV
    WHERE key >= ? AND key < ?
    ORDER BY key DESC
    LIMIT ?
  )");
  SqliteDatabase::Statement stmtDeleteAll = db.prepare(R"(
    DELETE FROM _cf_KV
  )");

  SqliteDatabase& ensureInitialized(SqliteDatabase& db);
  // Make sure the KV table is created, then return the same object.

  SqliteKv(SqliteDatabase& db, bool);
};

// =======================================================================================
// inline implementation details
//
// We define these two methods as templates rather than ues kj::Function since they're not too
// complicated and avoiding the virtual call is nice. Plus in list()'s case, the actual call sites
// pass constants for `order` so the `order ==` branch can be eliminated.

template <typename Func>
bool SqliteKv::get(KeyPtr key, Func&& callback) {
  auto query = stmtGet.run(key);

  if (query.isDone()) {
    return false;
  } else {
    callback(query.getBlob(0));
    return true;
  }
}

template <typename Func>
uint SqliteKv::list(KeyPtr begin, kj::Maybe<KeyPtr> end, kj::Maybe<uint> limit, Order order,
                    Func&& callback) {
  auto iterate = [&](SqliteDatabase::Query&& query) {
    size_t count = 0;
    while (!query.isDone()) {
      callback(query.getText(0), query.getBlob(1));
      query.nextRow();
      ++count;
    }
    return count;
  };

  if (order == Order::FORWARD) {
    KJ_IF_MAYBE(e, end) {
      KJ_IF_MAYBE(l, limit) {
        return iterate(stmtListEndLimit.run(begin, *e, (int64_t)*l));
      } else {
        return iterate(stmtListEnd.run(begin, *e));
      }
    } else {
      KJ_IF_MAYBE(l, limit) {
        return iterate(stmtListLimit.run(begin, (int64_t)*l));
      } else {
        return iterate(stmtList.run(begin));
      }
    }
  } else {
    KJ_IF_MAYBE(e, end) {
      KJ_IF_MAYBE(l, limit) {
        return iterate(stmtListEndLimitReverse.run(begin, *e, (int64_t)*l));
      } else {
        return iterate(stmtListEndReverse.run(begin, *e));
      }
    } else {
      KJ_IF_MAYBE(l, limit) {
        return iterate(stmtListLimitReverse.run(begin, (int64_t)*l));
      } else {
        return iterate(stmtListReverse.run(begin));
      }
    }
  }
}

}  // namespace workerd
