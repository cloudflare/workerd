// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "entropy.h"

#include <openssl/err.h>
#include <openssl/rand.h>

#include <kj/debug.h>
#include <kj/exception.h>

namespace workerd {

void getEntropy(kj::ArrayPtr<kj::byte> output) {
  static constexpr size_t BUFFER_SIZE = 4096;
  struct BufferState {
    kj::FixedArray<kj::byte, BUFFER_SIZE> store;
    kj::ArrayPtr<kj::byte> data;  // Starts empty to trigger initial fill
  };

  thread_local BufferState state{};

  while (output != nullptr) {
    if (state.data == nullptr) {
      if (RAND_bytes(state.store.begin(), BUFFER_SIZE) != 1) {
        ERR_clear_error();
        ERR_clear_system_error();
        KJ_FAIL_REQUIRE("RAND_bytes failed to generate random data");
      }
      state.data = state.store.asPtr();
    }

    size_t toCopy = kj::min(state.data.size(), output.size());
    output.first(toCopy).copyFrom(state.data.first(toCopy));
    state.data = state.data.slice(toCopy);
    output = output.slice(toCopy);
  }
}

}  // namespace workerd
