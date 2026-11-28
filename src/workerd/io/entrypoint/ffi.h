// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/entrypoint-invocation.h>
#include <workerd/io/io-context/ffi.h>
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

// CxxEntrypoint::takeInvocation().
kj::Own<Invocation> cxx_entrypoint_take_invocation(CxxEntrypoint& entrypoint);

// The event's request, which Rust passes to the IoContext::IncomingRequest calls of the io-context
// crate. Taken out of the invocation, by invocation_take_request(), to be finished.
IoContext::IncomingRequest& invocation_request(Invocation& invocation);
kj::Own<IoContext::IncomingRequest> invocation_take_request(Invocation& invocation);

// Takes what finds the test event's handler out of the invocation (its entrypoint name, version
// info and props), as the prelude of a run in a waitUntil task: in the run, before it calls Rust,
// the prelude finds the handler and its test() function with getTestHandles()
// (`io/entrypoint-invocation.h`) and returns the test() function, the handler, env and ctx, in
// that order.
kj::Own<io_context::RunPrelude> invocation_take_test_handler_lookup(Invocation& invocation);

// startScheduledInWaitUntil() (`io/entrypoint-invocation.h`), which
// WorkerEntrypoint::runScheduled() also calls, for the event's request, for `scheduledTime` and
// the cron pattern `cron`. Takes the event's entrypoint name, version info and props.
void invocation_start_scheduled_in_wait_until(
    Invocation& invocation, kj::Date scheduledTime, ::rust::Slice<const kj::byte> cron);

// Delivers the test event `invocation` to its handler's test() from Rust: reports it to the
// tracer, delivers it, calls the handler in a waitUntil task and waits for the request to finish
// (see `test.rs`). Resolves with whether the test passed. Everything but the wait happens before
// it returns, and an exception it fails with before then is thrown. Dropping the promise cancels
// the request.
kj::Promise<bool> runTest(kj::Own<Invocation> invocation);

// Makes a Rust WorkerEntrypoint (see `lib.rs`) that owns `cxx`, and returns its WorkerInterface.
// Behind the RUST_ENTRYPOINT_SCHEDULED autogate, it delivers the scheduled event from Rust, as
// WorkerEntrypoint::runScheduled() does in C++: everything but the wait for the request to finish
// happens before runScheduled() returns, and an exception before then is thrown, as in C++. Every
// other event goes to `cxx`, which then does its work as it would on its own.
kj::Own<WorkerInterface> newWorkerEntrypoint(kj::Own<CxxEntrypoint> cxx);

}  // namespace workerd::entrypoint
