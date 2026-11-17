// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "bench.h"

#include <capnp/message.h>
#include <kj/async.h>
#include <kj/test.h>

#include <cmath>

namespace workerd::api {
namespace {

using Flag = bench::BenchReport::Case::Flag;

constexpr BenchOverhead OVERHEAD{
  .syncNs = 10,
  .syncCpuNs = 8,
  .asyncNs = 1000,
  .asyncCpuNs = 900,
  .blackBoxNs = 2,
};

BenchCaseResult makeCase(kj::ArrayPtr<const double> samples, bool async = false) {
  return {
    .name = kj::str("case"),
    .iterationsPerSample = 100,
    .wallNs = kj::heapArray(samples),
    .cpuNs = kj::heapArray(samples),
    .async = async,
  };
}

// Fills a report for one case and returns the case.
bench::BenchReport::Case::Reader fill(capnp::MallocMessageBuilder& message,
    BenchCaseResult c,
    kj::Maybe<BenchOverhead> overhead = OVERHEAD) {
  BenchGroupResult result{.cases = kj::arr(kj::mv(c)), .overhead = overhead};
  auto group = message.initRoot<bench::BenchReport::Group>();
  fillBenchReport(result, group);
  return group.asReader().getCases()[0];
}

// Statistics come from a histogram with three significant figures.
bool near(double actual, double expected) {
  return std::abs(actual - expected) <= expected * 0.001 + 0.001;
}

bool hasFlag(bench::BenchReport::Case::Reader c, Flag flag) {
  for (auto f: c.getFlags()) {
    if (f == flag) return true;
  }
  return false;
}

KJ_TEST("fillBenchReport subtracts the sync overhead from cases that don't return promises") {
  capnp::MallocMessageBuilder message;
  double samples[] = {100, 101, 99, 100, 100};
  auto c = fill(message, makeCase(samples));

  auto wall = c.getWallNs();
  KJ_EXPECT(near(wall.getMedian(), 90), wall.getMedian());
  KJ_EXPECT(near(c.getCpuNs().getMedian(), 92), c.getCpuNs().getMedian());
  KJ_EXPECT(wall.getSamples()[1] == 91);
  KJ_EXPECT(c.getFlags().size() == 0);
  KJ_EXPECT(c.getIterationsPerSample() == 100);

  auto overhead = message.getRoot<bench::BenchReport::Group>().getOverhead();
  KJ_EXPECT(overhead.getSyncNs() == 10);
  KJ_EXPECT(overhead.getAsyncNs() == 1000);
  KJ_EXPECT(overhead.getBlackBoxNs() == 2);
}

KJ_TEST("fillBenchReport subtracts the async overhead from cases that return promises") {
  capnp::MallocMessageBuilder message;
  double samples[] = {3000, 3000, 3000};
  auto c = fill(message, makeCase(samples, /*async=*/true));
  KJ_EXPECT(near(c.getWallNs().getMedian(), 2000), c.getWallNs().getMedian());
  KJ_EXPECT(near(c.getCpuNs().getMedian(), 2100), c.getCpuNs().getMedian());
  KJ_EXPECT(!hasFlag(c, Flag::AT_FLOOR));
}

KJ_TEST("fillBenchReport flags cases at the floor and doesn't go below zero") {
  capnp::MallocMessageBuilder message;
  // A median under twice the overhead, with samples below the overhead and noise that would
  // otherwise count as high variance.
  double samples[] = {5, 11, 19, 15, 12};
  auto c = fill(message, makeCase(samples));
  KJ_EXPECT(hasFlag(c, Flag::AT_FLOOR));
  KJ_EXPECT(!hasFlag(c, Flag::HIGH_VARIANCE));
  KJ_EXPECT(c.getWallNs().getSamples()[0] == 0);
  KJ_EXPECT(c.getWallNs().getMin() == 0);
}

KJ_TEST("fillBenchReport flags high variance") {
  capnp::MallocMessageBuilder message;
  double samples[] = {100, 300, 100, 300};
  auto c = fill(message, makeCase(samples), kj::none);
  KJ_EXPECT(hasFlag(c, Flag::HIGH_VARIANCE));
  KJ_EXPECT(!hasFlag(c, Flag::AT_FLOOR));
  KJ_EXPECT(!message.getRoot<bench::BenchReport::Group>().hasOverhead());
}

KJ_TEST("fillBenchReport fails cases with calls longer than an hour") {
  // The histograms hold samples of up to an hour, 3.6e12 ns.
  {
    capnp::MallocMessageBuilder message;
    double samples[] = {100, 3.6e12, 100};
    auto c = fill(message, makeCase(samples));
    KJ_EXPECT(c.getStatus() == bench::BenchReport::Case::Status::OK);
    KJ_EXPECT(near(c.getWallNs().getMax(), 3.6e12), c.getWallNs().getMax());
  }
  {
    capnp::MallocMessageBuilder message;
    double samples[] = {100, 3.6e12 + 1, 100};
    auto c = fill(message, makeCase(samples));
    KJ_EXPECT(c.getStatus() == bench::BenchReport::Case::Status::FAILED);
    KJ_EXPECT(c.getError() ==
        "A call took longer than an hour, the longest that the statistics can represent.");
    KJ_EXPECT(!c.hasWallNs());
  }
}

KJ_TEST("fillBenchReport writes failures and skips without statistics") {
  capnp::MallocMessageBuilder message;
  BenchGroupResult result{
    .error = kj::str("handler failed"),
    .cases = kj::arr(
        BenchCaseResult{
          .name = kj::str("broken"),
          .status = bench::BenchReport::Case::Status::FAILED,
          .error = kj::str("Error: boom"),
        },
        BenchCaseResult{
          .name = kj::str("later"),
          .status = bench::BenchReport::Case::Status::SKIPPED,
        }),
  };
  auto group = message.initRoot<bench::BenchReport::Group>();
  fillBenchReport(result, group);

  KJ_EXPECT(group.getError() == "handler failed");
  auto cases = group.getCases();
  KJ_ASSERT(cases.size() == 2);
  KJ_EXPECT(cases[0].getError() == "Error: boom");
  KJ_EXPECT(!cases[0].hasWallNs());
  KJ_EXPECT(cases[1].getStatus() == bench::BenchReport::Case::Status::SKIPPED);
  KJ_EXPECT(!cases[1].hasWallNs());
}

// The params of a startup group. A `minTimeNs` of 0 leaves the default sampling budget, which the
// fake starts below never reach.
bench::BenchParams::Reader startupParams(capnp::MallocMessageBuilder& message,
    kj::StringPtr caseFilter,
    uint32_t samples,
    bool quick = false,
    uint64_t minTimeNs = 0) {
  auto params = message.initRoot<bench::BenchParams>();
  params.setCaseFilter(caseFilter);
  params.setQuick(quick);
  auto defaults = params.initDefaults();
  defaults.setSamples(samples);
  defaults.setMinTimeNs(minTimeNs);
  return params.asReader();
}

// A start whose phases take 1000, 2000, 3000 and 10000 times `n` ns of wall time, and half that
// of CPU time, where `n` counts the starts from 1.
kj::Function<kj::Promise<WorkerStartupTiming>()> fakeStart(uint& starts) {
  return [&starts]() {
    auto n = static_cast<int64_t>(++starts);
    auto timing = [n](int64_t ns) { return BenchTiming{.wallNs = ns * n, .cpuNs = ns * n / 2}; };
    return kj::Promise<WorkerStartupTiming>(WorkerStartupTiming{
      .isolate = timing(1000),
      .script = timing(2000),
      .worker = timing(3000),
      .total = timing(10000),
    });
  };
}

KJ_TEST("benchWorkerStartup reports each phase of the starts after a warmup") {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);
  capnp::MallocMessageBuilder paramsMessage;
  capnp::MallocMessageBuilder message;
  auto group = message.initRoot<bench::BenchReport::Group>();
  uint starts = 0;
  benchWorkerStartup(startupParams(paramsMessage, "", 3), fakeStart(starts), group).wait(ws);

