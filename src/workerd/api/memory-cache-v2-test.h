#pragma once

#include "memory-cache.h"

namespace workerd::api {

struct MemoryCacheV2TestStats {
  size_t bindings;
  size_t inFlightFallbacks;
  size_t waiters;
  size_t canceledWaiters;
};

MemoryCacheV2TestStats getMemoryCacheV2StatsForTest(const MemoryCacheUse& use);
bool isMemoryCacheV2ForTest(const MemoryCacheProvider& provider);

}  // namespace workerd::api
