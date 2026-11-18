// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The main() of wd_cc_benchmark() executables: Google Benchmark's, except that under `bazel test`
// it runs each benchmark once, to check that it works, rather than measuring it.

#include <benchmark/benchmark.h>

#include <kj/vector.h>

#include <cstdlib>

namespace {

// `bazel test` sets TEST_SRCDIR. So does `bazel run` of a test, which also sets
// BUILD_WORKSPACE_DIRECTORY.
bool underBazelTest() {
  return std::getenv("TEST_SRCDIR") != nullptr &&
      std::getenv("BUILD_WORKSPACE_DIRECTORY") == nullptr;
}

}  // namespace

int main(int argc, char** argv) {
  char dryRun[] = "--benchmark_dry_run=true";
  kj::Vector<char*> args(argc + 2);
  args.add(argv[0]);
  // Before the command line's flags, so that they override it.
  if (underBazelTest()) args.add(dryRun);
  args.addAll(argv + 1, argv + argc);
  int count = args.size();
  args.add(nullptr);

  benchmark::Initialize(&count, args.begin());
  if (benchmark::ReportUnrecognizedArguments(count, args.begin())) return 1;
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
