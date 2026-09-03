// Copyright (c) 2017-2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "sync-kv.h"

#include <workerd/io/stored-value.h>
#include <workerd/util/sqlite-kv.h>
#include <workerd/util/use-perfetto-categories.h>

namespace workerd::api {

namespace {

class PerfettoSyncKvList final: public SyncKvStorage::ListTrace {
 public:
  PerfettoSyncKvList() {
    TRACE_EVENT_BEGIN(WORKERD_TRACE_CATEGORY("io"), "Durable Object synchronous KV list cursor",
        PERFETTO_TRACK_FROM_POINTER(this), PERFETTO_FLOW_FROM_POINTER(this));
  }

  ~PerfettoSyncKvList() noexcept {
    TRACE_EVENT_END(WORKERD_TRACE_CATEGORY("io"), PERFETTO_TRACK_FROM_POINTER(this),
        PERFETTO_TERMINATING_FLOW_FROM_POINTER(this));
  }

  KJ_DISALLOW_COPY_AND_MOVE(PerfettoSyncKvList);
};

kj::Own<SyncKvStorage::ListTrace> traceSyncKvList() {
  if (TRACE_EVENT_CATEGORY_ENABLED(WORKERD_TRACE_CATEGORY("io"))) {
    return kj::heap<PerfettoSyncKvList>();
  }
  return {};
}

}  // namespace

jsg::JsValue SyncKvStorage::get(jsg::Lock& js, kj::String key) {
  TRACE_EVENT(
      WORKERD_TRACE_CATEGORY("io"), "Durable Object synchronous KV get", "key_size", key.size());
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

jsg::Ref<SyncKvStorage::ListIterator> SyncKvStorage::list(
    jsg::Lock& js, jsg::Optional<ListOptions> maybeOptions) {
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

  auto [start, end, reverse,
      limit] = KJ_UNWRAP_OR(DurableObjectStorageOperations::compileListOptions(asyncOptions), {
    // Key range is empty. Return empty map.
    return js.alloc<SyncKvStorage::ListIterator>(ListState(
        IoContext::current().createObject<SqliteKv::ListCursor>(nullptr), kj::Own<ListTrace>()));
  });

  auto perfettoTrace = traceSyncKvList();
  auto cursor = sqliteKv.list(start, end, limit, reverse ? SqliteKv::REVERSE : SqliteKv::FORWARD)
                    .attach(kj::mv(start), kj::mv(end));
  return js.alloc<SyncKvStorage::ListIterator>(
      ListState(IoContext::current().addObject(kj::mv(cursor)), kj::mv(perfettoTrace)));
}

kj::Maybe<jsg::JsArray> SyncKvStorage::listNext(jsg::Lock& js, ListState& state) {
  auto& stateRef = *state.cursor;
  KJ_IF_SOME(pair, stateRef.next()) {
    return js.arr(js.str(pair.key), deserializeV8Value(js, pair.key, pair.value));
  } else {
    state.trace = nullptr;
    if (stateRef.wasCanceled()) {
      JSG_FAIL_REQUIRE(Error,
          "kv.list() iterator was invalidated because a new call to kv.list() was started. Only "
          "one kv.list() iterator can exist at a time.");
    } else {
      return kj::none;
    }
  }
}

void SyncKvStorage::put(jsg::Lock& js, kj::String key, jsg::JsValue value) {
  TRACE_EVENT(
      WORKERD_TRACE_CATEGORY("io"), "Durable Object synchronous KV put", "key_size", key.size());
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
  TRACE_EVENT(
      WORKERD_TRACE_CATEGORY("io"), "Durable Object synchronous KV delete", "key_size", key.size());
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
