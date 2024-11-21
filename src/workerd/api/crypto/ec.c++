// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "ec.h"

#include "impl.h"
#include "keys.h"

#include <workerd/api/util.h>
#include <workerd/io/features.h>

#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/curve25519.h>
#include <openssl/ec_key.h>
#include <openssl/x509.h>

#include <kj/function.h>

#include <map>
#include <type_traits>

namespace workerd::api {

Ec::Ec(EC_KEY* key): key(key), x(OSSL_NEW(BIGNUM)), y(OSSL_NEW(BIGNUM)) {
  KJ_ASSERT(key != nullptr);
  group = EC_KEY_get0_group(key);
  JSG_REQUIRE(
      1 == EC_POINT_get_affine_coordinates(group, getPublicKey(), x.get(), y.get(), nullptr),
      InternalDOMOperationError, "Error getting affine coordinates for export",
      internalDescribeOpensslErrors());
}

int Ec::getCurveName() const {
  return EC_GROUP_get_curve_name(group);
}

uint32_t Ec::getDegree() const {
  return EC_GROUP_get_degree(getGroup());
}

const EC_POINT* Ec::getPublicKey() const {
  return EC_KEY_get0_public_key(key);
}

const BIGNUM* Ec::getPrivateKey() const {
  return EC_KEY_get0_private_key(key);
}

SubtleCrypto::JsonWebKey Ec::toJwk(KeyType keyType, kj::StringPtr curveName) const {
  JSG_REQUIRE(group != nullptr, DOMOperationError, "No elliptic curve group in this key",
      tryDescribeOpensslErrors());
  JSG_REQUIRE(getPublicKey() != nullptr, DOMOperationError,
      "No public elliptic curve key data in this key", tryDescribeOpensslErrors());

  auto groupDegreeInBytes = integerCeilDivision(getDegree(), 8u);
  // EC_GROUP_get_degree returns number of bits. We need this because x, y, & d need to match the
  // group degree according to JWK.

  SubtleCrypto::JsonWebKey jwk;
  jwk.kty = kj::str("EC");
  jwk.crv = kj::str(curveName);

  static constexpr auto handleBn = [](const BIGNUM& bn, size_t size) {
    return JSG_REQUIRE_NONNULL(bignumToArrayPadded(bn, size), InternalDOMOperationError,
        "Error converting EC affine co-ordinates to padded array", internalDescribeOpensslErrors());
  };

  auto xa = handleBn(*x, groupDegreeInBytes);
  auto ya = handleBn(*y, groupDegreeInBytes);

  jwk.x = fastEncodeBase64Url(xa);
  jwk.y = fastEncodeBase64Url(ya);

  if (keyType == KeyType::PRIVATE) {
    const auto privateKey = getPrivateKey();
    JSG_REQUIRE(privateKey != nullptr, InternalDOMOperationError,
        "Error getting private key material for JSON Web Key export",
        internalDescribeOpensslErrors());
    auto pk = handleBn(*privateKey, groupDegreeInBytes);
    jwk.d = fastEncodeBase64Url(pk);
  }
  return jwk;
}

jsg::BufferSource Ec::getRawPublicKey(jsg::Lock& js) const {
  JSG_REQUIRE_NONNULL(group, InternalDOMOperationError, "No elliptic curve group in this key",
      tryDescribeOpensslErrors());
  auto publicKey = getPublicKey();
  JSG_REQUIRE(publicKey != nullptr, InternalDOMOperationError,
      "No public elliptic curve key data in this key", tryDescribeOpensslErrors());

  // Serialize the public key as an uncompressed point in X9.62 form.
  uint8_t* raw;
  size_t raw_len;
  CBB cbb;

  JSG_REQUIRE(1 == CBB_init(&cbb, 0), InternalDOMOperationError, "Failed to init CBB",
      internalDescribeOpensslErrors());
  KJ_DEFER(CBB_cleanup(&cbb));

  JSG_REQUIRE(
      1 == EC_POINT_point2cbb(&cbb, group, publicKey, POINT_CONVERSION_UNCOMPRESSED, nullptr),
      InternalDOMOperationError, "Failed to convert to serialize EC key",
      internalDescribeOpensslErrors());

  JSG_REQUIRE(1 == CBB_finish(&cbb, &raw, &raw_len), InternalDOMOperationError,
      "Failed to finish CBB", internalDescribeOpensslErrors());

  auto backing = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, raw_len);
  auto src = kj::arrayPtr(raw, raw_len);
  backing.asArrayPtr().copyFrom(src);
  return jsg::BufferSource(js, kj::mv(backing));
}

CryptoKey::AsymmetricKeyDetails Ec::getAsymmetricKeyDetail() const {
  // Adapted from Node.js' GetEcKeyDetail
  return CryptoKey::AsymmetricKeyDetails{
    .namedCurve = kj::str(OBJ_nid2sn(EC_GROUP_get_curve_name(group)))};
}

kj::Maybe<Ec> Ec::tryGetEc(const EVP_PKEY* key) {
  int type = EVP_PKEY_id(key);
  if (type != EVP_PKEY_EC) return kj::none;
  auto ec = EVP_PKEY_get0_EC_KEY(key);
  if (ec == nullptr) return kj::none;
  return Ec(ec);
}

// =====================================================================================
// ECDSA & ECDH

namespace {

class EllipticKey final: public AsymmetricKeyCryptoKeyImpl {
 public:
  explicit EllipticKey(AsymmetricKeyData keyData,
      CryptoKey::EllipticKeyAlgorithm keyAlgorithm,
      uint rsSize,
      bool extractable)
      : AsymmetricKeyCryptoKeyImpl(kj::mv(keyData), extractable),
        keyAlgorithm(kj::mv(keyAlgorithm)),
        rsSize(rsSize) {}

  CryptoKey::AlgorithmVariant getAlgorithm(jsg::Lock& js) const override {
    return keyAlgorithm;
  }
  kj::StringPtr getAlgorithmName() const override {
    return keyAlgorithm.name;
  }

  void requireSigningAbility() const {
    // This assert is internal to our WebCrypto implementation because we share the AsymmetricKey
    // implementation between ECDH & ECDSA (the former only supports deriveBits/deriveKey, not
    // signing which is the usage for this function).
    JSG_REQUIRE(keyAlgorithm.name == "ECDSA", DOMNotSupportedError,
        "The sign and verify operations are not implemented for \"", keyAlgorithm.name, "\".");
  }

  kj::StringPtr chooseHash(
      const kj::Maybe<kj::OneOf<kj::String, SubtleCrypto::HashAlgorithm>>& callTimeHash)
      const override {
    requireSigningAbility();

    // ECDSA infamously expects the hash to be specified at call time.
    // See: https://github.com/w3c/webcrypto/issues/111
    return api::getAlgorithmName(JSG_REQUIRE_NONNULL(callTimeHash, TypeError,
        "Missing \"hash\" in AlgorithmIdentifier. (ECDSA requires that the hash algorithm be "
        "specified at call time rather than on the key. This differs from other WebCrypto "
        "algorithms for historical reasons.)"));
  }

