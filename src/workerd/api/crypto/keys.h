#pragma once

#include "impl.h"

namespace workerd::api {

enum class KeyEncoding {
  PKCS1,
  PKCS8,
  SPKI,
  SEC1,
};

inline kj::StringPtr KJ_STRINGIFY(KeyEncoding encoding) {
  switch (encoding) {
    case KeyEncoding::PKCS1:
      return "pkcs1";
    case KeyEncoding::PKCS8:
      return "pkcs8";
    case KeyEncoding::SPKI:
      return "spki";
    case KeyEncoding::SEC1:
      return "sec1";
  }
  KJ_UNREACHABLE;
}

enum class KeyFormat {
  PEM,
  DER,
  JWK,
};

enum class KeyType {
  SECRET,
  PUBLIC,
  PRIVATE,
};

kj::StringPtr toStringPtr(KeyType type);

struct AsymmetricKeyData {
  kj::Own<EVP_PKEY> evpPkey;
  KeyType keyType;
  CryptoKeyUsageSet usages;
};

class AsymmetricKeyCryptoKeyImpl: public CryptoKey::Impl {
 public:
  explicit AsymmetricKeyCryptoKeyImpl(AsymmetricKeyData&& key, bool extractable);

  // ---------------------------------------------------------------------------
  // Subclasses must implement these

  // virtual CryptoKey::AlgorithmVariant getAlgorithm() = 0;
  // kj::StringPtr getAlgorithmName() const = 0;
  // (inheritted from CryptoKey::Impl, needs to be implemented by subclass)

  // Determine the hash function to use. Some algorithms choose this at key import time while
  // others choose it at sign() or verify() time. `callTimeHash` is the hash name passed to the
  // call.
  virtual kj::StringPtr chooseHash(
      const kj::Maybe<kj::OneOf<kj::String, SubtleCrypto::HashAlgorithm>>& callTimeHash) const = 0;

  // Convert OpenSSL-format signature to WebCrypto-format signature, if different.
  virtual jsg::BufferSource signatureSslToWebCrypto(
      jsg::Lock& js, kj::Array<kj::byte> signature) const;

  // Convert WebCrypto-format signature to OpenSSL-format signature, if different.
  virtual jsg::BufferSource signatureWebCryptoToSsl(
      jsg::Lock& js, kj::ArrayPtr<const kj::byte> signature) const;

  // Add salt to digest context in order to generate or verify salted signature.
  // Currently only used for RSA-PSS sign and verify operations.
  virtual void addSalt(
      EVP_PKEY_CTX* digestCtx, const SubtleCrypto::SignAlgorithm& algorithm) const {}

  // ---------------------------------------------------------------------------
  // Implementation of CryptoKey

  SubtleCrypto::ExportKeyData exportKey(jsg::Lock& js, kj::StringPtr format) const override final;

  virtual jsg::BufferSource exportKeyExt(jsg::Lock& js,
      kj::StringPtr format,
      kj::StringPtr type,
      jsg::Optional<kj::String> cipher = kj::none,
      jsg::Optional<kj::Array<kj::byte>> passphrase = kj::none) const override final;

  jsg::BufferSource sign(jsg::Lock& js,
      SubtleCrypto::SignAlgorithm&& algorithm,
      kj::ArrayPtr<const kj::byte> data) const override;

  bool verify(jsg::Lock& js,
      SubtleCrypto::SignAlgorithm&& algorithm,
      kj::ArrayPtr<const kj::byte> signature,
      kj::ArrayPtr<const kj::byte> data) const override;

  kj::StringPtr getType() const override;
  KeyType getTypeEnum() const {
    return keyType;
  }

  inline EVP_PKEY* getEvpPkey() const {
    return keyData.get();
  }

  bool equals(const CryptoKey::Impl& other) const override final;

  kj::StringPtr jsgGetMemoryName() const override {
    return "AsymmetricKey";
  }
  size_t jsgGetMemorySelfSize() const override {
    return sizeof(AsymmetricKeyCryptoKeyImpl);
  }
  void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const override {}

  bool verifyX509Public(const X509* cert) const override;
  bool verifyX509Private(const X509* cert) const override;

 private:
  virtual SubtleCrypto::JsonWebKey exportJwk() const = 0;
  virtual jsg::BufferSource exportRaw(jsg::Lock& js) const = 0;

  mutable kj::Own<EVP_PKEY> keyData;
  // mutable because OpenSSL wants non-const pointers even when the object won't be modified...
  KeyType keyType;
};

// Performs asymmetric key import per the Web Crypto spec.
AsymmetricKeyData importAsymmetricForWebCrypto(jsg::Lock& js,
    kj::StringPtr format,
    SubtleCrypto::ImportKeyData keyData,
    kj::StringPtr normalizedName,
    bool extractable,
    kj::ArrayPtr<const kj::String> keyUsages,
    kj::FunctionParam<kj::Own<EVP_PKEY>(SubtleCrypto::JsonWebKey)> readJwk,
    CryptoKeyUsageSet allowedUsages);

}  // namespace workerd::api
