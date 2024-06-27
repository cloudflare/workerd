// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "crypto.h"
#include <workerd/api/crypto/impl.h>
#include <workerd/api/crypto/kdf.h>
#include <workerd/jsg/jsg.h>

namespace workerd::api::node {

kj::Array<kj::byte> CryptoImpl::getHkdf(kj::String hash,
                                        kj::Array<const kj::byte> key,
                                        kj::Array<const kj::byte> salt,
                                        kj::Array<const kj::byte> info,
                                        uint32_t length) {
  // The Node.js version of the HKDF is a bit different from the Web Crypto API
  // version. For one, the length here specifies the number of bytes, whereas
  // in Web Crypto the length is expressed in the number of bits. Second, the
  // Node.js implementation allows for a broader range of possible digest
  // algorithms whereas the Web Crypto API only allows for a few specific ones.
  // Third, the Node.js implementation enforces max size limits on the key,
  // salt, and info parameters. Fourth, the Web Crypto API relies on the key
  // being a CryptoKey object, whereas the Node.js implementation here takes a
  // raw byte array.
  ClearErrorOnReturn clearErrorOnReturn;
  const EVP_MD* digest = EVP_get_digestbyname(hash.begin());
  JSG_REQUIRE(digest != nullptr, TypeError, "Invalid Hkdf digest: ", hash);

  JSG_REQUIRE(info.size() <= INT32_MAX, RangeError, "Hkdf failed: info is too large");
  JSG_REQUIRE(salt.size() <= INT32_MAX, RangeError, "Hkdf failed: salt is too large");
  JSG_REQUIRE(key.size() <= INT32_MAX, RangeError, "Hkdf failed: key is too large");

  // HKDF-Expand computes up to 255 HMAC blocks, each having as many bits as the
  // output of the hash function. 255 is a hard limit because HKDF appends an
  // 8-bit counter to each HMAC'd message, starting at 1. What this means in a
  // practical sense is that the maximum value of length is 255 * hash size for
  // the specific hash algorithm specified.
  static constexpr size_t kMaxDigestMultiplier = 255;
  JSG_REQUIRE(length <= EVP_MD_size(digest) * kMaxDigestMultiplier, RangeError,
              "Invalid Hkdf key length");

  return JSG_REQUIRE_NONNULL(hkdf(length, digest, key, salt, info), Error, "Hkdf failed");
}

kj::Array<kj::byte> CryptoImpl::getPbkdf(jsg::Lock& js,
                                         kj::Array<const kj::byte> password,
                                         kj::Array<const kj::byte> salt,
                                         uint32_t num_iterations,
                                         uint32_t keylen,
                                         kj::String name) {
  // The Node.js version of the PBKDF2 is a bit different from the Web Crypto API.
  // For one, the Node.js implementation allows for a broader range of possible
  // digest algorithms whereas the Web Crypto API only allows for a few specific ones.
  // Second, the Node.js implementation enforces max size limits on the password and
  // salt parameters.
  ClearErrorOnReturn clearErrorOnReturn;
  const EVP_MD* digest = EVP_get_digestbyname(name.begin());
  JSG_REQUIRE(digest != nullptr, TypeError, "Invalid Pbkdf2 digest: ", name,
              internalDescribeOpensslErrors());

  JSG_REQUIRE(password.size() <= INT32_MAX, RangeError, "Pbkdf2 failed: password is too large");
  JSG_REQUIRE(salt.size() <= INT32_MAX, RangeError, "Pbkdf2 failed: salt is too large");
  // Note: The user could DoS us by selecting a very high iteration count. As with the Web Crypto
  // API, intentionally limit the maximum iteration count.
  checkPbkdfLimits(js, num_iterations);

  // Both pass and salt may be zero length here.
  return JSG_REQUIRE_NONNULL(pbkdf2(keylen, num_iterations, digest, password, salt),
      Error, "Pbkdf2 failed");
}

kj::Array<kj::byte> CryptoImpl::getScrypt(jsg::Lock& js,
                                          kj::Array<const kj::byte> password,
                                          kj::Array<const kj::byte> salt,
                                          uint32_t N,
                                          uint32_t r,
                                          uint32_t p,
                                          uint32_t maxmem,
                                          uint32_t keylen) {
  ClearErrorOnReturn clearErrorOnReturn;
  JSG_REQUIRE(password.size() <= INT32_MAX, RangeError, "Scrypt failed: password is too large");
  JSG_REQUIRE(salt.size() <= INT32_MAX, RangeError, "Scrypt failed: salt is too large");

  return JSG_REQUIRE_NONNULL(scrypt(keylen, N, r, p, maxmem, password, salt),
      Error, "Scrypt failed");
}
}  // namespace workerd::api::node
