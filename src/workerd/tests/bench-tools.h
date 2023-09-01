// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// Used to provide support tools for benchmarking. Many use cases will already be covered by the
// microbenchmark API.

#include <kj/test.h>
#include <benchmark/benchmark.h>

// Define a benchmark. Use microseconds instead of nanoseconds by default, most tests run long
// enough to not need ns precision.
#define WD_BENCHMARK(X) BENCHMARK(X)->Unit(benchmark::kMicrosecond)

/* Macro inspired by KJ_TEST() to enable benchmarking without requiring the benchmark::State */ \
/* argument and make it easy to convert tests to benchmarks. */ \
/* Make sure the linker fails if tests are not in anonymous namespaces. */ \
#define WD_BENCH(description) \
  extern int KJ_CONCAT(YouMustWrapTestsInAnonymousNamespace, __COUNTER__) KJ_UNUSED; \
  void KJ_UNIQUE_NAME(Bench)(); \
  void KJ_UNIQUE_NAME(BenchImpl)(benchmark::State& state) { \
    for (auto _ : state) { \
      KJ_UNIQUE_NAME(Bench)(); \
    } \
  } \
  WD_BENCHMARK(KJ_UNIQUE_NAME(BenchImpl))->Name(description); \
  void KJ_UNIQUE_NAME(Bench)()