  KJ_EXPECT(starts == 4);
  KJ_EXPECT(group.getStartup());
  KJ_EXPECT(!group.hasError());
  KJ_EXPECT(!group.hasOverhead());
  auto cases = group.getCases();
  KJ_ASSERT(cases.size() == 4);
  kj::StringPtr names[] = {"startup:isolate", "startup:script", "startup:worker", "startup:total"};
  double scales[] = {1000, 2000, 3000, 10000};
  for (auto i: kj::indices(names)) {
    auto c = cases[i];
    KJ_EXPECT(c.getName() == names[i]);
    KJ_EXPECT(c.getStatus() == bench::BenchReport::Case::Status::OK);
    KJ_EXPECT(c.getIterationsPerSample() == 1);
    // The first start is the warmup.
    auto wall = c.getWallNs().getSamples();
    auto cpu = c.getCpuNs().getSamples();
    KJ_ASSERT(wall.size() == 3);
    KJ_ASSERT(cpu.size() == 3);
    for (auto j: kj::indices(wall)) {
      KJ_EXPECT(wall[j] == scales[i] * static_cast<double>(j + 2), names[i], j);
      KJ_EXPECT(cpu[j] == scales[i] * static_cast<double>(j + 2) / 2, names[i], j);
    }
    KJ_EXPECT(near(c.getWallNs().getMedian(), scales[i] * 3), names[i]);
  }
}

KJ_TEST("benchWorkerStartup reports only the phases the case filter selects") {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);
  {
    capnp::MallocMessageBuilder paramsMessage;
    capnp::MallocMessageBuilder message;
    auto group = message.initRoot<bench::BenchReport::Group>();
    uint starts = 0;
    benchWorkerStartup(startupParams(paramsMessage, "startup:*r*", 2), fakeStart(starts), group)
        .wait(ws);
    KJ_EXPECT(starts == 3);
    auto cases = group.getCases();
    KJ_ASSERT(cases.size() == 2);
    KJ_EXPECT(cases[0].getName() == "startup:script");
    KJ_EXPECT(cases[1].getName() == "startup:worker");
  }
  // Patterns meant for a handler's cases select no phase, so nothing starts.
  for (auto pattern: {"encode*"_kj, "w*"_kj, "total"_kj}) {
    capnp::MallocMessageBuilder paramsMessage;
    capnp::MallocMessageBuilder message;
    auto group = message.initRoot<bench::BenchReport::Group>();
    uint starts = 0;
    benchWorkerStartup(startupParams(paramsMessage, pattern, 2), fakeStart(starts), group).wait(ws);
    KJ_EXPECT(starts == 0, pattern);
    KJ_EXPECT(group.getStartup());
    KJ_EXPECT(!group.hasError());
    KJ_EXPECT(group.getCases().size() == 0, pattern);
  }
}

