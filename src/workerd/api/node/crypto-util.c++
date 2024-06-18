// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

#include "crypto-util.h"
#include <openssl/rand.h>
#include <openssl/err.h>


namespace workerd::api::node::CryptoUtil {
MUST_USE_RESULT CSPRNGResult CSPRNG(void* buffer, size_t length) {
  do {
    if (1 == RAND_status())
      if (1 == RAND_bytes(static_cast<unsigned char*>(buffer), length))
        return {true};
#if OPENSSL_VERSION_MAJOR >= 3
    const auto code = ERR_peek_last_error();
    // A misconfigured OpenSSL 3 installation may report 1 from RAND_poll()
    // and RAND_status() but fail in RAND_bytes() if it cannot look up
    // a matching algorithm for the CSPRNG.
    if (ERR_GET_LIB(code) == ERR_LIB_RAND) {
      const auto reason = ERR_GET_REASON(code);
      if (reason == RAND_R_ERROR_INSTANTIATING_DRBG ||
          reason == RAND_R_UNABLE_TO_FETCH_DRBG ||
          reason == RAND_R_UNABLE_TO_CREATE_DRBG) {
        return {false};
      }
    }
#endif
  } while (1 == RAND_poll());

  return {false};
}
}  // namespace workerd::api::node::CryptoUtil
