// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "secp256k1-key.h"

#include "impl.h"

#include <workerd/io/io-context.h>

#include <openssl/evp.h>
#include <openssl/mem.h>
#include <secp256k1.h>

#include <kj/debug.h>

namespace workerd::api {

namespace {

constexpr size_t kSeckeyLen = 32;
constexpr size_t kPubkeyCompressedLen = 33;
constexpr size_t kPubkeyUncompressedLen = 65;
constexpr size_t kCoordinateLen = 32;
constexpr size_t kSignatureLen = 64;

// Lazily-initialized process-wide context. Safe to share across threads since we only ever
// pass it as a const pointer (the library guarantees that's lock-free).
const secp256k1_context* context() {
  static const secp256k1_context* ctx = []() {
    secp256k1_context* c = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    KJ_ASSERT(c != nullptr);
    return c;
  }();
  return ctx;
}

kj::Array<kj::byte> serializePubkey(const secp256k1_pubkey& pubkey, unsigned int flags) {
  size_t len = (flags == SECP256K1_EC_COMPRESSED) ? kPubkeyCompressedLen : kPubkeyUncompressedLen;
  auto out = kj::heapArray<kj::byte>(len);
  size_t outLen = len;
  auto ret = secp256k1_ec_pubkey_serialize(context(), out.begin(), &outLen, &pubkey, flags);
  KJ_ASSERT(ret == 1);
  KJ_ASSERT(outLen == len);
  return out;
}

kj::Array<kj::byte> computeDigest(kj::StringPtr hashName, kj::ArrayPtr<const kj::byte> data) {
  const EVP_MD* md = lookupDigestAlgorithm(hashName).second;
  auto out = kj::heapArray<kj::byte>(EVP_MD_size(md));
  unsigned int outLen = out.size();
  OSSLCALL(EVP_Digest(data.begin(), data.size(), out.begin(), &outLen, md, nullptr));
  KJ_ASSERT(outLen == out.size());
  return out;
}

kj::Own<ZeroOnFree> wrapSecret(kj::Array<kj::byte>&& bytes) {
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

kj::Own<CryptoKey::Impl> importRaw(kj::ArrayPtr<const kj::byte> keyData,
    CryptoKey::EllipticKeyAlgorithm keyAlgorithm,
    bool extractable,
    CryptoKeyUsageSet usages) {
  JSG_REQUIRE(keyData.size() == kPubkeyCompressedLen || keyData.size() == kPubkeyUncompressedLen,
      DOMDataError, "Invalid secp256k1 public key length (expected ", kPubkeyCompressedLen, " or ",
      kPubkeyUncompressedLen, " bytes, got ", keyData.size(), ").");

  secp256k1_pubkey parsed;
  JSG_REQUIRE(secp256k1_ec_pubkey_parse(context(), &parsed, keyData.begin(), keyData.size()) == 1,
      DOMDataError, "Invalid secp256k1 public key (failed to parse).");

  return kj::heap<Secp256k1Key>(
      KeyType::PUBLIC, parsed, kj::none, kj::mv(keyAlgorithm), extractable, usages);
}

kj::Own<CryptoKey::Impl> importJwk(SubtleCrypto::JsonWebKey&& jwk,
    CryptoKey::EllipticKeyAlgorithm keyAlgorithm,
    bool extractable,
    CryptoKeyUsageSet usages) {
  JSG_REQUIRE(jwk.kty == "EC", DOMDataError,
      "secp256k1 \"jwk\" import requires \"kty\" == \"EC\" (got \"", jwk.kty, "\").");

  auto& crv = JSG_REQUIRE_NONNULL(jwk.crv, DOMDataError, "Missing \"crv\" in JSON Web Key.");
  JSG_REQUIRE(crv == "secp256k1", DOMDataError, "JSON Web Key \"crv\" (\"", crv,
      "\") does not match expected \"secp256k1\".");

  // alg is optional; if present it must be ES256K (RFC 8812).
  KJ_IF_SOME(alg, jwk.alg) {
    JSG_REQUIRE(alg == "ES256K", DOMDataError, "JSON Web Key \"alg\" (\"", alg,
        "\") does not match expected \"ES256K\".");
  }

  auto xBytes = decodeJwkCoordinate(
      JSG_REQUIRE_NONNULL(kj::mv(jwk.x), DOMDataError, "Missing \"x\" in JSON Web Key."),
      kCoordinateLen, "x");
  auto yBytes = decodeJwkCoordinate(
      JSG_REQUIRE_NONNULL(kj::mv(jwk.y), DOMDataError, "Missing \"y\" in JSON Web Key."),
      kCoordinateLen, "y");

  kj::byte sec1[kPubkeyUncompressedLen] = {};
  sec1[0] = 0x04;
  memcpy(sec1 + 1, xBytes.begin(), kCoordinateLen);
  memcpy(sec1 + 1 + kCoordinateLen, yBytes.begin(), kCoordinateLen);

  secp256k1_pubkey pubkey;
  JSG_REQUIRE(secp256k1_ec_pubkey_parse(context(), &pubkey, sec1, sizeof(sec1)) == 1, DOMDataError,
      "Invalid secp256k1 public key in JSON Web Key (coordinates are not on curve).");

  if (jwk.d == kj::none) {
    return kj::heap<Secp256k1Key>(
        KeyType::PUBLIC, pubkey, kj::none, kj::mv(keyAlgorithm), extractable, usages);
  }

  // d is decoded into already-wrapped storage. The base64url intermediate is the only
  // plaintext copy and gets scrubbed on the way out, including on error paths. RFC 7518
  // permits shorter encodings, so we zero-pad to 32 bytes.
  auto scrubbed = wrapSecret(kj::heapArray<kj::byte>(kSeckeyLen));
  memset(scrubbed->asPtr().begin(), 0, kSeckeyLen);

  auto dRaw = JSG_REQUIRE_NONNULL(decodeBase64Url(kj::mv(KJ_ASSERT_NONNULL(jwk.d))), DOMDataError,
      "Invalid base64url encoding in JSON Web Key \"d\" field.");
  KJ_DEFER(OPENSSL_cleanse(dRaw.begin(), dRaw.size()));
  JSG_REQUIRE(dRaw.size() <= kSeckeyLen, DOMDataError,
      "secp256k1 JSON Web Key private scalar \"d\" must be at most ", kSeckeyLen, " bytes.");
  scrubbed->asPtr().slice(kSeckeyLen - dRaw.size(), kSeckeyLen).copyFrom(dRaw);

  // d must be in range and must derive the claimed public point. pubkey_create rejects
  // out-of-range scalars internally, so a single check covers both conditions.
  secp256k1_pubkey derivedPubkey;
  JSG_REQUIRE(secp256k1_ec_pubkey_create(context(), &derivedPubkey, scrubbed->begin()) == 1,
      DOMDataError, "Invalid secp256k1 private key in JSON Web Key (\"d\" is out of range).");
  auto derivedSerialized = serializePubkey(derivedPubkey, SECP256K1_EC_UNCOMPRESSED);
  JSG_REQUIRE(CRYPTO_memcmp(derivedSerialized.begin(), sec1, kPubkeyUncompressedLen) == 0,
      DOMDataError,
      "secp256k1 JSON Web Key is inconsistent: public coordinates do not match the point derived "
      "from the private scalar.");

  return kj::heap<Secp256k1Key>(
      KeyType::PRIVATE, pubkey, kj::mv(scrubbed), kj::mv(keyAlgorithm), extractable, usages);
}

}  // namespace

Secp256k1Key::Secp256k1Key(KeyType keyType,
    secp256k1_pubkey parsedPubkey,
    kj::Maybe<kj::Own<ZeroOnFree>> privateKey,
    CryptoKey::EllipticKeyAlgorithm keyAlgorithm,
    bool extractable,
    CryptoKeyUsageSet usages)
    : CryptoKey::Impl(extractable, usages),
      keyType(keyType),
      parsedPubkey(parsedPubkey),
      privateKey(kj::mv(privateKey)),
      keyAlgorithm(kj::mv(keyAlgorithm)) {}

kj::StringPtr Secp256k1Key::getAlgorithmName() const {
  return keyAlgorithm.name;
}

CryptoKey::AlgorithmVariant Secp256k1Key::getAlgorithm(jsg::Lock& js) const {
  return keyAlgorithm;
}

kj::StringPtr Secp256k1Key::getType() const {
  return keyType == KeyType::PRIVATE ? "private"_kj : "public"_kj;
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
  if (format == "raw") {
    JSG_REQUIRE(keyData.is<kj::Array<kj::byte>>(), DOMDataError,
        "Expected raw secp256k1 key but got a JSON Web Key.");
    auto usages = CryptoKeyUsageSet::validate(normalizedName,
        CryptoKeyUsageSet::Context::importPublic, keyUsages, CryptoKeyUsageSet::verify());
    return importRaw(
        keyData.get<kj::Array<kj::byte>>().asPtr(), kj::mv(keyAlgorithm), extractable, usages);
  }

  if (format == "jwk") {
    JSG_REQUIRE(keyData.is<SubtleCrypto::JsonWebKey>(), DOMDataError,
        "Expected JSON Web Key but got raw bytes.");
    auto& jwk = keyData.get<SubtleCrypto::JsonWebKey>();
    bool isPrivate = jwk.d != kj::none;
    auto usages = CryptoKeyUsageSet::validate(normalizedName,
        isPrivate ? CryptoKeyUsageSet::Context::importPrivate
                  : CryptoKeyUsageSet::Context::importPublic,
        keyUsages, isPrivate ? CryptoKeyUsageSet::sign() : CryptoKeyUsageSet::verify());
    return importJwk(kj::mv(jwk), kj::mv(keyAlgorithm), extractable, usages);
  }

  // TODO(secp256k1): "spki"/"pkcs8" need hand-rolled DER since BoringSSL can't marshal this curve.
  JSG_FAIL_REQUIRE(DOMNotSupportedError, "Key import format \"", format,
      "\" is not yet implemented for secp256k1 (only \"raw\" and \"jwk\" are supported).");
}

CryptoKeyPair Secp256k1Key::generatePair(jsg::Lock& js,
    CryptoKey::EllipticKeyAlgorithm keyAlgorithm,
    bool extractable,
    CryptoKeyUsageSet privateKeyUsages,
    CryptoKeyUsageSet publicKeyUsages) {
  // A uniformly-random 32-byte string is a valid secp256k1 scalar almost always (~1 in 2^224
  // chance it's invalid). The cap is just defense against a broken RNG.
  auto scrubbed = wrapSecret(kj::heapArray<kj::byte>(kSeckeyLen));
  constexpr int kMaxAttempts = 256;
  bool ok = false;
  for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
    IoContext::current().getEntropySource().generate(scrubbed->asPtr());
    if (secp256k1_ec_seckey_verify(context(), scrubbed->begin()) == 1) {
      ok = true;
      break;
    }
  }
  JSG_REQUIRE(ok, InternalDOMOperationError,
      "Failed to sample a valid secp256k1 private scalar after many attempts.");

