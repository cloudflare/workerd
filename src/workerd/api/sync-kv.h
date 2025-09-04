// Copyright (c) 2017-2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "actor-state.h"

#include <workerd/jsg/jsg.h>
#include <workerd/jsg/iterator.h>
#include <workerd/util/sqlite-kv.h>

namespace workerd::api {

// Synchronous KV storage. Available as ctx.storage.kv on SQLite-backed DOs.
class SyncKvStorage: public jsg::Object {
 public:
  SyncKvStorage(jsg::Ref<DurableObjectStorage> storage): storage(kj::mv(storage)) {}

  struct ListOptions {
    jsg::Optional<kj::String> start;
    jsg::Optional<kj::String> startAfter;
    jsg::Optional<kj::String> end;
    jsg::Optional<kj::String> prefix;
    jsg::Optional<bool> reverse;
    jsg::Optional<int> limit;

    JSG_STRUCT(start, startAfter, end, prefix, reverse, limit);
    JSG_STRUCT_TS_OVERRIDE(SyncKvListOptions);  // Rename from SyncKvStorageListOptions
  };

  jsg::JsValue get(jsg::Lock& js, kj::String key);

  JSG_ITERATOR_TYPE(ListIterator, jsg::JsArray, IoOwn<SqliteKv::ListCursor>, listNext);

  jsg::Ref<ListIterator> list(jsg::Lock& js, jsg::Optional<ListOptions> options);

  void put(jsg::Lock& js, kj::String key, jsg::JsValue value);

  kj::OneOf<bool, int> delete_(jsg::Lock& js, kj::String key);

  JSG_RESOURCE_TYPE(SyncKvStorage) {
    JSG_METHOD(get);
    JSG_METHOD(list);
    JSG_METHOD(put);
    JSG_METHOD_NAMED(delete, delete_);

    JSG_TS_OVERRIDE({
      get<T = unknown>(key: string): T | undefined;

      list<T = unknown>(options?: SyncKvStorageListOptions): Iterable<[string, T]>;

      put<T>(key: string, value: T): void;

      delete(key: string): boolean;
    });
  }

 private:
  jsg::Ref<DurableObjectStorage> storage;

  SqliteKv& getSqliteKv(jsg::Lock& js) {
    return storage->getSqliteKv(js);
  }

  static kj::Maybe<jsg::JsArray> listNext(jsg::Lock& js, IoOwn<SqliteKv::ListCursor>& state);
};

#define EW_SYNC_KV_ISOLATE_TYPES api::SyncKvStorage, api::SyncKvStorage::ListOptions,              \
    api::SyncKvStorage::ListIterator, api::SyncKvStorage::ListIterator::Next

}  // namespace workerd::api
