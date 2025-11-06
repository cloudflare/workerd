// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "entropy.h"

#include <ncrypto.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <kj/debug.h>
#include <kj/exception.h>

#ifdef __unix__
#include <unistd.h>
#endif

namespace workerd {

void getEntropy(kj::ArrayPtr<kj::byte> output) {
  static constexpr size_t BUFFER_SIZE = 4096;
  struct BufferState {
    kj::FixedArray<kj::byte, BUFFER_SIZE> store;
    kj::ArrayPtr<kj::byte> data;  // Starts empty to trigger initial fill
#if defined(KJ_DEBUG) && defined(__unix__)
        // Track the PID separately to detect cross-fork usage.
    // This should be preserved across fork so we can detect PID changes.
    pid_t lastSeenPid = 0;
#endif
  };

  thread_local BufferState state{};

#if defined(KJ_DEBUG) && defined(__unix__)
  // Verify that getpid() hasn't changed. This code should be called strictly post-fork.
  // If we see crashes here in tests, it means there's some pre-fork call to getEntropy()
  // that needs to be removed.
  pid_t currentPid = getpid();
  if (state.lastSeenPid == 0) {
    state.lastSeenPid = currentPid;
  } else {
    KJ_ASSERT(state.lastSeenPid == currentPid,
        "PID changed from previous call to getEntropy() - this indicates a pre-fork call "
        "to getEntropy() that should be removed",
        state.lastSeenPid, currentPid);
  }
#endif

  while (output != nullptr) {
    if (state.data == nullptr) {
      ncrypto::ClearErrorOnReturn clearErrorOnReturn;
      if (RAND_bytes(state.store.begin(), BUFFER_SIZE) != 1) {
        KJ_FAIL_REQUIRE("RAND_bytes failed to generate random data");
      }

      state.data = state.store.asPtr();
    }

    size_t toCopy = kj::min(state.data.size(), output.size());
    output.first(toCopy).copyFrom(state.data.first(toCopy));
    // Zero out the source buffer after copying to prevent sensitive data from remaining in memory
    OPENSSL_cleanse(state.data.first(toCopy).begin(), toCopy);
    state.data = state.data.slice(toCopy);
    output = output.slice(toCopy);
  }
}

}  // namespace workerd
