#pragma once

#include "crypto.h"
#include "impl.h"
#include "keys.h"

#include <secp256k1.h>

namespace workerd::api {

// WebCrypto ECDSA over secp256k1, backed by libsecp256k1. Inherits from CryptoKey::Impl
// rather than AsymmetricKeyCryptoKeyImpl because BoringSSL can't represent this curve as
// an EVP_PKEY. A single instance represents either a public or a private key.
class Secp256k1Key final: public CryptoKey::Impl {
 public:
  Secp256k1Key(KeyType keyType,
      secp256k1_pubkey parsedPubkey,
      kj::Maybe<kj::Own<ZeroOnFree>> privateKey,
      CryptoKey::EllipticKeyAlgorithm keyAlgorithm,
      bool extractable,
      CryptoKeyUsageSet usages);

  // Format is "raw" or "jwk". For "jwk", a `d` field selects the private-key path.
  static kj::Own<CryptoKey::Impl> import(jsg::Lock& js,
      kj::StringPtr normalizedName,
      kj::StringPtr format,
      SubtleCrypto::ImportKeyData keyData,
      CryptoKey::EllipticKeyAlgorithm keyAlgorithm,
      bool extractable,
      kj::ArrayPtr<const kj::String> keyUsages);

  // Must be called from within an IoContext (uses the request's entropy source).
  static CryptoKeyPair generatePair(jsg::Lock& js,
      CryptoKey::EllipticKeyAlgorithm keyAlgorithm,
      bool extractable,
      CryptoKeyUsageSet privateKeyUsages,
      CryptoKeyUsageSet publicKeyUsages);

  kj::StringPtr getAlgorithmName() const override;
  CryptoKey::AlgorithmVariant getAlgorithm(jsg::Lock& js) const override;
  kj::StringPtr getType() const override;

  jsg::JsArrayBuffer sign(jsg::Lock& js,
      SubtleCrypto::SignAlgorithm&& algorithm,
      kj::ArrayPtr<const kj::byte> data) const override;

  bool verify(jsg::Lock& js,
      SubtleCrypto::SignAlgorithm&& algorithm,
      kj::ArrayPtr<const kj::byte> signature,
      kj::ArrayPtr<const kj::byte> data) const override;

  SubtleCrypto::ExportKeyData exportKey(jsg::Lock& js, kj::StringPtr format) const override;

  bool equals(const CryptoKey::Impl& other) const override;

  kj::StringPtr jsgGetMemoryName() const override;
  size_t jsgGetMemorySelfSize() const override;

 private:
  KeyType keyType;
  secp256k1_pubkey parsedPubkey;
  // ZeroOnFree's destructor suppresses its implicit moves, so it can't go in kj::Maybe directly.
  kj::Maybe<kj::Own<ZeroOnFree>> privateKey;
  CryptoKey::EllipticKeyAlgorithm keyAlgorithm;
};

}  // namespace workerd::api
