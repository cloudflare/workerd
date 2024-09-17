// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "sqlite-metadata.h"
#include <kj/debug.h>

namespace workerd {

SqliteMetadata::SqliteMetadata(SqliteDatabase& db) {
  auto q = db.run("SELECT name FROM sqlite_master WHERE type='table' AND name='_cf_METADATA'");
  if (q.isDone()) {
    // The _cf_METADATA table doesn't exist. Defer initialization.
    dbState = Uninitialized{db};
  } else {
    // The metadata table was initialized in the past. We can go ahead and prepare our statements.
    // (We don't call ensureInitialized() here because the `CREATE TABLE IF NOT EXISTS` query it
    // executes would be redundant.)
    dbState = Initialized(db);
  }
}

kj::Maybe<kj::Date> SqliteMetadata::getAlarm() {
  if (cacheState == kj::none) {
    cacheState = Cache{.alarmTime = getAlarmUncached()};
  }
  return KJ_ASSERT_NONNULL(cacheState).alarmTime;
}

void SqliteMetadata::setAlarm(kj::Maybe<kj::Date> currentTime) {
  // Presumably fine to omit redundant writes
  KJ_IF_SOME(c, cacheState) {
    if (c.alarmTime == currentTime) {
      return;
    }
  }
  setAlarmUncached(currentTime);
  cacheState = Cache{.alarmTime = currentTime};
}

void SqliteMetadata::invalidate() {
  cacheState = kj::none;
}

kj::Maybe<kj::Date> SqliteMetadata::getAlarmUncached() {
  auto& stmts = KJ_UNWRAP_OR(dbState.tryGet<Initialized>(), return kj::none);

  auto query = stmts.stmtGetAlarm.run();
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

SqliteMetadata::Initialized& SqliteMetadata::ensureInitialized() {
  KJ_SWITCH_ONEOF(dbState) {
    KJ_CASE_ONEOF(uninitialized, Uninitialized) {
      auto& db = uninitialized.db;

      db.run(R"(
        CREATE TABLE IF NOT EXISTS _cf_METADATA (
          key INTEGER PRIMARY KEY,
          value BLOB
        );
      )");

      return dbState.init<Initialized>(db);
    }
    KJ_CASE_ONEOF(initialized, Initialized) {
      return initialized;
    }
  }
  KJ_UNREACHABLE;
}

}  // namespace workerd
