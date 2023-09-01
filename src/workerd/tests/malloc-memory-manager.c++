// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "malloc-memory-manager.h"
#include <malloc.h>

namespace workerd {

void* (*MallocMemoryManager::oldMallocHook)(size_t __size, const void*) = nullptr;

std::atomic<size_t> MallocMemoryManager::allocCount = 0;
std::atomic<size_t> MallocMemoryManager::allocSize = 0;

#pragma clang diagnostic push
// For some reason __malloc_hook is marked as deprecated.
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

void MallocMemoryManager::Start() {
  allocCount = 0;
  allocSize = 0;

  oldMallocHook = __malloc_hook;
  __malloc_hook = mallocHook;
}

void MallocMemoryManager::Stop(benchmark::MemoryManager::Result& result) {
  result.total_allocated_bytes = allocSize;
  result.num_allocs = allocCount;
}

void* MallocMemoryManager::mallocHook(size_t size, const void* caller) {
  allocCount++;
  allocSize += size;

  __malloc_hook = oldMallocHook;
  auto result = malloc(size);
  oldMallocHook = __malloc_hook;
  __malloc_hook = mallocHook;

  return result;
}

#pragma clang diagnostic pop

} // namespace workerd