  jsg::BufferSource deriveBits(jsg::Lock& js,
      SubtleCrypto::DeriveKeyAlgorithm&& algorithm,
      kj::Maybe<uint32_t> resultBitLength) const override final {
    JSG_REQUIRE(keyAlgorithm.name == "ECDH", DOMNotSupportedError,
        ""
        "The deriveBits operation is not implemented for \"",
        keyAlgorithm.name, "\".");

    JSG_REQUIRE(getTypeEnum() == KeyType::PRIVATE, DOMInvalidAccessError,
        ""
        "The deriveBits operation is only valid for a private key, not \"",
        getType(), "\".");

    auto& publicKey = JSG_REQUIRE_NONNULL(
        algorithm.$public, TypeError, "Missing field \"public\" in \"derivedKeyParams\".");

    JSG_REQUIRE(publicKey->getType() == "public"_kj, DOMInvalidAccessError,
        ""
        "The provided key has type \"",
        publicKey->getType(), "\", not \"public\"");

    JSG_REQUIRE(getAlgorithm(js).which() == publicKey->getAlgorithm(js).which(),
        DOMInvalidAccessError, "Base ", getAlgorithmName(),
        " private key cannot be used to derive"
        " a key from a peer ",
        publicKey->getAlgorithmName(), " public key");

    JSG_REQUIRE(getAlgorithmName() == publicKey->getAlgorithmName(), DOMInvalidAccessError,
        "Private key for derivation is using \"", getAlgorithmName(),
        "\" while public key is using \"", publicKey->getAlgorithmName(), "\".");

    auto publicCurve =
        publicKey->getAlgorithm(js).get<CryptoKey::EllipticKeyAlgorithm>().namedCurve;
    JSG_REQUIRE(keyAlgorithm.namedCurve == publicCurve, DOMInvalidAccessError,
        "Private key for derivation is using curve \"", keyAlgorithm.namedCurve,
        "\" while public key is using \"", publicCurve, "\".");

    // The check above for the algorithm `which` equality ensures that the impl can be downcast to
    // EllipticKey (assuming we don't accidentally create a class that doesn't inherit this one that
    // for some reason returns an EllipticKey).
    auto& publicKeyImpl = kj::downcast<EllipticKey>(*publicKey->impl);

    // Adapted from https://wiki.openssl.org/index.php/Elliptic_Curve_Diffie_Hellman:
    auto privateEcKey = JSG_REQUIRE_NONNULL(Ec::tryGetEc(getEvpPkey()), InternalDOMOperationError,
        "No elliptic curve data backing key", tryDescribeOpensslErrors());
    auto publicEcKey =
        JSG_REQUIRE_NONNULL(Ec::tryGetEc(publicKeyImpl.getEvpPkey()), InternalDOMOperationError,
            "No elliptic curve data backing key", tryDescribeOpensslErrors());
    JSG_REQUIRE(publicEcKey.getPublicKey() != nullptr, DOMOperationError,
        "No public elliptic curve key data in this key", tryDescribeOpensslErrors());
    auto fieldSize = privateEcKey.getDegree();

    // Assuming that `fieldSize` will always be a sane value since it's related to the keys we
    // construct in C++ (i.e. not untrusted user input).

    kj::Vector<kj::byte> sharedSecret;
    sharedSecret.resize(
        integerCeilDivision<std::make_unsigned<decltype(fieldSize)>::type>(fieldSize, 8u));
    auto written = ECDH_compute_key(sharedSecret.begin(), sharedSecret.capacity(),
        publicEcKey.getPublicKey(), privateEcKey.getKey(), nullptr);
    JSG_REQUIRE(written > 0, DOMOperationError, "Failed to generate shared ECDH secret",
        tryDescribeOpensslErrors());

    sharedSecret.resize(written);

    auto outputBitLength = resultBitLength.orDefault(sharedSecret.size() * 8);
    JSG_REQUIRE(outputBitLength <= sharedSecret.size() * 8, DOMOperationError,
        "Derived key length (", outputBitLength, " bits) is too long (should be at most ",
        sharedSecret.size() * 8, " bits).");

    // Round up since outputBitLength may not be a perfect multiple of 8.
    // However, the last byte may now have bits that have leaked which we handle below.
    auto resultByteLength = integerCeilDivision(outputBitLength, 8u);
    sharedSecret.truncate(resultByteLength);

    // We have to remember to mask off the bits that weren't requested (if a non multiple of 8 was
    // passed in). NOTE: The conformance tests DO NOT appear to test for this. This is my reading of
    // the spec, combining:
    //   * ECDH: Return an octet string containing the first length bits of secret.
    //   * octet string: b is the octet string obtained by first appending zero or more bits of
    //                   value zero to b such that the length of the resulting bit string is minimal
    //                   and an integer multiple of 8.
    auto numBitsToMaskOff = resultByteLength * 8 - outputBitLength;
    KJ_DASSERT(numBitsToMaskOff < 8, numBitsToMaskOff);

    // The mask should have `numBitsToMaskOff` bits set to 0 from least significant to most.
    // 0 = 1 1 1 1 1 1 1 1 (0xFF)
    // 1 = 1 1 1 1 1 1 1 0 (0xFE)
    // 2 = 1 1 1 1 1 1 0 0 (0xFD)
    // 3 = 1 1 1 1 1 0 0 0 (0xFC)
    // Let's rewrite this to have the lower bits set to 1 since that's typically the easier form to
    // generate with bit twiddling.
    // 0 = 0 0 0 0 0 0 0 0 (0)
    // 1 = 0 0 0 0 0 0 0 1 (1)
    // 2 = 0 0 0 0 0 0 1 1 (3)
    // 3 = 0 0 0 0 0 1 1 1 (7)
    // The pattern seems pretty clearly ~(2^n - 1) where n is the number of bits to mask off. Let's
    // check the last one though (8 is not a possible boundary condition).
    // (2^7 - 1) = 0x7f => ~0x7f = 0x80 (when truncated to a byte)
    uint8_t mask = ~((1 << numBitsToMaskOff) - 1);

    sharedSecret.back() &= mask;

    auto backing = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, sharedSecret.size());
    backing.asArrayPtr().copyFrom(sharedSecret);
    return jsg::BufferSource(js, kj::mv(backing));
  }

  jsg::BufferSource signatureSslToWebCrypto(
      jsg::Lock& js, kj::Array<kj::byte> signature) const override {
    // An EC signature is two big integers "r" and "s". WebCrypto wants us to just concatenate both
    // integers, using a constant size of each that depends on the curve size. OpenSSL wants to
    // encode them in some ASN.1 wrapper with variable-width sizes. Ugh.

    requireSigningAbility();

    // Manually decode ASN.1 BER.
    KJ_ASSERT(signature.size() >= 6);
    KJ_ASSERT(signature[0] == 0x30);
    kj::ArrayPtr<const kj::byte> rest;
    if (signature[1] < 128) {
      KJ_ASSERT(signature[1] == signature.size() - 2);
      rest = signature.slice(2, signature.size());
    } else {
      // Size of message did not fit in 7 bits, so the first byte encodes the size-of-size, but it
      // will always fit in 8 bits so the size-of-size will always be 1 (plus 128 because top bit
      // is set).
      KJ_ASSERT(signature[1] == 129);
      KJ_ASSERT(signature[2] == signature.size() - 3);
      rest = signature.slice(3, signature.size());
    }

    KJ_ASSERT(rest.size() >= 2);
    KJ_ASSERT(rest[0] == 0x02);
    size_t rSize = rest[1];
    KJ_ASSERT(rest.size() >= 2 + rSize);
    auto r = rest.slice(2, 2 + rSize);

    rest = rest.slice(2 + rSize, rest.size());

    KJ_ASSERT(rest.size() >= 2);
    KJ_ASSERT(rest[0] == 0x02);
    size_t sSize = rest[1];
    KJ_ASSERT(rest.size() == 2 + sSize);
    auto s = rest.slice(2, 2 + sSize);

    // If the top bit is set, BER encoding will add an extra 0-byte prefix to disambiguate from a
    // negative number. Uggghhh.
    while (r.size() > rsSize && r[0] == 0) r = r.slice(1, r.size());
    while (s.size() > rsSize && s[0] == 0) s = s.slice(1, s.size());
    KJ_ASSERT(r.size() <= rsSize);
    KJ_ASSERT(s.size() <= rsSize);

    // Construct WebCrypto format.
    auto out = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, rsSize * 2);

