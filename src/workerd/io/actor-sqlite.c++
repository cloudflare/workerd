// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "actor-sqlite.h"
#include <algorithm>

namespace workerd {

kj::OneOf<kj::Maybe<ActorCacheInterface::Value>,
          kj::Promise<kj::Maybe<ActorCacheInterface::Value>>>
    ActorSqlite::get(Key key, ReadOptions options) {
  kj::Maybe<ActorCacheInterface::Value> result;
  kv.get(key, [&](ValuePtr value) {
    result = kj::heapArray(value);
  });
  return result;
}

kj::OneOf<ActorCacheInterface::GetResultList, kj::Promise<ActorCacheInterface::GetResultList>>
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

kj::OneOf<ActorCacheInterface::GetResultList, kj::Promise<ActorCacheInterface::GetResultList>>
    ActorSqlite::list(Key begin, kj::Maybe<Key> end, kj::Maybe<uint> limit, ReadOptions options) {
  kj::Vector<KeyValuePair> results;
  kv.list(begin, end, limit, SqliteKv::FORWARD, [&](KeyPtr key, ValuePtr value) {
    results.add(KeyValuePair { kj::str(key), kj::heapArray(value) });
  });

  // Already guaranteed sorted.
  return GetResultList(kj::mv(results));
}

kj::OneOf<ActorCacheInterface::GetResultList, kj::Promise<ActorCacheInterface::GetResultList>>
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

}  // namespace workerd
