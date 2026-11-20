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

// Tells the request's tracer, if it has one, that the event is a scheduled event for
// `scheduledTime` and the cron pattern `cron`.
void invocation_set_scheduled_event_info(
    Invocation& invocation, kj::Date scheduledTime, ::rust::Slice<const kj::byte> cron);

// IoContext::IncomingRequest::delivered(), with `eventType`: the bytes of a NUL-terminated string
// that outlives the request, with its NUL, and with no other NUL. Fails otherwise.
void invocation_delivered(Invocation& invocation, ::rust::Slice<const kj::byte> eventType);

// Lets the request finish in the background, in the event's waitUntil tasks: see
// IoContext::IncomingRequest::drain(). Needed once the event is delivered, unless it is finished
// with invocation_finish_scheduled(). Rust calls it when it drops the event, so it logs an
// exception rather than throwing it.
void invocation_drain(kj::Own<Invocation> invocation);

// startScheduledInWaitUntil() (`io/entrypoint-invocation.h`), which
// WorkerEntrypoint::runScheduled() also calls, for the event's request, for `scheduledTime` and
// the cron pattern `cron`. Takes the event's entrypoint name, version info and props.
void invocation_start_scheduled_in_wait_until(
    Invocation& invocation, kj::Date scheduledTime, ::rust::Slice<const kj::byte> cron);

// IoContext::IncomingRequest::finishScheduled(): waits for the request's waitUntil tasks, up to
// the time limit of scheduled events. The request finishes in the promise, so it is not drained.
// An exception that finishScheduled() throws rejects the promise.
kj::Promise<::workerd::rust::worker::ScheduledResult> invocation_finish_scheduled(
    kj::Own<Invocation> invocation);

// Delivers the scheduled event `event` to its handler from Rust, for `scheduledTime` and `cron`,
// as WorkerEntrypoint::runScheduled() does in C++: reports it to the tracer, delivers it, starts
// the handler in a waitUntil task and waits for the request to finish (see `scheduled.rs`).
// Everything but the wait happens before it returns, and an exception it fails with before then
// is thrown, as in C++. Dropping the promise cancels the request.
kj::Promise<WorkerInterface::ScheduledResult> runScheduled(
    kj::Own<Invocation> invocation, kj::Date scheduledTime, kj::StringPtr cron);

}  // namespace workerd::entrypoint