    // We're dealing with big-endian, so we have to align the copy to the right. This is exactly
    // why big-endian is the wrong edian.
    memcpy(out.asArrayPtr().begin() + rsSize - r.size(), r.begin(), r.size());
    memcpy(out.asArrayPtr().end() - s.size(), s.begin(), s.size());
    return jsg::BufferSource(js, kj::mv(out));
  }

  jsg::BufferSource signatureWebCryptoToSsl(
      jsg::Lock& js, kj::ArrayPtr<const kj::byte> signature) const override {
    requireSigningAbility();

    if (signature.size() != rsSize * 2) {
      // The signature is the wrong size. Return an empty signature, which will be judged invalid.
      auto backing = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, 0);
      return jsg::BufferSource(js, kj::mv(backing));
    }

    auto r = signature.first(rsSize);
    auto s = signature.slice(rsSize, signature.size());

    // Trim leading zeros.
    while (r.size() > 1 && r[0] == 0) r = r.slice(1, r.size());
    while (s.size() > 1 && s[0] == 0) s = s.slice(1, s.size());

    // If the most significant bit is set, we have to add a zero, ugh.
    bool padR = r[0] >= 128;
    bool padS = s[0] >= 128;

    size_t bodySize = 4 + padR + padS + r.size() + s.size();
    size_t resultSize = 2 + bodySize + (bodySize >= 128);
    auto result = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, resultSize);

    kj::byte* pos = result.asArrayPtr().begin();
    *pos++ = 0x30;
    if (bodySize < 128) {
      *pos++ = bodySize;
    } else {
      *pos++ = 129;
      *pos++ = bodySize;
    }

    *pos++ = 0x02;
    *pos++ = r.size() + padR;
    if (padR) *pos++ = 0;
    memcpy(pos, r.begin(), r.size());
    pos += r.size();

    *pos++ = 0x02;
    *pos++ = s.size() + padS;
    if (padS) *pos++ = 0;
    memcpy(pos, s.begin(), s.size());
    pos += s.size();

    KJ_ASSERT(pos == result.asArrayPtr().end());

    return jsg::BufferSource(js, kj::mv(result));
  }

  static kj::OneOf<jsg::Ref<CryptoKey>, CryptoKeyPair> generateElliptic(
      kj::StringPtr normalizedName,
      SubtleCrypto::GenerateKeyAlgorithm&& algorithm,
      bool extractable,
      CryptoKeyUsageSet privateKeyUsages,
      CryptoKeyUsageSet publicKeyUsages);

  kj::StringPtr jsgGetMemoryName() const override {
    return "EllipticKey";
  }
  size_t jsgGetMemorySelfSize() const override {
    return sizeof(EllipticKey);
  }
  void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const override {
    AsymmetricKeyCryptoKeyImpl::jsgGetMemoryInfo(tracker);
    tracker.trackField("keyAlgorithm", keyAlgorithm);
  }

 private:
  SubtleCrypto::JsonWebKey exportJwk() const override final {
    auto ec = JSG_REQUIRE_NONNULL(Ec::tryGetEc(getEvpPkey()), DOMOperationError,
        "No elliptic curve data backing key", tryDescribeOpensslErrors());
    return ec.toJwk(getTypeEnum(), kj::str(keyAlgorithm.namedCurve));
  }

  jsg::BufferSource exportRaw(jsg::Lock& js) const override final {
    JSG_REQUIRE(getTypeEnum() == KeyType::PUBLIC, DOMInvalidAccessError,
        "Raw export of elliptic curve keys is only allowed for public keys.");
    return JSG_REQUIRE_NONNULL(Ec::tryGetEc(getEvpPkey()), InternalDOMOperationError,
        "No elliptic curve data backing key", tryDescribeOpensslErrors())
        .getRawPublicKey(js);
  }

  CryptoKey::AsymmetricKeyDetails getAsymmetricKeyDetail() const override {
    // Adapted from Node.js' GetEcKeyDetail
    return KJ_ASSERT_NONNULL(Ec::tryGetEc(getEvpPkey())).getAsymmetricKeyDetail();
  }

  CryptoKey::EllipticKeyAlgorithm keyAlgorithm;
  uint rsSize;
};

struct EllipticCurveInfo {
  kj::StringPtr normalizedName;
  int opensslCurveId;
  uint rsSize;  // size of "r" and "s" in the signature
};

EllipticCurveInfo lookupEllipticCurve(kj::StringPtr curveName) {
  static const std::map<kj::StringPtr, EllipticCurveInfo, CiLess> registeredCurves{
    {"P-256", {"P-256", NID_X9_62_prime256v1, 32}},
    {"P-384", {"P-384", NID_secp384r1, 48}},
    {"P-521", {"P-521", NID_secp521r1, 66}},
  };

  auto iter = registeredCurves.find(curveName);
  JSG_REQUIRE(iter != registeredCurves.end(), DOMNotSupportedError,
      "Unrecognized or unimplemented EC curve \"", curveName, "\" requested.");
  return iter->second;
}

kj::OneOf<jsg::Ref<CryptoKey>, CryptoKeyPair> EllipticKey::generateElliptic(
    kj::StringPtr normalizedName,
    SubtleCrypto::GenerateKeyAlgorithm&& algorithm,
    bool extractable,
    CryptoKeyUsageSet privateKeyUsages,
    CryptoKeyUsageSet publicKeyUsages) {
  kj::StringPtr namedCurve = JSG_REQUIRE_NONNULL(
      algorithm.namedCurve, TypeError, "Missing field \"namedCurve\" in \"algorithm\".");

  auto [normalizedNamedCurve, curveId, rsSize] = lookupEllipticCurve(namedCurve);

  auto keyAlgorithm = CryptoKey::EllipticKeyAlgorithm{
    normalizedName,
    normalizedNamedCurve,
  };

  // Used OpenBSD man pages starting with https://man.openbsd.org/ECDSA_SIG_new.3 for functions and
  // CryptoKey::Impl::generateRsa as a template.
  // https://stackoverflow.com/questions/18155559/how-does-one-access-the-raw-ecdh-public-key-private-key-and-params-inside-opens
  // for the reference on how to deserialize the public/private key.

  auto ecPrivateKey =
      OSSLCALL_OWN(EC_KEY, EC_KEY_new_by_curve_name(curveId), InternalDOMOperationError,
          "Error generating EC \"", namedCurve, "\" key", internalDescribeOpensslErrors());
  OSSLCALL(EC_KEY_generate_key(ecPrivateKey));

  auto privateEvpPKey = OSSL_NEW(EVP_PKEY);
  OSSLCALL(EVP_PKEY_set1_EC_KEY(privateEvpPKey.get(), ecPrivateKey.get()));

  auto ecPublicKey =
      OSSLCALL_OWN(EC_KEY, EC_KEY_new_by_curve_name(curveId), InternalDOMOperationError,
          "Error generating EC \"", namedCurve, "\" key", internalDescribeOpensslErrors());
  OSSLCALL(EC_KEY_set_public_key(ecPublicKey, EC_KEY_get0_public_key(ecPrivateKey)));
  auto publicEvpPKey = OSSL_NEW(EVP_PKEY);
  OSSLCALL(EVP_PKEY_set1_EC_KEY(publicEvpPKey.get(), ecPublicKey.get()));

  AsymmetricKeyData privateKeyData{
    .evpPkey = kj::mv(privateEvpPKey),
    .keyType = KeyType::PRIVATE,
    .usages = privateKeyUsages,
  };
  AsymmetricKeyData publicKeyData{
    .evpPkey = kj::mv(publicEvpPKey),
    .keyType = KeyType::PUBLIC,
    .usages = publicKeyUsages,
  };

  auto privateKey = jsg::alloc<CryptoKey>(
      kj::heap<EllipticKey>(kj::mv(privateKeyData), keyAlgorithm, rsSize, extractable));
  auto publicKey = jsg::alloc<CryptoKey>(
      kj::heap<EllipticKey>(kj::mv(publicKeyData), keyAlgorithm, rsSize, true));

  return CryptoKeyPair{.publicKey = kj::mv(publicKey), .privateKey = kj::mv(privateKey)};
}

