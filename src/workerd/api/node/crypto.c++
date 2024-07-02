// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "crypto.h"
#include <workerd/api/crypto/digest.h>
#include <workerd/api/crypto/impl.h>
#include <workerd/api/crypto/kdf.h>
#include <workerd/api/crypto/keys.h>
#include <workerd/api/crypto/prime.h>
#include <workerd/jsg/jsg.h>
#include <workerd/api/crypto/spkac.h>

namespace workerd::api::node {

kj::Array<kj::byte> NodeCrypto::getHkdf(kj::String hash,
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

kj::Array<kj::byte> NodeCrypto::getPbkdf(jsg::Lock& js,
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

kj::Array<kj::byte> NodeCrypto::getScrypt(jsg::Lock& js,
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

bool NodeCrypto::verifySpkac(kj::Array<const kj::byte> input) {
  return workerd::api::verifySpkac(input);
}

kj::Maybe<kj::Array<kj::byte>> NodeCrypto::exportPublicKey(kj::Array<const kj::byte> input) {
  return workerd::api::exportPublicKey(input);
}

kj::Maybe<kj::Array<kj::byte>> NodeCrypto::exportChallenge(kj::Array<const kj::byte> input) {
  return workerd::api::exportChallenge(input);
}

kj::Array<kj::byte> NodeCrypto::randomPrime(uint32_t size, bool safe,
                                            jsg::Optional<kj::Array<kj::byte>> add_buf,
                                            jsg::Optional<kj::Array<kj::byte>> rem_buf) {
  return workerd::api::randomPrime(size, safe,
      add_buf.map([](kj::Array<kj::byte>& buf) { return buf.asPtr(); }),
      rem_buf.map([](kj::Array<kj::byte>& buf) { return buf.asPtr(); }));
}

bool NodeCrypto::checkPrimeSync(kj::Array<kj::byte> bufferView, uint32_t num_checks) {
  return workerd::api::checkPrime(bufferView.asPtr(), num_checks);
}

// ======================================================================================
jsg::Ref<NodeCrypto::HmacHandle> NodeCrypto::HmacHandle::constructor(
    kj::String algorithm,
    kj::OneOf<kj::Array<kj::byte>, jsg::Ref<CryptoKey>> key) {
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

int NodeCrypto::HmacHandle::update(kj::Array<kj::byte> data) {
  ctx.update(data);
  return 1;  // This just always returns 1 no matter what.
}

kj::ArrayPtr<kj::byte> NodeCrypto::HmacHandle::digest() {
  return ctx.digest();
}

kj::Array<kj::byte> NodeCrypto::HmacHandle::oneshot(
    kj::String algorithm,
    NodeCrypto::HmacHandle::KeyParam key,
    kj::Array<kj::byte> data) {
  KJ_SWITCH_ONEOF(key) {
    KJ_CASE_ONEOF(key_data, kj::Array<kj::byte>) {
      HmacContext ctx(algorithm, key_data.asPtr());
      ctx.update(data);
      return kj::heapArray(ctx.digest());
    }
    KJ_CASE_ONEOF(key, jsg::Ref<CryptoKey>) {
      HmacContext ctx(algorithm, key->impl.get());
      ctx.update(data);
      return kj::heapArray(ctx.digest());
    }
  }
  KJ_UNREACHABLE;
}

void NodeCrypto::HmacHandle::visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
  tracker.trackFieldWithSize("digest", ctx.size());
}

// ======================================================================================
jsg::Ref<NodeCrypto::HashHandle> NodeCrypto::HashHandle::constructor(
    kj::String algorithm, kj::Maybe<uint32_t> xofLen) {
  return jsg::alloc<HashHandle>(HashContext(algorithm, xofLen));
}

int NodeCrypto::HashHandle::update(kj::Array<kj::byte> data) {
  ctx.update(data);
  return 1;
}

kj::ArrayPtr<kj::byte> NodeCrypto::HashHandle::digest() {
  return ctx.digest();
}

jsg::Ref<NodeCrypto::HashHandle> NodeCrypto::HashHandle::copy(kj::Maybe<uint32_t> xofLen) {
  return jsg::alloc<HashHandle>(ctx.clone(kj::mv(xofLen)));
}

void NodeCrypto::HashHandle::visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
  tracker.trackFieldWithSize("digest", ctx.size());
}

kj::Array<kj::byte> NodeCrypto::HashHandle::oneshot(
    kj::String algorithm,
    kj::Array<kj::byte> data,
    kj::Maybe<uint32_t> xofLen) {
  HashContext ctx(algorithm, xofLen);
  ctx.update(data);
  return kj::heapArray(ctx.digest());
}

// ======================================================================================
// DiffieHellman

jsg::Ref<NodeCrypto::DiffieHellmanHandle> NodeCrypto::DiffieHellmanGroupHandle(kj::String name) {
  return jsg::alloc<DiffieHellmanHandle>(DiffieHellman(name));
}

jsg::Ref<NodeCrypto::DiffieHellmanHandle> NodeCrypto::DiffieHellmanHandle::constructor(
    jsg::Lock &js, kj::OneOf<kj::Array<kj::byte>, int> sizeOrKey,
    kj::OneOf<kj::Array<kj::byte>, int> generator) {
  return jsg::alloc<DiffieHellmanHandle>(DiffieHellman(sizeOrKey, generator));
}

NodeCrypto::DiffieHellmanHandle::DiffieHellmanHandle(DiffieHellman dh) : dh(kj::mv(dh)) {
  verifyError = JSG_REQUIRE_NONNULL(this->dh.check(), Error, "DiffieHellman init failed");
};

void NodeCrypto::DiffieHellmanHandle::setPrivateKey(kj::Array<kj::byte> key) {
  dh.setPrivateKey(key);
}

void NodeCrypto::DiffieHellmanHandle::setPublicKey(kj::Array<kj::byte> key) {
  dh.setPublicKey(key);
}

kj::Array<kj::byte> NodeCrypto::DiffieHellmanHandle::getPublicKey() {
  return dh.getPublicKey();
}

kj::Array<kj::byte> NodeCrypto::DiffieHellmanHandle::getPrivateKey() {
  return dh.getPrivateKey();
}

kj::Array<kj::byte> NodeCrypto::DiffieHellmanHandle::getGenerator() {
  return dh.getGenerator();
}

kj::Array<kj::byte> NodeCrypto::DiffieHellmanHandle::getPrime() {
  return dh.getPrime();
}

kj::Array<kj::byte> NodeCrypto::DiffieHellmanHandle::computeSecret(kj::Array<kj::byte> key) {
  return dh.computeSecret(key);
}

kj::Array<kj::byte> NodeCrypto::DiffieHellmanHandle::generateKeys() {
  return dh.generateKeys();
}

int NodeCrypto::DiffieHellmanHandle::getVerifyError() { return verifyError; }

// ======================================================================================
// Keys

kj::OneOf<kj::String, kj::Array<kj::byte>, SubtleCrypto::JsonWebKey> NodeCrypto::exportKey(
    jsg::Lock& js,
    jsg::Ref<CryptoKey> key,
    KeyExportOptions options) {
  JSG_REQUIRE(key->getExtractable(), TypeError, "Unable to export non-extractable key");

  kj::StringPtr format = JSG_REQUIRE_NONNULL(options.format, TypeError, "Missing format option");
  if (format == "jwk"_kj) {
    // When format is jwk, all other options are ignored.
    return key->impl->exportKey(format);
  }

  if (key->getType() == "secret"_kj) {
    // For secret keys, we only pay attention to the format option, which will be
    // one of either "buffer" or "jwk". The "buffer" option correlates to the "raw"
    // format in Web Crypto. The "jwk" option is handled above.
    JSG_REQUIRE(format == "buffer"_kj, TypeError, "Invalid format for secret key export: ", format);
    return key->impl->exportKey("raw"_kj);
  }

  kj::StringPtr type = JSG_REQUIRE_NONNULL(options.type, TypeError, "Missing type option");
  auto data = key->impl->exportKeyExt(format, type,
                                      kj::mv(options.cipher),
                                      kj::mv(options.passphrase));
  if (format == "pem"_kj) {
    return kj::String(data.releaseAsChars());
  }
  return kj::mv(data);
}

bool NodeCrypto::equals(jsg::Lock& js, jsg::Ref<CryptoKey> key, jsg::Ref<CryptoKey> otherKey) {
  return *key == *otherKey;
}

CryptoKey::AsymmetricKeyDetails NodeCrypto::getAsymmetricKeyDetail(
    jsg::Lock& js, jsg::Ref<CryptoKey> key) {
  JSG_REQUIRE(key->getType() != "secret"_kj, Error, "Secret keys do not have asymmetric details");
  return key->getAsymmetricKeyDetails();
}

kj::StringPtr NodeCrypto::getAsymmetricKeyType(jsg::Lock& js, jsg::Ref<CryptoKey> key) {
  JSG_REQUIRE(key->getType() != "secret"_kj, TypeError,
      "Secret key does not have an asymmetric type");
  auto name = key->getAlgorithmName();

#define ALG_MAP(V)              \
  V("RSASSA-PKCS1-v1_5", "rsa") \
  V("RSA-PSS", "rsa")           \
  V("RSA-OAEP", "rsa")          \
  V("ECDSA", "ec")              \
  V("Ed25519", "ed25519")       \
  V("NODE-ED25519", "ed25519")  \
  V("ECDH", "ecdh")             \
  V("X25519", "x25519")

#define V(a, b) if (name == a) return b##_kj;

  ALG_MAP(V)

#undef V
#undef ALG_MAP

  // If the algorithm is not in the map, return the algorithm name.
  return key->getAlgorithmName();
}

jsg::Ref<CryptoKey> NodeCrypto::createSecretKey(jsg::Lock& js, kj::Array<kj::byte> keyData) {
  // We need to copy the key data because the received keyData will be from an
  // ArrayBuffer that remains mutatable after this function returns. We do not
  // want anyone to be able to modify the key data after it has been created.
  return jsg::alloc<CryptoKey>(kj::heap<SecretKey>(kj::heapArray(keyData.asPtr())));
}

jsg::Ref<CryptoKey> NodeCrypto::createPrivateKey(
    jsg::Lock& js,
    CreateAsymmetricKeyOptions options) {
  KJ_UNIMPLEMENTED("not implemented");
}

jsg::Ref<CryptoKey> NodeCrypto::createPublicKey(
    jsg::Lock& js,
    CreateAsymmetricKeyOptions options) {
  KJ_UNIMPLEMENTED("not implemented");
}

}  // namespace workerd::api::node
