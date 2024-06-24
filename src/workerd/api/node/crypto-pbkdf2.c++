// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

#include "crypto.h"
#include <openssl/evp.h>
#include <workerd/jsg/jsg.h>
#include <workerd/api/crypto/impl.h>

namespace workerd::api::node {

kj::Array<kj::byte> CryptoImpl::getPbkdf(
    jsg::Lock& js,
    kj::Array<kj::byte> password,
    kj::Array<kj::byte> salt,
    uint32_t num_iterations,
    uint32_t keylen,
    kj::String name) {
  // Should not be needed based on current memory limits, still good to have
  JSG_REQUIRE(password.size() <= INT32_MAX, RangeError, "Pbkdf2 failed: password is too large");
  JSG_REQUIRE(salt.size() <= INT32_MAX, RangeError, "Pbkdf2 failed: salt is too large");
  // Note: The user could DoS us by selecting a very high iteration count. As with the Web Crypto
  // API, intentionally limit the maximum iteration count.
  checkPbkdfLimits(js, num_iterations);

  const EVP_MD* digest = EVP_get_digestbyname(name.begin());
  JSG_REQUIRE(digest != nullptr, TypeError, "Invalid Pbkdf2 digest: ", name,
              internalDescribeOpensslErrors());

  // Both pass and salt may be zero length here.
  auto buf = kj::heapArray<byte>(keylen);
  OSSLCALL(PKCS5_PBKDF2_HMAC((const char *)password.begin(),
                        password.size(),
                        salt.begin(),
                        salt.size(),
                        num_iterations,
                        digest,
                        keylen,
                        buf.begin()));
  return buf;
}

}  // namespace workerd::api::node
