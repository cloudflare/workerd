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

// Rust types, defined in ffi.rs.
struct HandlerCallback;
struct WaitUntilContinuation;

// Tells the request's tracer, if it has one, that the event is a custom event.
void invocation_set_custom_event_info(Invocation& invocation);

// Tells the request's tracer, if it has one, that the event is a scheduled event for
// `scheduledTime` and the cron pattern `cron`.
void invocation_set_scheduled_event_info(
    Invocation& invocation, kj::Date scheduledTime, ::rust::Slice<const kj::byte> cron);

// IoContext::IncomingRequest::delivered(), with `eventType`: the bytes of a NUL-terminated string
// that outlives the request, with its NUL, and with no other NUL. Fails otherwise. The span names
// below are passed the same way.
void invocation_delivered(Invocation& invocation, ::rust::Slice<const kj::byte> eventType);

// Lets the request finish in the background, in the event's waitUntil tasks: see
// IoContext::IncomingRequest::drain(). Needed once the event is delivered, unless it is finished
// with invocation_finish_scheduled(). Rust calls it when it drops the event, so it logs an exception
// rather than throwing it.
void invocation_drain(kj::Own<Invocation> invocation);

// Runs `callback` in a waitUntil task of the request, as `context.addWaitUntil(context.run(...))`
// runs a function, under a Perfetto span named `spanName`: in the request's IoContext, with the
// isolate locked and the request's async trace scopes entered, on the handles of the exported
// handler, its env and its ctx (see getHandlerHandles()). The task then waits for `then`. It fails
// with the exception that `callback` leaves pending, which IoContext::run() logs as uncaught, or
// with the KJ exception `callback` fails with, unchanged, or with the exception of `then`. Takes
// the invocation's version info and props.
void invocation_run_in_wait_until(Invocation& invocation,
    ::rust::Slice<const kj::byte> spanName,
    ::rust::Box<HandlerCallback> callback,
    ::rust::Box<WaitUntilContinuation> then);

// Starts the event's scheduled handler, for `scheduledTime` and the cron pattern `cron`, in a
// waitUntil task of the request, as WorkerEntrypoint::runScheduled() does in C++:
// ServiceWorkerGlobalScope::startScheduled() runs in IoContext::run(), on the exported handler.
// The task fails with the exception the handler throws, or with the KJ exception that
// startScheduled() throws when there is no handler. Takes the event's version info and props.
void invocation_start_scheduled_in_wait_until(
    Invocation& invocation, kj::Date scheduledTime, ::rust::Slice<const kj::byte> cron);

// IoContext::IncomingRequest::finishScheduled(), under a Perfetto span named `spanName`: waits for
// the request's waitUntil tasks, up to the time limit of scheduled events. The request finishes in
// the promise, so it is not drained.
kj::Promise<::workerd::rust::worker::ScheduledResult> invocation_finish_scheduled(
    kj::Own<Invocation> invocation, ::rust::Slice<const kj::byte> spanName);

// Delivers the scheduled event `event` to its handler from Rust, for `scheduledTime` and `cron`,
// as WorkerEntrypoint::runScheduled() does in C++: reports it to the tracer, delivers it, starts
// the handler in a waitUntil task and waits for the request to finish (see `scheduled.rs`).
// Everything but the wait happens before it returns, and an exception it fails with before then
// is thrown, as in C++. Dropping the promise cancels the request.
kj::Promise<WorkerInterface::ScheduledResult> runScheduled(
    kj::Own<Invocation> invocation, kj::Date scheduledTime, kj::StringPtr cron);

// Delivers the test event `invocation` to its handler's test() from Rust, as
// WorkerEntrypoint::test() did in C++: reports it to the tracer, delivers it, calls the handler in a
// waitUntil task and waits for the request to finish (see `test.rs`). Resolves with whether the
// test passed. Everything but the wait happens before it returns, and an exception it fails with
// before then is thrown. Dropping the promise cancels the request.
kj::Promise<bool> runTest(kj::Own<Invocation> invocation);

}  // namespace workerd::entrypoint
