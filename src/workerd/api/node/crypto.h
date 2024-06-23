#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/api/crypto/crypto.h>
#include <openssl/evp.h>

namespace workerd::api::node {

class CryptoImpl final: public jsg::Object {
public:
  // DH
  class DiffieHellmanHandle final: public jsg::Object {
    public:
      DiffieHellmanHandle(kj::OneOf<kj::Array<kj::byte>, int>& sizeOrKey,
                          kj::OneOf<kj::Array<kj::byte>, int>& generator);
      DiffieHellmanHandle(kj::String& name);

      static jsg::Ref<DiffieHellmanHandle> constructor(jsg::Lock& js,
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
      kj::Own<DH> dh;
      int verifyError;

      bool VerifyContext();
      bool Init(kj::OneOf<kj::Array<kj::byte>, int>& sizeOrKey, kj::OneOf<kj::Array<kj::byte>,
                int>& generator);
      bool InitGroup(kj::String& name);
  };

  jsg::Ref<DiffieHellmanHandle> DiffieHellmanGroupHandle(kj::String name);

  // Primes
  kj::Array<kj::byte> randomPrime(uint32_t size, bool safe,
      jsg::Optional<kj::Array<kj::byte>> add, jsg::Optional<kj::Array<kj::byte>> rem);
  bool checkPrimeSync(kj::Array<kj::byte> bufferView, uint32_t num_checks);

  // Hash
  class HashHandle final: public jsg::Object {
    public:
      HashHandle(kj::String& algorithm, kj::Maybe<uint32_t> xofLen);
      HashHandle(EVP_MD_CTX* in_ctx, kj::Maybe<uint32_t> xofLen);

      jsg::Ref<HashHandle> copy(jsg::Lock& js, kj::Maybe<uint32_t> xofLen);
      int update(jsg::Lock& js, kj::Array<kj::byte> data);
      kj::Array<kj::byte> digest(jsg::Lock& js);
      static jsg::Ref<HashHandle> constructor(jsg::Lock& js, kj::String algorithm,
                                              kj::Maybe<uint32_t> xofLen);

      JSG_RESOURCE_TYPE(HashHandle) {
        JSG_METHOD(update);
        JSG_METHOD(digest);
        JSG_METHOD(copy);
      };

      void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
        tracker.trackField("digest", _digest);
      }

    private:
      void checkDigestLength(const EVP_MD* md, kj::Maybe<uint32_t> xofLen);

      jsg::Optional<kj::Array<kj::byte>> _digest;
      kj::Own<EVP_MD_CTX> md_ctx;
      unsigned md_len;
  };

  // Hmac
  class HmacHandle final: public jsg::Object {
    public:
      HmacHandle(jsg::Lock& js, kj::String& algorithm, kj::OneOf<kj::Array<kj::byte>,
                 jsg::Ref<CryptoKey>>&_key);

      int update(jsg::Lock& js, kj::Array<kj::byte> data);
      kj::Array<kj::byte> digest(jsg::Lock& js);
      static jsg::Ref<HmacHandle> constructor(jsg::Lock& js,
          kj::String algorithm, kj::OneOf<kj::Array<kj::byte>, jsg::Ref<CryptoKey>> key);

      JSG_RESOURCE_TYPE(HmacHandle) {
        JSG_METHOD(update);
        JSG_METHOD(digest);
      };

      void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
        tracker.trackField("digest", _digest);
      }

    private:
      jsg::Optional<kj::Array<kj::byte>> _digest;
      kj::Own<HMAC_CTX> hmac_ctx;
  };

  // Hkdf
  kj::Array<kj::byte> getHkdf(kj::String hash, kj::Array<kj::byte> key, kj::Array<kj::byte> salt,
                              kj::Array<kj::byte> info, uint32_t length);

  // Pbkdf2
  kj::Array<kj::byte> getPbkdf(jsg::Lock& js,
                               kj::Array<kj::byte> password,
                               kj::Array<kj::byte> salt,
                               uint32_t num_iterations,
                               uint32_t keylen,
                               kj::String name);

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
    // Keys
    JSG_METHOD(exportKey);
    JSG_METHOD(equals);
    JSG_METHOD(getAsymmetricKeyDetail);
    JSG_METHOD(getAsymmetricKeyType);
    JSG_METHOD(createSecretKey);
    JSG_METHOD(createPrivateKey);
    JSG_METHOD(createPublicKey);
  }
};

#define EW_NODE_CRYPTO_ISOLATE_TYPES                   \
    api::node::CryptoImpl,                             \
    api::node::CryptoImpl::DiffieHellmanHandle,        \
    api::node::CryptoImpl::HashHandle,                 \
    api::node::CryptoImpl::HmacHandle,                 \
    api::node::CryptoImpl::KeyExportOptions,           \
    api::node::CryptoImpl::GenerateKeyPairOptions,     \
    api::node::CryptoImpl::CreateAsymmetricKeyOptions
}  // namespace workerd::api::node

