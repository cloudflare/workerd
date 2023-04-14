// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "actor-sqlite.h"
#include <algorithm>
#include <workerd/jsg/jsg.h>

namespace workerd {

kj::OneOf<kj::Maybe<ActorCacheOps::Value>,
          kj::Promise<kj::Maybe<ActorCacheOps::Value>>>
    ActorSqlite::get(Key key, ReadOptions options) {
  kj::Maybe<ActorCacheOps::Value> result;
  kv.get(key, [&](ValuePtr value) {
    result = kj::heapArray(value);
  });
  return result;
}

kj::OneOf<ActorCacheOps::GetResultList, kj::Promise<ActorCacheOps::GetResultList>>
    ActorSqlite::get(kj::Array<Key> keys, ReadOptions options) {
  kj::Vector<KeyValuePair> results;
  for (auto& key: keys) {
    kv.get(key, [&](ValuePtr value) {
      results.add(KeyValuePair { kj::mv(key), kj::heapArray(value) });
    });
  }
  std::sort(results.begin(), results.end(),
      [](auto& a, auto& b) { return a.key < b.key; });
  return GetResultList(kj::mv(results));
}

kj::OneOf<kj::Maybe<kj::Date>, kj::Promise<kj::Maybe<kj::Date>>> ActorSqlite::getAlarm(
    ReadOptions options) {
  // TODO(sqlite): Implement alarms for sqlite storage.
  JSG_FAIL_REQUIRE(Error, "getAlarm() is not yet implemented for SQLite-backed Durable Objects");
}

kj::OneOf<ActorCacheOps::GetResultList, kj::Promise<ActorCacheOps::GetResultList>>
    ActorSqlite::list(Key begin, kj::Maybe<Key> end, kj::Maybe<uint> limit, ReadOptions options) {
  kj::Vector<KeyValuePair> results;
  kv.list(begin, end, limit, SqliteKv::FORWARD, [&](KeyPtr key, ValuePtr value) {
    results.add(KeyValuePair { kj::str(key), kj::heapArray(value) });
  });

  // Already guaranteed sorted.
  return GetResultList(kj::mv(results));
}

kj::OneOf<ActorCacheOps::GetResultList, kj::Promise<ActorCacheOps::GetResultList>>
    ActorSqlite::listReverse(Key begin, kj::Maybe<Key> end, kj::Maybe<uint> limit,
                               ReadOptions options) {
  kj::Vector<KeyValuePair> results;
  kv.list(begin, end, limit, SqliteKv::REVERSE, [&](KeyPtr key, ValuePtr value) {
    results.add(KeyValuePair { kj::str(key), kj::heapArray(value) });
  });

  // Already guaranteed sorted (reversed).
  return GetResultList(kj::mv(results));
}

kj::Maybe<kj::Promise<void>> ActorSqlite::put(Key key, Value value, WriteOptions options) {
  kv.put(key, value);
  return nullptr;
}

kj::Maybe<kj::Promise<void>> ActorSqlite::put(
    kj::Array<KeyValuePair> pairs, WriteOptions options) {
  for (auto& pair: pairs) {
    kv.put(pair.key, pair.value);
  }
  return nullptr;
}

kj::OneOf<bool, kj::Promise<bool>> ActorSqlite::delete_(Key key, WriteOptions options) {
  return kv.delete_(key);
}

kj::OneOf<uint, kj::Promise<uint>> ActorSqlite::delete_(
    kj::Array<Key> keys, WriteOptions options) {
  uint count = 0;
  for (auto& key: keys) {
    count += kv.delete_(key);
  }
  return count;
}

kj::Maybe<kj::Promise<void>> ActorSqlite::setAlarm(
    kj::Maybe<kj::Date> newAlarmTime, WriteOptions options) {
  // TODO(sqlite): Implement alarms for sqlite storage.
  JSG_FAIL_REQUIRE(Error, "getAlarm() is not yet implemented for SQLite-backed Durable Objects");
}

kj::Own<ActorCacheInterface::Transaction> ActorSqlite::startTransaction() {
  // TODO(sqlite): Implement transactions.
  JSG_FAIL_REQUIRE(Error, "transaction() not yet implemented for SQLite-backed storage");
}

ActorCacheInterface::DeleteAllResults ActorSqlite::deleteAll(WriteOptions options) {
  uint count = kv.deleteAll();
  return {
    .backpressure = nullptr,
    .count = count,
  };
}

kj::Maybe<kj::Promise<void>> ActorSqlite::evictStale(kj::Date now) {
  // This implementation never needs to apply backpressure.
  return nullptr;
}

void ActorSqlite::shutdown(kj::Maybe<const kj::Exception&> maybeException) {
  // TODO(sqlite): In theory this should cause all future storage ops to fail and should even
  //   roll back any storage writes that weren't "committed" yet according to the automatic
  //   atomic write batching policy.
}

kj::Maybe<kj::Own<void>> ActorSqlite::armAlarmHandler(kj::Date scheduledTime, bool noCache) {
  JSG_FAIL_REQUIRE(Error, "alarms are not yet implemented for SQLite-backed Durable Objects");
}

void ActorSqlite::cancelDeferredAlarmDeletion() {
  JSG_FAIL_REQUIRE(Error, "alarms are not yet implemented for SQLite-backed Durable Objects");
}

kj::Maybe<kj::Promise<void>> ActorSqlite::onNoPendingFlush() {
  // SQLite data is synced to local disk on commit, there's nothing to wait for.
  return nullptr;
}

}  // namespace workerd
