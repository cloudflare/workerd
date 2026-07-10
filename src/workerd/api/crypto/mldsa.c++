#include "impl.h"

#include <openssl/bytestring.h>
#include <openssl/mldsa.h>

#include <algorithm>

namespace workerd::api {
namespace {

// Traits structs for each ML-DSA parameter set.
struct MlDsa44Params {
  using PrivateKey = MLDSA44_private_key;
  using PublicKey = MLDSA44_public_key;
  static constexpr size_t PUBLIC_KEY_BYTES = MLDSA44_PUBLIC_KEY_BYTES;
  static constexpr size_t SIGNATURE_BYTES = MLDSA44_SIGNATURE_BYTES;

  static int generateKey(uint8_t* outPk, uint8_t* outSeed, PrivateKey* outSk) {
    return MLDSA44_generate_key(outPk, outSeed, outSk);
  }
  static int privateKeyFromSeed(PrivateKey* outSk, const uint8_t* seed, size_t seedLen) {
    return MLDSA44_private_key_from_seed(outSk, seed, seedLen);
  }
  static int publicFromPrivate(PublicKey* outPk, const PrivateKey* sk) {
    return MLDSA44_public_from_private(outPk, sk);
  }
  static int sign(uint8_t* outSig,
      const PrivateKey* sk,
      const uint8_t* msg,
      size_t msgLen,
      const uint8_t* ctx,
      size_t ctxLen) {
    return MLDSA44_sign(outSig, sk, msg, msgLen, ctx, ctxLen);
  }
  static int verify(const PublicKey* pk,
      const uint8_t* sig,
      size_t sigLen,
      const uint8_t* msg,
      size_t msgLen,
      const uint8_t* ctx,
      size_t ctxLen) {
    return MLDSA44_verify(pk, sig, sigLen, msg, msgLen, ctx, ctxLen);
  }
  static int marshalPublicKey(CBB* out, const PublicKey* pk) {
    return MLDSA44_marshal_public_key(out, pk);
  }
  static int parsePublicKey(PublicKey* outPk, CBS* in) {
    return MLDSA44_parse_public_key(outPk, in);
  }
};

struct MlDsa65Params {
  using PrivateKey = MLDSA65_private_key;
  using PublicKey = MLDSA65_public_key;
  static constexpr size_t PUBLIC_KEY_BYTES = MLDSA65_PUBLIC_KEY_BYTES;
  static constexpr size_t SIGNATURE_BYTES = MLDSA65_SIGNATURE_BYTES;

  static int generateKey(uint8_t* outPk, uint8_t* outSeed, PrivateKey* outSk) {
    return MLDSA65_generate_key(outPk, outSeed, outSk);
  }
  static int privateKeyFromSeed(PrivateKey* outSk, const uint8_t* seed, size_t seedLen) {
    return MLDSA65_private_key_from_seed(outSk, seed, seedLen);
  }
  static int publicFromPrivate(PublicKey* outPk, const PrivateKey* sk) {
    return MLDSA65_public_from_private(outPk, sk);
  }
  static int sign(uint8_t* outSig,
      const PrivateKey* sk,
      const uint8_t* msg,
      size_t msgLen,
      const uint8_t* ctx,
      size_t ctxLen) {
    return MLDSA65_sign(outSig, sk, msg, msgLen, ctx, ctxLen);
  }
  static int verify(const PublicKey* pk,
      const uint8_t* sig,
      size_t sigLen,
      const uint8_t* msg,
      size_t msgLen,
      const uint8_t* ctx,
      size_t ctxLen) {
    return MLDSA65_verify(pk, sig, sigLen, msg, msgLen, ctx, ctxLen);
  }
  static int marshalPublicKey(CBB* out, const PublicKey* pk) {
    return MLDSA65_marshal_public_key(out, pk);
  }
  static int parsePublicKey(PublicKey* outPk, CBS* in) {
    return MLDSA65_parse_public_key(outPk, in);
  }
};

struct MlDsa87Params {
  using PrivateKey = MLDSA87_private_key;
  using PublicKey = MLDSA87_public_key;
  static constexpr size_t PUBLIC_KEY_BYTES = MLDSA87_PUBLIC_KEY_BYTES;
  static constexpr size_t SIGNATURE_BYTES = MLDSA87_SIGNATURE_BYTES;

