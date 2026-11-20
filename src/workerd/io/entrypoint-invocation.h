// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/frankenvalue.h>
#include <workerd/io/io-context.h>
#include <workerd/io/worker-interface.h>
#include <workerd/io/worker.h>

namespace workerd::entrypoint {

// One invocation of a WorkerEntrypoint for an event, which WorkerEntrypoint hands to Rust to
// deliver: the request the event was made for, and what finds the handler that receives it. Rust
// owns it from then on, and operates on it through the calls in `entrypoint/ffi.h`.
struct Invocation {
  kj::Own<IoContext::IncomingRequest> request;
  kj::TaskSet& waitUntilTasks;
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

}  // namespace workerd::entrypoint
