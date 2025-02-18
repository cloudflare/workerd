// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "sqlite-metadata.h"

#include <kj/debug.h>

namespace workerd {

SqliteMetadata::SqliteMetadata(SqliteDatabase& db): ResetListener(db) {
  auto q = db.run("SELECT name FROM sqlite_master WHERE type='table' AND name='_cf_METADATA'");
  tableCreated = !q.isDone();
}

kj::Maybe<kj::Date> SqliteMetadata::getAlarm() {
  if (cacheState == kj::none) {
    cacheState = Cache{.alarmTime = getAlarmUncached()};
  }
  return KJ_ASSERT_NONNULL(cacheState).alarmTime;
}

void SqliteMetadata::setAlarm(kj::Maybe<kj::Date> currentTime) {
  KJ_IF_SOME(c, cacheState) {
    if (c.alarmTime == currentTime) {
      return;
    }
  }
  setAlarmUncached(currentTime);
  db.onRollback([this, oldCacheState = cacheState]() { cacheState = oldCacheState; });
  cacheState = Cache{.alarmTime = currentTime};
}

kj::Maybe<kj::Date> SqliteMetadata::getAlarmUncached() {
  if (!tableCreated) {
    return kj::none;
  }

  auto query = ensureInitialized().stmtGetAlarm.run();
  if (query.isDone() || query.isNull(0)) {
    return kj::none;
  } else {
    return kj::UNIX_EPOCH + query.getInt64(0) * kj::NANOSECONDS;
  }
}

void SqliteMetadata::setAlarmUncached(kj::Maybe<kj::Date> currentTime) {
  KJ_IF_SOME(t, currentTime) {
    ensureInitialized().stmtSetAlarm.run((t - kj::UNIX_EPOCH) / kj::NANOSECONDS);
  } else {
    // Our getter code also allows representing an empty alarm value as a
    // missing row or table, but a null-value row seems efficient and simple.
    ensureInitialized().stmtSetAlarm.run(nullptr);
  }
}

kj::Maybe<uint64_t> SqliteMetadata::getLocalDevelopmentBookmark() {
  auto query = ensureInitialized().stmtGetLocalDevelopmentBookmark.run();
  if (query.isDone() || query.isNull(0)) {
    return kj::none;
  } else {
    auto bookmark = query.getInt64(0);
    KJ_REQUIRE(bookmark >= 0);
    return bookmark;
  }
}

void SqliteMetadata::setLocalDevelopmentBookmark(uint64_t bookmark) {
  KJ_REQUIRE(bookmark <= static_cast<int64_t>(kj::maxValue));
  ensureInitialized().stmtSetLocalDevelopmentBookmark.run(static_cast<int64_t>(bookmark));
}

SqliteMetadata::Initialized& SqliteMetadata::ensureInitialized() {
  if (!tableCreated) {
    db.run(R"(
      CREATE TABLE IF NOT EXISTS _cf_METADATA (
        key INTEGER PRIMARY KEY,
        value BLOB
      );
    )");
    tableCreated = true;
    db.onRollback([this]() { tableCreated = false; });
  }

  KJ_SWITCH_ONEOF(dbState) {
    KJ_CASE_ONEOF(uninitialized, Uninitialized) {
      return dbState.init<Initialized>(db);
    }
    KJ_CASE_ONEOF(initialized, Initialized) {
      return initialized;
    }
  }
  KJ_UNREACHABLE;
}

void SqliteMetadata::beforeSqliteReset() {
  // We'll need to recreate the table on the next operation.
  tableCreated = false;
  cacheState = kj::none;
}

}  // namespace workerd