  static int generateKey(uint8_t* outPk, uint8_t* outSeed, PrivateKey* outSk) {
    return MLDSA87_generate_key(outPk, outSeed, outSk);
  }
  static int privateKeyFromSeed(PrivateKey* outSk, const uint8_t* seed, size_t seedLen) {
    return MLDSA87_private_key_from_seed(outSk, seed, seedLen);
  }
  static int publicFromPrivate(PublicKey* outPk, const PrivateKey* sk) {
    return MLDSA87_public_from_private(outPk, sk);
  }
  static int sign(uint8_t* outSig,
      const PrivateKey* sk,
      const uint8_t* msg,
      size_t msgLen,
      const uint8_t* ctx,
      size_t ctxLen) {
    return MLDSA87_sign(outSig, sk, msg, msgLen, ctx, ctxLen);
  }
  static int verify(const PublicKey* pk,
      const uint8_t* sig,
      size_t sigLen,
      const uint8_t* msg,
      size_t msgLen,
      const uint8_t* ctx,
      size_t ctxLen) {
    return MLDSA87_verify(pk, sig, sigLen, msg, msgLen, ctx, ctxLen);
  }
  static int marshalPublicKey(CBB* out, const PublicKey* pk) {
    return MLDSA87_marshal_public_key(out, pk);
  }
  static int parsePublicKey(PublicKey* outPk, CBS* in) {
    return MLDSA87_parse_public_key(outPk, in);
  }
};

// OIDs for ML-DSA algorithms (from NIST CSOR)
// id-ml-dsa-44: 2.16.840.1.101.3.4.3.17
constexpr uint8_t OID_ML_DSA_44[] = {0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x03, 0x11};
// id-ml-dsa-65: 2.16.840.1.101.3.4.3.18
constexpr uint8_t OID_ML_DSA_65[] = {0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x03, 0x12};
// id-ml-dsa-87: 2.16.840.1.101.3.4.3.19
constexpr uint8_t OID_ML_DSA_87[] = {0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x03, 0x13};

template <typename P>
kj::ArrayPtr<const uint8_t> getOid() {
  if constexpr (P::PUBLIC_KEY_BYTES == MLDSA44_PUBLIC_KEY_BYTES) {
    return kj::arrayPtr(OID_ML_DSA_44, sizeof(OID_ML_DSA_44));
  } else if constexpr (P::PUBLIC_KEY_BYTES == MLDSA65_PUBLIC_KEY_BYTES) {
    return kj::arrayPtr(OID_ML_DSA_65, sizeof(OID_ML_DSA_65));
  } else {
    return kj::arrayPtr(OID_ML_DSA_87, sizeof(OID_ML_DSA_87));
  }
}

// Get context bytes from the SignAlgorithm parameter.
std::pair<const uint8_t*, size_t> getContext(
    jsg::Lock& js, SubtleCrypto::SignAlgorithm& algorithm) {
  KJ_IF_SOME(ctx, algorithm.context) {
    auto ctxData = ctx.getHandle(js).asArrayPtr();
    return {ctxData.begin(), ctxData.size()};
  }
  // BoringSSL treats (nullptr, 0) as an empty context, equivalent to a zero-length byte string
  // per FIPS 204 §5.2.
  return {nullptr, 0};
}

template <typename P>
class MlDsaKey final: public CryptoKey::Impl {
 public:
  // Private key constructor
  MlDsaKey(kj::Array<kj::byte> seed,
      P::PrivateKey privateKey,
      kj::Array<kj::byte> publicKeyBytes,
      kj::StringPtr algorithmName,
      bool extractable,
      CryptoKeyUsageSet usages)
      : CryptoKey::Impl(extractable, usages),
        keyType(KeyType::PRIVATE),
        algorithmName(algorithmName),
        publicKeyBytes(kj::mv(publicKeyBytes)),
        seed(kj::mv(seed)),
        privateKey(kj::mv(privateKey)) {}

  // Public key constructor
  MlDsaKey(P::PublicKey publicKey,
      kj::Array<kj::byte> publicKeyBytes,
      kj::StringPtr algorithmName,
      bool extractable,
      CryptoKeyUsageSet usages)
      : CryptoKey::Impl(extractable, usages),
        keyType(KeyType::PUBLIC),
        algorithmName(algorithmName),
        publicKey(kj::mv(publicKey)),
        publicKeyBytes(kj::mv(publicKeyBytes)) {}

  ~MlDsaKey() noexcept(false) {
    KJ_IF_SOME(s, seed) {
      OPENSSL_cleanse(s.begin(), s.size());
    }
    KJ_IF_SOME(k, privateKey) {
      OPENSSL_cleanse(&k, sizeof(k));
    }
  }

  jsg::JsArrayBuffer sign(jsg::Lock& js,
      SubtleCrypto::SignAlgorithm&& algorithm,
      kj::ArrayPtr<const kj::byte> data) const override {
    JSG_REQUIRE(
        keyType == KeyType::PRIVATE, DOMInvalidAccessError, "Signing requires a private key.");

    auto [ctx, ctxLen] = getContext(js, algorithm);

    auto signature = jsg::JsArrayBuffer::create(js, P::SIGNATURE_BYTES);
    JSG_REQUIRE(1 ==
            P::sign(signature.asArrayPtr().begin(), &KJ_ASSERT_NONNULL(privateKey), data.begin(),
                data.size(), ctx, ctxLen),
        DOMOperationError, "ML-DSA signing failed", tryDescribeOpensslErrors());

    return signature;
  }

  bool verify(jsg::Lock& js,
      SubtleCrypto::SignAlgorithm&& algorithm,
      kj::ArrayPtr<const kj::byte> signature,
      kj::ArrayPtr<const kj::byte> data) const override {
    JSG_REQUIRE(
        keyType == KeyType::PUBLIC, DOMInvalidAccessError, "Verification requires a public key.");

    auto [ctx, ctxLen] = getContext(js, algorithm);

    auto result = P::verify(&KJ_ASSERT_NONNULL(publicKey), signature.begin(), signature.size(),
        data.begin(), data.size(), ctx, ctxLen);
    return result == 1;
  }

