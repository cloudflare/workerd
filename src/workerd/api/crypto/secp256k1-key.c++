// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "secp256k1-key.h"

#include "impl.h"

#include <workerd/api/util.h>
#include <workerd/rust/aws-lc/lib.rs.h>

#include <openssl/crypto.h>
#include <openssl/mem.h>

#include <kj/array.h>
#include <kj/encoding.h>

#include <algorithm>

namespace workerd::api {

namespace {

constexpr size_t SECKEY_LEN = 32;
constexpr size_t PUBKEY_UNCOMPRESSED_LEN = 65;
constexpr size_t COORDINATE_LEN = 32;
constexpr size_t SIGNATURE_LEN = 64;

kj::Own<ZeroOnFree> wrapSecret(kj::Array<kj::byte> bytes) {
  return kj::heap<ZeroOnFree>(kj::mv(bytes));
}

// Use on public keys only. Secret material must go through ZeroOnFree-wrapped storage.
kj::Array<kj::byte> decodeJwkCoordinate(
    kj::String&& encoded, size_t expectedLen, kj::StringPtr fieldName) {
  auto bytes = JSG_REQUIRE_NONNULL(decodeBase64Url(kj::mv(encoded)), DOMDataError,
      "Invalid base64url encoding in JSON Web Key \"", fieldName, "\" field.");
  JSG_REQUIRE(bytes.size() <= expectedLen, DOMDataError, "JSON Web Key \"", fieldName,
      "\" must be at most ", expectedLen, " bytes.");
  auto out = kj::heapArray<kj::byte>(expectedLen);
  memset(out.begin(), 0, expectedLen);
  out.slice(expectedLen - bytes.size(), expectedLen).copyFrom(bytes);
  return out;
}

void validateHash(
    const kj::Maybe<kj::OneOf<kj::String, SubtleCrypto::HashAlgorithm>>& callTimeHash) {
  auto hashName = api::getAlgorithmName(JSG_REQUIRE_NONNULL(callTimeHash, TypeError,
      "Missing \"hash\" in AlgorithmIdentifier. (ECDSA requires that the hash algorithm be "
      "specified at call time rather than on the key. This differs from other WebCrypto "
      "algorithms for historical reasons.)"));

  JSG_REQUIRE(hashName == "SHA-256", DOMNotSupportedError,
      "secp256k1 ECDSA only supports SHA-256 (got \"", hashName, "\").");
}

kj::Own<CryptoKey::Impl> importRaw(kj::ArrayPtr<const kj::byte> keyData,
    CryptoKey::EllipticKeyAlgorithm keyAlgorithm,
    bool extractable,
    CryptoKeyUsageSet usages) {
  JSG_REQUIRE(keyData.size() == PUBKEY_UNCOMPRESSED_LEN, DOMDataError,
      "Invalid secp256k1 public key length (expected ", PUBKEY_UNCOMPRESSED_LEN,
      " bytes uncompressed, got ", keyData.size(), ").");

  JSG_REQUIRE(::workerd::rust::aws_lc::validate_public(
                  ::rust::Slice<const uint8_t>(keyData.begin(), keyData.size())),
      DOMDataError, "Invalid secp256k1 public key (failed to parse).");

  auto pub = kj::heapArray<kj::byte>(PUBKEY_UNCOMPRESSED_LEN);
  memcpy(pub.begin(), keyData.begin(), PUBKEY_UNCOMPRESSED_LEN);
  return kj::heap<Secp256k1Key>(
      KeyType::PUBLIC, kj::mv(pub), kj::none, kj::mv(keyAlgorithm), extractable, usages);
}

kj::Own<CryptoKey::Impl> importJwk(SubtleCrypto::JsonWebKey&& jwk,
    CryptoKey::EllipticKeyAlgorithm keyAlgorithm,
    bool extractable,
    CryptoKeyUsageSet usages,
    kj::ArrayPtr<const kj::String> keyUsages) {
  JSG_REQUIRE(jwk.kty == "EC", DOMDataError,
      "Elliptic curve \"jwk\" key import requires a JSON Web Key with Key Type parameter "
      "\"kty\" (\"",
      jwk.kty, "\") equal to \"EC\".");

  auto& crv =
      JSG_REQUIRE_NONNULL(jwk.crv, DOMDataError, "Missing field \"crv\" for secp256k1 key.");
  JSG_REQUIRE(crv == "secp256k1", DOMDataError, "JSON Web Key Curve parameter \"crv\" (\"", crv,
      "\") does not match expected \"secp256k1\".");

  // alg is optional; if present it must be ES256K (RFC 8812).
  KJ_IF_SOME(alg, jwk.alg) {
    JSG_REQUIRE(alg == "ES256K", DOMDataError, "JSON Web Key Algorithm parameter \"alg\" (\"", alg,
        "\") does not match expected \"ES256K\".");
  }

  if (keyUsages.size() > 0) {
    KJ_IF_SOME(use, jwk.use) {
      JSG_REQUIRE(use == "sig", DOMDataError,
          "Asymmetric \"jwk\" key import with usages requires a JSON Web Key with "
          "Public Key Use parameter \"use\" (\"",
          use, "\") equal to \"sig\".");
    }
  }

  KJ_IF_SOME(ops, jwk.key_ops) {
    std::sort(ops.begin(), ops.end());
    JSG_REQUIRE(std::adjacent_find(ops.begin(), ops.end()) == ops.end(), DOMDataError,
        "A JSON Web Key's Key Operations parameter (\"key_ops\") must not contain duplicates.");

    KJ_IF_SOME(use, jwk.use) {
      JSG_REQUIRE(use == "sig", DOMDataError,
          "Asymmetric \"jwk\" import requires a JSON Web Key with Public Key Use \"use\" (\"", use,
          "\") equal to \"sig\".");
    }

    for (const auto& op: ops) {
      JSG_REQUIRE(op == "sign" || op == "verify", DOMDataError,
          "JSON Web Key Operations parameter (\"key_ops\") for ECDSA may only contain \"sign\" "
          "and/or \"verify\" (got \"",
          op, "\").");
    }

    for (const auto& requested: keyUsages) {
      JSG_REQUIRE(std::find(ops.begin(), ops.end(), requested) != ops.end(), DOMDataError,
          "All specified key usages must be present in the JSON Web Key's Key Operations "
          "parameter (\"key_ops\").");
    }
  }

  auto xBytes = decodeJwkCoordinate(JSG_REQUIRE_NONNULL(kj::mv(jwk.x), DOMDataError,
                                        "Invalid EC key in JSON Web Key; missing \"x\"."),
      COORDINATE_LEN, "x");
  auto yBytes = decodeJwkCoordinate(JSG_REQUIRE_NONNULL(kj::mv(jwk.y), DOMDataError,
                                        "Invalid EC key in JSON Web Key; missing \"y\"."),
      COORDINATE_LEN, "y");

  auto pub = kj::heapArray<kj::byte>(PUBKEY_UNCOMPRESSED_LEN);
  pub[0] = 0x04;
  memcpy(pub.begin() + 1, xBytes.begin(), COORDINATE_LEN);
  memcpy(pub.begin() + 1 + COORDINATE_LEN, yBytes.begin(), COORDINATE_LEN);

  // Validate the public point is on the curve before storing.
  JSG_REQUIRE(::workerd::rust::aws_lc::validate_public(
                  ::rust::Slice<const uint8_t>(pub.begin(), pub.size())),
      DOMDataError, "Invalid secp256k1 public key in JSON Web Key (coordinates are not on curve).");

  if (jwk.d == kj::none) {
    return kj::heap<Secp256k1Key>(
        KeyType::PUBLIC, kj::mv(pub), kj::none, kj::mv(keyAlgorithm), extractable, usages);
  }

  // d is decoded into already-wrapped storage. The base64url intermediate is the only
  // plaintext copy and gets scrubbed on the way out, including on error paths.
  auto scrubbed = wrapSecret(kj::heapArray<kj::byte>(SECKEY_LEN));
  memset(scrubbed->asPtr().begin(), 0, SECKEY_LEN);

  auto dRaw = JSG_REQUIRE_NONNULL(decodeBase64Url(kj::mv(KJ_ASSERT_NONNULL(jwk.d))), DOMDataError,
      "Invalid EC key in JSON Web Key; missing or invalid private key component (\"d\").");
  KJ_DEFER(OPENSSL_cleanse(dRaw.begin(), dRaw.size()));
  JSG_REQUIRE(
      dRaw.size() <= SECKEY_LEN, DOMDataError, "Invalid length ", dRaw.size(), " for private key");
  scrubbed->asPtr().slice(SECKEY_LEN - dRaw.size(), SECKEY_LEN).copyFrom(dRaw);

  // aws-lc-rs's from_private_key_and_public_key validates that d derives the claimed (x, y)
  // and that d is in range. Either failure surfaces here.
  JSG_REQUIRE(::workerd::rust::aws_lc::validate_keypair(
                  ::rust::Slice<const uint8_t>(scrubbed->begin(), scrubbed->size()),
                  ::rust::Slice<const uint8_t>(pub.begin(), pub.size())),
      DOMDataError,
      "Invalid EC key; private key component \"d\" is out of range or does not match the "
      "claimed public point.");

  return kj::heap<Secp256k1Key>(
      KeyType::PRIVATE, kj::mv(pub), kj::mv(scrubbed), kj::mv(keyAlgorithm), extractable, usages);
}

}  // namespace

Secp256k1Key::Secp256k1Key(KeyType keyType,
    kj::Array<kj::byte> publicKey,
    kj::Maybe<kj::Own<ZeroOnFree>> privateKey,
    CryptoKey::EllipticKeyAlgorithm keyAlgorithm,
    bool extractable,
    CryptoKeyUsageSet usages)
    : CryptoKey::Impl(extractable, usages),
      keyType(keyType),
      publicKey(kj::mv(publicKey)),
      privateKey(kj::mv(privateKey)),
      keyAlgorithm(kj::mv(keyAlgorithm)) {}

kj::StringPtr Secp256k1Key::getAlgorithmName() const {
  return keyAlgorithm.name;
}

CryptoKey::AlgorithmVariant Secp256k1Key::getAlgorithm(jsg::Lock& js) const {
  return keyAlgorithm;
}

kj::StringPtr Secp256k1Key::getType() const {
  return toStringPtr(keyType);
}

kj::StringPtr Secp256k1Key::jsgGetMemoryName() const {
  return "Secp256k1Key";
}

size_t Secp256k1Key::jsgGetMemorySelfSize() const {
  return sizeof(Secp256k1Key);
}

kj::Own<CryptoKey::Impl> Secp256k1Key::import(jsg::Lock& js,
    kj::StringPtr normalizedName,
    kj::StringPtr format,
    SubtleCrypto::ImportKeyData keyData,
    CryptoKey::EllipticKeyAlgorithm keyAlgorithm,
    bool extractable,
    kj::ArrayPtr<const kj::String> keyUsages) {
  if (format == "raw"_kj) {
    JSG_REQUIRE(keyData.is<kj::Array<kj::byte>>(), DOMDataError,
        "Expected raw secp256k1 key but instead got a JSON Web Key.");
    auto usages = CryptoKeyUsageSet::validate(normalizedName,
        CryptoKeyUsageSet::Context::importPublic, keyUsages, CryptoKeyUsageSet::verify());
    return importRaw(
        keyData.get<kj::Array<kj::byte>>().asPtr(), kj::mv(keyAlgorithm), extractable, usages);
  }

  if (format == "jwk"_kj) {
    JSG_REQUIRE(keyData.is<SubtleCrypto::JsonWebKey>(), DOMDataError,
        "Expected JSON Web Key but instead got raw bytes.");
    auto& jwk = keyData.get<SubtleCrypto::JsonWebKey>();
    bool isPrivate = jwk.d != kj::none;
    auto usages = CryptoKeyUsageSet::validate(normalizedName,
        isPrivate ? CryptoKeyUsageSet::Context::importPrivate
                  : CryptoKeyUsageSet::Context::importPublic,
        keyUsages, isPrivate ? CryptoKeyUsageSet::sign() : CryptoKeyUsageSet::verify());
    return importJwk(kj::mv(jwk), kj::mv(keyAlgorithm), extractable, usages, keyUsages);
  }

  JSG_FAIL_REQUIRE(DOMNotSupportedError, "Unrecognized or unimplemented key import format \"",
      format, "\" for secp256k1.");
}

CryptoKeyPair Secp256k1Key::generatePair(jsg::Lock& js,
    CryptoKey::EllipticKeyAlgorithm keyAlgorithm,
    bool extractable,
    CryptoKeyUsageSet privateKeyUsages,
    CryptoKeyUsageSet publicKeyUsages) {
  auto generated = ::workerd::rust::aws_lc::generate_keypair();
  JSG_REQUIRE(generated.size() == SECKEY_LEN + PUBKEY_UNCOMPRESSED_LEN, InternalDOMOperationError,
      "Failed to generate secp256k1 keypair.");

  auto secret = kj::heapArray<kj::byte>(SECKEY_LEN);
  memcpy(secret.begin(), generated.data(), SECKEY_LEN);
  auto wrappedSecret = wrapSecret(kj::mv(secret));
  OPENSSL_cleanse(generated.data(), SECKEY_LEN);

  const auto* pubSrc = generated.data() + SECKEY_LEN;
  auto privatePub = kj::heapArray<kj::byte>(PUBKEY_UNCOMPRESSED_LEN);
  memcpy(privatePub.begin(), pubSrc, PUBKEY_UNCOMPRESSED_LEN);
  auto publicPub = kj::heapArray<kj::byte>(PUBKEY_UNCOMPRESSED_LEN);
  memcpy(publicPub.begin(), pubSrc, PUBKEY_UNCOMPRESSED_LEN);

  auto privateKeyRef = js.alloc<CryptoKey>(kj::heap<Secp256k1Key>(KeyType::PRIVATE,
      kj::mv(privatePub), kj::mv(wrappedSecret), keyAlgorithm, extractable, privateKeyUsages));
  auto publicKeyRef = js.alloc<CryptoKey>(kj::heap<Secp256k1Key>(
      KeyType::PUBLIC, kj::mv(publicPub), kj::none, kj::mv(keyAlgorithm), true, publicKeyUsages));

  return CryptoKeyPair{
    .publicKey = kj::mv(publicKeyRef),
    .privateKey = kj::mv(privateKeyRef),
  };
}

jsg::JsArrayBuffer Secp256k1Key::sign(jsg::Lock& js,
    SubtleCrypto::SignAlgorithm&& algorithm,
    kj::ArrayPtr<const kj::byte> data) const {
  JSG_REQUIRE(keyType == KeyType::PRIVATE, DOMInvalidAccessError,
      "Asymmetric signing requires a private key.");
  auto& seckey = KJ_ASSERT_NONNULL(privateKey);

  validateHash(algorithm.hash);

  auto signature =
      ::workerd::rust::aws_lc::sign(::rust::Slice<const uint8_t>(seckey->begin(), seckey->size()),
          ::rust::Slice<const uint8_t>(publicKey.begin(), publicKey.size()),
          ::rust::Slice<const uint8_t>(data.begin(), data.size()));
  JSG_REQUIRE(
      signature.size() == SIGNATURE_LEN, DOMOperationError, "secp256k1 ECDSA signing failed.");

  return jsg::JsArrayBuffer::create(js, kj::arrayPtr(signature.data(), signature.size()));
}

bool Secp256k1Key::verify(jsg::Lock& js,
    SubtleCrypto::SignAlgorithm&& algorithm,
    kj::ArrayPtr<const kj::byte> signature,
    kj::ArrayPtr<const kj::byte> data) const {
  JSG_REQUIRE(keyType == KeyType::PUBLIC, DOMInvalidAccessError,
      "Asymmetric verification requires a public key.");

  // A malformed signature is invalid, not an error.
  if (signature.size() != SIGNATURE_LEN) return false;

  validateHash(algorithm.hash);

  return ::workerd::rust::aws_lc::verify(
      ::rust::Slice<const uint8_t>(publicKey.begin(), publicKey.size()),
      ::rust::Slice<const uint8_t>(data.begin(), data.size()),
      ::rust::Slice<const uint8_t>(signature.begin(), signature.size()));
}

SubtleCrypto::ExportKeyData Secp256k1Key::exportKey(jsg::Lock& js, kj::StringPtr format) const {
  if (format == "raw"_kj) {
    JSG_REQUIRE(keyType == KeyType::PUBLIC, DOMInvalidAccessError,
        "Raw export of elliptic curve keys is only allowed for public keys.");
    return jsg::JsArrayBuffer::create(js, publicKey.asPtr()).addRef(js);
  }

  if (format == "jwk"_kj) {
    KJ_ASSERT(publicKey.size() == PUBKEY_UNCOMPRESSED_LEN);
    KJ_ASSERT(publicKey[0] == 0x04);
    auto x = publicKey.slice(1, 1 + COORDINATE_LEN);
    auto y = publicKey.slice(1 + COORDINATE_LEN, PUBKEY_UNCOMPRESSED_LEN);

    SubtleCrypto::JsonWebKey jwk;
    jwk.kty = kj::str("EC");
    jwk.crv = kj::str("secp256k1");
    jwk.x = fastEncodeBase64Url(x);
    jwk.y = fastEncodeBase64Url(y);
    KJ_IF_SOME(seckey, privateKey) {
      jwk.d = fastEncodeBase64Url(seckey->asPtr());
    }
    jwk.ext = isExtractable();
    jwk.key_ops = getUsages().map([](auto usage) { return kj::str(usage.name()); });
    return jwk;
  }

  JSG_FAIL_REQUIRE(DOMNotSupportedError, "Unrecognized or unimplemented key export format \"",
      format, "\" for secp256k1.");
}

bool Secp256k1Key::equals(const CryptoKey::Impl& other) const {
  if (this == &other) return true;

  KJ_IF_SOME(that, kj::dynamicDowncastIfAvailable<const Secp256k1Key>(other)) {
    if (keyType != that.keyType) return false;
    if (publicKey.size() != that.publicKey.size()) return false;
    if (CRYPTO_memcmp(publicKey.begin(), that.publicKey.begin(), publicKey.size()) != 0) {
      return false;
    }
    // Equal public halves already imply equal private scalars for valid keys, but compare the
    // scalars directly and in constant time as defense in depth.
    if (keyType == KeyType::PRIVATE) {
      auto& a = KJ_ASSERT_NONNULL(privateKey);
      auto& b = KJ_ASSERT_NONNULL(that.privateKey);
      if (a->size() != b->size()) return false;
      if (CRYPTO_memcmp(a->begin(), b->begin(), a->size()) != 0) return false;
    }
    return true;
  }
  return false;
}

}  // namespace workerd::api
