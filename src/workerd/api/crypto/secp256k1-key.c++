// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "secp256k1-key.h"

#include "impl.h"

#include <openssl/evp.h>
#include <secp256k1.h>

#include <kj/debug.h>

namespace workerd::api {

namespace {

// A singleton libsecp256k1 context used for verify-only operations.
//
// `secp256k1_context_create()` is not cheap (it generates internal lookup
// tables), so the library documentation explicitly recommends creating one
// context per process and reusing it across calls. The context is
// thread-safe for read-only operations like `secp256k1_ecdsa_verify`;
// only randomization and destruction require external synchronization, and
// we do neither after creation.
//
// `SECP256K1_CONTEXT_NONE` is the right flag here — the `_SIGN` / `_VERIFY`
// flags are deprecated no-ops in the current libsecp256k1 API.
const secp256k1_context* getSecp256k1Context() {
  static const secp256k1_context* ctx = []() {
    secp256k1_context* c = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    KJ_ASSERT(c != nullptr, "failed to create libsecp256k1 context");
    return c;
  }();
  return ctx;
}

// Serialize a `secp256k1_pubkey` to its 33-byte compressed SEC1 form.
// Used for equality comparison and raw export.
kj::Array<kj::byte> serializePubkeyCompressed(const secp256k1_pubkey& pubkey) {
  auto out = kj::heapArray<kj::byte>(33);
  size_t outLen = out.size();
  auto ret = secp256k1_ec_pubkey_serialize(
      getSecp256k1Context(), out.begin(), &outLen, &pubkey, SECP256K1_EC_COMPRESSED);
  KJ_ASSERT(ret == 1, "secp256k1_ec_pubkey_serialize should never fail on a valid parsed pubkey");
  KJ_ASSERT(outLen == 33, "compressed secp256k1 pubkey should always be 33 bytes");
  return out;
}

// Serialize a `secp256k1_pubkey` to its 65-byte uncompressed SEC1 form:
// `04 || x (32 bytes) || y (32 bytes)`. Used by JWK export, which needs the
// `x` and `y` coordinates separately.
kj::Array<kj::byte> serializePubkeyUncompressed(const secp256k1_pubkey& pubkey) {
  auto out = kj::heapArray<kj::byte>(65);
  size_t outLen = out.size();
  auto ret = secp256k1_ec_pubkey_serialize(
      getSecp256k1Context(), out.begin(), &outLen, &pubkey, SECP256K1_EC_UNCOMPRESSED);
  KJ_ASSERT(ret == 1, "secp256k1_ec_pubkey_serialize should never fail on a valid parsed pubkey");
  KJ_ASSERT(outLen == 65, "uncompressed secp256k1 pubkey should always be 65 bytes");
  return out;
}

// Compute the SHA-256-family digest of `data`, matching WebCrypto's ECDSA
// behaviour where the hash is specified at sign/verify call time. Returns
// the digest bytes (typically 32 bytes for SHA-256, 48 for SHA-384, etc.).
kj::Array<kj::byte> computeDigest(kj::StringPtr hashName, kj::ArrayPtr<const kj::byte> data) {
  const EVP_MD* md = lookupDigestAlgorithm(hashName).second;
  auto digestCtx = kj::disposeWith<EVP_MD_CTX_free>(EVP_MD_CTX_new());
  KJ_ASSERT(digestCtx.get() != nullptr);
  OSSLCALL(EVP_DigestInit_ex(digestCtx.get(), md, nullptr));
  OSSLCALL(EVP_DigestUpdate(digestCtx.get(), data.begin(), data.size()));
  auto out = kj::heapArray<kj::byte>(EVP_MD_size(md));
  unsigned int outLen = 0;
  OSSLCALL(EVP_DigestFinal_ex(digestCtx.get(), out.begin(), &outLen));
  KJ_ASSERT(outLen == out.size());
  return out;
}

}  // namespace

Secp256k1Key::Secp256k1Key(secp256k1_pubkey parsedPubkey,
    CryptoKey::EllipticKeyAlgorithm keyAlgorithm,
    bool extractable,
    CryptoKeyUsageSet usages)
    : CryptoKey::Impl(extractable, usages),
      parsedPubkey(parsedPubkey),
      keyAlgorithm(kj::mv(keyAlgorithm)) {}

kj::Own<CryptoKey::Impl> Secp256k1Key::importJwk(jsg::Lock& js,
    SubtleCrypto::JsonWebKey&& jwk,
    CryptoKey::EllipticKeyAlgorithm keyAlgorithm,
    bool extractable,
    CryptoKeyUsageSet usages) {
  // RFC 7518 §6.2: EC public keys use `kty`=EC and carry `crv`, `x`, `y`.
  // secp256k1 is not one of the NIST curves registered in the IANA JOSE
  // registry, but RFC 8812 defines `crv`="secp256k1" (and `alg`="ES256K")
  // for JOSE / COSE usage. We accept that curve string directly.

  JSG_REQUIRE(jwk.kty == "EC", DOMDataError,
      "secp256k1 \"jwk\" import requires a JSON Web Key with Key Type parameter \"kty\" (\"",
      jwk.kty, "\") equal to \"EC\".");

  auto& crv = JSG_REQUIRE_NONNULL(
      jwk.crv, DOMDataError, "Missing \"crv\" field in JSON Web Key for secp256k1 import.");
  JSG_REQUIRE(crv == "secp256k1", DOMDataError, "\"crv\" field in JSON Web Key (\"", crv,
      "\") does not match expected \"secp256k1\".");

  // If an `alg` field is present, it must be the secp256k1 JOSE identifier
  // defined by RFC 8812. We don't require it (many JWKs omit `alg`).
  KJ_IF_SOME(alg, jwk.alg) {
    JSG_REQUIRE(alg == "ES256K", DOMDataError, "JSON Web Key Algorithm parameter \"alg\" (\"", alg,
        "\") does not match the expected value \"ES256K\" for secp256k1.");
  }

  // Private-key JWKs carry a `d` component. Private keys are not yet
  // supported for secp256k1; reject clearly rather than silently discarding
  // the private material.
  JSG_REQUIRE(jwk.d == kj::none, DOMNotSupportedError,
      "Importing secp256k1 private keys (JWKs with a \"d\" field) is not yet implemented.");

  // Decode the base64url-encoded coordinates. RFC 7518 §6.2.1.2/.3 requires
  // both `x` and `y` to be the full 32-byte scalar, left-padded with zeros
  // if necessary. We accept shorter encodings and zero-pad, matching what
  // the NIST-curve ECDSA path does.
  auto xBytes = JSG_REQUIRE_NONNULL(decodeBase64Url(JSG_REQUIRE_NONNULL(kj::mv(jwk.x), DOMDataError,
                                        "Missing \"x\" field in secp256k1 JSON Web Key.")),
      DOMDataError, "Invalid base64url encoding in JSON Web Key \"x\" field.");
  auto yBytes = JSG_REQUIRE_NONNULL(decodeBase64Url(JSG_REQUIRE_NONNULL(kj::mv(jwk.y), DOMDataError,
                                        "Missing \"y\" field in secp256k1 JSON Web Key.")),
      DOMDataError, "Invalid base64url encoding in JSON Web Key \"y\" field.");

  JSG_REQUIRE(xBytes.size() <= 32 && yBytes.size() <= 32, DOMDataError,
      "secp256k1 JSON Web Key coordinates must be at most 32 bytes each.");

  // Reconstruct the 65-byte uncompressed SEC1 encoding (0x04 || x || y),
  // left-padding x and y if the JWK provided shorter encodings. Hand the
  // result to the existing raw-import path for point validation.
  kj::byte sec1[65] = {};
  sec1[0] = 0x04;
  kj::arrayPtr(sec1).slice(1 + 32 - xBytes.size(), 1 + 32).copyFrom(xBytes);
  kj::arrayPtr(sec1).slice(1 + 32 + 32 - yBytes.size(), 65).copyFrom(yBytes);

  return importRawPublic(js, kj::arrayPtr(sec1, 65), kj::mv(keyAlgorithm), extractable, usages);
}

kj::Own<CryptoKey::Impl> Secp256k1Key::importRawPublic(jsg::Lock& js,
    kj::ArrayPtr<const kj::byte> keyData,
    CryptoKey::EllipticKeyAlgorithm keyAlgorithm,
    bool extractable,
    CryptoKeyUsageSet usages) {
  // SEC1 public key encodings: 33 bytes compressed (0x02/0x03 prefix) or
  // 65 bytes uncompressed (0x04 prefix). libsecp256k1's parser accepts
  // both formats and validates that the encoded point is actually on the
  // secp256k1 curve.
  JSG_REQUIRE(keyData.size() == 33 || keyData.size() == 65, DOMDataError,
      "Invalid secp256k1 public key length (expected 33 or 65 bytes, got ", keyData.size(), ").");

  secp256k1_pubkey parsed;
  JSG_REQUIRE(secp256k1_ec_pubkey_parse(
                  getSecp256k1Context(), &parsed, keyData.begin(), keyData.size()) == 1,
      DOMDataError, "Invalid secp256k1 public key (failed to parse).");

  return kj::heap<Secp256k1Key>(parsed, kj::mv(keyAlgorithm), extractable, usages);
}

bool Secp256k1Key::verify(jsg::Lock& js,
    SubtleCrypto::SignAlgorithm&& algorithm,
    kj::ArrayPtr<const kj::byte> signature,
    kj::ArrayPtr<const kj::byte> data) const {
  // ECDSA verify requires: the hash name (specified at call time, not on the
  // key), the signature in "raw" WebCrypto format (r || s, each 32 bytes for
  // secp256k1, so 64 bytes total), and the data to verify.

  // 1. Check signature size. WebCrypto ECDSA-secp256k1 signatures are always
  //    exactly 64 bytes (32-byte r concatenated with 32-byte s). A
  //    mismatched size is simply a bad signature; per spec we return false
  //    rather than throwing.
  if (signature.size() != 64) {
    return false;
  }

  // 2. Resolve the hash algorithm. ECDSA is one of the WebCrypto algorithms
  //    that takes `hash` at verify time, so it MUST be present.
  auto hashName = api::getAlgorithmName(JSG_REQUIRE_NONNULL(algorithm.hash, TypeError,
      "Missing \"hash\" in algorithm (ECDSA requires the hash algorithm to be specified at call "
      "time)."));

  // 3. Hash the message. We delegate the hashing itself to BoringSSL — it
  //    has SHA-2 / SHA-3 / etc. libsecp256k1 deliberately does not
  //    implement hashing.
  auto digest = computeDigest(hashName, data);

  // 4. Parse the (r || s) concatenated signature into libsecp256k1's
  //    internal form. `secp256k1_ecdsa_signature_parse_compact` expects
  //    exactly 64 bytes in big-endian r then s order, which is exactly
  //    what WebCrypto produces.
  secp256k1_ecdsa_signature sig;
  if (secp256k1_ecdsa_signature_parse_compact(getSecp256k1Context(), &sig, signature.begin()) !=
      1) {
    // Signature has invalid r or s (e.g. >= curve order). Per spec,
    // malformed signatures produce a `false` return, not an exception.
    return false;
  }

  // 5. Verify. libsecp256k1 enforces low-s normalization here (signatures
  //    with s > n/2 are rejected) which matches Bitcoin / Ethereum
  //    consensus rules and prevents malleability. This is stricter than
  //    generic ECDSA but matches what essentially every secp256k1
  //    consumer wants.
  return secp256k1_ecdsa_verify(getSecp256k1Context(), &sig, digest.begin(), &parsedPubkey) == 1;
}

SubtleCrypto::ExportKeyData Secp256k1Key::exportKey(jsg::Lock& js, kj::StringPtr format) const {
  // "raw" emits the 33-byte compressed SEC1 form, matching what the rest
  // of the WebCrypto ECDSA implementation does for public keys (see
  // `EllipticKey::exportRaw` for the NIST curve analogue).
  if (format == "raw") {
    auto serialized = serializePubkeyCompressed(parsedPubkey);
    // `ExportKeyData` is a `OneOf<jsg::JsRef<JsArrayBuffer>, JsonWebKey>`,
    // so we need to wrap the freshly-created JsArrayBuffer in a JsRef via
    // `.addRef(js)` — a plain JsArrayBuffer won't implicitly convert.
    return jsg::JsArrayBuffer::create(js, serialized.asPtr()).addRef(js);
  }

  // "jwk" emits RFC 7517 / RFC 7518 / RFC 8812 JSON Web Key form.
  if (format == "jwk") {
    auto uncompressed = serializePubkeyUncompressed(parsedPubkey);
    // uncompressed[0] is the 0x04 SEC1 tag; the following 32 bytes are x,
    // then 32 bytes of y.
    auto xBytes = uncompressed.slice(1, 33);
    auto yBytes = uncompressed.slice(33, 65);

    SubtleCrypto::JsonWebKey jwk;
    jwk.kty = kj::str("EC");
    jwk.crv = kj::str("secp256k1");
    jwk.x = fastEncodeBase64Url(xBytes);
    jwk.y = fastEncodeBase64Url(yBytes);
    // `ext` and `key_ops` are set by `SubtleCrypto::exportKey`'s caller in
    // the AsymmetricKeyCryptoKeyImpl path, but since we inherit directly
    // from `CryptoKey::Impl`, we set them ourselves here. See the parallel
    // code in AsymmetricKeyCryptoKeyImpl::exportKey.
    jwk.ext = true;
    jwk.key_ops = getUsages().map([](auto usage) { return kj::str(usage.name()); });
    return jwk;
  }

  // TODO(secp256k1): implement "spki" public-key export (requires writing
  // a small DER emitter since BoringSSL's EVP_marshal_public_key can't
  // handle this curve).
  JSG_FAIL_REQUIRE(DOMNotSupportedError, "Unsupported key export format for secp256k1: \"", format,
      "\" (only \"raw\" and \"jwk\" are supported).");
}

bool Secp256k1Key::equals(const CryptoKey::Impl& other) const {
  // Two secp256k1 keys are equal iff they have the same algorithm name,
  // same curve, same key type, and same serialized public key bytes.
  if (other.getAlgorithmName() != getAlgorithmName()) return false;
  if (other.getType() != getType()) return false;

  auto* downcast = dynamic_cast<const Secp256k1Key*>(&other);
  if (downcast == nullptr) return false;

  auto thisBytes = serializePubkeyCompressed(parsedPubkey);
  auto thatBytes = serializePubkeyCompressed(downcast->parsedPubkey);
  return thisBytes.asPtr() == thatBytes.asPtr();
}

}  // namespace workerd::api