  SubtleCrypto::ExportKeyData exportKey(jsg::Lock& js, kj::StringPtr format) const override {
    if (format == "raw-public") {
      JSG_REQUIRE(keyType == KeyType::PUBLIC, DOMInvalidAccessError,
          "raw-public export requires a public key.");
      return jsg::JsArrayBuffer::create(js, publicKeyBytes).addRef(js);
    } else if (format == "raw-seed") {
      JSG_REQUIRE(keyType == KeyType::PRIVATE, DOMInvalidAccessError,
          "raw-seed export requires a private key.");
      return jsg::JsArrayBuffer::create(js, KJ_ASSERT_NONNULL(seed)).addRef(js);
    } else if (format == "spki") {
      JSG_REQUIRE(
          keyType == KeyType::PUBLIC, DOMInvalidAccessError, "SPKI export requires a public key.");
      return exportSpki(js);
    } else if (format == "pkcs8") {
      JSG_REQUIRE(keyType == KeyType::PRIVATE, DOMInvalidAccessError,
          "PKCS8 export requires a private key.");
      return exportPkcs8(js);
    } else if (format == "jwk") {
      return exportJwk(js);
    } else {
      JSG_FAIL_REQUIRE(
          DOMNotSupportedError, "Unrecognized export format \"", format, "\" for ML-DSA.");
    }
  }

  kj::StringPtr getAlgorithmName() const override {
    return algorithmName;
  }

  CryptoKey::AlgorithmVariant getAlgorithm(jsg::Lock& js) const override {
    return CryptoKey::KeyAlgorithm{algorithmName};
  }

  kj::StringPtr getType() const override {
    return keyType == KeyType::PRIVATE ? "private"_kj : "public"_kj;
  }

  bool equals(const Impl& other) const override {
    auto* otherMlDsa = dynamic_cast<const MlDsaKey<P>*>(&other);
    if (otherMlDsa == nullptr) return false;
    if (keyType != otherMlDsa->keyType) return false;
    if (keyType == KeyType::PUBLIC) {
      return publicKeyBytes.size() == otherMlDsa->publicKeyBytes.size() &&
          CRYPTO_memcmp(publicKeyBytes.begin(), otherMlDsa->publicKeyBytes.begin(),
              publicKeyBytes.size()) == 0;
    } else {
      auto& thisSeed = KJ_ASSERT_NONNULL(seed);
      auto& otherSeed = KJ_ASSERT_NONNULL(otherMlDsa->seed);
      return thisSeed.size() == otherSeed.size() &&
          CRYPTO_memcmp(thisSeed.begin(), otherSeed.begin(), thisSeed.size()) == 0;
    }
  }

  kj::StringPtr jsgGetMemoryName() const override {
    return "MlDsaKey";
  }
  size_t jsgGetMemorySelfSize() const override {
    return sizeof(MlDsaKey<P>);
  }
  void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const override {
    tracker.trackFieldWithSize("publicKeyBytes", publicKeyBytes.size());
    KJ_IF_SOME(s, seed) {
      tracker.trackFieldWithSize("seed", s.size());
    }
  }

  kj::Own<CryptoKey::Impl> getPublicKey(jsg::Lock& js, CryptoKeyUsageSet usages) const override {
    JSG_REQUIRE(
        keyType == KeyType::PRIVATE, DOMInvalidAccessError, "getPublicKey requires a private key.");

    typename P::PublicKey pk;
    JSG_REQUIRE(1 == P::publicFromPrivate(&pk, &KJ_ASSERT_NONNULL(privateKey)),
        InternalDOMOperationError, "Failed to derive ", algorithmName, " public key.");

    return kj::heap<MlDsaKey<P>>(
        kj::mv(pk), kj::heapArray<kj::byte>(publicKeyBytes), algorithmName, true, usages);
  }

  // Static factory: generate key pair
  static CryptoKeyPair generateKeyPair(jsg::Lock& js,
      kj::StringPtr normalizedName,
      bool extractable,
      CryptoKeyUsageSet privateKeyUsages,
      CryptoKeyUsageSet publicKeyUsages) {
    uint8_t encodedPublicKey[P::PUBLIC_KEY_BYTES];
    uint8_t seedBuf[MLDSA_SEED_BYTES];
    typename P::PrivateKey sk;
    JSG_REQUIRE(1 == P::generateKey(encodedPublicKey, seedBuf, &sk), DOMOperationError,
        "ML-DSA key generation failed", tryDescribeOpensslErrors());
    KJ_DEFER(OPENSSL_cleanse(seedBuf, sizeof(seedBuf)));
    KJ_DEFER(OPENSSL_cleanse(&sk, sizeof(sk)));

    auto pkBytes = kj::heapArray<kj::byte>(encodedPublicKey, P::PUBLIC_KEY_BYTES);
    auto pkBytesCopy = kj::heapArray<kj::byte>(encodedPublicKey, P::PUBLIC_KEY_BYTES);
    auto seedArray = kj::heapArray<kj::byte>(seedBuf, MLDSA_SEED_BYTES);

    typename P::PublicKey pk;
    JSG_REQUIRE(1 == P::publicFromPrivate(&pk, &sk), InternalDOMOperationError, "Failed to derive ",
        normalizedName, " public key");

    auto privateKey = js.alloc<CryptoKey>(kj::heap<MlDsaKey<P>>(kj::mv(seedArray), kj::mv(sk),
        kj::mv(pkBytesCopy), normalizedName, extractable, privateKeyUsages));
    auto publicKey = js.alloc<CryptoKey>(
        kj::heap<MlDsaKey<P>>(kj::mv(pk), kj::mv(pkBytes), normalizedName, true, publicKeyUsages));

    return CryptoKeyPair{.publicKey = kj::mv(publicKey), .privateKey = kj::mv(privateKey)};
  }