AsymmetricKeyData importEllipticRaw(SubtleCrypto::ImportKeyData keyData,
    int curveId,
    kj::StringPtr normalizedName,
    kj::ArrayPtr<const kj::String> keyUsages,
    CryptoKeyUsageSet allowedUsages) {
  // Import an elliptic key represented by raw data, only public keys are supported.
  JSG_REQUIRE(keyData.is<kj::Array<kj::byte>>(), DOMDataError,
      "Expected raw EC key but instead got a Json Web Key.");

  const auto& raw = keyData.get<kj::Array<kj::byte>>();

  auto usages = CryptoKeyUsageSet::validate(
      normalizedName, CryptoKeyUsageSet::Context::importPublic, keyUsages, allowedUsages);

  if (curveId == NID_ED25519 || curveId == NID_X25519) {
    auto evpId = curveId == NID_X25519 ? EVP_PKEY_X25519 : EVP_PKEY_ED25519;
    auto curveName = curveId == NID_X25519 ? "X25519" : "Ed25519";

    JSG_REQUIRE(raw.size() == 32, DOMDataError, curveName,
        " raw keys must be exactly 32-bytes "
        "(provided ",
        raw.size(), ").");

    return {
      OSSLCALL_OWN(EVP_PKEY, EVP_PKEY_new_raw_public_key(evpId, nullptr, raw.begin(), raw.size()),
          InternalDOMOperationError, "Failed to import raw public EDDSA", raw.size(),
          internalDescribeOpensslErrors()),
      KeyType::PUBLIC, usages};
  }

  auto ecKey = OSSLCALL_OWN(EC_KEY, EC_KEY_new_by_curve_name(curveId), DOMOperationError,
      "Error importing EC key", tryDescribeOpensslErrors());
  auto ecGroup = EC_KEY_get0_group(ecKey.get());

  auto point = OSSL_NEW(EC_POINT, ecGroup);
  JSG_REQUIRE(1 == EC_POINT_oct2point(ecGroup, point.get(), raw.begin(), raw.size(), nullptr),
      DOMDataError, "Failed to import raw EC key data", tryDescribeOpensslErrors());
  JSG_REQUIRE(1 == EC_KEY_set_public_key(ecKey.get(), point.get()), InternalDOMOperationError,
      "Failed to set EC raw public key", internalDescribeOpensslErrors());
  JSG_REQUIRE(1 == EC_KEY_check_key(ecKey.get()), DOMDataError, "Invalid raw EC key provided",
      tryDescribeOpensslErrors());

  auto evpPkey = OSSL_NEW(EVP_PKEY);
  OSSLCALL(EVP_PKEY_set1_EC_KEY(evpPkey.get(), ecKey.get()));

  return AsymmetricKeyData{kj::mv(evpPkey), KeyType::PUBLIC, usages};
}

kj::Own<EVP_PKEY> ellipticJwkReader(
    int curveId, SubtleCrypto::JsonWebKey&& keyDataJwk, kj::StringPtr normalizedName) {
  if (curveId == NID_ED25519 || curveId == NID_X25519) {
    auto evpId = curveId == NID_X25519 ? EVP_PKEY_X25519 : EVP_PKEY_ED25519;
    auto curveName = curveId == NID_X25519 ? "X25519" : "Ed25519";

    JSG_REQUIRE(keyDataJwk.kty == "OKP", DOMDataError, curveName,
        " \"jwk\" key imports requires a JSON Web Key with Key Type parameter "
        "\"kty\" (\"",
        keyDataJwk.kty, "\") equal to \"OKP\".");
    auto& crv = JSG_REQUIRE_NONNULL(
        keyDataJwk.crv, DOMDataError, "Missing field \"crv\" for ", curveName, " key.");
    JSG_REQUIRE(crv == curveName, DOMNotSupportedError, "Only ", curveName, " is supported but \"",
        crv, "\" was requested.");
    KJ_IF_SOME(alg, keyDataJwk.alg) {
      // If this JWK specifies an algorithm, make sure it jives with the hash we were passed via
      // importKey().
      if (curveId == NID_ED25519) {
        JSG_REQUIRE(alg == "EdDSA", DOMDataError, "JSON Web Key Algorithm parameter \"alg\" (\"",
            alg,
            "\") does not match requested "
            "Ed25519 curve.");
      }
    }

    auto x = UNWRAP_JWK_BIGNUM(kj::mv(keyDataJwk.x), DOMDataError, "Invalid ", crv,
        " key in JSON WebKey; missing or invalid public key component (\"x\").");
    JSG_REQUIRE(x.size() == 32, DOMDataError, "Invalid length ", x.size(), " for public key");

    if (keyDataJwk.d == kj::none) {
      // This is a public key.
      return OSSLCALL_OWN(EVP_PKEY,
          EVP_PKEY_new_raw_public_key(evpId, nullptr, x.begin(), x.size()),
          InternalDOMOperationError, "Failed to construct ", crv, " public key",
          internalDescribeOpensslErrors());
    }

    // This is a private key. The Section 2 of the RFC says...
    // >  The parameter "x" MUST be present and contain the public key encoded using the base64url
    // >  [RFC4648] encoding.
    // https://tools.ietf.org/html/draft-ietf-jose-cfrg-curves-06
    // ... but there's nothing really to do beside enforce that it's set? The NodeJS implementation
    // seems to throw it away when a private key is provided.

    auto d = UNWRAP_JWK_BIGNUM(kj::mv(keyDataJwk.d), DOMDataError, "Invalid ", curveName,
        " key in JSON Web Key; missing or invalid private key component (\"d\").");
    JSG_REQUIRE(d.size() == 32, DOMDataError, "Invalid length ", d.size(), " for private key");

    return OSSLCALL_OWN(EVP_PKEY, EVP_PKEY_new_raw_private_key(evpId, nullptr, d.begin(), d.size()),
        InternalDOMOperationError, "Failed to construct ", crv, " private key",
        internalDescribeOpensslErrors());
  }

  JSG_REQUIRE(keyDataJwk.kty == "EC", DOMDataError,
      "Elliptic curve \"jwk\" key import requires a JSON Web Key with Key Type parameter "
      "\"kty\" (\"",
      keyDataJwk.kty, "\") equal to \"EC\".");

  if (normalizedName == "ECDSA") {
    KJ_IF_SOME(alg, keyDataJwk.alg) {
      // If this JWK specifies an algorithm, make sure it jives with the hash we were passed via
      // importKey().
      static const std::map<kj::StringPtr, int> ecdsaAlgorithms{
        {"ES256", NID_X9_62_prime256v1},
        {"ES384", NID_secp384r1},
        {"ES512", NID_secp521r1},
      };

      auto iter = ecdsaAlgorithms.find(alg);
      JSG_REQUIRE(iter != ecdsaAlgorithms.end(), DOMNotSupportedError,
          "Unrecognized or unimplemented algorithm \"", alg,
          "\" listed in JSON Web Key Algorithm parameter.");

      JSG_REQUIRE(iter->second == curveId, DOMDataError,
          "JSON Web Key Algorithm parameter \"alg\" (\"", alg,
          "\") does not match requested curve.");
    }
  }

  auto ecKey = OSSLCALL_OWN(EC_KEY, EC_KEY_new_by_curve_name(curveId), DOMOperationError,
      "Error importing EC key", tryDescribeOpensslErrors());

  auto x = UNWRAP_JWK_BIGNUM(
      kj::mv(keyDataJwk.x), DOMDataError, "Invalid EC key in JSON Web Key; missing \"x\".");
  auto y = UNWRAP_JWK_BIGNUM(
      kj::mv(keyDataJwk.y), DOMDataError, "Invalid EC key in JSON Web Key; missing \"y\".");

  auto group = EC_KEY_get0_group(ecKey);

  auto bigX = JSG_REQUIRE_NONNULL(toBignum(x), InternalDOMOperationError, "Error importing EC key",
      internalDescribeOpensslErrors());
  auto bigY = JSG_REQUIRE_NONNULL(toBignum(y), InternalDOMOperationError, "Error importing EC key",
      internalDescribeOpensslErrors());

  auto point = OSSL_NEW(EC_POINT, group);
  OSSLCALL(EC_POINT_set_affine_coordinates_GFp(group, point, bigX, bigY, nullptr));
  OSSLCALL(EC_KEY_set_public_key(ecKey, point));

  if (keyDataJwk.d != kj::none) {
    // This is a private key.

    auto d = UNWRAP_JWK_BIGNUM(kj::mv(keyDataJwk.d), DOMDataError,
        "Invalid EC key in JSON Web Key; missing or invalid private key component (\"d\").");

    auto bigD = JSG_REQUIRE_NONNULL(toBignum(d), InternalDOMOperationError,
        "Error importing EC key", internalDescribeOpensslErrors());

    OSSLCALL(EC_KEY_set_private_key(ecKey, bigD));
  }

  auto evpPkey = OSSL_NEW(EVP_PKEY);
  OSSLCALL(EVP_PKEY_set1_EC_KEY(evpPkey.get(), ecKey.get()));
  return evpPkey;
}
}  // namespace

