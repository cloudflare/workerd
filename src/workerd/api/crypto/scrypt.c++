// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "impl.h"
#include "kdf.h"

#include <ncrypto.h>

namespace workerd::api {

kj::Maybe<jsg::JsArrayBuffer> scrypt(jsg::Lock& js,
    size_t length,
    uint32_t N,
    uint32_t r,
    uint32_t p,
    uint32_t maxmem,
    kj::ArrayPtr<const kj::byte> pass,
    kj::ArrayPtr<const kj::byte> salt) {
  ncrypto::ClearErrorOnReturn clearErrorOnReturn;
  auto buf = jsg::JsArrayBuffer::create(js, length);
  auto ncBuf = ToNcryptoBuffer(buf.asArrayPtr());
  if (ncrypto::scryptInto(ToNcryptoBuffer(pass.asChars()), ToNcryptoBuffer(salt), N, r, p, maxmem,
          length, &ncBuf)) {
    return kj::mv(buf);
  }
  return kj::none;
}

}  // namespace workerd::api
