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