kj::OneOf<jsg::Ref<CryptoKey>, CryptoKeyPair> CryptoKey::Impl::generateEcdsa(jsg::Lock& js,
    kj::StringPtr normalizedName,
    SubtleCrypto::GenerateKeyAlgorithm&& algorithm,
    bool extractable,
    kj::ArrayPtr<const kj::String> keyUsages) {
  auto usages = CryptoKeyUsageSet::validate(normalizedName, CryptoKeyUsageSet::Context::generate,
      keyUsages, CryptoKeyUsageSet::sign() | CryptoKeyUsageSet::verify());
  auto privateKeyUsages = usages & CryptoKeyUsageSet::privateKeyMask();
  auto publicKeyUsages = usages & CryptoKeyUsageSet::publicKeyMask();

  return EllipticKey::generateElliptic(
      normalizedName, kj::mv(algorithm), extractable, privateKeyUsages, publicKeyUsages);
}

kj::Own<CryptoKey::Impl> CryptoKey::Impl::importEcdsa(jsg::Lock& js,
    kj::StringPtr normalizedName,
    kj::StringPtr format,
    SubtleCrypto::ImportKeyData keyData,
    SubtleCrypto::ImportKeyAlgorithm&& algorithm,
    bool extractable,
    kj::ArrayPtr<const kj::String> keyUsages) {
  kj::StringPtr namedCurve = JSG_REQUIRE_NONNULL(
      algorithm.namedCurve, TypeError, "Missing field \"namedCurve\" in \"algorithm\".");

  auto [normalizedNamedCurve, curveId, rsSize] = lookupEllipticCurve(namedCurve);

  auto importedKey = [&, curveId = curveId] {
    if (format != "raw") {
      return importAsymmetricForWebCrypto(js, format, kj::mv(keyData), normalizedName, extractable,
          keyUsages,
          // Verbose lambda capture needed because: https://bugs.llvm.org/show_bug.cgi?id=35984
          [curveId = curveId, normalizedName = kj::str(normalizedName)](
              SubtleCrypto::JsonWebKey keyDataJwk) -> kj::Own<EVP_PKEY> {
        return ellipticJwkReader(curveId, kj::mv(keyDataJwk), normalizedName);
      },
          CryptoKeyUsageSet::sign() | CryptoKeyUsageSet::verify());
    } else {
      return importEllipticRaw(
          kj::mv(keyData), curveId, normalizedName, keyUsages, CryptoKeyUsageSet::verify());
    }
  }();

  // get0 avoids adding a refcount...
  auto ecKey = JSG_REQUIRE_NONNULL(Ec::tryGetEc(importedKey.evpPkey.get()), DOMDataError,
      "Input was not an EC key", tryDescribeOpensslErrors());

  // Verify namedCurve matches what was specified in the key data.
  JSG_REQUIRE(ecKey.getGroup() != nullptr && ecKey.getCurveName() == curveId, DOMDataError,
      "\"algorithm.namedCurve\" \"", namedCurve,
      "\" does not match the curve specified by the "
      "input key data",
      tryDescribeOpensslErrors());

  auto keyAlgorithm = CryptoKey::EllipticKeyAlgorithm{
    normalizedName,
    normalizedNamedCurve,
  };

  return kj::heap<EllipticKey>(kj::mv(importedKey), kj::mv(keyAlgorithm), rsSize, extractable);
}

kj::OneOf<jsg::Ref<CryptoKey>, CryptoKeyPair> CryptoKey::Impl::generateEcdh(jsg::Lock& js,
    kj::StringPtr normalizedName,
    SubtleCrypto::GenerateKeyAlgorithm&& algorithm,
    bool extractable,
    kj::ArrayPtr<const kj::String> keyUsages) {
  auto usages = CryptoKeyUsageSet::validate(normalizedName, CryptoKeyUsageSet::Context::generate,
      keyUsages, CryptoKeyUsageSet::derivationKeyMask());
  return EllipticKey::generateElliptic(normalizedName, kj::mv(algorithm), extractable, usages, {});
}

