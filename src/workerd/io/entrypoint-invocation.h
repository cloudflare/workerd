// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/frankenvalue.h>
#include <workerd/io/io-context.h>
#include <workerd/io/worker.h>

namespace workerd::entrypoint {

// One invocation of a WorkerEntrypoint for an event, which WorkerEntrypoint hands to Rust to
// deliver: the request the event was made for, and what finds the handler that receives it. Rust owns it from then on, and
// operates on it through the calls in `entrypoint/ffi.h`.
struct Invocation {
  kj::Own<IoContext::IncomingRequest> request;
  kj::TaskSet& waitUntilTasks;
  kj::Maybe<kj::String> entrypointName;
  kj::Maybe<Worker::VersionInfo> versionInfo;
  Frankenvalue props;
};

// The handles of the exported handler that an event is delivered to, for Rust, which takes them
// as V8 handles. Each is undefined if there is no handler, as under Service Workers syntax, and
// `ctx` is also undefined if the handler has none.
struct HandlerHandles {
  v8::Local<v8::Value> handler;
  v8::Local<v8::Value> env;
  v8::Local<v8::Value> ctx;
};

// Finds the exported handler with Worker::Lock::getExportedHandler(), for the current request
// `context`, and returns its handles, in the lock's HandleScope.
HandlerHandles getHandlerHandles(Worker::Lock& lock,
    IoContext& context,
    kj::Maybe<kj::StringPtr> entrypointName,
    kj::Maybe<Worker::VersionInfo> versionInfo,
    Frankenvalue props);

}  // namespace workerd::entrypoint