KJ_TEST("benchWorkerStartup stops at the sampling budget once it has enough samples") {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);
  // The timed starts' totals are 20us, 30us, 40us, ...: five add up to 200us and seven to 350us.
  struct Expected {
    uint64_t minTimeNs;
    uint samples;
  };
  for (auto expected: {Expected{300'000, 7}, Expected{1, 5}}) {
    capnp::MallocMessageBuilder paramsMessage;
    capnp::MallocMessageBuilder message;
    auto group = message.initRoot<bench::BenchReport::Group>();
    uint starts = 0;
    benchWorkerStartup(startupParams(paramsMessage, "startup:total", 50, false, expected.minTimeNs),
        fakeStart(starts), group)
        .wait(ws);
    KJ_EXPECT(starts == expected.samples + 1, expected.minTimeNs, starts);
    auto cases = group.getCases();
    KJ_ASSERT(cases.size() == 1);
    KJ_EXPECT(cases[0].getWallNs().getSamples().size() == expected.samples, expected.minTimeNs);
  }
}

KJ_TEST("benchWorkerStartup under --quick skips the warmup and caps the samples") {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);
  capnp::MallocMessageBuilder paramsMessage;
  capnp::MallocMessageBuilder message;
  auto group = message.initRoot<bench::BenchReport::Group>();
  uint starts = 0;
  benchWorkerStartup(
      startupParams(paramsMessage, "startup:total", 1000, true), fakeStart(starts), group)
      .wait(ws);
  KJ_EXPECT(starts == 5);
  auto cases = group.getCases();
  KJ_ASSERT(cases.size() == 1);
  auto wall = cases[0].getWallNs().getSamples();
  KJ_ASSERT(wall.size() == 5);
  KJ_EXPECT(wall[0] == 10000);
}

KJ_TEST("benchWorkerStartup fails the group when a start fails") {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);
  capnp::MallocMessageBuilder paramsMessage;
  capnp::MallocMessageBuilder message;
  auto group = message.initRoot<bench::BenchReport::Group>();
  uint starts = 0;
  benchWorkerStartup(startupParams(paramsMessage, "", 3),
      [&starts]() {
    // The warmup succeeds; the first timed start fails.
    if (starts++ == 0) return kj::Promise<WorkerStartupTiming>(WorkerStartupTiming{});
    return kj::Promise<WorkerStartupTiming>(KJ_EXCEPTION(FAILED, "the worker failed to start"));
  }, group)
      .wait(ws);
  KJ_EXPECT(starts == 2);
  KJ_EXPECT(group.getStartup());
  KJ_EXPECT(
      kj::StringPtr(group.getError()).contains("the worker failed to start"), group.getError());
  KJ_EXPECT(group.getCases().size() == 0);
}

}  // namespace
}  // namespace workerd::api
