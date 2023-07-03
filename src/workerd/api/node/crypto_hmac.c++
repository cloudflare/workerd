// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

#include "crypto.h"
#include "crypto_util.h"
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <workerd/jsg/jsg.h>
#include <workerd/api/crypto-impl.h>

namespace workerd::api::node {
jsg::Ref<CryptoImpl::HmacHandle> CryptoImpl::HmacHandle::constructor(jsg::Lock& js,
    kj::String algorithm, kj::OneOf<kj::Array<kj::byte>, jsg::Ref<CryptoKey>>key) {
  return jsg::alloc<HmacHandle>(js, algorithm, key);
}

int CryptoImpl::HmacHandle::update(jsg::Lock& js, kj::Array<kj::byte> data) {
  JSG_REQUIRE(data.size() <= INT_MAX, RangeError, "data is too long");
  return HMAC_Update(hmac_ctx.get(), data.begin(), data.size()) == 1;
}

kj::Array<kj::byte> CryptoImpl::HmacHandle::digest(jsg::Lock& js) {
  KJ_IF_MAYBE(_existing_digest, _digest) {
    // Allow calling the internal digest several times, for the streams interface
    return kj::heapArray<kj::byte>(_existing_digest->asPtr());
  } else {
    unsigned digest_size = HMAC_size(hmac_ctx.get());
    unsigned len;
    auto digest = kj::heapArray<kj::byte>(digest_size);
    JSG_REQUIRE(HMAC_Final(hmac_ctx.get(), digest.begin(), &len), Error,
      "Failed to finalize HMAC");

    KJ_ASSERT(len == digest_size);

    auto data_out = kj::heapArray<kj::byte>(digest);
    _digest = kj::mv(digest);
    return data_out;
  }
}

CryptoImpl::HmacHandle::HmacHandle(jsg::Lock& js, kj::String& algorithm,
                                   kj::OneOf<kj::Array<kj::byte>, jsg::Ref<CryptoKey>>&_key) {
  const auto key = [&_key]() {
    KJ_SWITCH_ONEOF(_key) {
      KJ_CASE_ONEOF(key_data, kj::Array<kj::byte>) {
        return kj::mv(key_data);
      }
      KJ_CASE_ONEOF(key2, jsg::Ref<CryptoKey>) {
        // We already checked that the key is a secret key, so the following should succeed.
        SubtleCrypto::ExportKeyData keyData = key2->impl->exportKey("raw"_kj);

        KJ_SWITCH_ONEOF(keyData) {
          KJ_CASE_ONEOF(key_data, kj::Array<kj::byte>) {
            return kj::mv(key_data);
          }
          KJ_CASE_ONEOF(jwk, SubtleCrypto::JsonWebKey) {
            KJ_UNREACHABLE;
          }
        }
      }
    }
    KJ_UNREACHABLE;
  }();

  JSG_REQUIRE(key.size() <= INT_MAX, RangeError, "key is too long");
  const EVP_MD* md = EVP_get_digestbyname(algorithm.begin());
  JSG_REQUIRE(md != nullptr, Error, "Digest method not supported");
  kj::StringPtr mt = ""_kj;
  hmac_ctx = OSSL_NEW(HMAC_CTX);
  JSG_REQUIRE(HMAC_Init_ex(hmac_ctx.get(), key.size() ? (char*)key.begin() : mt.begin(),
                           key.size(),md, nullptr), Error, "Failed to initalize HMAC");
};

} // namespace workerd::api::node
