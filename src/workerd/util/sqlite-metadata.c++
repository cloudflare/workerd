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
    cacheState = Cache{};
  }
  auto& cache = KJ_ASSERT_NONNULL(cacheState);
  if (!cache.hasAlarmTime) {
    cache.alarmTime = getAlarmUncached();
    cache.hasAlarmTime = true;
  }
  return cache.alarmTime;
}

bool SqliteMetadata::setAlarm(kj::Maybe<kj::Date> currentTime, bool allowUnconfirmed) {
  KJ_IF_SOME(c, cacheState) {
    if (c.hasAlarmTime && c.alarmTime == currentTime) {
      return false;
    }
  }
  setAlarmUncached(currentTime, allowUnconfirmed);
  db.onRollback([this, oldCacheState = cacheState]() { cacheState = oldCacheState; });
  Cache cache;
  cache.alarmTime = currentTime;
  cache.hasAlarmTime = true;
  cacheState = kj::mv(cache);
  return true;
}

kj::Maybe<kj::Date> SqliteMetadata::getAlarmUncached() {
  if (!tableCreated) {
    return kj::none;
  }

  auto query = ensureInitialized(/*allowUnconfirmed=*/false).stmtGetAlarm.run();
  if (query.isDone() || query.isNull(0)) {
    return kj::none;
  } else {
    return kj::UNIX_EPOCH + query.getInt64(0) * kj::NANOSECONDS;
  }
}

void SqliteMetadata::setAlarmUncached(kj::Maybe<kj::Date> currentTime, bool allowUnconfirmed) {
  KJ_IF_SOME(t, currentTime) {
    ensureInitialized(allowUnconfirmed)
        .stmtSetAlarm.run(
            {.allowUnconfirmed = allowUnconfirmed}, (t - kj::UNIX_EPOCH) / kj::NANOSECONDS);
  } else {
    // Our getter code also allows representing an empty alarm value as a
    // missing row or table, but a null-value row seems efficient and simple.
    ensureInitialized(allowUnconfirmed)
        .stmtSetAlarm.run({.allowUnconfirmed = allowUnconfirmed}, nullptr);
  }
}

kj::Maybe<uint64_t> SqliteMetadata::getLocalDevelopmentBookmark() {
  auto query = ensureInitialized(/*allowUnconfirmed=*/false).stmtGetLocalDevelopmentBookmark.run();
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
  ensureInitialized(/*allowUnconfirmed=*/false)
      .stmtSetLocalDevelopmentBookmark.run(static_cast<int64_t>(bookmark));
}

kj::Maybe<kj::String> SqliteMetadata::getActorName() {
  if (cacheState == kj::none) {
    cacheState = Cache{};
  }
  auto& cache = KJ_ASSERT_NONNULL(cacheState);
  if (!cache.hasActorName) {
    cache.actorName = getActorNameUncached();
    cache.hasActorName = true;
  }
  return cache.actorName.map([](kj::String& name) { return kj::str(name); });
}

bool SqliteMetadata::setActorName(kj::StringPtr name, bool allowUnconfirmed) {
  KJ_IF_SOME(c, cacheState) {
    if (c.hasActorName) {
      KJ_IF_SOME(existingName, c.actorName) {
        if (existingName == name) {
          return false;
        }
      }
    }
  }
  setActorNameUncached(name, allowUnconfirmed);
  db.onRollback([this, oldCacheState = cacheState]() { cacheState = oldCacheState; });
  Cache cache;
  cache.actorName = kj::str(name);
  cache.hasActorName = true;
  cacheState = kj::mv(cache);
  return true;
}

kj::Maybe<kj::String> SqliteMetadata::getActorNameUncached() {
  if (!tableCreated) {
    return kj::none;
  }

  auto query = ensureInitialized(/*allowUnconfirmed=*/false).stmtGetActorName.run();
  if (query.isDone() || query.isNull(0)) {
    return kj::none;
  } else {
    return kj::str(query.getText(0));
  }
}

void SqliteMetadata::setActorNameUncached(kj::StringPtr name, bool allowUnconfirmed) {
  ensureInitialized(allowUnconfirmed).stmtSetActorName.run(
      {.allowUnconfirmed = allowUnconfirmed}, name);
}

SqliteMetadata::Initialized& SqliteMetadata::ensureInitialized(bool allowUnconfirmed) {
  if (!tableCreated) {
    db.run(SqliteDatabase::QueryOptions{.regulator = SqliteDatabase::TRUSTED,
             .allowUnconfirmed = allowUnconfirmed},
        R"(
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
