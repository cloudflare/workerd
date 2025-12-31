// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// Used to provide support tools for benchmarking. Many use cases will already be covered by the
// microbenchmark API.

#include <benchmark/benchmark.h>

#include <kj/test.h>

// Configure tcmalloc for deterministic benchmarks on Linux.
// tcmalloc uses probabilistic heap sampling which can introduce variance in benchmark results.
// WD_USE_TCMALLOC is defined when tcmalloc is enabled (Linux + use_tcmalloc flag).
#ifdef WD_USE_TCMALLOC
#include "tcmalloc/malloc_extension.h"

namespace workerd::bench {

struct TcmallocBenchmarkConfig {
  TcmallocBenchmarkConfig() {
    // Disable heap profiling sampling by setting interval to max value.
    // Default is ~512KB which causes probabilistic sampling of allocations.
    tcmalloc::MallocExtension::SetProfileSamplingInterval(std::numeric_limits<int64_t>::max());

    // Disable GWP-ASan guarded sampling. A negative value disables it.
    tcmalloc::MallocExtension::SetGuardedSamplingInterval(-1);

    // Disable background memory release actions that can cause timing variance.
    tcmalloc::MallocExtension::SetBackgroundProcessActionsEnabled(false);
  }
};

// Global instance ensures configuration runs before main().
inline TcmallocBenchmarkConfig tcmallocBenchmarkConfig;

}  // namespace workerd::bench
#endif  // defined(WD_USE_TCMALLOC)

// Define a benchmark. Use microseconds instead of nanoseconds by default, most tests run long
// enough to not need ns precision.
#define WD_BENCHMARK(X) BENCHMARK(X)->Unit(benchmark::kMicrosecond)

// Macro inspired by KJ_TEST() to enable benchmarking without requiring the benchmark::State
// argument and make it easy to convert tests to benchmarks. Make sure the linker fails if tests are
// not in anonymous namespaces.
#define WD_BENCH(description)                                                                      \
  extern int KJ_CONCAT(YouMustWrapTestsInAnonymousNamespace, __COUNTER__) KJ_UNUSED;               \
  void KJ_UNIQUE_NAME(Bench)();                                                                    \
  void KJ_UNIQUE_NAME(BenchImpl)(benchmark::State & state) {                                       \
    for (auto _: state) {                                                                          \
      KJ_UNIQUE_NAME(Bench)();                                                                     \
    }                                                                                              \
  }                                                                                                \
  WD_BENCHMARK(KJ_UNIQUE_NAME(BenchImpl))->Name(description);                                      \
  void KJ_UNIQUE_NAME(Bench)()
