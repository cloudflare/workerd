// Copyright (c) 2017-2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "sync-kv.h"

#include "actor-state.h"

#include <workerd/util/sqlite-kv.h>

namespace workerd::api {

jsg::JsValue SyncKvStorage::get(jsg::Lock& js, kj::String key) {
  auto userSpan = IoContext::current().makeUserTraceSpan("durable_object_storage_kv_get"_kjc);
  SqliteKv& sqliteKv = getSqliteKv(js);

  userSpan.setTag("db.system.name"_kjc, kj::str("cloudflare-durable-object-sql"_kjc));
  userSpan.setTag("db.operation.name"_kjc, kj::str("get"_kjc));
  userSpan.setTag("cloudflare.durable_object.kv.query.keys"_kjc, kj::str(key));
  userSpan.setTag("cloudflare.durable_object.kv.query.keys.count"_kjc, static_cast<int64_t>(1));

  kj::Maybe<jsg::JsValue> result;
  if (sqliteKv.get(key,
          [&](kj::ArrayPtr<const byte> value) { result = deserializeV8Value(js, key, value); })) {
    return KJ_ASSERT_NONNULL(result);
  } else {
    return js.undefined();
  }
}

jsg::Ref<SyncKvStorage::ListIterator> SyncKvStorage::list(
    jsg::Lock& js, jsg::Optional<ListOptions> maybeOptions) {
  auto userSpan = IoContext::current().makeUserTraceSpan("durable_object_storage_kv_list"_kjc);
  SqliteKv& sqliteKv = getSqliteKv(js);

  userSpan.setTag("db.system.name"_kjc, kj::str("cloudflare-durable-object-sql"_kjc));
  userSpan.setTag("db.operation.name"_kjc, kj::str("list"_kjc));

  KJ_IF_SOME(o, maybeOptions) {
    KJ_IF_SOME(start, o.start) {
      userSpan.setTag("cloudflare.durable_object.kv.query.start"_kjc, kj::str(start));
    }
    KJ_IF_SOME(startAfter, o.startAfter) {
      userSpan.setTag("cloudflare.durable_object.kv.query.startAfter"_kjc, kj::str(startAfter));
    }
    KJ_IF_SOME(end, o.end) {
      userSpan.setTag("cloudflare.durable_object.kv.query.end"_kjc, kj::str(end));
    }
    KJ_IF_SOME(prefix, o.prefix) {
      userSpan.setTag("cloudflare.durable_object.kv.query.prefix"_kjc, kj::str(prefix));
    }
    KJ_IF_SOME(reverse, o.reverse) {
      userSpan.setTag("cloudflare.durable_object.kv.query.reverse"_kjc, kj::str(reverse));
    }
    KJ_IF_SOME(limit, o.limit) {
      userSpan.setTag("cloudflare.kv.query.limit"_kjc, static_cast<int64_t>(limit));
    }
  }

  // Convert our options to DurableObjectStorageOperations::ListOptions (which also have the
  // `allowConcurrency` and `noCache` options, which are irrelevant in the sync interface).
  auto asyncOptions = kj::mv(maybeOptions).map([&](ListOptions&& options) {
    return DurableObjectStorageOperations::ListOptions{
      .start = kj::mv(options.start),
      .startAfter = kj::mv(options.startAfter),
      .end = kj::mv(options.end),
      .prefix = kj::mv(options).prefix,
      .reverse = options.reverse,
      .limit = options.limit,
    };
  });

  auto [start, end, reverse, limit] =
      KJ_UNWRAP_OR(DurableObjectStorageOperations::compileListOptions(asyncOptions), {
        // Key range is empty. Return empty map.
        return js.alloc<SyncKvStorage::ListIterator>(
            IoContext::current().addObject(kj::heap<SqliteKv::ListCursor>(nullptr)));
      });

  auto cursor = sqliteKv.list(start, end, limit, reverse ? SqliteKv::REVERSE : SqliteKv::FORWARD)
                    .attach(kj::mv(start), kj::mv(end));

  return js.alloc<SyncKvStorage::ListIterator>(IoContext::current().addObject(kj::mv(cursor)));
}

kj::Maybe<jsg::JsArray> SyncKvStorage::listNext(jsg::Lock& js, IoOwn<SqliteKv::ListCursor>& state) {
  auto& stateRef = *state;
  KJ_IF_SOME(pair, stateRef.next()) {
    return js.arr(js.str(pair.key), deserializeV8Value(js, pair.key, pair.value));
  } else if (stateRef.wasCanceled()) {
    JSG_FAIL_REQUIRE(Error,
        "kv.list() iterator was invalidated because a new call to kv.list() was started. Only one "
        "kv.list() iterator can exist at a time.");
  } else {
    return kj::none;
  }
}

void SyncKvStorage::put(jsg::Lock& js, kj::String key, jsg::JsValue value) {
  auto userSpan = IoContext::current().makeUserTraceSpan("durable_object_storage_kv_put"_kjc);
  SqliteKv& sqliteKv = getSqliteKv(js);

  userSpan.setTag("db.system.name"_kjc, kj::str("cloudflare-durable-object-sql"_kjc));
  userSpan.setTag("db.operation.name"_kjc, kj::str("put"_kjc));
  userSpan.setTag("cloudflare.durable_object.kv.query.keys"_kjc, kj::str(key));
  userSpan.setTag("cloudflare.durable_object.kv.query.keys.count"_kjc, static_cast<int64_t>(1));

  sqliteKv.put(key, serializeV8Value(js, value));
}

kj::OneOf<bool, int> SyncKvStorage::delete_(jsg::Lock& js, kj::String key) {
  auto userSpan = IoContext::current().makeUserTraceSpan("durable_object_storage_kv_delete"_kjc);
  SqliteKv& sqliteKv = getSqliteKv(js);

  userSpan.setTag("db.system.name"_kjc, kj::str("cloudflare-durable-object-sql"_kjc));
  userSpan.setTag("db.operation.name"_kjc, kj::str("delete"_kjc));
  userSpan.setTag("cloudflare.durable_object.kv.query.keys"_kjc, kj::str(key));
  userSpan.setTag("cloudflare.durable_object.kv.query.keys.count"_kjc, static_cast<int64_t>(1));

  auto deleted = sqliteKv.delete_(key);

  userSpan.setTag("cloudflare.durable_object.kv.response.deleted_count"_kjc,
      static_cast<int64_t>(deleted ? 1 : 0));

  return deleted;
}

}  // namespace workerd::api