kj::Own<CryptoKey::Impl> CryptoKey::Impl::importEcdh(jsg::Lock& js,
    kj::StringPtr normalizedName,
    kj::StringPtr format,
    SubtleCrypto::ImportKeyData keyData,
    SubtleCrypto::ImportKeyAlgorithm&& algorithm,
    bool extractable,
    kj::ArrayPtr<const kj::String> keyUsages) {
  kj::StringPtr namedCurve = JSG_REQUIRE_NONNULL(
      algorithm.namedCurve, TypeError, "Missing field \"namedCurve\" in \"algorithm\".");

  auto [normalizedNamedCurve, curveId, rsSize] = lookupEllipticCurve(namedCurve);

  auto importedKey = [&, curveId = curveId] {
    auto strictCrypto = FeatureFlags::get(js).getStrictCrypto();
    auto usageSet = strictCrypto ? CryptoKeyUsageSet() : CryptoKeyUsageSet::derivationKeyMask();

    if (format != "raw") {
      return importAsymmetricForWebCrypto(js, format, kj::mv(keyData), normalizedName, extractable,
          keyUsages,
          // Verbose lambda capture needed because: https://bugs.llvm.org/show_bug.cgi?id=35984
          [curveId = curveId, normalizedName = kj::str(normalizedName)](
              SubtleCrypto::JsonWebKey keyDataJwk) -> kj::Own<EVP_PKEY> {
        return ellipticJwkReader(curveId, kj::mv(keyDataJwk), normalizedName);
      },
          CryptoKeyUsageSet::derivationKeyMask());
    } else {
      // The usage set is required to be empty for public ECDH keys, including raw keys.
      return importEllipticRaw(kj::mv(keyData), curveId, normalizedName, keyUsages, usageSet);
    }
  }();

  auto ecKey = JSG_REQUIRE_NONNULL(Ec::tryGetEc(importedKey.evpPkey.get()), DOMDataError,
      "Input was not an EC public key nor a DH key", tryDescribeOpensslErrors());

  // We ignore id-ecDH because BoringSSL doesn't implement this.
  // https://bugs.chromium.org/p/chromium/issues/detail?id=532728
  // https://bugs.chromium.org/p/chromium/issues/detail?id=389400

  // Verify namedCurve matches what was specified in the key data.
  JSG_REQUIRE(ecKey.getGroup() != nullptr && ecKey.getCurveName() == curveId, DOMDataError,
      "\"algorithm.namedCurve\" \"", namedCurve,
      "\", does not match the curve "
      "specified by the input key data",
      tryDescribeOpensslErrors());

  auto keyAlgorithm = CryptoKey::EllipticKeyAlgorithm{
    normalizedName,
    normalizedNamedCurve,
  };

  return kj::heap<EllipticKey>(kj::mv(importedKey), kj::mv(keyAlgorithm), rsSize, extractable);
}

// =====================================================================================
// EDDSA & EDDH

namespace {

// Abstract base class for EDDSA and EDDH. The legacy NODE-ED25519 identifier for EDDSA has a
// namedCurve field whereas the algorithms in the Secure Curves spec do not. We handle this by
// keeping track of the algorithm identifier and returning an algorithm struct based on that.
class EdDsaKey final: public AsymmetricKeyCryptoKeyImpl {
 public:
  explicit EdDsaKey(AsymmetricKeyData keyData, kj::StringPtr keyAlgorithm, bool extractable)
      : AsymmetricKeyCryptoKeyImpl(kj::mv(keyData), extractable),
        keyAlgorithm(kj::mv(keyAlgorithm)) {}

  static kj::OneOf<jsg::Ref<CryptoKey>, CryptoKeyPair> generateKey(kj::StringPtr normalizedName,
      int nid,
      CryptoKeyUsageSet privateKeyUsages,
      CryptoKeyUsageSet publicKeyUsages,
      bool extractablePrivateKey);

  CryptoKey::AlgorithmVariant getAlgorithm(jsg::Lock& js) const override {
    // For legacy node-based keys with NODE-ED25519, algorithm contains a namedCurve field.
    if (keyAlgorithm == "NODE-ED25519") {
      return CryptoKey::EllipticKeyAlgorithm{
        keyAlgorithm,
        keyAlgorithm,
      };
    } else {
      return CryptoKey::KeyAlgorithm{keyAlgorithm};
    }
  }

  kj::StringPtr getAlgorithmName() const override {
    return keyAlgorithm;
  }

  kj::StringPtr chooseHash(
      const kj::Maybe<kj::OneOf<kj::String, SubtleCrypto::HashAlgorithm>>& callTimeHash)
      const override {
    KJ_UNIMPLEMENTED();
  }

  jsg::BufferSource sign(jsg::Lock& js,
      SubtleCrypto::SignAlgorithm&& algorithm,
      kj::ArrayPtr<const kj::byte> data) const override {
    JSG_REQUIRE(getTypeEnum() == KeyType::PRIVATE, DOMInvalidAccessError,
        "Asymmetric signing requires a private key.");

    JSG_REQUIRE(getAlgorithmName() == "Ed25519" || getAlgorithmName() == "NODE-ED25519",
        DOMOperationError, "Not implemented for algorithm \"", getAlgorithmName(), "\".");
    // Why NODE-ED25519? NodeJS uses NODE-ED25519/NODE-448 as algorithm names but that feels
    // inconsistent with the broader WebCrypto standard. Filed an issue with the standard for
    // clarification: https://github.com/tQsW/webcrypto-curve25519/issues/7

    auto signature = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, ED25519_SIGNATURE_LEN);
    size_t signatureLength = signature.size();

    // NOTE: Even though there's a ED25519_sign/ED25519_verify methods, they don't actually seem to
    // work or are intended for some other use-case. I tried adding the verify immediately after
    // signing here & the verification failed.
    auto digestCtx = OSSL_NEW(EVP_MD_CTX);

    JSG_REQUIRE(1 == EVP_DigestSignInit(digestCtx.get(), nullptr, nullptr, nullptr, getEvpPkey()),
        InternalDOMOperationError, "Failed to initialize Ed25519 signing digest",
        internalDescribeOpensslErrors());
    JSG_REQUIRE(1 ==
            EVP_DigestSign(digestCtx.get(), signature.asArrayPtr().begin(), &signatureLength,
                data.begin(), data.size()),
        InternalDOMOperationError, "Failed to sign with Ed25119 key",
        internalDescribeOpensslErrors());

    JSG_REQUIRE(signatureLength == signature.size(), InternalDOMOperationError,
        "Unexpected change in size signing Ed25519", signatureLength);

    return jsg::BufferSource(js, kj::mv(signature));
  }

  bool verify(jsg::Lock& js,
      SubtleCrypto::SignAlgorithm&& algorithm,
      kj::ArrayPtr<const kj::byte> signature,
      kj::ArrayPtr<const kj::byte> data) const override {
    ClearErrorOnReturn clearErrorOnReturn;

    JSG_REQUIRE(getTypeEnum() == KeyType::PUBLIC, DOMInvalidAccessError,
        "Asymmetric verification requires a public key.");

    JSG_REQUIRE(getAlgorithmName() == "Ed25519" || getAlgorithmName() == "NODE-ED25519",
        DOMOperationError, "Not implemented for this algorithm", getAlgorithmName());

    JSG_REQUIRE(signature.size() == ED25519_SIGNATURE_LEN, DOMOperationError, "Invalid ",
        getAlgorithmName(), " signature length ", signature.size());

    auto digestCtx = OSSL_NEW(EVP_MD_CTX);
    JSG_REQUIRE(1 == EVP_DigestSignInit(digestCtx.get(), nullptr, nullptr, nullptr, getEvpPkey()),
        InternalDOMOperationError, "Failed to initialize Ed25519 verification digest",
        internalDescribeOpensslErrors());

    auto result = EVP_DigestVerify(
        digestCtx.get(), signature.begin(), signature.size(), data.begin(), data.size());

    JSG_REQUIRE(result == 0 || result == 1, InternalDOMOperationError, "Unexpected return code",
        result, internalDescribeOpensslErrors());

    return !!result;
  }

