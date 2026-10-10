// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "bench-report.h"

#include <capnp/message.h>
#include <kj/test.h>

namespace workerd::server {
namespace {

using Case = bench::BenchReport::Case;

KJ_TEST("formatBenchDuration uses three significant figures and a fitting unit") {
  KJ_EXPECT(formatBenchDuration(0) == "0.00 ns");
  KJ_EXPECT(formatBenchDuration(1.234) == "1.23 ns");
  KJ_EXPECT(formatBenchDuration(12.34) == "12.3 ns");
  KJ_EXPECT(formatBenchDuration(123.4) == "123 ns");
  KJ_EXPECT(formatBenchDuration(1234) == "1.23 us");
  KJ_EXPECT(formatBenchDuration(1.5e6) == "1.50 ms");
  KJ_EXPECT(formatBenchDuration(2.5e9) == "2.50 s");
  KJ_EXPECT(formatBenchDuration(7200e9) == "7200 s");
}

KJ_TEST("formatBenchReport renders environment, cases, and failures") {
  capnp::MallocMessageBuilder message;
  auto report = message.initRoot<bench::BenchReport>();
  auto environment = report.initEnvironment();
  environment.setWorkerdVersion("2026-10-08");
  environment.setBuildMode("debug");
  environment.setV8Version("14.0.0");
  environment.setCompatDate("2026-01-01");
  environment.initWarnings(1).set(0, "This is a debug build.");

  auto groups = report.initGroups(2);
  auto group = groups[0];
  group.setName("main:encoding");
  auto cases = group.initCases(4);

  cases[0].setName("encode small");
  cases[0].setIterationsPerSample(1024);
  auto wall = cases[0].initWallNs();
  wall.initSamples(3);
  wall.setMedian(12.34);
  wall.setMedianLow(12.2);
  wall.setMedianHigh(12.5);
  wall.setMean(12.4);
  cases[0].initCpuNs().setMedian(12.3);

  cases[1].setName("noisy");
  cases[1].setIterationsPerSample(1);
  cases[1].initWallNs().setMedian(2e6);
  cases[1].initCpuNs();
  auto flags = cases[1].initFlags(2);
  flags.set(0, Case::Flag::HIGH_VARIANCE);
  flags.set(1, Case::Flag::AT_FLOOR);

  auto overhead = group.initOverhead();
  overhead.setSyncNs(5.5);
  overhead.setAsyncNs(250);
  overhead.setBlackBoxNs(2);

  cases[2].setName("broken");
  cases[2].setStatus(Case::Status::FAILED);
  cases[2].setError("Error: boom\n    at stack");

  cases[3].setName("later");
  cases[3].setStatus(Case::Status::SKIPPED);

  groups[1].setName("main:empty");
  groups[1].setError("bench() did not register any cases.");

  auto text = formatBenchReport(report);
  KJ_EXPECT(text ==
          "Results compare runs on this machine and build; they don't predict production "
          "latency.\n"
          "workerd 2026-10-08, debug build, V8 14.0.0, compat date 2026-01-01\n"
          "WARNING: This is a debug build.\n"
          "\n"
          "main:encoding\n"
          "  overhead per call, subtracted: 5.50 ns, 250 ns async; blackBox() 2.00 ns\n"
          "  case          median   95% CI             mean     cpu median  samples\n"
          "  encode small  12.3 ns  12.2 ns - 12.5 ns  12.4 ns  12.3 ns     3 x 1024\n"
          "  noisy         2.00 ms  0.00 ns - 0.00 ns  0.00 ns  0.00 ns     0 x 1     "
          "(high variance: try more samples or a longer minTime; at measurement floor: pass "
          "results through b.blackBox())\n"
          "  broken        FAILED: Error: boom\n"
          "  later         skipped\n"
          "\n"
          "main:empty\n"
          "  FAILED: bench() did not register any cases.\n",
      text);
}

KJ_TEST("formatBenchReport shows the default compat date") {
  capnp::MallocMessageBuilder message;
  auto report = message.initRoot<bench::BenchReport>();
  auto environment = report.initEnvironment();
  environment.setBuildMode("release");
  environment.setV8Version("14.0.0");
  environment.setDefaultCompatDate("2026-10-08");

  auto text = formatBenchReport(report);
  KJ_EXPECT(text ==
          "Results compare runs on this machine and build; they don't predict production "
          "latency.\n"
          "release build, V8 14.0.0, default compat date 2026-10-08\n",
      text);
}

}  // namespace
}  // namespace workerd::server
