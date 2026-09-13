// Copyright (c) 2017-2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "sync-kv.h"

#include <workerd/io/stored-value.h>
#include <workerd/util/sqlite-kv.h>

namespace workerd::api {

jsg::JsValue SyncKvStorage::get(jsg::Lock& js, kj::String key) {
  TraceContext traceContext =
      IoContext::current().makeUserTraceSpan("durable_object_storage_kv_get"_kjc);

  SqliteKv& sqliteKv = getSqliteKv(js);

  traceContext.setTag("db.system.name"_kjc, "cloudflare-durable-object-sql"_kjc);
  traceContext.setTag("db.operation.name"_kjc, "get"_kjc);
  traceContext.setTag("cloudflare.durable_object.kv.query.keys"_kjc, key.asPtr());
  traceContext.setTag("cloudflare.durable_object.kv.query.keys.count"_kjc, static_cast<int64_t>(1));

  kj::Maybe<jsg::JsValue> result;
  if (sqliteKv.get(key,
          [&](kj::ArrayPtr<const byte> value) { result = deserializeV8Value(js, key, value); })) {
    return KJ_ASSERT_NONNULL(result);
  } else {
    return js.undefined();
  }
}

SyncKvStorage::Projection SyncKvStorage::parseProjection(jsg::Optional<kj::String>& projection) {
  KJ_IF_SOME(p, projection) {
    if (p == "entries") return Projection::ENTRIES;
    if (p == "keys") return Projection::KEYS;
    if (p == "values") return Projection::VALUES;
    JSG_FAIL_REQUIRE(TypeError, "options.projection must be \"keys\", \"values\", or \"entries\".");
  }
  return Projection::ENTRIES;
}

jsg::Ref<SyncKvStorage::ListIterator> SyncKvStorage::list(
    jsg::Lock& js, jsg::Optional<ListOptions> maybeOptions) {
  // Validate the projection up front: before the trace span exists, so that an invalid value is
  // reported like any other bad argument rather than recording a storage operation that never
  // happened; before the options are moved below; and before the empty-range early return, so that
  // an invalid value is rejected regardless of the key range.
  Projection projection = Projection::ENTRIES;
  KJ_IF_SOME(o, maybeOptions) {
    projection = parseProjection(o.projection);
  }

  // The keys projection never looks at the value, so ask the storage layer not to read it.
  auto valueMode = projection == Projection::KEYS ? SqliteKv::KEYS_ONLY : SqliteKv::WITH_VALUES;

  TraceContext traceContext =
      IoContext::current().makeUserTraceSpan("durable_object_storage_kv_list"_kjc);
  SqliteKv& sqliteKv = getSqliteKv(js);

  traceContext.setTag("db.system.name"_kjc, "cloudflare-durable-object-sql"_kjc);
  traceContext.setTag("db.operation.name"_kjc, "list"_kjc);

  KJ_IF_SOME(o, maybeOptions) {
    KJ_IF_SOME(start, o.start) {
      traceContext.setTag("cloudflare.durable_object.kv.query.start"_kjc, start.asPtr());
    }
    KJ_IF_SOME(startAfter, o.startAfter) {
      traceContext.setTag("cloudflare.durable_object.kv.query.startAfter"_kjc, startAfter.asPtr());
    }
    KJ_IF_SOME(end, o.end) {
      traceContext.setTag("cloudflare.durable_object.kv.query.end"_kjc, end.asPtr());
    }
    KJ_IF_SOME(prefix, o.prefix) {
      traceContext.setTag("cloudflare.durable_object.kv.query.prefix"_kjc, prefix.asPtr());
    }
    KJ_IF_SOME(reverse, o.reverse) {
      traceContext.setTag("cloudflare.durable_object.kv.query.reverse"_kjc, reverse);
    }
    KJ_IF_SOME(limit, o.limit) {
      traceContext.setTag(
          "cloudflare.durable_object.kv.query.limit"_kjc, static_cast<int64_t>(limit));
    }

    if (o.projection != kj::none) {
      switch (projection) {
        case Projection::ENTRIES:
          traceContext.setTag("cloudflare.durable_object.kv.query.projection"_kjc, "entries"_kjc);
          break;
        case Projection::KEYS:
          traceContext.setTag("cloudflare.durable_object.kv.query.projection"_kjc, "keys"_kjc);
          break;
        case Projection::VALUES:
          traceContext.setTag("cloudflare.durable_object.kv.query.projection"_kjc, "values"_kjc);
          break;
      }
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
        // Key range is empty. Return an exhausted iterator.
        return js.alloc<SyncKvStorage::ListIterator>(ListState{
          .cursor = IoContext::current().createObject<SqliteKv::ListCursor>(nullptr, valueMode),
          .projection = projection,
        });
      });

  auto order = reverse ? SqliteKv::REVERSE : SqliteKv::FORWARD;
  auto cursor = (valueMode == SqliteKv::KEYS_ONLY ? sqliteKv.listKeys(start, end, limit, order)
                                                  : sqliteKv.list(start, end, limit, order))
                    .attach(kj::mv(start), kj::mv(end));

  return js.alloc<SyncKvStorage::ListIterator>(ListState{
    .cursor = IoContext::current().addObject(kj::mv(cursor)),
    .projection = projection,
  });
}

kj::Maybe<jsg::JsValue> SyncKvStorage::listNext(jsg::Lock& js, ListState& state) {
  auto& cursor = *state.cursor;

  if (state.projection == Projection::KEYS) {
    // The cursor came from listKeys() and has no value column to read.
    KJ_IF_SOME(key, cursor.nextKey()) {
      jsg::JsValue result = js.str(key);
      return result;
    }
  } else {
    KJ_IF_SOME(pair, cursor.next()) {
      if (state.projection == Projection::VALUES) {
        return deserializeV8Value(js, pair.key, pair.value);
      }
      jsg::JsValue entry = js.arr(js.str(pair.key), deserializeV8Value(js, pair.key, pair.value));
      return entry;
    }
  }

  if (cursor.wasCanceled()) {
    JSG_FAIL_REQUIRE(Error,
        "kv.list() iterator was invalidated because a new call to kv.list() was started. Only one "
        "kv.list() iterator can exist at a time.");
  } else {
    return kj::none;
  }
}

void SyncKvStorage::put(jsg::Lock& js, kj::String key, jsg::JsValue value) {
  TraceContext traceContext =
      IoContext::current().makeUserTraceSpan("durable_object_storage_kv_put"_kjc);
  SqliteKv& sqliteKv = getSqliteKv(js);

  traceContext.setTag("db.system.name"_kjc, "cloudflare-durable-object-sql"_kjc);
  traceContext.setTag("db.operation.name"_kjc, "put"_kjc);
  traceContext.setTag("cloudflare.durable_object.kv.query.keys"_kjc, key.asPtr());
  traceContext.setTag("cloudflare.durable_object.kv.query.keys.count"_kjc, static_cast<int64_t>(1));

  sqliteKv.put(key, serializeV8Value(js, key, value));
}

kj::OneOf<bool, int> SyncKvStorage::delete_(jsg::Lock& js, kj::String key) {
  auto& ioctx = IoContext::current();

  KJ_IF_SOME(handler, KJ_ASSERT_NONNULL(ioctx.getActor()).getStoredExternalHandler()) {
    handler.cancelPutExternals(key);
  }

  TraceContext traceContext = ioctx.makeUserTraceSpan("durable_object_storage_kv_delete"_kjc);
  SqliteKv& sqliteKv = getSqliteKv(js);

  traceContext.setTag("db.system.name"_kjc, "cloudflare-durable-object-sql"_kjc);
  traceContext.setTag("db.operation.name"_kjc, "delete"_kjc);
  traceContext.setTag("cloudflare.durable_object.kv.query.keys"_kjc, key.asPtr());
  traceContext.setTag("cloudflare.durable_object.kv.query.keys.count"_kjc, static_cast<int64_t>(1));

  auto deleted = sqliteKv.delete_(key);

  traceContext.setTag("cloudflare.durable_object.kv.response.deleted_count"_kjc,
      static_cast<int64_t>(deleted ? 1 : 0));

  return deleted;
}

}  // namespace workerd::api