  jsg::BufferSource deriveBits(jsg::Lock& js,
      SubtleCrypto::DeriveKeyAlgorithm&& algorithm,
      kj::Maybe<uint32_t> resultBitLength) const override final {
    JSG_REQUIRE(getAlgorithmName() == "X25519", DOMNotSupportedError,
        ""
        "The deriveBits operation is not implemented for \"",
        getAlgorithmName(), "\".");

    JSG_REQUIRE(getTypeEnum() == KeyType::PRIVATE, DOMInvalidAccessError,
        ""
        "The deriveBits operation is only valid for a private key, not \"",
        getType(), "\".");

    auto& publicKey = JSG_REQUIRE_NONNULL(
        algorithm.$public, TypeError, "Missing field \"public\" in \"derivedKeyParams\".");

    JSG_REQUIRE(publicKey->getType() == "public"_kj, DOMInvalidAccessError,
        ""
        "The provided key has type \"",
        publicKey->getType(), "\", not \"public\"");

    JSG_REQUIRE(getAlgorithm(js).which() == publicKey->getAlgorithm(js).which(),
        DOMInvalidAccessError, "Base ", getAlgorithmName(),
        " private key cannot be used to derive"
        " a key from a peer ",
        publicKey->getAlgorithmName(), " public key");

    JSG_REQUIRE(getAlgorithmName() == publicKey->getAlgorithmName(), DOMInvalidAccessError,
        "Private key for derivation is using \"", getAlgorithmName(),
        "\" while public key is using \"", publicKey->getAlgorithmName(), "\".");

    auto outputBitLength = resultBitLength.orDefault(X25519_SHARED_KEY_LEN * 8);
    JSG_REQUIRE(outputBitLength <= X25519_SHARED_KEY_LEN * 8, DOMOperationError,
        "Derived key length (", outputBitLength, " bits) is too long (should be at most ",
        X25519_SHARED_KEY_LEN * 8, " bits).");

    // The check above for the algorithm `which` equality ensures that the impl can be downcast to
    // EdDsaKey (assuming we don't accidentally create a class that doesn't inherit this one that
    // for some reason returns an EdDsaKey).
    auto& publicKeyImpl = kj::downcast<EdDsaKey>(*publicKey->impl);

    // EDDH code derived from https://www.openssl.org/docs/manmaster/man3/EVP_PKEY_derive.html
    auto ctx = OSSL_NEW(EVP_PKEY_CTX, getEvpPkey(), nullptr);
    JSG_REQUIRE(1 == EVP_PKEY_derive_init(ctx), InternalDOMOperationError,
        "Failed to init EDDH key derivation", internalDescribeOpensslErrors());
    JSG_REQUIRE(1 == EVP_PKEY_derive_set_peer(ctx, publicKeyImpl.getEvpPkey()),
        InternalDOMOperationError, "Failed to set EDDH peer", internalDescribeOpensslErrors());

    kj::Vector<kj::byte> sharedSecret;
    sharedSecret.resize(X25519_SHARED_KEY_LEN);
    size_t skeylen = X25519_SHARED_KEY_LEN;
    JSG_REQUIRE(1 == EVP_PKEY_derive(ctx, sharedSecret.begin(), &skeylen), DOMOperationError,
        "Failed to derive EDDH key", internalDescribeOpensslErrors());
    KJ_ASSERT(skeylen == X25519_SHARED_KEY_LEN);

    // Check for all-zero value as mandated by spec
    kj::byte isNonZeroSecret = 0;
    for (kj::byte b: sharedSecret) {
      isNonZeroSecret |= b;
    }
    JSG_REQUIRE(isNonZeroSecret, DOMOperationError,
        "Detected small order secure curve points, aborting EDDH derivation");

    // mask off bits like in ECDH's deriveBits()
    auto resultByteLength = integerCeilDivision(outputBitLength, 8u);
    sharedSecret.truncate(resultByteLength);
    auto numBitsToMaskOff = resultByteLength * 8 - outputBitLength;
    KJ_DASSERT(numBitsToMaskOff < 8, numBitsToMaskOff);
    uint8_t mask = ~((1 << numBitsToMaskOff) - 1);
    sharedSecret.back() &= mask;

    auto backing = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, sharedSecret.size());
    backing.asArrayPtr().copyFrom(sharedSecret);
    return jsg::BufferSource(js, kj::mv(backing));
  }

  CryptoKey::AsymmetricKeyDetails getAsymmetricKeyDetail() const override {
    // Node.js implementation for EdDsa keys currently does not provide any detail
    return CryptoKey::AsymmetricKeyDetails{};
  }

  kj::StringPtr jsgGetMemoryName() const override {
    return "EdDsaKey";
  }
  size_t jsgGetMemorySelfSize() const override {
    return sizeof(EdDsaKey);
  }
  void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const override {
    AsymmetricKeyCryptoKeyImpl::jsgGetMemoryInfo(tracker);
  }

 private:
  kj::StringPtr keyAlgorithm;

  SubtleCrypto::JsonWebKey exportJwk() const override final {
    KJ_ASSERT(getAlgorithmName() == "X25519"_kj || getAlgorithmName() == "Ed25519"_kj ||
        getAlgorithmName() == "NODE-ED25519"_kj);

    uint8_t rawPublicKey[ED25519_PUBLIC_KEY_LEN]{};
    size_t publicKeyLen = sizeof(rawPublicKey);
    JSG_REQUIRE(1 == EVP_PKEY_get_raw_public_key(getEvpPkey(), rawPublicKey, &publicKeyLen),
        InternalDOMOperationError, "Failed to retrieve public key",
        internalDescribeOpensslErrors());

    KJ_ASSERT(publicKeyLen == 32, publicKeyLen);

    SubtleCrypto::JsonWebKey jwk;
    jwk.kty = kj::str("OKP");
    jwk.crv = kj::str(getAlgorithmName() == "X25519"_kj ? "X25519"_kj : "Ed25519"_kj);
    jwk.x = fastEncodeBase64Url(kj::arrayPtr(rawPublicKey, publicKeyLen));
    if (getAlgorithmName() == "Ed25519"_kj) {
      jwk.alg = kj::str("EdDSA");
    }

    if (getTypeEnum() == KeyType::PRIVATE) {
      // Deliberately use ED25519_PUBLIC_KEY_LEN here.
      // boringssl defines ED25519_PRIVATE_KEY_LEN as 64B since it stores the private key together
      // with public key data in some functions, but in the EVP interface only the 32B private key
      // itself is returned.
      uint8_t rawPrivateKey[ED25519_PUBLIC_KEY_LEN]{};
      size_t privateKeyLen = ED25519_PUBLIC_KEY_LEN;
      JSG_REQUIRE(1 == EVP_PKEY_get_raw_private_key(getEvpPkey(), rawPrivateKey, &privateKeyLen),
          InternalDOMOperationError, "Failed to retrieve private key",
          internalDescribeOpensslErrors());

      KJ_ASSERT(privateKeyLen == 32, privateKeyLen);

      jwk.d = fastEncodeBase64Url(kj::arrayPtr(rawPrivateKey, privateKeyLen));
    }

    return jwk;
  }

  jsg::BufferSource exportRaw(jsg::Lock& js) const override final {
    JSG_REQUIRE(getTypeEnum() == KeyType::PUBLIC, DOMInvalidAccessError, "Raw export of ",
        getAlgorithmName(), " keys is only allowed for public keys.");

    auto raw = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, ED25519_PUBLIC_KEY_LEN);
    size_t exportedLength = raw.size();

    JSG_REQUIRE(
        1 == EVP_PKEY_get_raw_public_key(getEvpPkey(), raw.asArrayPtr().begin(), &exportedLength),
        InternalDOMOperationError, "Failed to retrieve public key",
        internalDescribeOpensslErrors());

    JSG_REQUIRE(exportedLength == raw.size(), InternalDOMOperationError,
        "Unexpected change in size", raw.size(), exportedLength);

    return jsg::BufferSource(js, kj::mv(raw));
  }
};

