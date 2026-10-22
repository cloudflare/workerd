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
    // Disable heap profiling sampling.
    // Default is ~512KB which causes probabilistic sampling of allocations.
    tcmalloc::MallocExtension::SetProfileSamplingInterval(0);

    // Disable GWP-ASan guarded sampling. A negative value disables it.
    tcmalloc::MallocExtension::SetGuardedSamplingInterval(-1);

    // Disable background memory release actions that can cause timing variance.
    tcmalloc::MallocExtension::SetBackgroundProcessActionsEnabled(false);
  }
};

// Global instance ensures configuration runs before main().
inline TcmallocBenchmarkConfig tcmallocBenchmarkConfig;

inline void warmUpAllocationSampling() {
  // Each thread keeps its sampling countdown until it reaches the next sample. Advance it with
  // probes until this thread has picked up the new interval, before measuring the callback.
  constexpr size_t PROBE_SIZE = (1 << 20) + 17;
  for (uint i = 0; i < 256; ++i) {
    auto token = tcmalloc::MallocExtension::StartAllocationProfiling();
    auto probe = kj::heapArray<kj::byte>(PROBE_SIZE);
    benchmark::DoNotOptimize(probe.begin());
    auto profile = kj::mv(token).Stop();
    bool sampled = false;
    profile.Iterate([&](const tcmalloc::Profile::Sample& sample) {
      if (sample.requested_size == PROBE_SIZE) sampled = true;
    });
    if (sampled) return;
  }
  KJ_FAIL_REQUIRE("Allocation profiling did not sample the warm-up probes");
}

}  // namespace workerd::bench
#endif  // defined(WD_USE_TCMALLOC)

namespace workerd::bench {

// Collect allocation samples separately so profiling does not affect the timing measurements.
template <typename Callback>
void recordAllocationStats(benchmark::State& state, Callback&& callback) {
#ifdef WD_USE_TCMALLOC
  auto samplingInterval = tcmalloc::MallocExtension::GetProfileSamplingInterval();
  tcmalloc::MallocExtension::SetProfileSamplingInterval(1);
  KJ_DEFER(tcmalloc::MallocExtension::SetProfileSamplingInterval(samplingInterval));
  warmUpAllocationSampling();
  callback();
  auto token = tcmalloc::MallocExtension::StartAllocationProfiling();
  constexpr uint SAMPLES = 128;
  for (uint i = 0; i < SAMPLES; ++i) {
    callback();
  }
  auto profile = kj::mv(token).Stop();
  int64_t bytes = 0;
  int64_t allocations = 0;
  profile.Iterate([&](const tcmalloc::Profile::Sample& sample) {
    bytes += sample.sum;
    allocations += sample.count;
  });
  state.counters["native_bytes/op"] = static_cast<double>(bytes) / SAMPLES;
  state.counters["native_allocs/op"] = static_cast<double>(allocations) / SAMPLES;
#else
  (void)state;
  (void)callback;
#endif
}

}  // namespace workerd::bench

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