  // Static factory: import from raw-public
  static kj::Own<CryptoKey::Impl> importRawPublic(kj::ArrayPtr<const kj::byte> keyData,
      kj::StringPtr normalizedName,
      bool extractable,
      CryptoKeyUsageSet usages) {
    JSG_REQUIRE(keyData.size() == P::PUBLIC_KEY_BYTES, DOMDataError, "Invalid ", normalizedName,
        " public key length: expected ", P::PUBLIC_KEY_BYTES, " bytes, got ", keyData.size());

    typename P::PublicKey pk;
    CBS cbs;
    CBS_init(&cbs, keyData.begin(), keyData.size());
    JSG_REQUIRE(1 == P::parsePublicKey(&pk, &cbs), DOMDataError, "Failed to parse ", normalizedName,
        " public key");

    return kj::heap<MlDsaKey<P>>(
        kj::mv(pk), kj::heapArray<kj::byte>(keyData), normalizedName, extractable, usages);
  }

  // Static factory: import from raw-seed
  static kj::Own<CryptoKey::Impl> importRawSeed(kj::ArrayPtr<const kj::byte> keyData,
      kj::StringPtr normalizedName,
      bool extractable,
      CryptoKeyUsageSet usages) {
    JSG_REQUIRE(keyData.size() == MLDSA_SEED_BYTES, DOMDataError, "Invalid ", normalizedName,
        " seed length: expected ", MLDSA_SEED_BYTES, " bytes, got ", keyData.size());

    typename P::PrivateKey sk;
    JSG_REQUIRE(1 == P::privateKeyFromSeed(&sk, keyData.begin(), keyData.size()), DOMDataError,
        "Failed to regenerate ", normalizedName, " private key from seed");
    KJ_DEFER(OPENSSL_cleanse(&sk, sizeof(sk)));

    // Derive public key bytes for getPublicKey/export
    typename P::PublicKey pk;
    JSG_REQUIRE(1 == P::publicFromPrivate(&pk, &sk), InternalDOMOperationError, "Failed to derive ",
        normalizedName, " public key from private key");

    bssl::ScopedCBB cbb;
    JSG_REQUIRE(CBB_init(cbb.get(), P::PUBLIC_KEY_BYTES) && P::marshalPublicKey(cbb.get(), &pk) &&
            CBB_flush(cbb.get()),
        InternalDOMOperationError, "Failed to marshal ", normalizedName, " public key");

    auto pkBytes = kj::heapArray<kj::byte>(CBB_data(cbb.get()), CBB_len(cbb.get()));

    return kj::heap<MlDsaKey<P>>(kj::heapArray<kj::byte>(keyData), kj::mv(sk), kj::mv(pkBytes),
        normalizedName, extractable, usages);
  }

  // Static factory: import from SPKI
  static kj::Own<CryptoKey::Impl> importSpki(kj::ArrayPtr<const kj::byte> keyData,
      kj::StringPtr normalizedName,
      bool extractable,
      CryptoKeyUsageSet usages) {
    auto oid = getOid<P>();

    // Parse the SubjectPublicKeyInfo structure
    CBS cbs, spki, algorithmId, oidCbs, subjectPublicKey;
    CBS_init(&cbs, keyData.begin(), keyData.size());

    JSG_REQUIRE(CBS_get_asn1(&cbs, &spki, CBS_ASN1_SEQUENCE) && CBS_len(&cbs) == 0, DOMDataError,
        "Invalid SPKI structure.");

    JSG_REQUIRE(CBS_get_asn1(&spki, &algorithmId, CBS_ASN1_SEQUENCE), DOMDataError,
        "Invalid SPKI AlgorithmIdentifier.");

    JSG_REQUIRE(CBS_get_asn1(&algorithmId, &oidCbs, CBS_ASN1_OBJECT), DOMDataError,
        "Invalid SPKI algorithm OID.");

    // Verify OID matches
    JSG_REQUIRE(CBS_len(&oidCbs) == oid.size() &&
            CRYPTO_memcmp(CBS_data(&oidCbs), oid.begin(), oid.size()) == 0,
        DOMDataError, "SPKI algorithm OID does not match ", normalizedName, ".");

    // Parameters must be absent (per spec)
    JSG_REQUIRE(CBS_len(&algorithmId) == 0, DOMDataError, "SPKI AlgorithmIdentifier for ",
        normalizedName, " must not have parameters.");

    // Get subjectPublicKey BIT STRING
    JSG_REQUIRE(CBS_get_asn1(&spki, &subjectPublicKey, CBS_ASN1_BITSTRING), DOMDataError,
        "Invalid SPKI subjectPublicKey.");

    // BIT STRING has a leading byte for the number of unused bits (must be 0)
    uint8_t unusedBits;
    JSG_REQUIRE(CBS_get_u8(&subjectPublicKey, &unusedBits) && unusedBits == 0, DOMDataError,
        "Invalid SPKI subjectPublicKey BIT STRING padding.");

    JSG_REQUIRE(CBS_len(&spki) == 0, DOMDataError, "Trailing data in SPKI.");

    // Parse the raw public key
    typename P::PublicKey pk;
    CBS pkCbs;
    CBS_init(&pkCbs, CBS_data(&subjectPublicKey), CBS_len(&subjectPublicKey));
    JSG_REQUIRE(1 == P::parsePublicKey(&pk, &pkCbs), DOMDataError, "Failed to parse ",
        normalizedName, " public key from SPKI.");

    auto publicKeyBytes =
        kj::heapArray<kj::byte>(CBS_data(&subjectPublicKey), CBS_len(&subjectPublicKey));

    return kj::heap<MlDsaKey<P>>(
        kj::mv(pk), kj::mv(publicKeyBytes), normalizedName, extractable, usages);
  }

