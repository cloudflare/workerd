// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "ffi.h"

#include <workerd/io/entrypoint/ffi.rs.h>
#include <workerd/io/tracer.h>
#include <workerd/rust/worker/bridge.h>
#include <workerd/util/use-perfetto-categories.h>

namespace workerd::entrypoint {

void invocation_set_scheduled_event_info(
    Invocation& invocation, kj::Date scheduledTime, ::rust::Slice<const kj::byte> cron) {
  KJ_IF_SOME(tracer, invocation.request->getWorkerTracer()) {
    double eventTime = (scheduledTime - kj::UNIX_EPOCH) / kj::MILLISECONDS;
    tracer.setEventInfo(*invocation.request,
        tracing::ScheduledEventInfo(eventTime, kj::str(kj::from<kj_rs::Rust>(cron).asChars())));
  }
}

void invocation_delivered(Invocation& invocation, ::rust::Slice<const kj::byte> eventType) {
  // A kj::StringPtr needs a NUL right after its text and none within it. Rust passes a CStr's
  // bytes with its NUL (see ffi::delivered()), which meet both, but this doesn't rely on that.
  auto bytes = kj::arrayPtr(eventType.data(), eventType.size());
  KJ_REQUIRE(bytes.size() > 0 && bytes.back() == '\0', "the event type is not NUL-terminated");
  auto text = bytes.first(bytes.size() - 1);
  KJ_REQUIRE(text.findFirst('\0') == kj::none, "the event type contains a NUL");
  invocation.request->delivered(kj::StringPtr(text.asChars().begin(), text.size()));
}

void invocation_drain(kj::Own<Invocation> invocation) {
  KJ_IF_SOME(exception, kj::runCatchingExceptions([&]() {
    invocation->request->drain(invocation->waitUntilTasks, kj::mv(invocation->request));
  })) {
    KJ_LOG(ERROR, "draining an entrypoint invocation failed", exception);
  }
}

void invocation_start_scheduled_in_wait_until(
    Invocation& invocation, kj::Date scheduledTime, ::rust::Slice<const kj::byte> cron) {
  startScheduledInWaitUntil(invocation.request->getContext(), scheduledTime,
      kj::str(kj::from<kj_rs::Rust>(cron).asChars()), kj::mv(invocation.entrypointName),
      kj::mv(invocation.versionInfo), kj::mv(invocation.props));
}

kj::Promise<::workerd::rust::worker::ScheduledResult> invocation_finish_scheduled(
    kj::Own<Invocation> invocation) {
  TRACE_EVENT("workerd", "WorkerEntrypoint::runScheduled() waitForFinished()");
  // kj::evalNow() makes an exception that finishScheduled() throws reject the promise: cxx
  // has no way to carry a synchronous throw from an async function to Rust.
  return kj::evalNow([&]() {
    auto& request = *invocation->request;
    return request.finishScheduled(kj::mv(invocation->request));
  }).then([](WorkerInterface::ScheduledResult result) {
    return ::workerd::rust::worker::ScheduledResult{
      .retry = result.retry,
      .outcome = ::workerd::rust::worker::toRustOutcome(result.outcome),
    };
  });
}

kj::Promise<WorkerInterface::ScheduledResult> runScheduled(
    kj::Own<Invocation> invocation, kj::Date scheduledTime, kj::StringPtr cron) {
  // Only the Rust path has this span, so a trace shows which path delivered the event.
  TRACE_EVENT("workerd", "entrypoint::runScheduled()");
  auto scheduled =
      scheduled_event_start(kj::mv(invocation), scheduledTime, cron.asBytes().as<kj_rs::Rust>());
  return scheduled_event_finish(kj::mv(scheduled))
      .then([](::workerd::rust::worker::ScheduledResult result) {
    return kj::from<kj_rs::Rust>(result);
  });
}

}  // namespace workerd::entrypoint
