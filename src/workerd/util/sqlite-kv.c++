// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "sqlite-kv.h"

#include <workerd/jsg/exception.h>

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

SqliteKv::~SqliteKv() noexcept(false) {
  cancelCurrentCursor();
}

void SqliteKv::cancelCurrentCursor() {
  KJ_IF_SOME(c, currentCursor) {
    c.state = kj::none;
    c.canceled = true;
  }
}

SqliteKv::Initialized& SqliteKv::ensureInitialized(bool allowUnconfirmed) {
  if (!tableCreated) {
    db.run(SqliteDatabase::QueryOptions{.regulator = SqliteDatabase::TRUSTED,
             .allowUnconfirmed = allowUnconfirmed},
        R"(
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

kj::Own<SqliteKv::ListCursor> SqliteKv::list(
    KeyPtr begin, kj::Maybe<KeyPtr> end, kj::Maybe<uint> limit, Order order) {
  if (!tableCreated) return kj::heap<ListCursor>(nullptr);
  auto& stmts = KJ_UNWRAP_OR(state.tryGet<Initialized>(), return kj::heap<ListCursor>(nullptr));

  if (order == Order::FORWARD) {
    KJ_IF_SOME(e, end) {
      KJ_IF_SOME(l, limit) {
        return kj::heap<ListCursor>(
            kj::Badge<SqliteKv>(), *this, stmts.stmtListEndLimit, begin, e, (int64_t)l);
      } else {
        return kj::heap<ListCursor>(kj::Badge<SqliteKv>(), *this, stmts.stmtListEnd, begin, e);
      }
    } else {
      KJ_IF_SOME(l, limit) {
        return kj::heap<ListCursor>(
            kj::Badge<SqliteKv>(), *this, stmts.stmtListLimit, begin, (int64_t)l);
      } else {
        return kj::heap<ListCursor>(kj::Badge<SqliteKv>(), *this, stmts.stmtList, begin);
      }
    }
  } else {
    KJ_IF_SOME(e, end) {
      KJ_IF_SOME(l, limit) {
        return kj::heap<ListCursor>(
            kj::Badge<SqliteKv>(), *this, stmts.stmtListEndLimitReverse, begin, e, (int64_t)l);
      } else {
        return kj::heap<ListCursor>(
            kj::Badge<SqliteKv>(), *this, stmts.stmtListEndReverse, begin, e);
      }
    } else {
      KJ_IF_SOME(l, limit) {
        return kj::heap<ListCursor>(
            kj::Badge<SqliteKv>(), *this, stmts.stmtListLimitReverse, begin, (int64_t)l);
      } else {
        return kj::heap<ListCursor>(kj::Badge<SqliteKv>(), *this, stmts.stmtListReverse, begin);
      }
    }
  }
}

kj::Maybe<SqliteKv::ListCursor::KeyValuePair> SqliteKv::ListCursor::next() {
  auto& state = KJ_UNWRAP_OR(this->state, return kj::none);
  if (first) {
    first = false;
  } else {
    state.query.nextRow();
  }
  if (state.query.isDone()) {
    this->state = kj::none;
    return kj::none;
  }

  return KeyValuePair{state.query.getText(0), state.query.getBlob(1)};
}

void SqliteKv::put(KeyPtr key, ValuePtr value) {
  return put(key, value, {});
}

void SqliteKv::put(KeyPtr key, ValuePtr value, WriteOptions options) {
  ensureInitialized(options.allowUnconfirmed)
      .stmtPut.run({.allowUnconfirmed = options.allowUnconfirmed}, key, value);
}

bool SqliteKv::delete_(KeyPtr key) {
  return delete_(key, {});
}

bool SqliteKv::delete_(KeyPtr key, WriteOptions options) {
  auto query = ensureInitialized(options.allowUnconfirmed)
                   .stmtDelete.run({.allowUnconfirmed = options.allowUnconfirmed}, key);
  return query.changeCount() > 0;
}

uint SqliteKv::deleteAll() {
  // TODO(perf): Consider introducing a compatibility flag that causes deleteAll() to always return
  //   1. Apps almost certainly don't care about the return value but historically we returned the
  //   count of keys deleted, so now we're stuck counting the table size for no good reason.
  uint count = tableCreated ? ensureInitialized(false).stmtCountKeys.run().getInt(0) : 0;
  db.reset();
  return count;
}

void SqliteKv::beforeSqliteReset() {
  // We'll need to recreate the table on the next operation.
  tableCreated = false;
}

void SqliteKv::rollbackMultiPut(Initialized& stmts, WriteOptions options) {
  KJ_IF_SOME(e, kj::runCatchingExceptions([&]() {
    // This should be rare, so we don't prepare a statement for it.
    stmts.db.run({.regulator = stmts.regulator, .allowUnconfirmed = options.allowUnconfirmed},
        kj::str("ROLLBACK TO _cf_put_multiple_savepoint"));
    stmts.stmtMultiPutRelease.run({.allowUnconfirmed = options.allowUnconfirmed});
  })) {
    KJ_LOG(WARNING, "silencing exception encountered while rolling back multi-put", e);
  }
}

}  // namespace workerd