  // Static factory: import from PKCS8
  static kj::Own<CryptoKey::Impl> importPkcs8(kj::ArrayPtr<const kj::byte> keyData,
      kj::StringPtr normalizedName,
      bool extractable,
      CryptoKeyUsageSet usages) {
    auto oid = getOid<P>();

    // Parse PrivateKeyInfo
    CBS cbs, privateKeyInfo, algorithmId, oidCbs, privateKeyCbs;
    CBS_init(&cbs, keyData.begin(), keyData.size());

    JSG_REQUIRE(CBS_get_asn1(&cbs, &privateKeyInfo, CBS_ASN1_SEQUENCE) && CBS_len(&cbs) == 0,
        DOMDataError, "Invalid PKCS8 structure.");

    // Version must be 0
    uint64_t version;
    JSG_REQUIRE(CBS_get_asn1_uint64(&privateKeyInfo, &version) && version == 0, DOMDataError,
        "Invalid PKCS8 version.");

    JSG_REQUIRE(CBS_get_asn1(&privateKeyInfo, &algorithmId, CBS_ASN1_SEQUENCE), DOMDataError,
        "Invalid PKCS8 AlgorithmIdentifier.");

    JSG_REQUIRE(CBS_get_asn1(&algorithmId, &oidCbs, CBS_ASN1_OBJECT), DOMDataError,
        "Invalid PKCS8 algorithm OID.");

    JSG_REQUIRE(CBS_len(&oidCbs) == oid.size() &&
            CRYPTO_memcmp(CBS_data(&oidCbs), oid.begin(), oid.size()) == 0,
        DOMDataError, "PKCS8 algorithm OID does not match ", normalizedName, ".");

    // Parameters must be absent
    JSG_REQUIRE(CBS_len(&algorithmId) == 0, DOMDataError, "PKCS8 AlgorithmIdentifier for ",
        normalizedName, " must not have parameters.");

    // Get the privateKey OCTET STRING
    JSG_REQUIRE(CBS_get_asn1(&privateKeyInfo, &privateKeyCbs, CBS_ASN1_OCTETSTRING), DOMDataError,
        "Invalid PKCS8 privateKey.");

    // Parse the inner ML-DSA-PrivateKey — only the seed format is supported:
    //   seed [0] IMPLICIT OCTET STRING (SIZE(32))
    CBS innerKey;
    CBS_init(&innerKey, CBS_data(&privateKeyCbs), CBS_len(&privateKeyCbs));

    kj::Array<kj::byte> seedArray;
    CBS seedCbs;
    if (CBS_get_asn1(&innerKey, &seedCbs, CBS_ASN1_CONTEXT_SPECIFIC | 0) &&
        CBS_len(&innerKey) == 0) {
      JSG_REQUIRE(CBS_len(&seedCbs) == MLDSA_SEED_BYTES, DOMDataError, "Invalid ", normalizedName,
          " seed length in PKCS8.");
      seedArray = kj::heapArray<kj::byte>(CBS_data(&seedCbs), CBS_len(&seedCbs));
    } else {
      JSG_FAIL_REQUIRE(DOMNotSupportedError,
          "Only the seed PKCS8 private key format is supported for ", normalizedName, ".");
    }

    // Regenerate private key from seed
    typename P::PrivateKey sk;
    JSG_REQUIRE(1 == P::privateKeyFromSeed(&sk, seedArray.begin(), seedArray.size()), DOMDataError,
        "Failed to regenerate ", normalizedName, " private key from PKCS8 seed.");
    KJ_DEFER(OPENSSL_cleanse(&sk, sizeof(sk)));

    // Derive public key
    typename P::PublicKey pk;
    JSG_REQUIRE(1 == P::publicFromPrivate(&pk, &sk), InternalDOMOperationError, "Failed to derive ",
        normalizedName, " public key.");

    bssl::ScopedCBB cbb;
    JSG_REQUIRE(CBB_init(cbb.get(), P::PUBLIC_KEY_BYTES) && P::marshalPublicKey(cbb.get(), &pk) &&
            CBB_flush(cbb.get()),
        InternalDOMOperationError, "Failed to marshal ", normalizedName, " public key.");
    auto pkBytes = kj::heapArray<kj::byte>(CBB_data(cbb.get()), CBB_len(cbb.get()));

    return kj::heap<MlDsaKey<P>>(
        kj::mv(seedArray), kj::mv(sk), kj::mv(pkBytes), normalizedName, extractable, usages);
  }