template <size_t keySize, void (*KeypairInit)(uint8_t[keySize], uint8_t[keySize * 2])>
CryptoKeyPair generateKeyImpl(kj::StringPtr normalizedName,
    int nid,
    CryptoKeyUsageSet privateKeyUsages,
    CryptoKeyUsageSet publicKeyUsages,
    bool extractablePrivateKey,
    kj::StringPtr curveName) {
  uint8_t rawPublicKey[keySize] = {0};
  uint8_t rawPrivateKey[keySize * 2] = {0};
  KeypairInit(rawPublicKey, rawPrivateKey);

  // The private key technically also contains the public key. Why does the keypair function bother
  // writing out the public key to a separate buffer?

  auto privateEvpPKey = OSSLCALL_OWN(EVP_PKEY,
      EVP_PKEY_new_raw_private_key(nid, nullptr, rawPrivateKey, keySize), InternalDOMOperationError,
      "Error constructing ", curveName, " private key", internalDescribeOpensslErrors());

  auto publicEvpPKey = OSSLCALL_OWN(EVP_PKEY,
      EVP_PKEY_new_raw_public_key(nid, nullptr, rawPublicKey, keySize), InternalDOMOperationError,
      "Internal error construct ", curveName, "public key", internalDescribeOpensslErrors());

  AsymmetricKeyData privateKeyData{
    .evpPkey = kj::mv(privateEvpPKey),
    .keyType = KeyType::PRIVATE,
    .usages = privateKeyUsages,
  };
  AsymmetricKeyData publicKeyData{
    .evpPkey = kj::mv(publicEvpPKey),
    .keyType = KeyType::PUBLIC,
    .usages = publicKeyUsages,
  };

  auto privateKey = jsg::alloc<CryptoKey>(
      kj::heap<EdDsaKey>(kj::mv(privateKeyData), normalizedName, extractablePrivateKey));
  auto publicKey =
      jsg::alloc<CryptoKey>(kj::heap<EdDsaKey>(kj::mv(publicKeyData), normalizedName, true));

  return CryptoKeyPair{.publicKey = kj::mv(publicKey), .privateKey = kj::mv(privateKey)};
}

kj::OneOf<jsg::Ref<CryptoKey>, CryptoKeyPair> EdDsaKey::generateKey(kj::StringPtr normalizedName,
    int nid,
    CryptoKeyUsageSet privateKeyUsages,
    CryptoKeyUsageSet publicKeyUsages,
    bool extractablePrivateKey) {
  switch (nid) {
    // BoringSSL doesn't support ED448/X448.
    case NID_ED25519:
      return generateKeyImpl<ED25519_PUBLIC_KEY_LEN, ED25519_keypair>(normalizedName, nid,
          privateKeyUsages, publicKeyUsages, extractablePrivateKey, "Ed25519"_kj);
    case NID_X25519:
      return generateKeyImpl<X25519_PUBLIC_VALUE_LEN, X25519_keypair>(normalizedName, nid,
          privateKeyUsages, publicKeyUsages, extractablePrivateKey, "X25519"_kj);
  }

  KJ_FAIL_REQUIRE("ED ", normalizedName, " unimplemented", nid);
}

}  // namespace

kj::OneOf<jsg::Ref<CryptoKey>, CryptoKeyPair> CryptoKey::Impl::generateEddsa(jsg::Lock& js,
    kj::StringPtr normalizedName,
    SubtleCrypto::GenerateKeyAlgorithm&& algorithm,
    bool extractable,
    kj::ArrayPtr<const kj::String> keyUsages) {
  auto usages =
      CryptoKeyUsageSet::validate(normalizedName, CryptoKeyUsageSet::Context::generate, keyUsages,
          normalizedName == "X25519" ? CryptoKeyUsageSet::derivationKeyMask()
                                     : CryptoKeyUsageSet::sign() | CryptoKeyUsageSet::verify());
  auto privateKeyUsages = usages & CryptoKeyUsageSet::privateKeyMask();
  auto publicKeyUsages = usages & CryptoKeyUsageSet::publicKeyMask();

  if (normalizedName == "NODE-ED25519") {
    kj::StringPtr namedCurve = JSG_REQUIRE_NONNULL(
        algorithm.namedCurve, TypeError, "Missing field \"namedCurve\" in \"algorithm\".");
    JSG_REQUIRE(namedCurve == "NODE-ED25519", DOMNotSupportedError, "EDDSA curve \"", namedCurve,
        "\" isn't supported.");
  }

  return EdDsaKey::generateKey(normalizedName,
      normalizedName == "X25519" ? NID_X25519 : NID_ED25519, privateKeyUsages, publicKeyUsages,
      extractable);
}

kj::Own<CryptoKey::Impl> CryptoKey::Impl::importEddsa(jsg::Lock& js,
    kj::StringPtr normalizedName,
    kj::StringPtr format,
    SubtleCrypto::ImportKeyData keyData,
    SubtleCrypto::ImportKeyAlgorithm&& algorithm,
    bool extractable,
    kj::ArrayPtr<const kj::String> keyUsages) {

  // BoringSSL doesn't support ED448.
  if (normalizedName == "NODE-ED25519") {
    // TODO: I prefer this style (declaring variables within the scope where they are needed) –
    // does KJ style want this to be done differently?
    kj::StringPtr namedCurve = JSG_REQUIRE_NONNULL(
        algorithm.namedCurve, TypeError, "Missing field \"namedCurve\" in \"algorithm\".");
    JSG_REQUIRE(namedCurve == "NODE-ED25519", DOMNotSupportedError, "EDDSA curve \"", namedCurve,
        "\" isn't supported.");
  }

  auto importedKey = [&] {
    auto nid = normalizedName == "X25519" ? NID_X25519 : NID_ED25519;
    if (format != "raw") {
      return importAsymmetricForWebCrypto(js, format, kj::mv(keyData), normalizedName, extractable,
          keyUsages,
          [nid, normalizedName = kj::str(normalizedName)](
              SubtleCrypto::JsonWebKey keyDataJwk) -> kj::Own<EVP_PKEY> {
        return ellipticJwkReader(nid, kj::mv(keyDataJwk), normalizedName);
      },
          normalizedName == "X25519" ? CryptoKeyUsageSet::derivationKeyMask()
                                     : CryptoKeyUsageSet::sign() | CryptoKeyUsageSet::verify());
    } else {
      return importEllipticRaw(kj::mv(keyData), nid, normalizedName, keyUsages,
          normalizedName == "X25519" ? CryptoKeyUsageSet() : CryptoKeyUsageSet::verify());
    }
  }();

  // In X25519 we ignore the id-X25519 identifier, as with id-ecDH above.
  return kj::heap<EdDsaKey>(kj::mv(importedKey), normalizedName, extractable);
}

kj::Own<CryptoKey::Impl> fromEcKey(kj::Own<EVP_PKEY> key) {
  auto nid = EVP_PKEY_id(key.get());
  if (nid == NID_X25519 || nid == NID_ED25519) {
    return fromEd25519Key(kj::mv(key));
  }

  auto curveName = OBJ_nid2sn(nid);
  if (curveName == nullptr) {
    curveName = "unknown";
  }

  auto [normalizedNamedCurve, curveId, rsSize] = lookupEllipticCurve(curveName);

  return kj::heap<EllipticKey>(
      AsymmetricKeyData{
        .evpPkey = kj::mv(key),
        .keyType = KeyType::PUBLIC,
        .usages = CryptoKeyUsageSet::verify(),
      },
      CryptoKey::EllipticKeyAlgorithm{.name = "ECDSA"_kj, .namedCurve = normalizedNamedCurve},
      rsSize, true);
}

kj::Own<CryptoKey::Impl> fromEd25519Key(kj::Own<EVP_PKEY> key) {
  return kj::heap<EdDsaKey>(
      AsymmetricKeyData{
        .evpPkey = kj::mv(key),
        .keyType = KeyType::PUBLIC,
        .usages = CryptoKeyUsageSet::sign() | CryptoKeyUsageSet::verify(),
      },
      "Ed25519"_kj, true);
}
}  // namespace workerd::api
