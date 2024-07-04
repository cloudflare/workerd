// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "crypto.h"
#include <workerd/api/crypto/digest.h>
#include <workerd/api/crypto/ec.h>
#include <workerd/api/crypto/impl.h>
#include <workerd/api/crypto/kdf.h>
#include <workerd/api/crypto/prime.h>
#include <workerd/api/crypto/rsa.h>
#include <workerd/jsg/jsg.h>
#include <workerd/api/crypto/spkac.h>
#include <openssl/crypto.h>

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

bool CryptoImpl::verifySpkac(kj::Array<const kj::byte> input) {
  return workerd::api::verifySpkac(input);
}

kj::Maybe<kj::Array<kj::byte>> CryptoImpl::exportPublicKey(kj::Array<const kj::byte> input) {
  return workerd::api::exportPublicKey(input);
}

kj::Maybe<kj::Array<kj::byte>> CryptoImpl::exportChallenge(kj::Array<const kj::byte> input) {
  return workerd::api::exportChallenge(input);
}

kj::Array<kj::byte> CryptoImpl::randomPrime(uint32_t size, bool safe,
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
    kj::String algorithm, KeyParam key) {
  KJ_SWITCH_ONEOF(key) {
    KJ_CASE_ONEOF(key_data, kj::Array<kj::byte>) {
      return jsg::alloc<HmacHandle>(HmacContext(algorithm, key_data.asPtr().asConst()));
    }
    KJ_CASE_ONEOF(key, jsg::Ref<CryptoKey>) {
      return jsg::alloc<HmacHandle>(HmacContext(algorithm, key->impl.get()));
    }
    KJ_CASE_ONEOF(key, jsg::Ref<SecretKeyObjectHandle>) {
      return jsg::alloc<HmacHandle>(HmacContext(algorithm, key->asPtr()));
    }
  }
  KJ_UNREACHABLE;
}

int CryptoImpl::HmacHandle::update(kj::Array<kj::byte> data) {
  ctx.update(data);
  return 1;  // This just always returns 1 no matter what.
}

kj::ArrayPtr<kj::byte> CryptoImpl::HmacHandle::digest() {
  return ctx.digest();
}