  // Static factory: import from JWK
  static kj::Own<CryptoKey::Impl> importJwk(SubtleCrypto::JsonWebKey&& jwk,
      kj::StringPtr normalizedName,
      bool extractable,
      CryptoKeyUsageSet usages) {
    JSG_REQUIRE(jwk.kty == "AKP", DOMDataError, "JWK \"kty\" field must be \"AKP\" for ",
        normalizedName, ".");

    auto& alg = JSG_REQUIRE_NONNULL(
        jwk.alg, DOMDataError, "JWK \"alg\" field is required for ", normalizedName, ".");
    JSG_REQUIRE(alg == normalizedName, DOMDataError, "JWK \"alg\" field \"", alg,
        "\" does not match \"", normalizedName, "\".");

    if (usages.size() != 0) {
      KJ_IF_SOME(use, jwk.use) {
        JSG_REQUIRE(use == "sig", DOMDataError, "JWK \"use\" field must be \"sig\" for ",
            normalizedName, ".");
      }
    }

    KJ_IF_SOME(ops, jwk.key_ops) {
      std::sort(ops.begin(), ops.end());
      auto duplicate = std::adjacent_find(ops.begin(), ops.end());
      JSG_REQUIRE(duplicate == ops.end(), DOMDataError, "JWK contains duplicate value \"",
          *duplicate, "\", in \"key_ops\".");

      for (const auto& usage: CryptoKeyUsageSet::singletons()) {
        if (usage <= usages) {
          JSG_REQUIRE(std::any_of(ops.begin(), ops.end(),
                          [&](const auto& op) { return op == usage.name(); }),
              DOMDataError, "\"jwk\" key missing usage \"", usage.name(), "\", in \"key_ops\".");
        }
      }
    }

    KJ_IF_SOME(ext, jwk.ext) {
      JSG_REQUIRE(
          ext || !extractable, DOMDataError, "JWK \"ext\" is false but extractable is true.");
    }

    KJ_IF_SOME(privField, jwk.priv) {
      // Private key (seed)
      auto seedResult = decodeBase64Url(kj::mv(privField));
      JSG_REQUIRE(!seedResult.hadErrors, DOMDataError, "Invalid base64url in JWK \"priv\" for ",
          normalizedName, ".");
      auto seedBytes = kj::mv(seedResult);

      JSG_REQUIRE(seedBytes.size() == MLDSA_SEED_BYTES, DOMDataError,
          "Invalid JWK \"priv\" length for ", normalizedName, ".");

      // Regenerate private key from seed
      typename P::PrivateKey sk;
      JSG_REQUIRE(1 == P::privateKeyFromSeed(&sk, seedBytes.begin(), seedBytes.size()),
          DOMDataError, "Failed to regenerate ", normalizedName, " private key from JWK seed.");
      KJ_DEFER(OPENSSL_cleanse(&sk, sizeof(sk)));

      // Derive public key
      typename P::PublicKey pk;
      JSG_REQUIRE(1 == P::publicFromPrivate(&pk, &sk), InternalDOMOperationError,
          "Failed to derive ", normalizedName, " public key.");

      bssl::ScopedCBB cbb;
      JSG_REQUIRE(CBB_init(cbb.get(), P::PUBLIC_KEY_BYTES) && P::marshalPublicKey(cbb.get(), &pk) &&
              CBB_flush(cbb.get()),
          InternalDOMOperationError, "Failed to marshal ", normalizedName, " public key.");
      auto pkBytes = kj::heapArray<kj::byte>(CBB_data(cbb.get()), CBB_len(cbb.get()));

      // If pub field is present, verify it matches
      KJ_IF_SOME(pubField, jwk.pub) {
        auto pubResult = decodeBase64Url(kj::mv(pubField));
        JSG_REQUIRE(!pubResult.hadErrors, DOMDataError, "Invalid base64url in JWK \"pub\" for ",
            normalizedName, ".");
        JSG_REQUIRE(pubResult.size() == pkBytes.size() &&
                CRYPTO_memcmp(pubResult.begin(), pkBytes.begin(), pkBytes.size()) == 0,
            DOMDataError, "JWK \"pub\" does not match the public key derived from \"priv\".");
      }

      return kj::heap<MlDsaKey<P>>(kj::heapArray<kj::byte>(seedBytes.asPtr()), kj::mv(sk),
          kj::mv(pkBytes), normalizedName, extractable, usages);
    } else {
      // Public key only
      auto& pubField = JSG_REQUIRE_NONNULL(
          jwk.pub, DOMDataError, "JWK for ", normalizedName, " must have \"pub\" field.");

      auto pubResult = decodeBase64Url(kj::mv(pubField));
      JSG_REQUIRE(!pubResult.hadErrors, DOMDataError, "Invalid base64url in JWK \"pub\" for ",
          normalizedName, ".");

      JSG_REQUIRE(pubResult.size() == P::PUBLIC_KEY_BYTES, DOMDataError,
          "Invalid JWK \"pub\" length for ", normalizedName, ".");

      typename P::PublicKey pk;
      CBS cbs;
      CBS_init(&cbs, pubResult.begin(), pubResult.size());
      JSG_REQUIRE(1 == P::parsePublicKey(&pk, &cbs), DOMDataError, "Failed to parse ",
          normalizedName, " public key from JWK.");

      return kj::heap<MlDsaKey<P>>(kj::mv(pk), kj::heapArray<kj::byte>(pubResult.asPtr()),
          normalizedName, extractable, usages);
    }
  }

 private:
  enum class KeyType { PUBLIC, PRIVATE };
  KeyType keyType;
  kj::StringPtr algorithmName;

  // Public key data. The parsed public key is present only for public CryptoKeys; private keys keep
  // publicKeyBytes for export and getPublicKey().
  kj::Maybe<typename P::PublicKey> publicKey;
  kj::Array<kj::byte> publicKeyBytes;

  // Private key data (present only for private keys)
  kj::Maybe<kj::Array<kj::byte>> seed;
  kj::Maybe<typename P::PrivateKey> privateKey;

