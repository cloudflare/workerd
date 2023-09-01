// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// Benchmarking memory manager. Uses glibc malloc hooks to track allocation statistics.
// https://www.gnu.org/savannah-checkouts/gnu/libc/manual/html_node/Hooks-for-Malloc.html

#include <benchmark/benchmark.h>

namespace workerd {

class MallocMemoryManager: public benchmark::MemoryManager {
public:
  void Start() override;
  void Stop(benchmark::MemoryManager::Result& result) override;

private:
  static std::atomic<size_t> allocCount;
  static std::atomic<size_t> allocSize;

  static void *(*oldMallocHook)(size_t __size, const void *);
  static void * mallocHook(size_t size, const void *caller);
};

}  // namespace workerd
