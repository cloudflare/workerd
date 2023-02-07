// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "sqlite-kv.h"

namespace workerd {

SqliteKv::SqliteKv(SqliteDatabase& db, bool): db(db) {}

SqliteDatabase& SqliteKv::ensureInitialized(SqliteDatabase& db) {
  // TODO(sqlite): Do this automatically at a lower layer?
  db.run("PRAGMA journal_mode=WAL;");

  db.run(R"(
    CREATE TABLE IF NOT EXISTS _cf_KV (
      key TEXT PRIMARY KEY,
      value BLOB
    ) WITHOUT ROWID;
  )");

  return db;
}

bool SqliteKv::get(KeyPtr key, kj::FunctionParam<void(ValuePtr)> callback) {
  auto query = stmtGet.run(key);

  if (query.isDone()) {
    return false;
  } else {
    callback(query.getBlob(0));
    return true;
  }
}

uint SqliteKv::list(KeyPtr begin, kj::Maybe<KeyPtr> end, kj::Maybe<uint> limit, Order order,
                    kj::FunctionParam<void(KeyPtr, ValuePtr)> callback) {
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

void SqliteKv::put(KeyPtr key, ValuePtr value) {
  stmtPut.run(key, value);
}

bool SqliteKv::delete_(KeyPtr key) {
  auto query = stmtDelete.run(key);
  return query.changeCount() > 0;
}

void SqliteKv::deleteAll() {
  stmtDeleteAll.run();
}

}  // namespace workerd
