// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "inspector.h"

#include <workerd/io/worker.h>
#include <workerd/util/thread-scopes.h>

namespace workerd::api::node {

InspectorConnection::InspectorConnection(jsg::Lock& js, MessageCallback callbackParam)
    : callback(kj::mv(callbackParam)),
      channel(kj::heap<ChannelImpl>(*this)) {
  // Defense in depth: the `enable_nodejs_inspector_local_dev` flag is `$experimental` (so it cannot
  // be used in Workers deployed to Cloudflare) and the V8 inspector is only created outside
  // multi-tenant processes. We re-check the multi-tenant invariant here so this code path can never
  // do anything in the production runtime even if the gating above were ever bypassed.
  JSG_REQUIRE(!isMultiTenantProcess(), Error,
      "node:inspector Session is not available in this environment.");

  auto& inspector = JSG_REQUIRE_NONNULL(Worker::Isolate::from(js).tryGetV8Inspector(), Error,
      "node:inspector Session requires the experimental 'enable_nodejs_inspector_local_dev' "
      "compatibility flag and is only available when running workerd locally with --experimental.");

  // Connect a fully-trusted, independent session to the isolate's V8 inspector. Context group 1
  // matches the id used in setupContext()'s contextCreated() call (see worker.c++).
  session = inspector.connect(
      1, channel.get(), v8_inspector::StringView(), v8_inspector::V8Inspector::kFullyTrusted);
}

jsg::Ref<InspectorConnection> InspectorConnection::constructor(
    jsg::Lock& js, MessageCallback callback) {
  return js.alloc<InspectorConnection>(js, kj::mv(callback));
}

void InspectorConnection::dispatch(jsg::Lock& js, kj::String message) {
  auto* s = session.get();
  JSG_REQUIRE(s != nullptr, Error, "Inspector connection is not connected.");

  auto sv = jsg::toInspectorStringView(message);
  // dispatchProtocolMessage() runs synchronously and cannot re-enter JavaScript: our ChannelImpl
  // only buffers responses/notifications into `outgoing` and never invokes the JS callback, so
  // `this` cannot be destroyed while it runs. JavaScript only runs below in flush(), where `this`
  // is kept alive by the JS Session that owns this connection for the duration of the call. Hence
  // no use-after-free across the flush(js) below.
  s->dispatchProtocolMessage(sv.stringView);

  // The V8 inspector handled the message synchronously and queued any responses/notifications in
  // `outgoing`; deliver them to JavaScript now.
  flush(js);
}

void InspectorConnection::disconnect(jsg::Lock& js) {
  // Destroying the session must happen under the isolate lock, which we hold via `js`.
  session = nullptr;
  outgoing.clear();
}

void InspectorConnection::flush(jsg::Lock& js) {
  // A delivered message can cause JS to call dispatch() reentrantly, which appends to `outgoing`
  // and calls flush() again. Guard against a nested drain so messages are always delivered in FIFO
  // order: a reentrant flush() returns immediately, and the outer loop below picks up the newly
  // appended messages and delivers them after the ones already queued.
  if (draining) {
    return;
  }
  draining = true;
  KJ_DEFER(draining = false);

  // Drain in batches: move the queued messages out into a local kj::Array before delivering them,
  // so any reentrant dispatch() can safely append to the now-empty `outgoing` without invalidating
  // what we're iterating. Each `batch` is a stable, independently-allocated kj::Array, so a
  // range-for over it can never be a use-after-move or iterator-invalidation hazard regardless of
  // what callbacks do to `outgoing`. Loop until quiet to deliver the reentrant appends in order.
  while (!outgoing.empty()) {
    auto batch = outgoing.releaseAsArray();
    for (auto& msg: batch) {
      callback(js, kj::mv(msg));
    }
  }
}

}  // namespace workerd::api::node
