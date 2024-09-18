// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "impl.h"

#include <workerd/api/crypto/kdf.h>

#include <openssl/evp.h>

namespace workerd::api {

kj::Maybe<kj::Array<kj::byte>> scrypt(size_t length,
    uint32_t N,
    uint32_t r,
    uint32_t p,
    uint32_t maxmem,
    kj::ArrayPtr<const kj::byte> pass,
    kj::ArrayPtr<const kj::byte> salt) {
  ClearErrorOnReturn clearErrorOnReturn;
  auto buf = kj::heapArray<kj::byte>(length);
  if (!EVP_PBE_scrypt(pass.asChars().begin(), pass.size(), salt.begin(), salt.size(), N, r, p,
          maxmem, buf.begin(), length)) {
    // This does not currently handle the errors in exactly the same way as
    // the Node.js implementation but that's probably ok? We can update the
    // error thrown to match Node.js more closely later if necessary. There
    // are lots of places in the API currently where the errors do not match.
    if (clearErrorOnReturn.peekError()) {
      throwOpensslError(__FILE__, __LINE__, "crypto::scrypt");
    }
    return kj::none;
  }
  return kj::mv(buf);
}

}  // namespace workerd::api
