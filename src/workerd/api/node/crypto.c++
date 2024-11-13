// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "crypto.h"

#include <workerd/api/crypto/digest.h>
#include <workerd/api/crypto/impl.h>
#include <workerd/api/crypto/kdf.h>
#include <workerd/api/crypto/prime.h>
#include <workerd/api/crypto/spkac.h>
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
  JSG_REQUIRE(
      length <= EVP_MD_size(digest) * kMaxDigestMultiplier, RangeError, "Invalid Hkdf key length");

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
  return JSG_REQUIRE_NONNULL(
      pbkdf2(keylen, num_iterations, digest, password, salt), Error, "Pbkdf2 failed");
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

  return JSG_REQUIRE_NONNULL(
      scrypt(keylen, N, r, p, maxmem, password, salt), Error, "Scrypt failed");
}

bool CryptoImpl::verifySpkac(kj::Array<const kj::byte> input) {
  return workerd::api::verifySpkac(input);
}

kj::Maybe<jsg::BufferSource> CryptoImpl::exportPublicKey(
    jsg::Lock& js, kj::Array<const kj::byte> input) {
  return workerd::api::exportPublicKey(js, input);
}

kj::Maybe<jsg::BufferSource> CryptoImpl::exportChallenge(
    jsg::Lock& js, kj::Array<const kj::byte> input) {
  return workerd::api::exportChallenge(js, input);
}

kj::Array<kj::byte> CryptoImpl::randomPrime(uint32_t size,
    bool safe,
    jsg::Optional<kj::Array<kj::byte>> add_buf,
    jsg::Optional<kj::Array<kj::byte>> rem_buf) {
  return workerd::api::randomPrime(size, safe,
      add_buf.map([](kj::Array<kj::byte>& buf) { return buf.asPtr(); }),
      rem_buf.map([](kj::Array<kj::byte>& buf) { return buf.asPtr(); }));
}

bool CryptoImpl::checkPrimeSync(kj::Array<kj::byte> bufferView, uint32_t num_checks) {
  return workerd::api::checkPrime(bufferView.asPtr(), num_checks);
}

// ======================================================================================
jsg::Ref<CryptoImpl::HmacHandle> CryptoImpl::HmacHandle::constructor(
    kj::String algorithm, kj::OneOf<kj::Array<kj::byte>, jsg::Ref<CryptoKey>> key) {
  KJ_SWITCH_ONEOF(key) {
    KJ_CASE_ONEOF(key_data, kj::Array<kj::byte>) {
      return jsg::alloc<HmacHandle>(HmacContext(algorithm, key_data.asPtr()));
    }
    KJ_CASE_ONEOF(key, jsg::Ref<CryptoKey>) {
      return jsg::alloc<HmacHandle>(HmacContext(algorithm, key->impl.get()));
    }
  }
  KJ_UNREACHABLE;
}

int CryptoImpl::HmacHandle::update(kj::Array<kj::byte> data) {
  ctx.update(data);
  return 1;  // This just always returns 1 no matter what.
}

jsg::BufferSource CryptoImpl::HmacHandle::digest(jsg::Lock& js) {
  return ctx.digest(js);
}

jsg::BufferSource CryptoImpl::HmacHandle::oneshot(jsg::Lock& js,
    kj::String algorithm,
    CryptoImpl::HmacHandle::KeyParam key,
    kj::Array<kj::byte> data) {
  KJ_SWITCH_ONEOF(key) {
    KJ_CASE_ONEOF(key_data, kj::Array<kj::byte>) {
      HmacContext ctx(algorithm, key_data.asPtr());
      ctx.update(data);
      return ctx.digest(js);
    }
    KJ_CASE_ONEOF(key, jsg::Ref<CryptoKey>) {
      HmacContext ctx(algorithm, key->impl.get());
      ctx.update(data);
      return ctx.digest(js);
    }
  }
  KJ_UNREACHABLE;
}

void CryptoImpl::HmacHandle::visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
  tracker.trackFieldWithSize("digest", ctx.size());
}

// ======================================================================================
jsg::Ref<CryptoImpl::HashHandle> CryptoImpl::HashHandle::constructor(
    kj::String algorithm, kj::Maybe<uint32_t> xofLen) {
  return jsg::alloc<HashHandle>(HashContext(algorithm, xofLen));
}

int CryptoImpl::HashHandle::update(kj::Array<kj::byte> data) {
  ctx.update(data);
  return 1;
}

jsg::BufferSource CryptoImpl::HashHandle::digest(jsg::Lock& js) {
  return ctx.digest(js);
}

jsg::Ref<CryptoImpl::HashHandle> CryptoImpl::HashHandle::copy(
    jsg::Lock& js, kj::Maybe<uint32_t> xofLen) {
  return jsg::alloc<HashHandle>(ctx.clone(js, kj::mv(xofLen)));
}

void CryptoImpl::HashHandle::visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
  tracker.trackFieldWithSize("digest", ctx.size());
}

jsg::BufferSource CryptoImpl::HashHandle::oneshot(
    jsg::Lock& js, kj::String algorithm, kj::Array<kj::byte> data, kj::Maybe<uint32_t> xofLen) {
  HashContext ctx(algorithm, xofLen);
  ctx.update(data);
  return ctx.digest(js);
}

// ======================================================================================
// DiffieHellman

jsg::Ref<CryptoImpl::DiffieHellmanHandle> CryptoImpl::DiffieHellmanGroupHandle(kj::String name) {
  return jsg::alloc<DiffieHellmanHandle>(DiffieHellman(name));
}

jsg::Ref<CryptoImpl::DiffieHellmanHandle> CryptoImpl::DiffieHellmanHandle::constructor(
    jsg::Lock& js,
    kj::OneOf<kj::Array<kj::byte>, int> sizeOrKey,
    kj::OneOf<kj::Array<kj::byte>, int> generator) {
  return jsg::alloc<DiffieHellmanHandle>(DiffieHellman(sizeOrKey, generator));
}

CryptoImpl::DiffieHellmanHandle::DiffieHellmanHandle(DiffieHellman dh): dh(kj::mv(dh)) {
  verifyError = JSG_REQUIRE_NONNULL(this->dh.check(), Error, "DiffieHellman init failed");
};

void CryptoImpl::DiffieHellmanHandle::setPrivateKey(kj::Array<kj::byte> key) {
  dh.setPrivateKey(key);
}

void CryptoImpl::DiffieHellmanHandle::setPublicKey(kj::Array<kj::byte> key) {
  dh.setPublicKey(key);
}

kj::Array<kj::byte> CryptoImpl::DiffieHellmanHandle::getPublicKey() {
  return dh.getPublicKey();
}

kj::Array<kj::byte> CryptoImpl::DiffieHellmanHandle::getPrivateKey() {
  return dh.getPrivateKey();
}

kj::Array<kj::byte> CryptoImpl::DiffieHellmanHandle::getGenerator() {
  return dh.getGenerator();
}

kj::Array<kj::byte> CryptoImpl::DiffieHellmanHandle::getPrime() {
  return dh.getPrime();
}

kj::Array<kj::byte> CryptoImpl::DiffieHellmanHandle::computeSecret(kj::Array<kj::byte> key) {
  return dh.computeSecret(key);
}

kj::Array<kj::byte> CryptoImpl::DiffieHellmanHandle::generateKeys() {
  return dh.generateKeys();
}

int CryptoImpl::DiffieHellmanHandle::getVerifyError() {
  return verifyError;
}

}  // namespace workerd::api::node
