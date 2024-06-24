// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

#include "crypto.h"
#include <workerd/jsg/jsg.h>
#include <openssl/hkdf.h>
#include <workerd/api/crypto/impl.h>

namespace workerd::api::node {
kj::Array<kj::byte> CryptoImpl::getHkdf(kj::String hash, kj::Array<kj::byte> key,
    kj::Array<kj::byte> salt, kj::Array<kj::byte> info, uint32_t length) {
  const EVP_MD* digest = EVP_get_digestbyname(hash.begin());
  JSG_REQUIRE(digest != nullptr, TypeError, "Invalid Hkdf digest: ", hash);

  JSG_REQUIRE(info.size() <= INT32_MAX, RangeError, "Hkdf failed: info is too large");
  JSG_REQUIRE(salt.size() <= INT32_MAX, RangeError, "Hkdf failed: salt is too large");
  JSG_REQUIRE(key.size() <= INT32_MAX, RangeError, "Hkdf failed: key is too large");

  // HKDF-Expand computes up to 255 HMAC blocks, each having as many bits as the
  // output of the hash function. 255 is a hard limit because HKDF appends an
  // 8-bit counter to each HMAC'd message, starting at 1.
  constexpr size_t kMaxDigestMultiplier = 255;
  size_t max_length = EVP_MD_size(digest) * kMaxDigestMultiplier;
  JSG_REQUIRE(length <= max_length, RangeError, "Invalid Hkdf key length");
  size_t out_key_length = length;
  auto buf = kj::heapArray<kj::byte>(length);

  OSSLCALL(HKDF(buf.begin(), out_key_length, digest, key.begin(), key.size(), salt.begin(),
                salt.size(), info.begin(), info.size()));
  return buf;
}

}  // namespace workerd::api::node
