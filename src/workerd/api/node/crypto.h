// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/api/crypto/crypto.h>
#include <workerd/api/crypto/digest.h>
#include <workerd/api/crypto/dh.h>
#include <workerd/api/crypto/x509.h>
#include <openssl/evp.h>

namespace workerd::api::node {

class CryptoImpl final: public jsg::Object {
public:
  // DH
  class DiffieHellmanHandle final: public jsg::Object {
    public:
      DiffieHellmanHandle(DiffieHellman dh);

      static jsg::Ref<DiffieHellmanHandle> constructor(
          jsg::Lock& js,
          kj::OneOf<kj::Array<kj::byte>, int> sizeOrKey,
          kj::OneOf<kj::Array<kj::byte>, int> generator);

      void setPrivateKey(kj::Array<kj::byte> key);
      void setPublicKey(kj::Array<kj::byte> key);
      kj::Array<kj::byte> getPublicKey();
      kj::Array<kj::byte> getPrivateKey();
      kj::Array<kj::byte> getGenerator();
      kj::Array<kj::byte> getPrime();
      kj::Array<kj::byte> computeSecret(kj::Array<kj::byte> key);
      kj::Array<kj::byte> generateKeys();
      int getVerifyError();

      JSG_RESOURCE_TYPE(DiffieHellmanHandle) {
        JSG_METHOD(setPublicKey);
        JSG_METHOD(setPrivateKey);
        JSG_METHOD(getPublicKey);
        JSG_METHOD(getPrivateKey);
        JSG_METHOD(getGenerator);
        JSG_METHOD(getPrime);
        JSG_METHOD(computeSecret);
        JSG_METHOD(generateKeys);
        JSG_METHOD(getVerifyError);
      };

    private:
      DiffieHellman dh;
      int verifyError;
  };

  jsg::Ref<DiffieHellmanHandle> DiffieHellmanGroupHandle(kj::String name);

  // Primes
  kj::Array<kj::byte> randomPrime(uint32_t size, bool safe,
      jsg::Optional<kj::Array<kj::byte>> add, jsg::Optional<kj::Array<kj::byte>> rem);
  bool checkPrimeSync(kj::Array<kj::byte> bufferView, uint32_t num_checks);

  // Hash
  class HashHandle final: public jsg::Object {
    public:
      HashHandle(HashContext ctx) : ctx(kj::mv(ctx)) {}

      static jsg::Ref<HashHandle> constructor(kj::String algorithm, kj::Maybe<uint32_t> xofLen);
      static kj::Array<kj::byte> oneshot(kj::String algorithm, kj::Array<kj::byte> data,
                                         kj::Maybe<uint32_t> xofLen);

      jsg::Ref<HashHandle> copy(kj::Maybe<uint32_t> xofLen);
      int update(kj::Array<kj::byte> data);
      kj::ArrayPtr<kj::byte> digest();

      JSG_RESOURCE_TYPE(HashHandle) {
        JSG_METHOD(update);
        JSG_METHOD(digest);
        JSG_METHOD(copy);
        JSG_STATIC_METHOD(oneshot);
      };

      void visitForMemoryInfo(jsg::MemoryTracker& tracker) const;

    private:
      HashContext ctx;
  };

  // Hmac
  class HmacHandle final: public jsg::Object {
    public:
      using KeyParam = kj::OneOf<kj::Array<kj::byte>, jsg::Ref<CryptoKey>>;

      HmacHandle(HmacContext ctx) : ctx(kj::mv(ctx)) {};

      static jsg::Ref<HmacHandle> constructor(kj::String algorithm, KeyParam key);

      // Efficiently implement one-shot hmac that avoids multiple calls
      // across the C++/JS boundary.
      static kj::Array<kj::byte> oneshot(kj::String algorithm, KeyParam key,
                                         kj::Array<kj::byte> data);

      int update(kj::Array<kj::byte> data);
      kj::ArrayPtr<kj::byte> digest();

      JSG_RESOURCE_TYPE(HmacHandle) {
        JSG_METHOD(update);
        JSG_METHOD(digest);
        JSG_STATIC_METHOD(oneshot);
      };

      void visitForMemoryInfo(jsg::MemoryTracker& tracker) const;

    private:
      HmacContext ctx;
  };

  // Hkdf
  kj::Array<kj::byte> getHkdf(kj::String hash,
                              kj::Array<const kj::byte> key,
                              kj::Array<const kj::byte> salt,
                              kj::Array<const kj::byte> info,
                              uint32_t length);

  // Pbkdf2
  kj::Array<kj::byte> getPbkdf(jsg::Lock& js,
                               kj::Array<const kj::byte> password,
                               kj::Array<const kj::byte> salt,
                               uint32_t num_iterations,
                               uint32_t keylen,
                               kj::String name);

  // Scrypt
  kj::Array<kj::byte> getScrypt(jsg::Lock& js,
                                kj::Array<const kj::byte> password,
                                kj::Array<const kj::byte> salt,
                                uint32_t N,
                                uint32_t r,
                                uint32_t p,
                                uint32_t maxmem,
                                uint32_t keylen);

  // Keys
  struct KeyExportOptions {
    jsg::Optional<kj::String> type;
    jsg::Optional<kj::String> format;
    jsg::Optional<kj::String> cipher;
    jsg::Optional<kj::Array<kj::byte>> passphrase;
    JSG_STRUCT(type, format, cipher, passphrase);
  };