  SubtleCrypto::ExportKeyData exportSpki(jsg::Lock& js) const {
    auto oid = getOid<P>();

    // Manually build the SPKI
    bssl::ScopedCBB cbb;
    CBB spki, algId, oidCbb, bitString;
    JSG_REQUIRE(CBB_init(cbb.get(), P::PUBLIC_KEY_BYTES + 64), InternalDOMOperationError,
        "Failed to init SPKI CBB.");
    JSG_REQUIRE(CBB_add_asn1(cbb.get(), &spki, CBS_ASN1_SEQUENCE), InternalDOMOperationError,
        "Failed to add SPKI SEQUENCE.");
    JSG_REQUIRE(CBB_add_asn1(&spki, &algId, CBS_ASN1_SEQUENCE), InternalDOMOperationError,
        "Failed to add AlgorithmIdentifier SEQUENCE.");
    JSG_REQUIRE(CBB_add_asn1(&algId, &oidCbb, CBS_ASN1_OBJECT), InternalDOMOperationError,
        "Failed to add OID.");
    JSG_REQUIRE(CBB_add_bytes(&oidCbb, oid.begin(), oid.size()), InternalDOMOperationError,
        "Failed to add OID bytes.");
    JSG_REQUIRE(
        CBB_flush(&algId), InternalDOMOperationError, "Failed to flush AlgorithmIdentifier.");
    JSG_REQUIRE(CBB_add_asn1(&spki, &bitString, CBS_ASN1_BITSTRING), InternalDOMOperationError,
        "Failed to add BIT STRING.");
    JSG_REQUIRE(
        CBB_add_u8(&bitString, 0), InternalDOMOperationError, "Failed to add unused bits byte.");
    JSG_REQUIRE(CBB_add_bytes(&bitString, publicKeyBytes.begin(), publicKeyBytes.size()),
        InternalDOMOperationError, "Failed to add public key bytes.");
    JSG_REQUIRE(CBB_flush(cbb.get()), InternalDOMOperationError, "Failed to flush SPKI.");

    uint8_t* der = nullptr;
    size_t derLen;
    JSG_REQUIRE(CBB_finish(cbb.get(), &der, &derLen), InternalDOMOperationError,
        "Failed to finish SPKI encoding.");
    KJ_DEFER(OPENSSL_free(der));

    return jsg::JsArrayBuffer::create(js, kj::arrayPtr(der, derLen)).addRef(js);
  }

  SubtleCrypto::ExportKeyData exportPkcs8(jsg::Lock& js) const {
    auto oid = getOid<P>();
    auto& seedData = KJ_ASSERT_NONNULL(seed);

    bssl::ScopedCBB cbb;
    CBB pkcs8, algId, oidCbb, privateKeyOctet, innerPrivateKey;
    JSG_REQUIRE(CBB_init(cbb.get(), MLDSA_SEED_BYTES + 64), InternalDOMOperationError,
        "Failed to init PKCS8 CBB.");
    JSG_REQUIRE(CBB_add_asn1(cbb.get(), &pkcs8, CBS_ASN1_SEQUENCE), InternalDOMOperationError,
        "Failed to add PKCS8 SEQUENCE.");

    // Version 0
    JSG_REQUIRE(
        CBB_add_asn1_uint64(&pkcs8, 0), InternalDOMOperationError, "Failed to add PKCS8 version.");

    // AlgorithmIdentifier
    JSG_REQUIRE(CBB_add_asn1(&pkcs8, &algId, CBS_ASN1_SEQUENCE), InternalDOMOperationError,
        "Failed to add AlgorithmIdentifier.");
    JSG_REQUIRE(CBB_add_asn1(&algId, &oidCbb, CBS_ASN1_OBJECT), InternalDOMOperationError,
        "Failed to add OID.");
    JSG_REQUIRE(CBB_add_bytes(&oidCbb, oid.begin(), oid.size()), InternalDOMOperationError,
        "Failed to add OID bytes.");
    JSG_REQUIRE(
        CBB_flush(&algId), InternalDOMOperationError, "Failed to flush AlgorithmIdentifier.");

    // PrivateKey: OCTET STRING containing context-specific [0] tag with seed
    JSG_REQUIRE(CBB_add_asn1(&pkcs8, &privateKeyOctet, CBS_ASN1_OCTETSTRING),
        InternalDOMOperationError, "Failed to add privateKey OCTET STRING.");
    JSG_REQUIRE(CBB_add_asn1(&privateKeyOctet, &innerPrivateKey, CBS_ASN1_CONTEXT_SPECIFIC | 0),
        InternalDOMOperationError, "Failed to add seed context-specific tag.");
    JSG_REQUIRE(CBB_add_bytes(&innerPrivateKey, seedData.begin(), seedData.size()),
        InternalDOMOperationError, "Failed to add seed bytes.");
    JSG_REQUIRE(CBB_flush(cbb.get()), InternalDOMOperationError, "Failed to flush PKCS8.");

    uint8_t* der = nullptr;
    size_t derLen;
    JSG_REQUIRE(CBB_finish(cbb.get(), &der, &derLen), InternalDOMOperationError,
        "Failed to finish PKCS8 encoding.");
    KJ_DEFER(OPENSSL_free(der));

    return jsg::JsArrayBuffer::create(js, kj::arrayPtr(der, derLen)).addRef(js);
  }

