#if defined(__linux__) && defined(WORKERD_FUZZILLI)
#include "fuzzilli.h"

#include <workerd/api/util.h>
#include <workerd/jsg/jsg.h>

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

// setup trace pc guard to let fuzzilli get some coverage info
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

void fuzzilli_handler(workerd::jsg::Lock& js, workerd::jsg::Arguments<workerd::jsg::Value>& args) {
  if (args.size() == 0) {
    // No arguments provided, just return
    return;
  }

  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  v8::Local<v8::Value> value = v8::Local<v8::Value>::Cast(args[0].getHandle(isolate));
  v8::Local<v8::String> str = workerd::jsg::check(value->ToDetailString(js.v8Context()));
  v8::String::Utf8Value operation(js.v8Isolate, str);
  if (*operation == nullptr) {
    return;
  }

  if (strcmp(*operation, "FUZZILLI_CRASH") == 0) {
    auto maybeArg =
        v8::Local<v8::Int32>::Cast(args[1].getHandle(isolate))->Int32Value(js.v8Context());
    if (!maybeArg.IsJust()) {
      KJ_LOG(ERROR, "Maybe arg is empty...\n");
      fflush(stdout);
      return;
    }
    int32_t arg = maybeArg.FromJust();
    switch (arg) {
      case 0:
        IMMEDIATE_CRASH();
        break;
      case 1:
        assert(0);
        //CHECK(false);
        break;
      case 2:
        assert(0);
        //DCHECK(false);
        break;
      case 3: {
        perform_wild_write();
        break;
      }
      case 4: {
        // Use-after-free, should be caught by ASan (if active).
        auto* vec = new std::vector<int>(4);
        delete vec;
        USE(vec->at(0));
#ifndef V8_USE_ADDRESS_SANITIZER
        // The testcase must also crash on non-asan builds.
        perform_wild_write();
#endif  // !V8_USE_ADDRESS_SANITIZER
        break;
      }
      case 5: {
        // Out-of-bounds access (1), likely only crashes in ASan or
        // "hardened"/"safe" libc++ builds.
        std::vector<int> vec(5);
        USE(vec[5]);
        break;
      }
      case 6: {
        // Out-of-bounds access (2), likely only crashes in ASan builds.
        std::vector<int> vec(6);
        //linter complains about this...
        // NOLINTNEXTLINE(edgeworker-ban-memset)
        memset(vec.data(), 42, 0x100);
        break;
      }
      default:
        break;
    }
  } else if (strcmp(*operation, "FUZZILLI_PRINT") == 0) {
    static FILE* fzliout = nullptr;
    if (!fzliout) {
      fzliout = fdopen(REPRL_DWFD, "w");
      if (!fzliout) {
        KJ_LOG(ERROR, "Fuzzer output channel not available, printing to stdout instead\n");
        fzliout = stdout;
      }
    }

    value = v8::Local<v8::Value>::Cast(args[1].getHandle(isolate));
    str = workerd::jsg::check(value->ToDetailString(js.v8Context()));
    v8::String::Utf8Value string(js.v8Isolate, str);
    if (*string == nullptr) {
      return;
    }
    fprintf(fzliout, "%s\n", *string);
    fflush(fzliout);
  }
}

#endif
