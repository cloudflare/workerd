// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "ffi.h"

#include "bridge.h"

#include <workerd/rust/worker/ffi.rs.h>

#include <kj-rs/date.h>

#include <capnp/serialize.h>

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

namespace {

kj::ArrayPtr<const capnp::word> asWords(kj::ArrayPtr<const uint64_t> words) {
  return kj::arrayPtr(reinterpret_cast<const capnp::word*>(words.begin()), words.size());
}

kj::ArrayPtr<const uint64_t> asU64s(kj::ArrayPtr<const capnp::word> words) {
  return kj::arrayPtr(reinterpret_cast<const uint64_t*>(words.begin()), words.size());
}

// A report holds every sample of every case, so it can exceed the default traversal limit.
constexpr capnp::ReaderOptions BENCH_GROUP_READER_OPTIONS = {.traversalLimitInWords = kj::maxValue};

}  // namespace

kj::Promise<::rust::Vec<uint64_t>> worker_bench(
    WorkerInterface& worker, ::rust::Slice<const uint64_t> params) {
  capnp::FlatArrayMessageReader paramsMessage(asWords(kj::from<kj_rs::Rust>(params)));
  capnp::MallocMessageBuilder groupMessage;
  co_await worker.bench(paramsMessage.getRoot<bench::BenchParams>(),
      groupMessage.initRoot<bench::BenchReport::Group>());
  auto words = capnp::messageToFlatArray(groupMessage);
  co_return asU64s(words).as<kj_rs::RustCopy>();
}

kj::Promise<void> RustWorkerInterface::bench(
    bench::BenchParams::Reader params, bench::BenchReport::Group::Builder report) {
  capnp::MallocMessageBuilder paramsMessage;
  paramsMessage.setRoot(params);
  auto paramsWords = capnp::messageToFlatArray(paramsMessage);
  auto groupWords = co_await impl->bench(asU64s(paramsWords).as<kj_rs::Rust>());
  capnp::FlatArrayMessageReader groupMessage(
      asWords(kj::from<kj_rs::Rust>(groupWords)), BENCH_GROUP_READER_OPTIONS);
  // The group's name and startup flag belong to the caller.
  auto group = groupMessage.getRoot<bench::BenchReport::Group>();
  if (group.hasError()) {
    report.setError(group.getError());
  }
  if (group.hasOverhead()) {
    report.setOverhead(group.getOverhead());
  }
  if (group.hasCases()) {
    report.setCases(group.getCases());
  }
}

kj::Promise<CustomEventResult> custom_event_not_supported(kj::Own<CustomEvent> event) {
  // `event` is a coroutine parameter, so it stays alive until the returned promise settles.
  auto result = co_await event->notSupported();
  co_return CustomEventResult{
    .outcome = toRustOutcome(result.outcome),
  };
}

void custom_event_failed(kj::Own<CustomEvent> event, kj::Exception exception) {
  event->failed(exception);
}

kj::Own<WorkerInterface> wrapper_into_kj(::rust::Box<Wrapper> wrapper) {
  return kj::from<kj_rs::Rust>(kj::mv(wrapper));
}

}  // namespace workerd::rust::worker
