// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "entrypoint-invocation.h"

#include <workerd/api/global-scope.h>
#include <workerd/util/use-perfetto-categories.h>

namespace workerd::entrypoint {

void startScheduledInWaitUntil(IoContext& context,
    kj::Date scheduledTime,
    kj::String cron,
    kj::Maybe<kj::String> entrypointName,
    kj::Maybe<Worker::VersionInfo> versionInfo,
    Frankenvalue props) {
  // Scheduled handlers run entirely in waitUntil() tasks.
  context.addWaitUntil(
      context.run([scheduledTime, cron = kj::mv(cron), entrypointName = kj::mv(entrypointName),
                      versionInfo = kj::mv(versionInfo),
                      props = kj::mv(props)](Worker::Lock& lock, IoContext& context) mutable {
    TRACE_EVENT("workerd", "WorkerEntrypoint::runScheduled() run");
    auto traceScope = context.makeRequestTraceScope(lock);

    lock.getGlobalScope().startScheduled(scheduledTime, cron, lock,
        lock.getExportedHandler(
            entrypointName, kj::mv(versionInfo), kj::mv(props), context.getActor()));
  }));
}

TestHandles getTestHandles(Worker::Lock& lock, IoContext& context, HandlerLookup lookup) {
  jsg::Lock& js = lock;
  auto exportedHandler = lock.getExportedHandler(
      lookup.entrypointName.map([](kj::String& name) -> kj::StringPtr { return name; }),
      kj::mv(lookup.versionInfo), kj::mv(lookup.props), context.getActor());
  auto test = lock.getGlobalScope().getTestHandler(js,
      exportedHandler.map(
          [](kj::Own<api::ExportedHandler>& h) -> api::ExportedHandler& { return *h; }));
  // getTestHandler() has thrown if there is no exported handler.
  auto& h = *KJ_ASSERT_NONNULL(exportedHandler);
  // The handles stay valid in the lock's HandleScope once the ExportedHandler is gone.
  TestHandles handles{
    .test = test,
    .handler = h.self.getHandle(js),
    .env = h.env.getHandle(js),
    .ctx = js.v8Undefined(),
  };
  KJ_IF_SOME(c, h.getCtx(js)) {
    handles.ctx = c.getHandle(js);
  }
  return handles;
}

v8::Local<v8::Promise> startBench(Worker::Lock& lock,
    IoContext& context,
    HandlerLookup lookup,
    kj::ArrayPtr<const capnp::word> params) {
  jsg::Lock& js = lock;
  auto exportedHandler = lock.getExportedHandler(
      lookup.entrypointName.map([](kj::String& name) -> kj::StringPtr { return name; }),
      kj::mv(lookup.versionInfo), kj::mv(lookup.props), context.getActor());
  auto& h = *JSG_REQUIRE_NONNULL(exportedHandler, Error,
      "Benchmarks are not currently supported with Service Workers syntax.");
  v8::Local<v8::Value> ctx = js.v8Undefined();
  KJ_IF_SOME(c, h.getCtx(js)) {
    ctx = c.getHandle(js);
  }
  return lock.getWorker().getIsolate().getApi().runBench(
      js, h.self.getHandle(js), h.env.getHandle(js), ctx, params);
}

}  // namespace workerd::entrypoint
