#include <benchmark/benchmark.h>
#include <kj/debug.h>

#include "tcmalloc/malloc_extension.h"

namespace {

class TCMallocMemoryManager: public benchmark::MemoryManager {
public:
  void Start() override {
    allocCount = 0;
    allocSize = 0;
    tcmalloc::MallocExtension::SetUserHooks(&beforeAlloc, &beforeFree);
  }

  void Stop(benchmark::MemoryManager::Result& result) override {
    tcmalloc::MallocExtension::SetUserHooks(nullptr, nullptr);
    result.total_allocated_bytes = allocSize;
    result.num_allocs = allocCount;
  }

  static void beforeAlloc(size_t size) {
    // do not do any allocations here
    allocCount++;
    allocSize += size;
  }

  static void beforeFree() { }

  static std::atomic<size_t> allocCount;
  static std::atomic<size_t> allocSize;
};

std::atomic<size_t> TCMallocMemoryManager::allocCount = 0;
std::atomic<size_t> TCMallocMemoryManager::allocSize = 0;

} // namespace

// Main function for benchmark.
// Skeleton implementation comes from BENCHMARK_MAIN in benchmark.h
int main(int argc, char** argv) {
  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv))
    return 1;

  TCMallocMemoryManager memoryManager;
  benchmark::RegisterMemoryManager(&memoryManager);

  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