  struct GenerateKeyPairOptions {
    jsg::Optional<uint32_t> modulusLength;
    jsg::Optional<uint64_t> publicExponent;
    jsg::Optional<kj::String> hashAlgorithm;
    jsg::Optional<kj::String> mgf1HashAlgorithm;
    jsg::Optional<uint32_t> saltLength;
    jsg::Optional<uint32_t> divisorLength;
    jsg::Optional<kj::String> namedCurve;
    jsg::Optional<kj::Array<kj::byte>> prime;
    jsg::Optional<uint32_t> primeLength;
    jsg::Optional<uint32_t> generator;
    jsg::Optional<kj::String> groupName;
    jsg::Optional<kj::String> paramEncoding; // one of either 'named' or 'explicit'
    jsg::Optional<KeyExportOptions> publicKeyEncoding;
    jsg::Optional<KeyExportOptions> privateKeyEncoding;

    JSG_STRUCT(modulusLength,
               publicExponent,
               hashAlgorithm,
               mgf1HashAlgorithm,
               saltLength,
               divisorLength,
               namedCurve,
               prime,
               primeLength,
               generator,
               groupName,
               paramEncoding,
               publicKeyEncoding,
               privateKeyEncoding);
  };

  struct CreateAsymmetricKeyOptions {
    kj::OneOf<kj::Array<kj::byte>, SubtleCrypto::JsonWebKey, jsg::Ref<CryptoKey>> key;
    // For a PrivateKey, the key is one of either kj::Array<kj::byte> or
    // SubtleCrypto::JsonWebKey. For a PublicKey it can also be a CryptoKey
    // containing a private key from which the public key will be derived.
    jsg::Optional<kj::String> format;
    jsg::Optional<kj::String> type;
    jsg::Optional<kj::Array<kj::byte>> passphrase;
    // The passphrase is only used for private keys. The format, type, and passphrase
    // options are only used if the key is a kj::Array<kj::byte>.
    JSG_STRUCT(key, format, type, passphrase);
  };

  CryptoImpl() = default;
  CryptoImpl(jsg::Lock&, const jsg::Url&) {}

  kj::OneOf<kj::String, kj::Array<kj::byte>, SubtleCrypto::JsonWebKey> exportKey(
      jsg::Lock& js,
      jsg::Ref<CryptoKey> key,
      jsg::Optional<KeyExportOptions> options);

  bool equals(jsg::Lock& js, jsg::Ref<CryptoKey> key, jsg::Ref<CryptoKey> otherKey);

  CryptoKey::AsymmetricKeyDetails getAsymmetricKeyDetail(jsg::Lock& js, jsg::Ref<CryptoKey> key);
  kj::StringPtr getAsymmetricKeyType(jsg::Lock& js, jsg::Ref<CryptoKey> key);

  jsg::Ref<CryptoKey> createSecretKey(jsg::Lock& js, kj::Array<kj::byte>);
  jsg::Ref<CryptoKey> createPrivateKey(jsg::Lock& js, CreateAsymmetricKeyOptions options);
  jsg::Ref<CryptoKey> createPublicKey(jsg::Lock& js, CreateAsymmetricKeyOptions options);

  bool verifySpkac(kj::Array<const kj::byte> input);
  kj::Maybe<kj::Array<kj::byte>> exportPublicKey(kj::Array<const kj::byte> input);
  kj::Maybe<kj::Array<kj::byte>> exportChallenge(kj::Array<const kj::byte> input);

  JSG_RESOURCE_TYPE(CryptoImpl) {
    // DH
    JSG_NESTED_TYPE(DiffieHellmanHandle);
    JSG_METHOD(DiffieHellmanGroupHandle);
    // Primes
    JSG_METHOD(randomPrime);
    JSG_METHOD(checkPrimeSync);
    // Hash and Hmac
    JSG_NESTED_TYPE(HashHandle);
    JSG_NESTED_TYPE(HmacHandle);
    // Hkdf
    JSG_METHOD(getHkdf);
    // Pbkdf2
    JSG_METHOD(getPbkdf);
    // Scrypt
    JSG_METHOD(getScrypt);
    // Keys
    JSG_METHOD(exportKey);
    JSG_METHOD(equals);
    JSG_METHOD(getAsymmetricKeyDetail);
    JSG_METHOD(getAsymmetricKeyType);
    JSG_METHOD(createSecretKey);
    JSG_METHOD(createPrivateKey);
    JSG_METHOD(createPublicKey);
    // Spkac
    JSG_METHOD(verifySpkac);
    JSG_METHOD(exportPublicKey);
    JSG_METHOD(exportChallenge);
    // X509
    JSG_NESTED_TYPE(X509Certificate);
  }
};

#define EW_NODE_CRYPTO_ISOLATE_TYPES                   \
    api::node::CryptoImpl,                             \
    api::node::CryptoImpl::DiffieHellmanHandle,        \
    api::node::CryptoImpl::HashHandle,                 \
    api::node::CryptoImpl::HmacHandle,                 \
    api::node::CryptoImpl::KeyExportOptions,           \
    api::node::CryptoImpl::GenerateKeyPairOptions,     \
    api::node::CryptoImpl::CreateAsymmetricKeyOptions, \
    EW_CRYPTO_X509_ISOLATE_TYPES
}  // namespace workerd::api::node

