// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/frankenvalue.h>
#include <workerd/io/io-context.h>
#include <workerd/io/worker-interface.h>
#include <workerd/io/worker.h>
#include <workerd/util/strong-bool.h>

namespace workerd {

// Whether a WorkerEntrypoint's entrypoint name was supplied at request time (see
// Worker::Lock::getExportedHandler()).
WD_STRONG_BOOL(IsDynamicDispatch);

}  // namespace workerd

namespace workerd::entrypoint {

class Fetch;

// One invocation of a WorkerEntrypoint for an event, which WorkerEntrypoint hands to Rust to
// deliver: the request the event was made for, and what finds the handler that receives it. Rust
// owns it from then on, and operates on it through the calls in `entrypoint/ffi.h`.
struct Invocation {
  kj::Own<IoContext::IncomingRequest> request;
  kj::Maybe<kj::String> entrypointName;
  kj::Maybe<Worker::VersionInfo> versionInfo;
  Frankenvalue props;
};

// A C++ WorkerEntrypoint (`io/worker-entrypoint.c++`), as the Rust WorkerEntrypoint that owns it
// sees it (`entrypoint/lib.rs`). The Rust one delivers an event that Rust implements by taking
// the invocation, and any other event by calling this one as a WorkerInterface.
class CxxEntrypoint: public WorkerInterface {
 public:
  // Takes the invocation of the entrypoint's one event, for Rust to deliver. Fails if an event was
  // already delivered.
  virtual kj::Own<Invocation> takeInvocation() = 0;

  // Takes the invocation of the entrypoint's one event as a fetch event (see entrypoint-fetch.h),
  // with the arguments of request(). `flowId` keys its Perfetto flow events. Fails if an event was
  // already delivered.
  virtual kj::Own<Fetch> takeFetch(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      kj::HttpService::Response& response,
      const void* flowId) = 0;

  // The canceler that cancels the entrypoint's event when its actor is aborted, if it has one.
  virtual kj::Maybe<kj::Canceler&> getCanceler() = 0;
};

// Starts the scheduled handler of the exported handler that `entrypointName`, `versionInfo` and
// `props` find, for `scheduledTime` and the cron pattern `cron`, in a waitUntil task of `context`:
// ServiceWorkerGlobalScope::startScheduled() runs in IoContext::run(). The task fails with the
// exception the handler throws, or with the KJ exception that startScheduled() throws when there
// is no handler. WorkerEntrypoint::runScheduled() calls it, and so does Rust, through
// `invocation_start_scheduled_in_wait_until()` in `entrypoint/ffi.h`.
void startScheduledInWaitUntil(IoContext& context,
    kj::Date scheduledTime,
    kj::String cron,
    kj::Maybe<kj::String> entrypointName,
    kj::Maybe<Worker::VersionInfo> versionInfo,
    Frankenvalue props);

// What finds the exported handler that an event is delivered to, taken out of an Invocation for
// the function that runs the handler, as a C++ event's lambda captures it.
struct HandlerLookup {
  kj::Maybe<kj::String> entrypointName;
  kj::Maybe<Worker::VersionInfo> versionInfo;
  Frankenvalue props;
};

// The handles that a test event calls its handler with: the handler's test() function, the
// exported handler it is a method of, env, and ctx, which is undefined if the handler has none.
struct TestHandles {
  v8::Local<v8::Value> test;
  v8::Local<v8::Value> handler;
  v8::Local<v8::Value> env;
  v8::Local<v8::Value> ctx;
};

// Finds the exported handler that `lookup` names with Worker::Lock::getExportedHandler(), in
// IoContext::run() for `context`, and its test() function with
// ServiceWorkerGlobalScope::getTestHandler(), and returns their handles, in the lock's HandleScope.
// JavaScript that finding the handler runs, such as a class entrypoint's constructor, can throw,
// and so does getTestHandler(), under Service Workers syntax or without a test() function; the
// exception is thrown on. The entrypoint crate runs it as the C++ prelude of a run in a waitUntil
// task (`io_context::RunPrelude`), before Rust holds the lock.
TestHandles getTestHandles(Worker::Lock& lock, IoContext& context, HandlerLookup lookup);

}  // namespace workerd::entrypoint
