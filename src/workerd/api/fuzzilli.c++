#if defined(__linux__) && defined(WORKERD_FUZZILLI)
#include "fuzzilli.h"

#include <errno.h>
#include <string.h>

#include <kj/common.h>
#include <kj/debug.h>
#include <kj/exception.h>

// NOLINTBEGIN(edgeworker-mutable-globals)
struct shmem_data* __shmem;
uint32_t* __edges_start;
uint32_t* __edges_stop;
// NOLINTEND(edgeworker-mutable-globals)

void perform_wild_write() {
  // Access an invalid address.
  // We want to use an "interesting" address for the access (instead of
  // e.g. nullptr). In the (unlikely) case that the address is actually
  // mapped, simply increment the pointer until it crashes.
  // The cast ensures that this works correctly on both 32-bit and 64-bit.
  uintptr_t addr = static_cast<uintptr_t>(0x414141414141ull);
  char* ptr = reinterpret_cast<char*>(addr);
  for (int i = 0; i < 1024; i++) {
    *ptr = 'A';
    ptr += 1 * 1024 * 1024;
  }
}

void __sanitizer_cov_reset_edgeguards() {
  uint64_t N = 0;
  for (uint32_t* x = __edges_start; x < __edges_stop && N < MAX_EDGES; x++) *x = ++N;
}

extern "C" void __sanitizer_cov_trace_pc_guard_init(uint32_t* start, uint32_t* stop) {
  // Avoid duplicate initialization
  if (start == stop || *start) return;

  if (__edges_start != NULL || __edges_stop != NULL) {
    KJ_LOG(ERROR, "Coverage instrumentation is only supported for a single module\n");
    _exit(-1);
  }

  __edges_start = start;
  __edges_stop = stop;

  // Map the shared memory region
  const char* shm_key = getenv("SHM_ID");
  if (!shm_key) {
    KJ_LOG(INFO, "[COV] no shared memory bitmap available, skipping");
    __shmem = (struct shmem_data*)malloc(SHM_SIZE);
  } else {
    int fd = shm_open(shm_key, O_RDWR, S_IREAD | S_IWRITE);
    if (fd <= -1) {
      KJ_LOG(ERROR, "Failed to open shared memory region: %s\n", strerror(errno));
      _exit(-1);
    }

    __shmem = (struct shmem_data*)mmap(0, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (__shmem == MAP_FAILED) {
      KJ_LOG(ERROR, "Failed to mmap shared memory region\n");
      _exit(-1);
    }
  }

  __sanitizer_cov_reset_edgeguards();
  __shmem->num_edges = stop - start;
}

extern "C" void __sanitizer_cov_trace_pc_guard(uint32_t* guard) {
  // There's a small race condition here: if this function executes in two threads for the same
  // edge at the same time, the first thread might disable the edge (by setting the guard to zero)
  // before the second thread fetches the guard value (and thus the index). However, our
  // instrumentation ignores the first edge (see libcoverage.c) and so the race is unproblematic.
  uint32_t index = *guard;
  // If this function is called before coverage instrumentation is properly initialized we want to return early.
  if (!index) return;
  __shmem->edges[index / 8] |= 1 << (index % 8);
  *guard = 0;
}
#endif
