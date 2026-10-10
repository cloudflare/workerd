// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "bench.h"

#include <capnp/message.h>
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

}  // namespace
}  // namespace workerd::api