  secp256k1_pubkey pubkey;
  JSG_REQUIRE(secp256k1_ec_pubkey_create(context(), &pubkey, scrubbed->begin()) == 1,
      InternalDOMOperationError, "Failed to derive secp256k1 public key from generated scalar.");

  // Public keys are always extractable per WebCrypto; only the private half honours `extractable`.
  auto privateKeyAlg = keyAlgorithm;
  auto privateKeyRef = js.alloc<CryptoKey>(kj::heap<Secp256k1Key>(KeyType::PRIVATE, pubkey,
      kj::mv(scrubbed), kj::mv(privateKeyAlg), extractable, privateKeyUsages));
  auto publicKeyRef = js.alloc<CryptoKey>(kj::heap<Secp256k1Key>(
      KeyType::PUBLIC, pubkey, kj::none, kj::mv(keyAlgorithm), true, publicKeyUsages));

  return CryptoKeyPair{
    .publicKey = kj::mv(publicKeyRef),
    .privateKey = kj::mv(privateKeyRef),
  };
}

jsg::JsArrayBuffer Secp256k1Key::sign(jsg::Lock& js,
    SubtleCrypto::SignAlgorithm&& algorithm,
    kj::ArrayPtr<const kj::byte> data) const {
  JSG_REQUIRE(keyType == KeyType::PRIVATE, DOMInvalidAccessError,
      "ECDSA sign requires a private key, not \"", getType(), "\".");
  auto& seckey = KJ_ASSERT_NONNULL(privateKey);

  auto hashName = api::getAlgorithmName(JSG_REQUIRE_NONNULL(algorithm.hash, TypeError,
      "Missing \"hash\" in algorithm (ECDSA requires hash to be specified at call time)."));
  auto digest = computeDigest(hashName, data);

  // Passing nullptr for noncefp picks libsecp256k1's default nonce function, which is the
  // RFC 6979 deterministic one. The library produces signatures in low-s form by default.
  secp256k1_ecdsa_signature sig;
  JSG_REQUIRE(
      secp256k1_ecdsa_sign(context(), &sig, digest.begin(), seckey->begin(), nullptr, nullptr) == 1,
      DOMOperationError, "secp256k1 ECDSA signing failed.");

  auto out = jsg::JsArrayBuffer::create(js, kSignatureLen);
  JSG_REQUIRE(
      secp256k1_ecdsa_signature_serialize_compact(context(), out.asArrayPtr().begin(), &sig) == 1,
      DOMOperationError, "Failed to serialize secp256k1 ECDSA signature.");
  return out;
}

bool Secp256k1Key::verify(jsg::Lock& js,
    SubtleCrypto::SignAlgorithm&& algorithm,
    kj::ArrayPtr<const kj::byte> signature,
    kj::ArrayPtr<const kj::byte> data) const {
  // Malformed signatures → false (per WebCrypto spec), not an exception.
  if (signature.size() != kSignatureLen) return false;

  auto hashName = api::getAlgorithmName(JSG_REQUIRE_NONNULL(algorithm.hash, TypeError,
      "Missing \"hash\" in algorithm (ECDSA requires hash to be specified at call time)."));
  auto digest = computeDigest(hashName, data);

  secp256k1_ecdsa_signature sig;
  if (secp256k1_ecdsa_signature_parse_compact(context(), &sig, signature.begin()) != 1) {
    return false;
  }

  // `secp256k1_ecdsa_verify` rejects high-s signatures, preventing signature malleability.
  return secp256k1_ecdsa_verify(context(), &sig, digest.begin(), &parsedPubkey) == 1;
}

SubtleCrypto::ExportKeyData Secp256k1Key::exportKey(jsg::Lock& js, kj::StringPtr format) const {
  if (format == "raw") {
    JSG_REQUIRE(keyType == KeyType::PUBLIC, DOMInvalidAccessError,
        "secp256k1 \"raw\" export requires a public key, not \"", getType(), "\".");
    // Match `EllipticKey::getRawPublicKey` and WebCrypto convention: uncompressed SEC1 (65 bytes,
    // `04 || x || y`). Accepted on import are either 33-byte compressed or 65-byte uncompressed.
    auto serialized = serializePubkey(parsedPubkey, SECP256K1_EC_UNCOMPRESSED);
    return jsg::JsArrayBuffer::create(js, serialized.asPtr()).addRef(js);
  }

  if (format == "jwk") {
    auto uncompressed = serializePubkey(parsedPubkey, SECP256K1_EC_UNCOMPRESSED);
    SubtleCrypto::JsonWebKey jwk;
    jwk.kty = kj::str("EC");
    jwk.crv = kj::str("secp256k1");
    jwk.x = fastEncodeBase64Url(uncompressed.slice(1, 1 + kCoordinateLen));
    jwk.y = fastEncodeBase64Url(uncompressed.slice(1 + kCoordinateLen, kPubkeyUncompressedLen));
    KJ_IF_SOME(seckey, privateKey) {
      jwk.d = fastEncodeBase64Url(seckey->asPtr());
    }
    jwk.ext = true;
    jwk.key_ops = getUsages().map([](auto usage) { return kj::str(usage.name()); });
    return jwk;
  }

  // TODO(secp256k1): "spki"/"pkcs8" need hand-rolled DER since BoringSSL can't marshal this curve.
  JSG_FAIL_REQUIRE(DOMNotSupportedError, "Unsupported key export format for secp256k1: \"", format,
      "\" (only \"raw\" and \"jwk\" are supported).");
}

bool Secp256k1Key::equals(const CryptoKey::Impl& other) const {
  if (this == &other) return true;

  KJ_IF_SOME(that, kj::dynamicDowncastIfAvailable<const Secp256k1Key>(other)) {
    if (keyType != that.keyType) return false;

    auto thisPub = serializePubkey(parsedPubkey, SECP256K1_EC_COMPRESSED);
    auto thatPub = serializePubkey(that.parsedPubkey, SECP256K1_EC_COMPRESSED);
    if (CRYPTO_memcmp(thisPub.begin(), thatPub.begin(), kPubkeyCompressedLen) != 0) {
      return false;
    }

    // Equal public halves already imply equal private scalars for valid keys, but compare the
    // scalars directly and in constant time as defense in depth.
    if (keyType == KeyType::PRIVATE) {
      auto& a = KJ_ASSERT_NONNULL(privateKey);
      auto& b = KJ_ASSERT_NONNULL(that.privateKey);
      if (CRYPTO_memcmp(a->begin(), b->begin(), kSeckeyLen) != 0) return false;
    }
    return true;
  }
  return false;
}

}  // namespace workerd::api
