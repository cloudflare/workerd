#include "crypto.h"
#include <workerd/api/crypto-impl.h>
#include <openssl/crypto.h>
#include <map>

namespace workerd::api::node {

namespace {
// An algorithm-independent secret key. Used as the underlying
// implementation of Node.js SecretKey objects. Unlike Web Crypto,
// a Node.js secret key is not algorithm specific. For instance, a
// single secret key can be used for both AES and HMAC, where as
// Web Crypto requires a separate key for each algorithm.
class SecretKey final: public CryptoKey::Impl {
public:
  explicit SecretKey(kj::Array<kj::byte> keyData)
      : Impl(true, CryptoKeyUsageSet::privateKeyMask() |
                   CryptoKeyUsageSet::publicKeyMask()),
        keyData(kj::mv(keyData)) {}

  kj::StringPtr getAlgorithmName() const override { return "secret"_kj; }
  CryptoKey::AlgorithmVariant getAlgorithm() const override {
    return CryptoKey::KeyAlgorithm {
      .name = kj::str("secret"_kj)
    };
  }

  bool equals(const CryptoKey::Impl& other) const override final {
    return this == &other || (other.getType() == "secret"_kj && other.equals(keyData));
  }

  bool equals(const kj::Array<kj::byte>& other) const override final {
    return keyData.size() == other.size() &&
           CRYPTO_memcmp(keyData.begin(), other.begin(), keyData.size()) == 0;
  }

  SubtleCrypto::ExportKeyData exportKey(kj::StringPtr format) const override final {
    JSG_REQUIRE(format == "raw" || format == "jwk", DOMNotSupportedError,
        getAlgorithmName(), " key only supports exporting \"raw\" & \"jwk\", not \"", format,
        "\".");

    if (format == "jwk") {
      SubtleCrypto::JsonWebKey jwk;
      jwk.kty = kj::str("oct");
      jwk.k = kj::encodeBase64Url(keyData);
      jwk.ext = true;
      return jwk;
    }

    return kj::heapArray(keyData.asPtr());
  }

private:
  kj::Array<kj::byte> keyData;
};
}  // namespace

kj::OneOf<kj::String, kj::Array<kj::byte>, SubtleCrypto::JsonWebKey> CryptoImpl::exportKey(
    jsg::Lock& js,
    jsg::Ref<CryptoKey> key,
    jsg::Optional<KeyExportOptions> options) {
  JSG_REQUIRE(key->getExtractable(), TypeError, "Unable to export non-extractable key");
  auto& opts = JSG_REQUIRE_NONNULL(options, TypeError, "Options must be an object");

  kj::StringPtr format = JSG_REQUIRE_NONNULL(opts.format, TypeError, "Missing format option");
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

  kj::StringPtr type = JSG_REQUIRE_NONNULL(opts.type, TypeError, "Missing type option");
  auto data = key->impl->exportKeyExt(format, type, kj::mv(opts.cipher), kj::mv(opts.passphrase));
  if (format == "pem"_kj) {
    // TODO(perf): As a later performance optimization, change this so that it doesn't copy.
    return kj::str(data.asChars());
  }
  return kj::mv(data);
}

bool CryptoImpl::equals(jsg::Lock& js, jsg::Ref<CryptoKey> key, jsg::Ref<CryptoKey> otherKey) {
  return *key == *otherKey;
}

CryptoKey::AsymmetricKeyDetails CryptoImpl::getAsymmetricKeyDetail(
    jsg::Lock& js, jsg::Ref<CryptoKey> key) {
  JSG_REQUIRE(key->getType() != "secret"_kj, Error, "Secret keys do not have asymmetric details");
  return key->getAsymmetricKeyDetails();
}

kj::StringPtr CryptoImpl::getAsymmetricKeyType(jsg::Lock& js, jsg::Ref<CryptoKey> key) {
  static std::map<kj::StringPtr, kj::StringPtr> mapping{
    {"RSASSA-PKCS1-v1_5", "rsa"},
    {"RSA-PSS", "rsa"},
    {"RSA-OAEP", "rsa"},
    {"ECDSA", "ec"},
    {"Ed25519", "ed25519"},
    {"NODE-ED25519", "ed25519"},
    {"ECDH", "ecdh"},
    {"X25519", "x25519"},
  };
  JSG_REQUIRE(key->getType() != "secret"_kj, TypeError,
      "Secret key does not have an asymmetric type");
  auto found = mapping.find(key->getAlgorithmName());
  if (found != mapping.end()) {
    return found->second;
  }
  return key->getAlgorithmName();
}

jsg::Ref<CryptoKey> CryptoImpl::createSecretKey(jsg::Lock& js, kj::Array<kj::byte> keyData) {
  return jsg::alloc<CryptoKey>(kj::heap<SecretKey>(kj::heapArray(keyData.asPtr())));
}

jsg::Ref<CryptoKey> CryptoImpl::createPrivateKey(
    jsg::Lock& js,
    CreateAsymmetricKeyOptions options) {
  KJ_UNIMPLEMENTED("not implemented");
}

jsg::Ref<CryptoKey> CryptoImpl::createPublicKey(
    jsg::Lock& js,
    CreateAsymmetricKeyOptions options) {
  KJ_UNIMPLEMENTED("not implemented");
}

}  // namespace workerd::api::node
