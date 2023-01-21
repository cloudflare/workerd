// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "actor-cache.h"
#include <workerd/util/sqlite-kv.h>

namespace workerd {

class ActorSqlite final: public ActorCacheInterface {
  // An implementation of ActorCacheInterface that is backed by SqliteKv.
  //
  // TODO(perf): This interface is not designed ideally for wrapping SqliteKv. In particular, we
  //   end up allocating extra copies of all the results. It would be nicer if we could actually
  //   parse the V8-serialized values directly from the blob pointers that SQLite spits out.
  //   However, that probably requires rewriting `DurableObjectStorageOperations`. For now, hooking
  //   here is easier and not too costly.

public:
  ActorSqlite(Sqlite::Vfs& vfs, kj::PathPtr path)
      : db(vfs, path, kj::WriteMode::CREATE | kj::WriteMode::MODIFY | kj::WriteMode::CREATE_PARENT),
        kv(db) {}

  kj::OneOf<kj::Maybe<Value>, kj::Promise<kj::Maybe<Value>>> get(
      Key key, ReadOptions options) override;
  kj::OneOf<GetResultList, kj::Promise<GetResultList>> get(
      kj::Array<Key> keys, ReadOptions options) override;
  kj::OneOf<kj::Maybe<kj::Date>, kj::Promise<kj::Maybe<kj::Date>>> getAlarm(
      ReadOptions options) override;
  kj::OneOf<GetResultList, kj::Promise<GetResultList>> list(
      Key begin, kj::Maybe<Key> end, kj::Maybe<uint> limit, ReadOptions options) override;
  kj::OneOf<GetResultList, kj::Promise<GetResultList>> listReverse(
      Key begin, kj::Maybe<Key> end, kj::Maybe<uint> limit, ReadOptions options) override;
  kj::Maybe<kj::Promise<void>> put(Key key, Value value, WriteOptions options) override;
  kj::Maybe<kj::Promise<void>> put(kj::Array<KeyValuePair> pairs, WriteOptions options) override;
  kj::OneOf<bool, kj::Promise<bool>> delete_(Key key, WriteOptions options) override;
  kj::OneOf<uint, kj::Promise<uint>> delete_(kj::Array<Key> keys, WriteOptions options) override;
  kj::Maybe<kj::Promise<void>> setAlarm(kj::Maybe<kj::Date> newAlarmTime, WriteOptions options) override;

  void deleteAll() { kv.deleteAll(); }
  // TODO(sqlite): deleteAll() should delete the database from disk if there are no other tables.
  //   For that matter, the database should not be created on disk until it is first written.

  kj::Maybe<kj::Promise<void>> sync() { return nullptr; }
  // TODO(sqlite): synk() should wait for replication if applicable.

private:
  Sqlite db;
  SqliteKv kv;
};

}  // namespace workerd
