#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/api/crypto.h>

namespace workerd::api::node {

class CryptoImpl final: public jsg::Object {
public:
  // Primes
  kj::Array<kj::byte> randomPrime(uint32_t size, bool safe,
      jsg::Optional<kj::Array<kj::byte>> add, jsg::Optional<kj::Array<kj::byte>> rem);

  bool checkPrimeSync(kj::Array<kj::byte> bufferView, uint32_t num_checks);

  // Pbkdf2
  kj::Array<kj::byte> getPbkdf(kj::Array<kj::byte> password, kj::Array<kj::byte> salt,
                               uint32_t num_iterations, uint32_t keylen, kj::String name);

  // Keys
  struct KeyExportOptions {
    jsg::Optional<kj::String> type;
    jsg::Optional<kj::String> format;
    jsg::Optional<kj::String> cipher;
    jsg::Optional<kj::Array<kj::byte>> passphrase;
    JSG_STRUCT(type, format, cipher, passphrase);
  };

  struct AsymmetricKeyDetails {
    jsg::Optional<uint32_t> modulusLength;
    jsg::Optional<uint64_t> publicExponent;
    jsg::Optional<kj::String> hashAlgorithm;
    jsg::Optional<kj::String> mgf1HashAlgorithm;
    jsg::Optional<uint32_t> saltLength;
    jsg::Optional<uint32_t> divisorLength;
    jsg::Optional<kj::String> namedCurve;
    JSG_STRUCT(modulusLength,
               publicExponent,
               hashAlgorithm,
               mgf1HashAlgorithm,
               saltLength,
               divisorLength,
               namedCurve);
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

  AsymmetricKeyDetails getAsymmetricKeyDetail(jsg::Lock& js, jsg::Ref<CryptoKey> key);
  kj::StringPtr getAsymmetricKeyType(jsg::Lock& js, jsg::Ref<CryptoKey> key);

  CryptoKeyPair generateKeyPair(jsg::Lock& js, kj::String type, GenerateKeyPairOptions options);

  jsg::Ref<CryptoKey> createSecretKey(jsg::Lock& js, kj::Array<kj::byte>);
  jsg::Ref<CryptoKey> createPrivateKey(jsg::Lock& js, CreateAsymmetricKeyOptions options);
  jsg::Ref<CryptoKey> createPublicKey(jsg::Lock& js, CreateAsymmetricKeyOptions options);

  JSG_RESOURCE_TYPE(CryptoImpl) {
    // Primes
    JSG_METHOD(randomPrime);
    JSG_METHOD(checkPrimeSync);
    // Pbkdf2
    JSG_METHOD(getPbkdf);
    // Keys
    JSG_METHOD(exportKey);
    JSG_METHOD(equals);
    JSG_METHOD(getAsymmetricKeyDetail);
    JSG_METHOD(getAsymmetricKeyType);
    JSG_METHOD(generateKeyPair);
    JSG_METHOD(createSecretKey);
    JSG_METHOD(createPrivateKey);
    JSG_METHOD(createPublicKey);
  }
};

#define EW_NODE_CRYPTO_ISOLATE_TYPES                   \
    api::node::CryptoImpl,                             \
    api::node::CryptoImpl::KeyExportOptions,           \
    api::node::CryptoImpl::AsymmetricKeyDetails,       \
    api::node::CryptoImpl::GenerateKeyPairOptions,     \
    api::node::CryptoImpl::CreateAsymmetricKeyOptions
}  // namespace workerd::api::node
