// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "sqlite-kv.h"

#include <sqlite3.h>

namespace workerd {

void SqliteKvRegulator::onError(kj::Maybe<int> sqliteErrorCode, kj::StringPtr message) const {
  KJ_IF_SOME(ec, sqliteErrorCode) {
    switch (ec) {
      case SQLITE_TOOBIG:
        // We want to return SQLITE_TOOBIG to the user since it's usually because of user error.
        JSG_ASSERT(false, Error, message);
        return;
      // We don't want to return other errors to the user since they're usually our fault.
      // In that case we do nothing because the contract of onError is not to handle the error in
      // its entirety, but instead to optionally handle it, and do nothing otherwise.
      // When onError does nothing, the code calling into onError is still responsible for
      // handling the error by other means, usually by throwing a KJ exception itself.
      default:
        return;
    }
    KJ_UNREACHABLE;
  }
}

SqliteKv::SqliteKv(SqliteDatabase& db): ResetListener(db) {
  if (db.run("SELECT name FROM sqlite_master WHERE type='table' AND name='_cf_KV'").isDone()) {
    // The _cf_KV table doesn't exist. Defer initialization.
    state.init<Uninitialized>(Uninitialized{});
  } else {
    // The KV table was initialized in the past. We can go ahead and prepare our statements.
    // (We don't call ensureInitialized() here because the `CREATE TABLE IF NOT EXISTS` query it
    // executes would be redundant.)
    tableCreated = true;
    state.init<Initialized>(db);
  }
}

SqliteKv::Initialized& SqliteKv::ensureInitialized() {
  if (!tableCreated) {
    db.run(R"(
      CREATE TABLE IF NOT EXISTS _cf_KV (
        key TEXT PRIMARY KEY,
        value BLOB
      ) WITHOUT ROWID;
    )");

    tableCreated = true;

    // If we're in a transaction and it gets rolled back, we better mark that the table is actually
    // not created anymore.
    db.onRollback([this]() { tableCreated = false; });
  }

  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(uninitialized, Uninitialized) {
      return state.init<Initialized>(db);
    }
    KJ_CASE_ONEOF(initialized, Initialized) {
      return initialized;
    }
  }
  KJ_UNREACHABLE;
}

void SqliteKv::put(KeyPtr key, ValuePtr value) {
  ensureInitialized().stmtPut.run(key, value);
}

bool SqliteKv::delete_(KeyPtr key) {
  auto query = ensureInitialized().stmtDelete.run(key);
  return query.changeCount() > 0;
}

uint SqliteKv::deleteAll() {
  // TODO(perf): Consider introducing a compatibility flag that causes deleteAll() to always return
  //   1. Apps almost certainly don't care about the return value but historically we returned the
  //   count of keys deleted, so now we're stuck counting the table size for no good reason.
  uint count = tableCreated ? ensureInitialized().stmtCountKeys.run().getInt(0) : 0;
  db.reset();
  return count;
}

void SqliteKv::beforeSqliteReset() {
  // We'll need to recreate the table on the next operation.
  tableCreated = false;
}

}  // namespace workerd
