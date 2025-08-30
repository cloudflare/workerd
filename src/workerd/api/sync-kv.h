// Copyright (c) 2017-2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "actor-state.h"

#include <workerd/jsg/jsg.h>

namespace workerd {
class SqliteKv;
}

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

  jsg::JsValue get(jsg::Lock& js, kj::OneOf<kj::String, kj::Array<kj::String>> keys);

  jsg::JsValue list(jsg::Lock& js, jsg::Optional<ListOptions> options);

  void put(jsg::Lock& js,
      kj::OneOf<kj::String, jsg::Dict<jsg::JsValue>> keyOrEntries,
      jsg::Optional<jsg::JsValue> value);

  kj::OneOf<bool, int> delete_(jsg::Lock& js, kj::OneOf<kj::String, kj::Array<kj::String>> keys);

  JSG_RESOURCE_TYPE(SyncKvStorage) {
    JSG_METHOD(get);
    JSG_METHOD(list);
    JSG_METHOD(put);
    JSG_METHOD_NAMED(delete, delete_);

    JSG_TS_OVERRIDE({
      get<T = unknown>(key: string): T | undefined;
      get<T = unknown>(keys: string[]): Map<string, T>;

      list<T = unknown>(options?: SyncKvStorageListOptions): Map<string, T>;

      put<T>(key: string, value: T): void;
      put<T>(entries: Record<string, T>): void;

      delete(key: string): boolean;
      delete(keys: string[]): number;
    });
  }

 private:
  jsg::Ref<DurableObjectStorage> storage;

  SqliteKv& getSqliteKv(jsg::Lock& js) {
    return storage->getSqliteKv(js);
  }
};

#define EW_SYNC_KV_ISOLATE_TYPES api::SyncKvStorage, api::SyncKvStorage::ListOptions

}  // namespace workerd::api
