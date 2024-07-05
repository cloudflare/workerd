#pragma once

#include "impl.h"

namespace workerd::api {

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
  virtual kj::Array<kj::byte> signatureSslToWebCrypto(kj::Array<kj::byte> signature) const;

  // Convert WebCrypto-format signature to OpenSSL-format signature, if different.
  virtual kj::Array<const kj::byte> signatureWebCryptoToSsl(
      kj::ArrayPtr<const kj::byte> signature) const;

  // Add salt to digest context in order to generate or verify salted signature.
  // Currently only used for RSA-PSS sign and verify operations.
  virtual void addSalt(EVP_PKEY_CTX* digestCtx,
                       const SubtleCrypto::SignAlgorithm& algorithm) const {}

  // ---------------------------------------------------------------------------
  // Implementation of CryptoKey

  SubtleCrypto::ExportKeyData exportKey(kj::StringPtr format) const override final;

  virtual kj::Array<kj::byte> exportKeyExt(
      kj::StringPtr format,
      kj::StringPtr type,
      jsg::Optional<kj::String> cipher = kj::none,
      jsg::Optional<kj::Array<kj::byte>> passphrase = kj::none) const override final;

  kj::Array<kj::byte> sign(
      SubtleCrypto::SignAlgorithm&& algorithm,
      kj::ArrayPtr<const kj::byte> data) const override;

  bool verify(
      SubtleCrypto::SignAlgorithm&& algorithm,
      kj::ArrayPtr<const kj::byte> signature, kj::ArrayPtr<const kj::byte> data) const override;

  kj::StringPtr getType() const override;
  KeyType getTypeEnum() const { return keyType; }

  inline EVP_PKEY* getEvpPkey() const { return keyData.get(); }

  bool equals(const CryptoKey::Impl& other) const override final;

  kj::StringPtr jsgGetMemoryName() const override { return "AsymmetricKey"; }
  size_t jsgGetMemorySelfSize() const override { return sizeof(AsymmetricKeyCryptoKeyImpl); }
  void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const override {}

private:
  virtual SubtleCrypto::JsonWebKey exportJwk() const = 0;
  virtual kj::Array<kj::byte> exportRaw() const = 0;

  mutable kj::Own<EVP_PKEY> keyData;
  // mutable because OpenSSL wants non-const pointers even when the object won't be modified...
  KeyType keyType;
};

// Performs asymmetric key import per the Web Crypto spec.
AsymmetricKeyData importAsymmetricForWebCrypto(
    jsg::Lock& js,
    kj::StringPtr format,
    SubtleCrypto::ImportKeyData keyData,
    kj::StringPtr normalizedName,
    bool extractable,
    kj::ArrayPtr<const kj::String> keyUsages,
    kj::FunctionParam<kj::Own<EVP_PKEY>(SubtleCrypto::JsonWebKey)> readJwk,
    CryptoKeyUsageSet allowedUsages);

}  // namespace workerd::api
