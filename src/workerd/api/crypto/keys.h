#pragma once

#include "impl.h"

namespace workerd::api {

// An algorithm-independent secret key. Used as the underlying implementation of
// things like Node.js SecretKey objects. Unlike Web Crypto keys, a secret key is
// not algorithm specific. For instance, a single secret key can be used for both
// AES and HMAC, where as Web Crypto requires a separate key for each algorithm.
class SecretKey final: public CryptoKey::Impl {
public:
  SecretKey(kj::Array<kj::byte> keyData);
  KJ_DISALLOW_COPY_AND_MOVE(SecretKey);

  kj::StringPtr getAlgorithmName() const override;
  CryptoKey::AlgorithmVariant getAlgorithm(jsg::Lock& js) const override;

  bool equals(const CryptoKey::Impl& other) const override;
  bool equals(const kj::Array<kj::byte>& other) const override;

  SubtleCrypto::ExportKeyData exportKey(kj::StringPtr format) const override;

  kj::StringPtr jsgGetMemoryName() const override { return "SecretKey"; }
  size_t jsgGetMemorySelfSize() const override { return sizeof(SecretKey); }
  void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const override;

private:
  ZeroOnFree keyData;
};

// ======================================================================================
// Keys

enum class PkEncoding {
  Object,
  PKCS1,
  PKCS8,
  SPKI,
  SEC1
};

enum class PkFormat {
  DER,
  PEM,
  JWK
};

enum class KeyType {
  Secret,
  Public,
  Private
};

struct ParsedKey {
  KeyType type;
  kj::Own<EVP_PKEY> key;
};

struct ParseKeyOptions {
  PkEncoding encoding = PkEncoding::Object;
  PkFormat format = PkFormat::DER;
  kj::Maybe<kj::StringPtr> maybeCipherName;
  kj::Maybe<kj::ArrayPtr<kj::byte>> maybePassphrase;
};

// Attempt to parse the given key data. The result may be a public key or private key.
kj::Maybe<ParsedKey> tryParseKey(kj::ArrayPtr<const kj::byte> keyData,
                                 kj::Maybe<ParseKeyOptions> options);

// Attempt to parse the given key data. The result must be a private key.
kj::Maybe<ParsedKey> tryParseKeyPrivate(kj::ArrayPtr<const kj::byte> keyData,
                                        kj::Maybe<ParseKeyOptions> options);

// Creates the correct CryptoKey::Impl for the given EVP_PKEY type or returns kj::none
// if the key type is not supported.
kj::Maybe<jsg::Ref<CryptoKey>> newCryptoKeyImpl(ParsedKey&& parsedKey);

kj::Maybe<kj::Own<CryptoKey::Impl>> newRsaCryptoKeyImpl(KeyType type, kj::Own<EVP_PKEY> key);
kj::Maybe<kj::Own<CryptoKey::Impl>> newRsaPssCryptoKeyImpl(KeyType type, kj::Own<EVP_PKEY> key);
kj::Maybe<kj::Own<CryptoKey::Impl>> newEcCryptoKeyImpl(KeyType type, kj::Own<EVP_PKEY> key);
kj::Maybe<kj::Own<CryptoKey::Impl>> newEd25519CryptoKeyImpl(KeyType type, kj::Own<EVP_PKEY> key);

inline kj::Maybe<kj::Own<CryptoKey::Impl>> newDsaCryptoKeyImpl(KeyType type, kj::Own<EVP_PKEY> key) {
  return kj::none;
}
}  // namespace workerd::api
