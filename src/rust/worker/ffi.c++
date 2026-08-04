// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "ffi.h"

#include <workerd/rust/worker/ffi.rs.h>

#include <kj-rs/date.h>

// Out-of-line reverse-direction shims. These return the cxx shared result structs, which are only
// complete in the generated bridge header (ffi.rs.h) -- and that header includes ffi.h, so ffi.h
// itself cannot include it. See kj/ffi.c++ for the same pattern with HttpConnectSettings.

namespace workerd::rust::worker {

// C++ EventOutcome -> the bridge's shared EventOutcome enum (reverse of bridge.h's fromImpl).
static EventOutcome toRustOutcome(workerd::EventOutcome outcome) {
  switch (outcome) {
    case workerd::EventOutcome::UNKNOWN:
      return EventOutcome::Unknown;
    case workerd::EventOutcome::OK:
      return EventOutcome::Ok;
    case workerd::EventOutcome::EXCEPTION:
      return EventOutcome::Exception;
    case workerd::EventOutcome::EXCEEDED_CPU:
      return EventOutcome::ExceededCpu;
    case workerd::EventOutcome::KILL_SWITCH:
      return EventOutcome::KillSwitch;
    case workerd::EventOutcome::DAEMON_DOWN:
      return EventOutcome::DaemonDown;
    case workerd::EventOutcome::SCRIPT_NOT_FOUND:
      return EventOutcome::ScriptNotFound;
    case workerd::EventOutcome::CANCELED:
      return EventOutcome::Canceled;
    case workerd::EventOutcome::EXCEEDED_MEMORY:
      return EventOutcome::ExceededMemory;
    case workerd::EventOutcome::LOAD_SHED:
      return EventOutcome::LoadShed;
    case workerd::EventOutcome::RESPONSE_STREAM_DISCONNECTED:
      return EventOutcome::ResponseStreamDisconnected;
    case workerd::EventOutcome::INTERNAL_ERROR:
      return EventOutcome::InternalError;
    case workerd::EventOutcome::EXCEEDED_WALL_TIME:
      return EventOutcome::ExceededWallTime;
  }
  KJ_UNREACHABLE;
}

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

}  // namespace workerd::rust::worker
