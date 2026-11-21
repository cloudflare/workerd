// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "bench.h"

#include <capnp/message.h>
#include <kj/test.h>

#include <cmath>

namespace workerd::api {
namespace {

// Statistics come from a histogram with three significant figures.
bool near(double actual, double expected) {
  return std::abs(actual - expected) <= expected * 0.001 + 0.001;
}

// fillBenchReport() is a shim over the Rust bench-core crate, whose tests (lib-test.rs) cover the
// statistics. These cover what the shim does: handing the results over and copying the encoded
// group into the report.

// A case of `sampleCount` samples of `sampleNs`, wall and CPU.
BenchCaseResult makeCase(kj::StringPtr name, size_t sampleCount, double sampleNs) {
  BenchCaseResult c{
    .name = kj::str(name),
    .iterationsPerSample = 100,
    .wallNs = kj::heapArray<double>(sampleCount),
    .cpuNs = kj::heapArray<double>(sampleCount),
  };
  for (auto& sample: c.wallNs) sample = sampleNs;
  for (auto& sample: c.cpuNs) sample = sampleNs;
  return c;
}

KJ_TEST("fillBenchReport copies the group that Rust encodes into the report") {
  auto failed = makeCase("fails \xff", 5, 100);
  failed.status = bench::BenchReport::Case::Status::FAILED;
  failed.error = kj::str("");
  BenchGroupResult result{
    .error = kj::str("bench() threw"),
    .cases = kj::arr(makeCase("ok", 5, 100), kj::mv(failed)),
    .overhead =
        BenchOverhead{
          .syncNs = 10, .syncCpuNs = 8, .asyncNs = 1000, .asyncCpuNs = 900, .blackBoxNs = 2},
  };
  capnp::MallocMessageBuilder message;
  auto group = message.initRoot<bench::BenchReport::Group>();
  fillBenchReport(result, group);

  auto report = group.asReader();
  KJ_EXPECT(report.getError() == "bench() threw");
  KJ_EXPECT(report.getOverhead().getSyncNs() == 10);
  KJ_EXPECT(report.getOverhead().getBlackBoxNs() == 2);
  auto cases = report.getCases();
  KJ_ASSERT(cases.size() == 2);
  KJ_EXPECT(cases[0].getName() == "ok");
  KJ_EXPECT(!cases[0].hasError());
  KJ_EXPECT(cases[0].getIterationsPerSample() == 100);
  KJ_EXPECT(cases[0].getWallNs().getSamples().size() == 5);
  KJ_EXPECT(near(cases[0].getWallNs().getMedian(), 90), cases[0].getWallNs().getMedian());
  // Names and errors are copied as bytes, and an empty error is still an error.
  KJ_EXPECT(cases[1].getName() == "fails \xff");
  KJ_EXPECT(cases[1].getStatus() == bench::BenchReport::Case::Status::FAILED);
  KJ_EXPECT(cases[1].hasError());
  KJ_EXPECT(cases[1].getError() == "");
}

KJ_TEST("fillBenchReport writes groups larger than capnp's default traversal limit") {
  // 5 cases of MAX_SAMPLES wall and CPU samples are 10M words, past the default limit of 8M
  // words for reading a message.
  constexpr size_t CASES = 5;
  constexpr size_t SAMPLES = 1'000'000;
  auto cases = kj::heapArrayBuilder<BenchCaseResult>(CASES);
  for (size_t i = 0; i < CASES; i++) {
    cases.add(makeCase(kj::str("case ", i), SAMPLES, 100));
  }
  BenchGroupResult result{.cases = cases.finish()};
  capnp::MallocMessageBuilder message;
  auto group = message.initRoot<bench::BenchReport::Group>();
  fillBenchReport(result, group);

  auto report = group.asReader();
  KJ_ASSERT(report.getCases().size() == CASES);
  for (auto c: report.getCases()) {
    KJ_EXPECT(c.getWallNs().getSamples().size() == SAMPLES);
    KJ_EXPECT(c.getCpuNs().getSamples().size() == SAMPLES);
  }
}

}  // namespace
}  // namespace workerd::api
