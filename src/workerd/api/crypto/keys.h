#pragma once

#include "impl.h"

namespace workerd::api {

enum class KeyType {
  SECRET,
  PUBLIC,
  PRIVATE,
};

enum class KeyEncoding {
  PKCS1,
  PKCS8,
  SPKI,
  SEC1,
};

enum class KeyFormat {
  PEM,
  DER,
  JWK,
};

kj::Maybe<KeyEncoding> tryGetKeyEncoding(const kj::Maybe<kj::String>& encoding);
kj::Maybe<KeyFormat> tryGetKeyFormat(const kj::Maybe<kj::String>& format);

kj::StringPtr toStringPtr(KeyType type);

struct AsymmetricKeyData : public kj::Refcounted {
  enum class Kind {
    UNKNOWN = EVP_PKEY_NONE,
    RSA = EVP_PKEY_RSA,
    RSA_PSS = EVP_PKEY_RSA_PSS,
    DH = EVP_PKEY_DH,
    DSA = EVP_PKEY_DSA,
    EC = EVP_PKEY_EC,
    ED25519 = EVP_PKEY_ED25519,
    X25519 = EVP_PKEY_X25519,
  };

  kj::Own<EVP_PKEY> evpPkey;
  KeyType keyType;
  CryptoKeyUsageSet usages;
  AsymmetricKeyData(kj::Own<EVP_PKEY> evpPkey, KeyType keyType, CryptoKeyUsageSet usages)
      : evpPkey(kj::mv(evpPkey)), keyType(keyType), usages(usages) {}

  bool equals(const kj::Rc<AsymmetricKeyData>& other) const;

  Kind getKind() const;
  kj::StringPtr getKindName() const;
  KJ_DISALLOW_COPY_AND_MOVE(AsymmetricKeyData);
};

class AsymmetricKeyCryptoKeyImpl: public CryptoKey::Impl {
public:
  explicit AsymmetricKeyCryptoKeyImpl(kj::Rc<AsymmetricKeyData> key, bool extractable);

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

  kj::Array<kj::byte> sign(
      SubtleCrypto::SignAlgorithm&& algorithm,
      kj::ArrayPtr<const kj::byte> data) const override;

  bool verify(
      SubtleCrypto::SignAlgorithm&& algorithm,
      kj::ArrayPtr<const kj::byte> signature, kj::ArrayPtr<const kj::byte> data) const override;

  kj::StringPtr getType() const override;
  KeyType getTypeEnum() const { return keyData->keyType; }

  inline EVP_PKEY* getEvpPkey() const {
    return const_cast<EVP_PKEY*>(keyData->evpPkey.get());
  }
  kj::Maybe<kj::Rc<AsymmetricKeyData>> getAsymmetricKeyData() override {
    return keyData.addRef();
  }

  bool equals(const CryptoKey::Impl& other) const override final;

  kj::StringPtr jsgGetMemoryName() const override { return "AsymmetricKey"; }
  size_t jsgGetMemorySelfSize() const override { return sizeof(AsymmetricKeyCryptoKeyImpl); }
  void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const override {}

  bool verifyX509Public(const X509* cert) const override;
  bool verifyX509Private(const X509* cert) const override;

private:
  virtual SubtleCrypto::JsonWebKey exportJwk() const = 0;
  virtual kj::Array<kj::byte> exportRaw() const = 0;

  kj::Rc<AsymmetricKeyData> keyData;
};

// Performs asymmetric key import per the Web Crypto spec.
kj::Rc<AsymmetricKeyData> importAsymmetricForWebCrypto(
    jsg::Lock& js,
    kj::StringPtr format,
    SubtleCrypto::ImportKeyData keyData,
    kj::StringPtr normalizedName,
    bool extractable,
    kj::ArrayPtr<const kj::String> keyUsages,
    kj::FunctionParam<kj::Own<EVP_PKEY>(SubtleCrypto::JsonWebKey)> readJwk,
    CryptoKeyUsageSet allowedUsages);

kj::Maybe<kj::Rc<AsymmetricKeyData>> importAsymmetricPrivateKeyForNodeJs(
    kj::ArrayPtr<const kj::byte> keyData,
    KeyFormat format,
    const kj::Maybe<KeyEncoding>& encoding,
    kj::Maybe<kj::Array<kj::byte>>& passphrase);

kj::Maybe<kj::Rc<AsymmetricKeyData>> importAsymmetricPublicKeyForNodeJs(
    kj::ArrayPtr<const kj::byte> keyData,
    KeyFormat format,
    const kj::Maybe<KeyEncoding>& encoding,
    kj::Maybe<kj::Array<kj::byte>>& passphrase);

kj::Maybe<kj::Rc<AsymmetricKeyData>> derivePublicKeyFromPrivateKey(
    kj::Rc<AsymmetricKeyData> privateKeyData);

}  // namespace workerd::api
