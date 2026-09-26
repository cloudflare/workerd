// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "ffi.h"

#include "bridge.h"

#include <workerd/rust/worker/ffi.rs.h>

#include <kj-rs/date.h>

// Out-of-line reverse-direction shims. These return the cxx shared result structs, which are only
// complete in the generated bridge header (ffi.rs.h) -- and that header includes ffi.h, so ffi.h
// itself cannot include it. See kj/ffi.c++ for the same pattern with HttpConnectSettings.

namespace workerd::rust::worker {

kj::Promise<ScheduledResult> worker_run_scheduled(
    WorkerInterface& worker, int64_t scheduledTimeNanos, ::rust::Slice<const kj::byte> cron) {
  auto cronStr = kj::str(kj::from<kj_rs::Rust>(cron).asChars());
  auto result = co_await worker.runScheduled(kj_rs::repr::fromNanos(scheduledTimeNanos), cronStr);
  co_return ScheduledResult{
    .retry = result.retry,
    .outcome = toRustOutcome(result.outcome),
  };
}

kj::Promise<AlarmResult> worker_run_alarm(
    WorkerInterface& worker, int64_t scheduledTimeNanos, uint32_t retryCount) {
  auto result = co_await worker.runAlarm(kj_rs::repr::fromNanos(scheduledTimeNanos), retryCount);
  kj::Maybe<::rust::String> errorDescription;
  KJ_IF_SOME(desc, result.errorDescription) {
    errorDescription = ::rust::String(desc.begin(), desc.size());
  }
  co_return AlarmResult{
    .retry = result.retry,
    .retry_counts_against_limit = result.retryCountsAgainstLimit,
    .outcome = toRustOutcome(result.outcome),
    .error_description = kj::mv(errorDescription),
  };
}

kj::Promise<CustomEventResult> worker_custom_event(
    WorkerInterface& worker, kj::Own<CustomEvent> event) {
  auto result = co_await worker.customEvent(kj::mv(event));
  co_return CustomEventResult{
    .outcome = toRustOutcome(result.outcome),
  };
}

kj::Promise<CustomEventResult> custom_event_not_supported(kj::Own<CustomEvent> event) {
  // `event` is a coroutine parameter, so it lives until the promise it returns has settled.
  auto result = co_await event->notSupported();
  co_return CustomEventResult{
    .outcome = toRustOutcome(result.outcome),
  };
}

void custom_event_failed(kj::Own<CustomEvent> event, ::rust::Box<Error> error) {
  // Raising the error across the bridge is the bridge's own KjError -> kj::Exception conversion,
  // so the exception the event sees keeps the error's type, description, location and details.
  event->failed(KJ_ASSERT_NONNULL(kj::runCatchingExceptions([&]() { error->raise(); })));
}

kj::Own<WorkerInterface> wrapper_into_kj(::rust::Box<Wrapper> wrapper) {
  return kj::from<kj_rs::Rust>(kj::mv(wrapper));
}

}  // namespace workerd::rust::worker
