#include <benchmark/benchmark.h>

#ifdef __GLIBC__
#include "malloc-memory-manager.h"
#endif

// Main function for benchmark.
// Skeleton implementation comes from BENCHMARK_MAIN in benchmark.h
int main(int argc, char** argv) {
  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv))
    return 1;

#ifdef __GLIBC__
  workerd::MallocMemoryManager memoryManager;
  benchmark::RegisterMemoryManager(&memoryManager);
#endif

  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