kj::Array<kj::byte> CryptoImpl::HmacHandle::oneshot(
    kj::String algorithm,
    CryptoImpl::HmacHandle::KeyParam key,
    kj::Array<kj::byte> data) {
  KJ_SWITCH_ONEOF(key) {
    KJ_CASE_ONEOF(key_data, kj::Array<kj::byte>) {
      HmacContext ctx(algorithm, key_data.asPtr().asConst());
      ctx.update(data);
      return kj::heapArray(ctx.digest());
    }
    KJ_CASE_ONEOF(key, jsg::Ref<CryptoKey>) {
      HmacContext ctx(algorithm, key->impl.get());
      ctx.update(data);
      return kj::heapArray(ctx.digest());
    }
    KJ_CASE_ONEOF(key, jsg::Ref<SecretKeyObjectHandle>) {
      HmacContext ctx(algorithm, key->asPtr());
      ctx.update(data);
      return kj::heapArray(ctx.digest());
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

kj::ArrayPtr<kj::byte> CryptoImpl::HashHandle::digest() {
  return ctx.digest();
}

jsg::Ref<CryptoImpl::HashHandle> CryptoImpl::HashHandle::copy(kj::Maybe<uint32_t> xofLen) {
  return jsg::alloc<HashHandle>(ctx.clone(kj::mv(xofLen)));
}

void CryptoImpl::HashHandle::visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
  tracker.trackFieldWithSize("digest", ctx.size());
}

kj::Array<kj::byte> CryptoImpl::HashHandle::oneshot(
    kj::String algorithm,
    kj::Array<kj::byte> data,
    kj::Maybe<uint32_t> xofLen) {
  HashContext ctx(algorithm, xofLen);
  ctx.update(data);
  return kj::heapArray(ctx.digest());
}

// ======================================================================================
// DiffieHellman

jsg::Ref<CryptoImpl::DiffieHellmanHandle> CryptoImpl::DiffieHellmanGroupHandle(kj::String name) {
  return jsg::alloc<DiffieHellmanHandle>(DiffieHellman(name));
}

jsg::Ref<CryptoImpl::DiffieHellmanHandle> CryptoImpl::DiffieHellmanHandle::constructor(
    jsg::Lock &js, kj::OneOf<kj::Array<kj::byte>, int> sizeOrKey,
    kj::OneOf<kj::Array<kj::byte>, int> generator) {
  return jsg::alloc<DiffieHellmanHandle>(DiffieHellman(sizeOrKey, generator));
}

CryptoImpl::DiffieHellmanHandle::DiffieHellmanHandle(DiffieHellman dh) : dh(kj::mv(dh)) {
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

int CryptoImpl::DiffieHellmanHandle::getVerifyError() { return verifyError; }

// ======================================================================================

jsg::Ref<CryptoImpl::SecretKeyObjectHandle> CryptoImpl::SecretKeyObjectHandle::constructor(
    kj::Array<kj::byte> keyData) {
  // Copy the key data into a new buffer.
  return jsg::alloc<CryptoImpl::SecretKeyObjectHandle>(kj::heapArray<kj::byte>(keyData));
}

bool CryptoImpl::SecretKeyObjectHandle::equals(
    jsg::Ref<CryptoImpl::SecretKeyObjectHandle> other) {
  if (data.size() != other->data.size()) {
    return false;
  }
  return CRYPTO_memcmp(data.begin(), other->data.begin(), data.size()) == 0;
}

CryptoImpl::ExportedKey CryptoImpl::SecretKeyObjectHandle::export_(
    jsg::Lock& js, KeyExportOptions options) {
  kj::StringPtr format = JSG_REQUIRE_NONNULL(options.format, TypeError, "Missing format option");
  if (format == "jwk"_kj) {
    SubtleCrypto::JsonWebKey jwk;
    jwk.kty = kj::str("oct");
    jwk.k = kj::encodeBase64Url(data);
    jwk.ext = true;
    return jwk;
  }
  JSG_REQUIRE(format == "buffer"_kj, TypeError, "Invalid format for secret key export: ", format);
  return kj::heapArray<kj::byte>(data.asPtr());
}

kj::Maybe<jsg::Ref<CryptoImpl::SecretKeyObjectHandle>>
CryptoImpl::SecretKeyObjectHandle::fromCryptoKey(jsg::Ref<CryptoKey> key) {
  if (key->getType() != "secret"_kj) return kj::none;
  SubtleCrypto::ExportKeyData data = key->impl->exportKey("raw"_kj);
  KJ_SWITCH_ONEOF(data) {
    KJ_CASE_ONEOF(data, kj::Array<kj::byte>) {
      return jsg::alloc<CryptoImpl::SecretKeyObjectHandle>(kj::mv(data));
    }
    KJ_CASE_ONEOF(jwk, SubtleCrypto::JsonWebKey) {
      // Fall-through
    }
  }
  KJ_UNREACHABLE;
}

kj::Maybe<jsg::Ref<CryptoKey>> CryptoImpl::SecretKeyObjectHandle::toCryptoKey(
    jsg::Lock& js,
    SubtleCrypto::ImportKeyAlgorithm algorithm,
    bool extractable,
    kj::Array<kj::String> usages) {
  return SubtleCrypto().importKeySync(js, "raw"_kj,
      kj::heapArray<kj::byte>(data.asPtr()),
      kj::mv(algorithm),
      extractable,
      usages.asPtr());
}

void CryptoImpl::SecretKeyObjectHandle::visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
  tracker.trackFieldWithSize("data", data.size());
}

// ======================================================================================

kj::Maybe<kj::Rc<AsymmetricKeyData>> CryptoImpl::getAsymmetricKeyDataFromCryptoKey(
    jsg::Ref<CryptoKey> key) {
  // Since Node.js' API unconditionally allows for key export, let's make sure the
  // key is extractable.
  JSG_REQUIRE(key->getExtractable(), TypeError, "Key is not extractable");
  return key->impl->getAsymmetricKeyData();
}

jsg::Ref<CryptoImpl::AsymmetricKeyObjectHandle> CryptoImpl::AsymmetricKeyObjectHandle::constructor(
    CreateAsymmetricKeyOptions options) {

  auto format = tryGetKeyFormat(options.format).orDefault(KeyFormat::PEM);
  auto type = tryGetKeyEncoding(options.type);
  if (format == KeyFormat::DER) {
    JSG_REQUIRE(type != kj::none, Error, "The 'type' option is required for DER format");
  }

  if (options.isPublicKey) {
    KJ_SWITCH_ONEOF(options.key) {
      KJ_CASE_ONEOF(data, kj::Array<kj::byte>) {
        // The key data in this case might be a public key or private key. If it
        // is a private key, then the public key will be derived from it.
        auto key = JSG_REQUIRE_NONNULL(
            importAsymmetricPublicKeyForNodeJs(data, format, type, options.passphrase),
            Error, "Failed to create public key");
        return jsg::alloc<AsymmetricKeyObjectHandle>(kj::mv(key));
      }
      KJ_CASE_ONEOF(jwk, SubtleCrypto::JsonWebKey) {
        JSG_REQUIRE(format == KeyFormat::JWK, Error, "Invalid format for public key");
        // When creating a key from a JWK, the key type must be one of `RSA`, `EC`, or `OKP`.
        // The remaining options are ignored.
        if (jwk.kty == "RSA"_kj) {
          auto key = JSG_REQUIRE_NONNULL(Rsa::importFromJwk(KeyType::PUBLIC, jwk),
                                        Error, "Invalid JWK");
          return jsg::alloc<AsymmetricKeyObjectHandle>(kj::mv(key));
        } else if (jwk.kty == "EC"_kj) {
          auto key = JSG_REQUIRE_NONNULL(Ec::importFromJwk(KeyType::PUBLIC, jwk),
                                        Error, "Invalid JWK");
          return jsg::alloc<AsymmetricKeyObjectHandle>(kj::mv(key));
        } else if (jwk.kty == "OKP"_kj) {
          auto key = JSG_REQUIRE_NONNULL(EdDsa::importFromJwk(KeyType::PUBLIC, jwk),
                                        Error, "Invalid JWK");
          return jsg::alloc<AsymmetricKeyObjectHandle>(kj::mv(key));
        }
        JSG_FAIL_REQUIRE(Error, "Unsupported JWK key type");
      }
      KJ_CASE_ONEOF(key, jsg::Ref<CryptoKey>) {
        JSG_REQUIRE(key->getType() == "private"_kj, Error, "Invalid key type");
        auto inner = KJ_ASSERT_NONNULL(getAsymmetricKeyDataFromCryptoKey(kj::mv(key)));
        auto derived = JSG_REQUIRE_NONNULL(derivePublicKeyFromPrivateKey(kj::mv(inner)),
            Error, "Unable to derive public key from given private key");
        return jsg::alloc<AsymmetricKeyObjectHandle>(kj::mv(derived));
      }
      KJ_CASE_ONEOF(key, jsg::Ref<AsymmetricKeyObjectHandle>) {
        JSG_REQUIRE(key->getTypeEnum() == KeyType::PRIVATE, Error, "Invalid key type");
        auto derived = JSG_REQUIRE_NONNULL(derivePublicKeyFromPrivateKey(key->getKeyData()),
            Error, "Unable to derive public key from given private key");
        return jsg::alloc<AsymmetricKeyObjectHandle>(kj::mv(derived));
      }
    }
    JSG_FAIL_REQUIRE(Error, "Failed to create public key");
  }

  // Create private key.
  KJ_SWITCH_ONEOF(options.key) {
    KJ_CASE_ONEOF(data, kj::Array<kj::byte>) {
      auto key = JSG_REQUIRE_NONNULL(
          importAsymmetricPrivateKeyForNodeJs(data, format, type, options.passphrase),
          Error, "Failed to create private key");
      return jsg::alloc<AsymmetricKeyObjectHandle>(kj::mv(key));
    }
    KJ_CASE_ONEOF(jwk, SubtleCrypto::JsonWebKey) {
      JSG_REQUIRE(format == KeyFormat::JWK, Error, "Invalid format for public key");
      // When creating a key from a JWK, the key type must be one of `RSA`, `EC`, or `OKP`.
      // The remaining options are ignored.
      if (jwk.kty == "RSA"_kj) {
        auto key = JSG_REQUIRE_NONNULL(Rsa::importFromJwk(KeyType::PRIVATE, jwk),
                                       Error, "Invalid JWK");
        return jsg::alloc<AsymmetricKeyObjectHandle>(kj::mv(key));
      } else if (jwk.kty == "EC"_kj) {
        auto key = JSG_REQUIRE_NONNULL(Ec::importFromJwk(KeyType::PRIVATE, jwk),
                                      Error, "Invalid JWK");
        return jsg::alloc<AsymmetricKeyObjectHandle>(kj::mv(key));
      } else if (jwk.kty == "OKP"_kj) {
        auto key = JSG_REQUIRE_NONNULL(EdDsa::importFromJwk(KeyType::PRIVATE, jwk),
                                      Error, "Invalid JWK");
        return jsg::alloc<AsymmetricKeyObjectHandle>(kj::mv(key));
      }
      JSG_FAIL_REQUIRE(Error, "Unsupported JWK key type");
    }
    KJ_CASE_ONEOF(key, jsg::Ref<CryptoKey>) {
      KJ_UNREACHABLE;
    }
    KJ_CASE_ONEOF(key, jsg::Ref<AsymmetricKeyObjectHandle>) {
      KJ_UNREACHABLE;
    }
  }
  JSG_FAIL_REQUIRE(Error, "Failed to create private key");
}

CryptoImpl::AsymmetricKeyObjectHandle::Kind CryptoImpl::AsymmetricKeyObjectHandle::getKind() const {
  return keyData->getKind();
}

CryptoImpl::ExportedKey CryptoImpl::AsymmetricKeyObjectHandle::export_(
    jsg::Lock& js, KeyExportOptions options) {
  ClearErrorOnReturn clearErrorOnReturn;

  // Do some validations.
  // Per Node.js, for public keys, the type must be one of `pkcs1` or `spki`,
  // For private keys, the type must be one of `pkcs1`, `pkcs8`, or `sec1`.
  // For both kinds, the format must be one of `pem`, `der`, or `jwk`.

  auto keyType = getTypeEnum();

  auto kind = getKind();
  auto format = JSG_REQUIRE_NONNULL(tryGetKeyFormat(options.format), Error,
      "Unsupported format for key export");
  auto type = JSG_REQUIRE_NONNULL(tryGetKeyEncoding(options.type), Error,
      "Unsupported encoding for key export");

  switch (keyType) {
    case KeyType::PUBLIC: {
      // PKCS1 is only valid for RSA keys.
      if ((type == KeyEncoding::PKCS1 && kind == Kind::RSA && kind == Kind::RSA_PSS) ||
          type == KeyEncoding::SPKI) {
        break;
      }
      JSG_FAIL_REQUIRE(Error, "Unsupported type for public key export");
    }
    case KeyType::PRIVATE: {
      if ((type == KeyEncoding::PKCS1 && kind == Kind::RSA && kind == Kind::RSA_PSS) ||
          type == KeyEncoding::PKCS8 ||
          type == KeyEncoding::SEC1) {
        break;
      }
      JSG_FAIL_REQUIRE(Error, "Unsupported type for private key export");
    }
    case KeyType::SECRET: KJ_UNREACHABLE;
  }


  switch (getKind()) {
    case Kind::RSA: KJ_FALLTHROUGH;
    case Kind::RSA_PSS: {
      auto rsa = KJ_ASSERT_NONNULL(Rsa::tryGetRsa(getEvpPkey()));
      switch (format) {
        case KeyFormat::PEM: return rsa.toPem(type, keyType);
        case KeyFormat::DER: return rsa.toDer(type, keyType);
        case KeyFormat::JWK: return rsa.toJwk(keyType, kj::none);
      }
      break;
    }
    case Kind::EC: {
      auto ec = KJ_ASSERT_NONNULL(Ec::tryGetEc(getEvpPkey()));
      auto curveName = ec.getCurveNameString();
      switch (format) {
        case KeyFormat::PEM: return ec.toPem(type, keyType);
        case KeyFormat::DER: return ec.toDer(type, keyType);
        case KeyFormat::JWK: return ec.toJwk(keyType, curveName.asPtr());
      }
      break;
    }
    case Kind::ED25519: KJ_FALLTHROUGH;
    case Kind::X25519: {
      auto eddsa = KJ_ASSERT_NONNULL(EdDsa::tryGetEdDsa(getEvpPkey()));
      switch (format) {
        case KeyFormat::PEM: return eddsa.toPem(type, keyType);
        case KeyFormat::DER: return eddsa.toDer(type, keyType);
        case KeyFormat::JWK: return eddsa.toJwk(keyType);
      }
      break;
    };
    case Kind::DH: KJ_FALLTHROUGH;
    case Kind::DSA: KJ_FALLTHROUGH;
    case Kind::UNKNOWN: {
      // TODO(now): Implement the above key exports
      JSG_FAIL_REQUIRE(Error, "Unsupported key type for export");
    }
  }
  KJ_UNREACHABLE;
}

bool CryptoImpl::AsymmetricKeyObjectHandle::equals(jsg::Ref<AsymmetricKeyObjectHandle> other) {
  return keyData->equals(other->keyData);
}

CryptoKey::AsymmetricKeyDetails CryptoImpl::AsymmetricKeyObjectHandle::getAsymmetricKeyDetail() {
  switch (getKind()) {
    case Kind::RSA: {
      return KJ_ASSERT_NONNULL(Rsa::tryGetRsa(getEvpPkey())).getAsymmetricKeyDetail();
    }
    case Kind::RSA_PSS: {
      return KJ_ASSERT_NONNULL(Rsa::tryGetRsa(getEvpPkey())).getAsymmetricKeyDetail();
    }
    case Kind::EC: {
      return KJ_ASSERT_NONNULL(Ec::tryGetEc(getEvpPkey())).getAsymmetricKeyDetail();
    }
    case Kind::DSA: {
      // TODO(soon): Implement DSA keys
      KJ_FALLTHROUGH;
    }
    case Kind::ED25519: {
      KJ_FALLTHROUGH;
    }
    case Kind::X25519: {
       KJ_FALLTHROUGH;
    }
    default: return CryptoKey::AsymmetricKeyDetails {};
  }
  KJ_UNREACHABLE;
}

kj::StringPtr CryptoImpl::AsymmetricKeyObjectHandle::getAsymmetricKeyType() {
  return keyData->getKindName();
}

jsg::Ref<CryptoImpl::AsymmetricKeyObjectHandle>
CryptoImpl::AsymmetricKeyObjectHandle::fromCryptoKey(jsg::Ref<CryptoKey> key) {
  return jsg::alloc<AsymmetricKeyObjectHandle>(
      JSG_REQUIRE_NONNULL(getAsymmetricKeyDataFromCryptoKey(kj::mv(key)), Error,
          "Unable to extract key data from CryptoKey"));
}

kj::Maybe<jsg::Ref<CryptoKey>> CryptoImpl::AsymmetricKeyObjectHandle::toCryptoKey(
    jsg::Lock& js,
    CryptoKey::AlgorithmVariant algorithm,
    bool extractable) {
  KJ_UNIMPLEMENTED("...");
}

kj::Maybe<CryptoImpl::AsymmetricKeyObjectHandle::Pair>
CryptoImpl::AsymmetricKeyObjectHandle::generateKeyPair(
    CryptoImpl::GenerateKeyPairOptions options) {
  KJ_UNIMPLEMENTED("...");
}

}  // namespace workerd::api::node
