// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

#include "crypto.h"
#include "crypto-util.h"
#include <v8.h>
#include <openssl/evp.h>
#include <workerd/jsg/jsg.h>
#include <workerd/api/crypto/impl.h>

namespace workerd::api::node {
jsg::Ref<CryptoImpl::HashHandle> CryptoImpl::HashHandle::constructor(
    jsg::Lock& js, kj::String algorithm, kj::Maybe<uint32_t> xofLen) {
  return jsg::alloc<HashHandle>(algorithm, xofLen);
}

int CryptoImpl::HashHandle::update(jsg::Lock& js, kj::Array<kj::byte> data) {
  JSG_REQUIRE(data.size() <= INT_MAX, RangeError, "data is too long");
  OSSLCALL(EVP_DigestUpdate(md_ctx.get(), data.begin(), data.size()));
  return 1;
}

kj::Array<kj::byte> CryptoImpl::HashHandle::digest(jsg::Lock& js) {
  KJ_IF_SOME(_existing_digest, _digest) {
    // Allow calling the internal digest several times, for the streams interface
    return kj::heapArray<kj::byte>(_existing_digest.asPtr());
  } else {
    unsigned len = md_len;
    auto digest = kj::heapArray<kj::byte>(md_len);
    if (EVP_MD_CTX_size(md_ctx.get()) == md_len) {
      JSG_REQUIRE(EVP_DigestFinal_ex(md_ctx.get(), digest.begin(), &len) == 1, Error,
                  "failed to compute hash digest");
      KJ_ASSERT(len == md_len);
    } else {
      JSG_REQUIRE(EVP_DigestFinalXOF(md_ctx.get(), digest.begin(), len) == 1, Error,
                  "failed to compute XOF hash digest");
    }
    auto data_out = kj::heapArray<kj::byte>(digest);
    _digest = kj::mv(digest);
    return data_out;
  }
}

jsg::Ref<CryptoImpl::HashHandle> CryptoImpl::HashHandle::copy(jsg::Lock& js,
                                                              kj::Maybe<uint32_t> xofLen) {
  return jsg::alloc<HashHandle>(this->md_ctx.get(), xofLen);
}

void CryptoImpl::HashHandle::checkDigestLength(const EVP_MD* md, kj::Maybe<uint32_t> xofLen) {
  md_ctx = OSSL_NEW(EVP_MD_CTX);
  OSSLCALL(EVP_DigestInit(md_ctx.get(), md));
  md_len = EVP_MD_size(md);
  KJ_IF_SOME(xof_md_len, xofLen) {
    if (xof_md_len != md_len) {
      JSG_REQUIRE((EVP_MD_flags(md) & EVP_MD_FLAG_XOF) != 0, Error, "invalid digest size");
      md_len = xof_md_len;
    }
  }
}

CryptoImpl::HashHandle::HashHandle(EVP_MD_CTX* in_ctx, kj::Maybe<uint32_t> xofLen) {
  const EVP_MD* md = EVP_MD_CTX_md(in_ctx);
  KJ_ASSERT(md != nullptr);
  checkDigestLength(md, xofLen);
  OSSLCALL(EVP_MD_CTX_copy_ex(md_ctx.get(), in_ctx));
};

CryptoImpl::HashHandle::HashHandle(kj::String& algorithm, kj::Maybe<uint32_t> xofLen) {
  const EVP_MD* md = EVP_get_digestbyname(algorithm.begin());
  JSG_REQUIRE(md != nullptr, Error, "Digest method not supported");
  checkDigestLength(md, xofLen);
};

} // namespace workerd::api::node
