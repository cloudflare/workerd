// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// Secp256k1Key — WebCrypto ECDSA CryptoKey::Impl for the secp256k1 curve.
//
// BoringSSL does not implement secp256k1, so this key type cannot be stored
// in a BoringSSL `EVP_PKEY` and therefore cannot inherit from the shared
// `AsymmetricKeyCryptoKeyImpl` base (which is parameterized over `EVP_PKEY`).
// Instead, we inherit directly from `CryptoKey::Impl` and hold the raw
// public key bytes in the 64-byte libsecp256k1 parsed form.
//
// Scope: this initial version supports public keys only, and `verify()` only.
// Private keys, signing, generation, and JWK support land in follow-up PRs.

#include "crypto.h"
#include "impl.h"

#include <secp256k1.h>

#include <kj/array.h>
#include <kj/common.h>

namespace workerd::api {

class Secp256k1Key final: public CryptoKey::Impl {
 public:
  // Construct a public-key `Secp256k1Key` from an already-parsed
  // `secp256k1_pubkey`. Callers own validating / parsing the source bytes
  // before constructing.
  Secp256k1Key(secp256k1_pubkey parsedPubkey,
      CryptoKey::EllipticKeyAlgorithm keyAlgorithm,
      bool extractable,
      CryptoKeyUsageSet usages);

  // Import a secp256k1 public key in WebCrypto "raw" format. Accepts both
  // the 33-byte compressed SEC1 form (leading 0x02 or 0x03) and the
  // 65-byte uncompressed form (leading 0x04). Throws `DOMDataError` on
  // malformed input.
  static kj::Own<CryptoKey::Impl> importRawPublic(jsg::Lock& js,
      kj::ArrayPtr<const kj::byte> keyData,
      CryptoKey::EllipticKeyAlgorithm keyAlgorithm,
      bool extractable,
      CryptoKeyUsageSet usages);

  // Import a secp256k1 public key from a JSON Web Key (RFC 7517 / RFC 7518).
  // The JWK must have `kty` = "EC" and `crv` = "secp256k1". The `x` and `y`
  // coordinates are base64url-encoded 32-byte scalars. Private-key JWKs
  // (those with a `d` field) are rejected until the signing path lands.
  static kj::Own<CryptoKey::Impl> importJwk(jsg::Lock& js,
      SubtleCrypto::JsonWebKey&& jwk,
      CryptoKey::EllipticKeyAlgorithm keyAlgorithm,
      bool extractable,
      CryptoKeyUsageSet usages);

  // ---------------------------------------------------------------------
  // CryptoKey::Impl overrides

  kj::StringPtr getAlgorithmName() const override {
    return "ECDSA";
  }

  CryptoKey::AlgorithmVariant getAlgorithm(jsg::Lock& js) const override {
    return keyAlgorithm;
  }

  kj::StringPtr getType() const override {
    return "public";
  }

  bool verify(jsg::Lock& js,
      SubtleCrypto::SignAlgorithm&& algorithm,
      kj::ArrayPtr<const kj::byte> signature,
      kj::ArrayPtr<const kj::byte> data) const override;

  SubtleCrypto::ExportKeyData exportKey(jsg::Lock& js, kj::StringPtr format) const override;

  bool equals(const CryptoKey::Impl& other) const override;

  kj::StringPtr jsgGetMemoryName() const override {
    return "Secp256k1Key";
  }
  size_t jsgGetMemorySelfSize() const override {
    return sizeof(Secp256k1Key);
  }

 private:
  // The parsed public key. libsecp256k1's internal representation is 64
  // bytes but should be treated as opaque and only manipulated through
  // library functions.
  secp256k1_pubkey parsedPubkey;
  CryptoKey::EllipticKeyAlgorithm keyAlgorithm;
};

}  // namespace workerd::api
