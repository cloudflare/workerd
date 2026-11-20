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

// CxxEntrypoint::takeInvocation().
kj::Own<Invocation> cxx_entrypoint_take_invocation(CxxEntrypoint& entrypoint);

// Fails if the event's request is for an actor, to which scheduled events are never delivered.
void invocation_assert_no_actor(Invocation& invocation);

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

// Makes a Rust WorkerEntrypoint (see `lib.rs`) that owns `cxx`, and returns its WorkerInterface.
// It delivers the scheduled event from Rust and passes every other event to `cxx`, which then
// does its work as it would on its own.
kj::Own<WorkerInterface> newWorkerEntrypoint(kj::Own<CxxEntrypoint> cxx);

}  // namespace workerd::entrypoint