  SubtleCrypto::ExportKeyData exportJwk(jsg::Lock& js) const {
    SubtleCrypto::JsonWebKey jwk;
    jwk.kty = kj::str("AKP");
    jwk.alg = kj::str(algorithmName);
    jwk.ext = isExtractable();
    jwk.key_ops = getUsages().map([](auto usage) { return kj::str(usage.name()); });

    jwk.pub = kj::encodeBase64Url(publicKeyBytes);

    if (keyType == KeyType::PRIVATE) {
      auto& seedData = KJ_ASSERT_NONNULL(seed);
      jwk.priv = kj::encodeBase64Url(seedData);
    }

    return jwk;
  }
};

// Dispatch based on algorithm name
template <typename Func>
auto dispatchMlDsa(kj::StringPtr normalizedName, Func&& func) {
  if (normalizedName == "ML-DSA-44") {
    return func(MlDsa44Params{});
  } else if (normalizedName == "ML-DSA-65") {
    return func(MlDsa65Params{});
  } else if (normalizedName == "ML-DSA-87") {
    return func(MlDsa87Params{});
  } else {
    JSG_FAIL_REQUIRE(
        DOMNotSupportedError, "Unsupported ML-DSA algorithm \"", normalizedName, "\".");
  }
}

}  // namespace

kj::OneOf<jsg::Ref<CryptoKey>, CryptoKeyPair> CryptoKey::Impl::generateMlDsa(jsg::Lock& js,
    kj::StringPtr normalizedName,
    SubtleCrypto::GenerateKeyAlgorithm&& algorithm,
    bool extractable,
    kj::ArrayPtr<const kj::String> keyUsages) {
  auto usages = CryptoKeyUsageSet::validate(normalizedName, CryptoKeyUsageSet::Context::generate,
      keyUsages, CryptoKeyUsageSet::sign() | CryptoKeyUsageSet::verify());
  auto privateKeyUsages = usages & CryptoKeyUsageSet::privateKeyMask();
  auto publicKeyUsages = usages & CryptoKeyUsageSet::publicKeyMask();

  return dispatchMlDsa(
      normalizedName, [&](auto params) -> kj::OneOf<jsg::Ref<CryptoKey>, CryptoKeyPair> {
    using P = decltype(params);
    return MlDsaKey<P>::generateKeyPair(
        js, normalizedName, extractable, privateKeyUsages, publicKeyUsages);
  });
}

kj::Own<CryptoKey::Impl> CryptoKey::Impl::importMlDsa(jsg::Lock& js,
    kj::StringPtr normalizedName,
    kj::StringPtr format,
    SubtleCrypto::ImportKeyData keyData,
    SubtleCrypto::ImportKeyAlgorithm&& algorithm,
    bool extractable,
    kj::ArrayPtr<const kj::String> keyUsages) {
  return dispatchMlDsa(normalizedName, [&](auto params) -> kj::Own<CryptoKey::Impl> {
    using P = decltype(params);

    if (format == "raw-public") {
      auto usages = CryptoKeyUsageSet::validate(normalizedName,
          CryptoKeyUsageSet::Context::importPublic, keyUsages, CryptoKeyUsageSet::verify());
      auto& source = JSG_REQUIRE_NONNULL(keyData.tryGet<jsg::JsRef<jsg::JsBufferSource>>(),
          DOMDataError, "Import data for raw-public must be a buffer.");
      auto handle = source.getHandle(js);
      return MlDsaKey<P>::importRawPublic(handle.asArrayPtr(), normalizedName, extractable, usages);
    } else if (format == "raw-seed") {
      auto usages = CryptoKeyUsageSet::validate(normalizedName,
          CryptoKeyUsageSet::Context::importPrivate, keyUsages, CryptoKeyUsageSet::sign());
      auto& source = JSG_REQUIRE_NONNULL(keyData.tryGet<jsg::JsRef<jsg::JsBufferSource>>(),
          DOMDataError, "Import data for raw-seed must be a buffer.");
      auto handle = source.getHandle(js);
      return MlDsaKey<P>::importRawSeed(handle.asArrayPtr(), normalizedName, extractable, usages);
    } else if (format == "spki") {
      auto usages = CryptoKeyUsageSet::validate(normalizedName,
          CryptoKeyUsageSet::Context::importPublic, keyUsages, CryptoKeyUsageSet::verify());
      auto& source = JSG_REQUIRE_NONNULL(keyData.tryGet<jsg::JsRef<jsg::JsBufferSource>>(),
          DOMDataError, "Import data for spki must be a buffer.");
      auto handle = source.getHandle(js);
      return MlDsaKey<P>::importSpki(handle.asArrayPtr(), normalizedName, extractable, usages);
    } else if (format == "pkcs8") {
      auto usages = CryptoKeyUsageSet::validate(normalizedName,
          CryptoKeyUsageSet::Context::importPrivate, keyUsages, CryptoKeyUsageSet::sign());
      auto& source = JSG_REQUIRE_NONNULL(keyData.tryGet<jsg::JsRef<jsg::JsBufferSource>>(),
          DOMDataError, "Import data for pkcs8 must be a buffer.");
      auto handle = source.getHandle(js);
      return MlDsaKey<P>::importPkcs8(handle.asArrayPtr(), normalizedName, extractable, usages);
    } else if (format == "jwk") {
      auto& jwk = JSG_REQUIRE_NONNULL(keyData.tryGet<SubtleCrypto::JsonWebKey>(), DOMDataError,
          "Import data for jwk must be a JsonWebKey.");
      // Determine usages based on key type (priv field present = private key)
      CryptoKeyUsageSet usages;
      if (jwk.priv != kj::none) {
        usages = CryptoKeyUsageSet::validate(normalizedName,
            CryptoKeyUsageSet::Context::importPrivate, keyUsages, CryptoKeyUsageSet::sign());
      } else {
        usages = CryptoKeyUsageSet::validate(normalizedName,
            CryptoKeyUsageSet::Context::importPublic, keyUsages, CryptoKeyUsageSet::verify());
      }
      return MlDsaKey<P>::importJwk(kj::mv(jwk), normalizedName, extractable, usages);
    } else {
      JSG_FAIL_REQUIRE(
          DOMNotSupportedError, "Unsupported import format \"", format, "\" for ML-DSA.");
    }
  });
}

}  // namespace workerd::api
