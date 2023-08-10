// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// Used to provide support tools for benchmarking. Many use cases will already be covered by the
// microbenchmark API.

#include <benchmark/benchmark.h>

// Define a benchmark. Use microseconds instead of nanoseconds by default, most tests run long
// enough to not need ns precision.
#define WD_BENCHMARK(X) BENCHMARK(X)->Unit(benchmark::kMicrosecond)

#if WD_IS_BENCHMARK
#define WD_BENCHMARK_TESTS 1
#endif

#define WD_BENCH(description) \
  /* Inspired by KJ_TEST() */ \
  /* Make sure the linker fails if tests are not in anonymous namespaces. */ \
  extern int KJ_CONCAT(YouMustWrapTestsInAnonymousNamespace, __COUNTER__) KJ_UNUSED; \
  void KJ_UNIQUE_NAME(Bench)(); \
  void KJ_UNIQUE_NAME(BenchImpl)(benchmark::State& state) { \
    for (auto _ : state) { \
      KJ_UNIQUE_NAME(Bench)(); \
    } \
  } \
  WD_BENCHMARK(KJ_UNIQUE_NAME(BenchImpl))->Name(description); \
  void KJ_UNIQUE_NAME(Bench)()

// This defines a macro that can turn into either KJ_TEST() or into benchmarking the provided
// function based on a command line define. This way we can easily benchmark many existing workerd
// tests.
// TODO: Rename this to KJ_TEST_WRAPPER() to make it less intrusive
#if WD_BENCHMARK_TESTS
#define WD_TEST_OR_BENCH WD_BENCH
#else
#define WD_TEST_OR_BENCH KJ_TEST
#endif

namespace workerd::microbench {

}  // namespace workerd::api::microbench
