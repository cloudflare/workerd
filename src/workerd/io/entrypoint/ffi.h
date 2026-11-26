// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/entrypoint-invocation.h>
#include <workerd/io/io-context/ffi.h>
#include <workerd/io/worker-interface.h>

#include <rust/cxx.h>

#include <capnp/common.h>
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

// Takes what finds the test event's handler out of the invocation (its entrypoint name, version
// info and props), as the prelude of a run in a waitUntil task: in the run, before it calls Rust,
// the prelude finds the handler and its test() function with getTestHandles()
// (`io/entrypoint-invocation.h`) and returns the test() function, the handler, env and ctx, in
// that order.
kj::Own<io_context::RunPrelude> invocation_take_test_handler_lookup(Invocation& invocation);

// Takes what finds the bench event's handler out of the invocation, as
// invocation_take_test_handler_lookup() does, with a copy of `params`, an encoded `BenchParams`
// message, as the prelude of the run that waits for the event's group: the prelude starts the
// handler's bench() with startBench() (`io/entrypoint-invocation.h`) and returns the group's
// promise.
kj::Own<io_context::RunPrelude> invocation_take_bench_start(
    Invocation& invocation, ::rust::Slice<const uint64_t> params);

// The task set in which draining the event's request waits for its waitUntil tasks.
kj::TaskSet& invocation_wait_until_tasks(Invocation& invocation);

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

// Delivers the test event `invocation` to its handler's test() from Rust: reports it to the
// tracer, delivers it, calls the handler in a waitUntil task and waits for the request to finish
// (see `test.rs`). Resolves with whether the test passed. Everything but the wait happens before
// it returns, and an exception it fails with before then is thrown. Dropping the promise cancels
// the request.
kj::Promise<bool> runTest(kj::Own<Invocation> invocation);

// Delivers the bench event `invocation` to its handler's bench() from Rust, with `params`, an
// encoded `BenchParams` message: reports it to the tracer, delivers it, starts bench() in
// IoContext::run() through Worker::Api::runBench(), waits for the cases it registers to run, and
// drains the request, whether the run succeeds, fails or is cancelled (see `bench.rs`). Resolves
// with the group's `BenchReport.Group`, without its name, as an encoded message. Everything but the
// run happens before it returns, and an exception it fails with before then is thrown. Dropping
// the promise cancels the run.
kj::Promise<kj::Array<capnp::word>> runBench(
    kj::Own<Invocation> invocation, kj::Array<capnp::word> params);

}  // namespace workerd::entrypoint
