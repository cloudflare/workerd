// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/entrypoint-invocation.h>
#include <workerd/io/worker-interface.h>

#include <rust/cxx.h>

#include <kj/async.h>
#include <kj/time.h>

namespace workerd::rust::worker {
struct ScheduledResult;
}  // namespace workerd::rust::worker

// The calls through which Rust delivers an event of a WorkerEntrypoint (see `ffi.rs`). Each one
// does its work when it is called; an async one returns a promise for what remains to wait for.
namespace workerd::entrypoint {

// The event's request, which Rust passes to the IoContext::IncomingRequest calls of the io-context
// crate. Taken out of the invocation, by invocation_take_request(), to be finished.
IoContext::IncomingRequest& invocation_request(Invocation& invocation);
kj::Own<IoContext::IncomingRequest> invocation_take_request(Invocation& invocation);

// startScheduledInWaitUntil() (`io/entrypoint-invocation.h`), which
// WorkerEntrypoint::runScheduled() also calls, for the event's request, for `scheduledTime` and
// the cron pattern `cron`. Takes the event's entrypoint name, version info and props.
void invocation_start_scheduled_in_wait_until(
    Invocation& invocation, kj::Date scheduledTime, ::rust::Slice<const kj::byte> cron);

// Delivers the scheduled event of `invocation` to its handler from Rust, for `scheduledTime` and
// `cron`, as WorkerEntrypoint::runScheduled() does in C++: reports it to the tracer, delivers it,
// starts the handler in a waitUntil task and waits for the request to finish (see
// `scheduled.rs`). Everything but the wait happens before it returns, and an exception it fails
// with before then is thrown, as in C++. Dropping the promise cancels the request.
kj::Promise<WorkerInterface::ScheduledResult> runScheduled(
    kj::Own<Invocation> invocation, kj::Date scheduledTime, kj::StringPtr cron);

}  // namespace workerd::entrypoint
