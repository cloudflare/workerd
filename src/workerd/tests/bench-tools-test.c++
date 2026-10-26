// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "bench-tools.h"

namespace workerd::bench {
namespace {

#ifdef WD_USE_TCMALLOC
KJ_TEST("allocation sampling resumes after being disabled") {
  auto interval = tcmalloc::MallocExtension::GetProfileSamplingInterval();
  KJ_DEFER(tcmalloc::MallocExtension::SetProfileSamplingInterval(interval));

  for (uint i = 0; i < 2; ++i) {
    tcmalloc::MallocExtension::SetProfileSamplingInterval(0);
    auto unsampled = kj::heapArray<kj::byte>(4093);
    benchmark::DoNotOptimize(unsampled.begin());

    tcmalloc::MallocExtension::SetProfileSamplingInterval(1);
    warmUpAllocationSampling();
    auto token = tcmalloc::MallocExtension::StartAllocationProfiling();
    auto first = kj::heapArray<kj::byte>(4093);
    auto second = kj::heapArray<kj::byte>(4093);
    benchmark::DoNotOptimize(first.begin());
    benchmark::DoNotOptimize(second.begin());
    auto profile = kj::mv(token).Stop();
    int64_t allocations = 0;
    profile.Iterate([&](const tcmalloc::Profile::Sample& sample) {
      if (sample.requested_size == 4093) allocations += sample.count;
    });
    KJ_EXPECT(allocations == 2, allocations);
  }
}
#endif

}  // namespace
}  // namespace workerd::bench
