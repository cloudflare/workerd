// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "crypto.h"

#include "impl.h"
#include "rsa.h"

#include <workerd/api/crypto/crc-impl.h>
#include <workerd/api/crypto/endianness.h>
#include <workerd/api/streams/standard.h>
#include <workerd/api/util.h>
#include <workerd/io/io-context.h>
#include <workerd/jsg/jsg.h>
#include <workerd/util/uuid.h>

#include <openssl/digest.h>
#include <openssl/mem.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <set>
#include <typeinfo>

namespace workerd::api {
namespace {
// BoringSSL does not tolerate null pointers even when the length is zero.
// JsBufferSource::asArrayPtr() can return {nullptr, 0} for empty buffers,
// so we ensure a non-null pointer before passing to OpenSSL.
kj::ArrayPtr<const kj::byte> nonNullBytes(kj::ArrayPtr<kj::byte> ptr) {
  static const kj::byte DUMMY = 0;
  return ptr == nullptr ? kj::arrayPtr(&DUMMY, 0) : ptr;
}
}  // namespace

kj::StringPtr CryptoKeyUsageSet::name() const {
  if (*this == encrypt()) return "encrypt";
  if (*this == decrypt()) return "decrypt";
  if (*this == sign()) return "sign";
  if (*this == verify()) return "verify";
  if (*this == deriveKey()) return "deriveKey";
  if (*this == deriveBits()) return "deriveBits";
  if (*this == wrapKey()) return "wrapKey";
  if (*this == unwrapKey()) return "unwrapKey";
  if (*this == encapsulateKey()) return "encapsulateKey";
  if (*this == encapsulateBits()) return "encapsulateBits";
  if (*this == decapsulateKey()) return "decapsulateKey";
  if (*this == decapsulateBits()) return "decapsulateBits";
  KJ_FAIL_REQUIRE("CryptoKeyUsageSet does not contain exactly one key usage");
}

CryptoKeyUsageSet CryptoKeyUsageSet::byName(kj::StringPtr name) {
  for (auto& usage: singletons()) {
    if (name == usage.name()) return usage;
  }
  return {};
}

kj::ArrayPtr<const CryptoKeyUsageSet> CryptoKeyUsageSet::singletons() {
  static const workerd::api::CryptoKeyUsageSet singletons[] = {encrypt(), decrypt(), sign(),
    verify(), deriveKey(), deriveBits(), wrapKey(), unwrapKey(), encapsulateKey(),
    encapsulateBits(), decapsulateKey(), decapsulateBits()};
  return singletons;
}

CryptoKeyUsageSet CryptoKeyUsageSet::validate(kj::StringPtr normalizedName,
    Context ctx,
    kj::ArrayPtr<const kj::String> actual,
    CryptoKeyUsageSet mask) {
  const auto op = (ctx == Context::generate) ? "generate"
      : (ctx == Context::importSecret)       ? "import secret"
      : (ctx == Context::importPublic)       ? "import public"
                                             : "import private";
  CryptoKeyUsageSet usages;
  for (const auto& usage: actual) {
    CryptoKeyUsageSet match = byName(usage);
    JSG_REQUIRE(match.isSingleton() && match <= mask, DOMSyntaxError, "Attempt to ", op, " ",
        normalizedName, " key with invalid usage \"", usage, "\".");
    usages |= match;
  }
  return usages;
}

namespace {

// IMPLEMENTATION STRATEGY
//
// Each SubtleCrypto method is polymorphic, with different implementations selected based on the
// `name` property of the Algorithm dictionary passed (or KeyAlgorithm dictionary of the CryptoKey
// passed, in the case of subtle.exportKey()).
//
// This polymorphism is implemented in CryptoKey::Impl. All of the key-based crypto algorithm
// operations (encrypt, decrypt, sign, verify, deriveBits, wrapKey, unwrapKey) are virtual functions
// on CryptoKey::Impl -- SubtleCrypto forwards to CryptoKey which forwards to Impl.
//
// TODO(cleanup): We validate crypto algorithm/operation/key sanity in a preamble in the functions
//   defined in the SubtleCrypto interface. This is because this whole thing was originally
//   implemented differently and I haven't completed refactoring it. We should put this validation
//   somewhere in CryptoKey, perhaps implicitly in the default implementations of the
//   encrypt/decrypt/sign/verify/etc. functions.
//
// Note that SubtleCrypto.digest() is special. It is not a key-based operation and we only support
// one hash family, SHA, so its implementation is non-virtual.
//
// NOTE(perf): The SubtleCrypto interface is asynchronous, but all of our implementations perform
//   the crypto synchronously before returning. In theory, we could be performing bulk crypto in a
//   separate thread, maybe improving performance. However, it's unclear what real use case would
//   benefit from this. It's also unclear that we would want a single request to be able to use
//   multiple cores -- certainly it would greatly complicate our implementation of request CPU
//   limits. So, we probably shouldn't implement true asynchronous crypto.
//
//   Additionally, performing the crypto synchronously actually has a performance benefit: we can
//   safely avoid copying input BufferSources -- most of our functions can take
//   kj::ArrayPtr<const kj::byte>s, rather than kj::Array<kj::byte>s.

// =======================================================================================
// Registered algorithms

static kj::Maybe<const CryptoAlgorithm&> lookupAlgorithm(kj::StringPtr name) {
  static const std::set<CryptoAlgorithm> ALGORITHMS = {
    {"AES-CTR"_kj, &CryptoKey::Impl::importAes, &CryptoKey::Impl::generateAes},
    {"AES-CBC"_kj, &CryptoKey::Impl::importAes, &CryptoKey::Impl::generateAes},
    {"AES-GCM"_kj, &CryptoKey::Impl::importAes, &CryptoKey::Impl::generateAes},
    {"AES-KW"_kj, &CryptoKey::Impl::importAes, &CryptoKey::Impl::generateAes},
    {"HMAC"_kj, &CryptoKey::Impl::importHmac, &CryptoKey::Impl::generateHmac},
    {"PBKDF2"_kj, &CryptoKey::Impl::importPbkdf2},
    {"HKDF"_kj, &CryptoKey::Impl::importHkdf},
    {"RSASSA-PKCS1-v1_5"_kj, &CryptoKey::Impl::importRsa, &CryptoKey::Impl::generateRsa},
    {"RSA-PSS"_kj, &CryptoKey::Impl::importRsa, &CryptoKey::Impl::generateRsa},
    {"RSA-OAEP"_kj, &CryptoKey::Impl::importRsa, &CryptoKey::Impl::generateRsa},
    {"ECDSA"_kj, &CryptoKey::Impl::importEcdsa, &CryptoKey::Impl::generateEcdsa},
    {"ECDH"_kj, &CryptoKey::Impl::importEcdh, &CryptoKey::Impl::generateEcdh},
    {"NODE-ED25519"_kj, &CryptoKey::Impl::importEddsa, &CryptoKey::Impl::generateEddsa},
    {"Ed25519"_kj, &CryptoKey::Impl::importEddsa, &CryptoKey::Impl::generateEddsa},
    {"X25519"_kj, &CryptoKey::Impl::importEddsa, &CryptoKey::Impl::generateEddsa},
    {"RSA-RAW"_kj, &CryptoKey::Impl::importRsaRaw},
    {"ML-DSA-44"_kj, &CryptoKey::Impl::importMlDsa, &CryptoKey::Impl::generateMlDsa},
    {"ML-DSA-65"_kj, &CryptoKey::Impl::importMlDsa, &CryptoKey::Impl::generateMlDsa},
    {"ML-DSA-87"_kj, &CryptoKey::Impl::importMlDsa, &CryptoKey::Impl::generateMlDsa},
    {"ML-KEM-768"_kj, &CryptoKey::Impl::importMlKem, &CryptoKey::Impl::generateMlKem},
    {"ML-KEM-1024"_kj, &CryptoKey::Impl::importMlKem, &CryptoKey::Impl::generateMlKem},
  };

  auto iter = ALGORITHMS.find(CryptoAlgorithm{name});
  if (iter == ALGORITHMS.end()) {
    // No such built-in algorithm, so fall back to checking if the Api has a custom
    // algorithm registered.
    return Worker::Api::current().getCryptoAlgorithm(name);
  } else {
    if ((iter->name.startsWith("ML-DSA-"_kj) || iter->name.startsWith("ML-KEM-"_kj)) &&
        !Worker::Api::current().getFeatureFlags().getWebCryptoModernAlgorithms()) {
      return kj::none;
    }
    return *iter;
  }
}

// =======================================================================================
// Helper functions

// Throws InvalidAccessError if the key is incompatible with the given normalized algorithm name,
// or if it doesn't support the given usage.
void validateOperation(const CryptoKey& key, kj::StringPtr requestedName, CryptoKeyUsageSet usage) {
  // TODO(someday): Throw a NotSupportedError? The Web Crypto API spec says InvalidAccessError, but
  //   Web IDL says that's deprecated.
  //
  // TODO(cleanup): Make this function go away. Maybe this can be rolled into the default
  //   implementations of the CryptoKey::Impl::<crypto operation>() functions.

  JSG_REQUIRE(strcasecmp(requestedName.cStr(), key.getAlgorithmName().cStr()) == 0,
      DOMInvalidAccessError, "Requested algorithm \"", requestedName,
      "\" does not match this CryptoKey's algorithm \"", key.getAlgorithmName(), "\".");
  JSG_REQUIRE(usage <= key.getUsageSet(), DOMInvalidAccessError, "Requested key usage \"",
      usage.name(), "\" does not match any usage listed in this CryptoKey.");
}

// Helper for `deriveKey()`. This private crypto operation is actually defined by the spec as
// the "get key length" operation.
kj::Maybe<uint32_t> getKeyLength(const SubtleCrypto::ImportKeyAlgorithm& derivedKeyAlgorithm) {

  kj::StringPtr algName = derivedKeyAlgorithm.name;

  // TODO(cleanup): This should be a method of CryptoKey::Impl so it can be abstracted. Currently
  //   we ad-hoc match various algorithms below, so the set of supported algorithms must be
  //   hard-coded.
  static const std::set<kj::StringPtr, CiLess> registeredAlgorithms{
    {"AES-CTR"},
    {"AES-CBC"},
    {"AES-GCM"},
    {"AES-KW"},
    {"HMAC"},
    {"HKDF"},
    {"PBKDF2"},
  };
  auto algIter = registeredAlgorithms.find(algName);
  JSG_REQUIRE(algIter != registeredAlgorithms.end(), DOMNotSupportedError,
      "Unrecognized derived key type \"", algName, "\" requested.");

  // We could implement getKeyLength() with the same map-of-strings-to-implementation-functions
  // strategy as the rest of the crypto operations, but this function is so simple that it hardly
  // seems worth the bother. The spec only identifies three cases: the AES family, HMAC, and the KDF
  // algorithms.
  if (algIter->startsWith("AES-")) {
    int length = JSG_REQUIRE_NONNULL(
        derivedKeyAlgorithm.length, TypeError, "Missing field \"length\" in \"derivedKeyParams\".");
    switch (length) {
      case 128:
        [[fallthrough]];
      case 192:
        [[fallthrough]];
      case 256:
        break;
      default:
        JSG_FAIL_REQUIRE(DOMOperationError,
            "Derived AES key must be 128, 192, or 256 bits in length but provided ", length, ".");
    }
    return length;
  } else if (*algIter == "HMAC") {
    KJ_IF_SOME(length, derivedKeyAlgorithm.length) {
      // If the user requested a specific HMAC key length, honor it.
      if (length > 0) {
        return length;
      }
      JSG_FAIL_REQUIRE(TypeError, "HMAC key length must be a non-zero unsigned long integer.");
    }
    // Otherwise, assume the user wants the default HMAC key size.
    auto digestAlg = getAlgorithmName(JSG_REQUIRE_NONNULL(
        derivedKeyAlgorithm.hash, TypeError, "Missing field \"hash\" in \"derivedKeyParams\"."));
    return EVP_MD_block_size(lookupDigestAlgorithm(digestAlg).second) * 8;
  } else {
    // HKDF or PBKDF2. I'm not not sure what it means to derive a HKDF/PBKDF2 key from a base key
    // (are you deriving a password from a password?) but based on my reading of the spec, this code
    // path will become meaningful once we support ECDH, which handles null-length deriveBits()
    // operations. This is the entire reason getKeyLength() returns a Maybe<uint32_t> rather than a
    // uint32_t (and also why we do not throw an OperationError here but rather later on in
    // deriveBitsPbkdf2Impl()).
    return kj::none;
  }
}

enum class SubtleOperation {
  ENCRYPT,
  DECRYPT,
  SIGN,
  VERIFY,
  DIGEST,
  GENERATE_KEY,
  DERIVE_KEY,
  DERIVE_BITS,
  IMPORT_KEY,
  EXPORT_KEY,
  WRAP_KEY,
  UNWRAP_KEY,
  ENCAPSULATE_KEY,
  ENCAPSULATE_BITS,
  DECAPSULATE_KEY,
  DECAPSULATE_BITS,
  GET_PUBLIC_KEY,
};

kj::Maybe<SubtleOperation> parseSubtleOperation(kj::StringPtr operation) {
  struct OperationMapping {
    kj::StringPtr name;
    SubtleOperation operation;
  };

  static constexpr OperationMapping OPERATIONS[] = {
    {"encrypt"_kj, SubtleOperation::ENCRYPT},
    {"decrypt"_kj, SubtleOperation::DECRYPT},
    {"sign"_kj, SubtleOperation::SIGN},
    {"verify"_kj, SubtleOperation::VERIFY},
    {"digest"_kj, SubtleOperation::DIGEST},
    {"generateKey"_kj, SubtleOperation::GENERATE_KEY},
    {"deriveKey"_kj, SubtleOperation::DERIVE_KEY},
    {"deriveBits"_kj, SubtleOperation::DERIVE_BITS},
    {"importKey"_kj, SubtleOperation::IMPORT_KEY},
    {"exportKey"_kj, SubtleOperation::EXPORT_KEY},
    {"wrapKey"_kj, SubtleOperation::WRAP_KEY},
    {"unwrapKey"_kj, SubtleOperation::UNWRAP_KEY},
    {"encapsulateKey"_kj, SubtleOperation::ENCAPSULATE_KEY},
    {"encapsulateBits"_kj, SubtleOperation::ENCAPSULATE_BITS},
    {"decapsulateKey"_kj, SubtleOperation::DECAPSULATE_KEY},
    {"decapsulateBits"_kj, SubtleOperation::DECAPSULATE_BITS},
    {"getPublicKey"_kj, SubtleOperation::GET_PUBLIC_KEY},
  };

  for (const auto& item: OPERATIONS) {
    if (operation == item.name) return item.operation;
  }
  return kj::none;
}

template <typename Algorithm>
Algorithm normalizeSupportAlgorithm(jsg::Lock& js,
    v8::Local<v8::Value> value,
    const jsg::TypeHandler<kj::OneOf<kj::String, Algorithm>>& handler) {
  KJ_IF_SOME(algorithm, handler.tryUnwrap(js, value)) {
    return interpretAlgorithmParam(kj::mv(algorithm));
  }

  JSG_FAIL_REQUIRE(TypeError, "AlgorithmIdentifier could not be converted.");
}

bool isOneOf(kj::StringPtr value, std::initializer_list<kj::StringPtr> names) {
  return std::find(names.begin(), names.end(), value) != names.end();
}

void validateAesKeyLength(int length) {
  JSG_REQUIRE(length == 128 || length == 192 || length == 256, DOMOperationError,
      "AES key length must be 128, 192, or 256 bits.");
}

void validateAesGcmTagLength(int tagLength) {
  switch (tagLength) {
    case 32:
    case 64:
    case 96:
    case 104:
    case 112:
    case 120:
    case 128:
      return;
    default:
      JSG_FAIL_REQUIRE(DOMOperationError, "Invalid AES-GCM tag length ", tagLength, ".");
  }
}

void validateNamedCurve(kj::StringPtr namedCurve) {
  JSG_REQUIRE(namedCurve == "P-256" || namedCurve == "P-384" || namedCurve == "P-521",
      DOMNotSupportedError, "Unsupported namedCurve \"", namedCurve, "\".");
}

void validateHashAlgorithm(
    const kj::Maybe<kj::OneOf<kj::String, SubtleCrypto::HashAlgorithm>>& hash,
    kj::StringPtr fieldName = "algorithm") {
  lookupDigestAlgorithm(api::getAlgorithmName(
      JSG_REQUIRE_NONNULL(hash, TypeError, "Missing field \"hash\" in \"", fieldName, "\".")));
}

uint32_t getEcdhMaxDeriveBits(jsg::Lock& js, const CryptoKey& publicKey) {
  auto algorithm = publicKey.getAlgorithmName();
  if (algorithm == "X25519") {
    return 256;
  }

  auto keyAlgorithm = publicKey.getAlgorithm(js).get<CryptoKey::EllipticKeyAlgorithm>();
  auto namedCurve = keyAlgorithm.namedCurve;
  if (namedCurve == "P-256") return 256;
  if (namedCurve == "P-384") return 384;
  KJ_ASSERT(namedCurve == "P-521", namedCurve);
  return 521;
}

void validateGenerateKeyAlgorithm(jsg::Lock& js,
    kj::StringPtr normalizedName,
    const SubtleCrypto::GenerateKeyAlgorithm& algorithm) {
  if (normalizedName.startsWith("AES-")) {
    validateAesKeyLength(JSG_REQUIRE_NONNULL(
        algorithm.length, TypeError, "Missing field \"length\" in \"algorithm\"."));
  } else if (normalizedName == "HMAC") {
    validateHashAlgorithm(algorithm.hash);
    KJ_IF_SOME(length, algorithm.length) {
      JSG_REQUIRE(length > 0, DOMOperationError,
          "HMAC key length must be a non-zero unsigned long integer.");
    }
  } else if (normalizedName == "RSASSA-PKCS1-v1_5" || normalizedName == "RSA-PSS" ||
      normalizedName == "RSA-OAEP") {
    auto publicExponent = JSG_REQUIRE_NONNULL(
        algorithm.publicExponent, TypeError, "Missing field \"publicExponent\" in \"algorithm\".")
                              .getHandle(js);
    validateHashAlgorithm(algorithm.hash);
    auto modulusLength = JSG_REQUIRE_NONNULL(
        algorithm.modulusLength, TypeError, "Missing field \"modulusLength\" in \"algorithm\".");
    JSG_REQUIRE(modulusLength > 0, DOMOperationError,
        "modulusLength must be greater than zero (requested ", modulusLength, ").");
    Rsa::validateRsaParams(js, modulusLength, publicExponent.asArrayPtr());
    JSG_REQUIRE(!(FeatureFlags::get(js).getStrictCrypto() && (modulusLength & 127)),
        DOMOperationError, "Can't generate key: RSA key size is required to be a multiple of 128");
  } else if (normalizedName == "ECDSA" || normalizedName == "ECDH") {
    validateNamedCurve(JSG_REQUIRE_NONNULL(
        algorithm.namedCurve, TypeError, "Missing field \"namedCurve\" in \"algorithm\"."));
  } else if (normalizedName == "NODE-ED25519") {
    const auto& namedCurve = JSG_REQUIRE_NONNULL(
        algorithm.namedCurve, TypeError, "Missing field \"namedCurve\" in \"algorithm\".");
    JSG_REQUIRE(namedCurve == "NODE-ED25519", DOMNotSupportedError, "EDDSA curve \"", namedCurve,
        "\" isn't supported.");
  }
}

void validateImportKeyAlgorithm(
    kj::StringPtr normalizedName, const SubtleCrypto::ImportKeyAlgorithm& algorithm) {
  if (normalizedName == "HMAC" || normalizedName == "RSASSA-PKCS1-v1_5" ||
      normalizedName == "RSA-PSS" || normalizedName == "RSA-OAEP") {
    validateHashAlgorithm(algorithm.hash);
    if (normalizedName == "HMAC") {
      KJ_IF_SOME(length, algorithm.length) {
        JSG_REQUIRE(length > 0, DOMDataError, "Imported HMAC key length (", length,
            ") must be a non-zero value.");
      }
    }
  } else if (normalizedName == "ECDSA" || normalizedName == "ECDH") {
    validateNamedCurve(JSG_REQUIRE_NONNULL(
        algorithm.namedCurve, TypeError, "Missing field \"namedCurve\" in \"algorithm\"."));
  } else if (normalizedName == "NODE-ED25519") {
    const auto& namedCurve = JSG_REQUIRE_NONNULL(
        algorithm.namedCurve, TypeError, "Missing field \"namedCurve\" in \"algorithm\".");
    JSG_REQUIRE(namedCurve == "NODE-ED25519", DOMNotSupportedError, "EDDSA curve \"", namedCurve,
        "\" isn't supported.");
  }
}

bool supportsEncryptDecrypt(kj::StringPtr normalizedName) {
  return isOneOf(normalizedName, {"AES-CTR"_kj, "AES-CBC"_kj, "AES-GCM"_kj, "RSA-OAEP"_kj});
}

bool supportsSign(kj::StringPtr normalizedName) {
  return isOneOf(normalizedName,
      {"RSASSA-PKCS1-v1_5"_kj, "RSA-PSS"_kj, "ECDSA"_kj, "NODE-ED25519"_kj, "Ed25519"_kj, "HMAC"_kj,
        "RSA-RAW"_kj, "ML-DSA-44"_kj, "ML-DSA-65"_kj, "ML-DSA-87"_kj});
}

bool supportsVerify(kj::StringPtr normalizedName) {
  return isOneOf(normalizedName,
      {"RSASSA-PKCS1-v1_5"_kj, "RSA-PSS"_kj, "ECDSA"_kj, "NODE-ED25519"_kj, "Ed25519"_kj, "HMAC"_kj,
        "ML-DSA-44"_kj, "ML-DSA-65"_kj, "ML-DSA-87"_kj});
}

bool supportsDeriveBits(kj::StringPtr normalizedName) {
  return isOneOf(normalizedName, {"PBKDF2"_kj, "HKDF"_kj, "ECDH"_kj, "X25519"_kj});
}

bool supportsExportKey(kj::StringPtr normalizedName) {
  return isOneOf(normalizedName,
      {"AES-CTR"_kj, "AES-CBC"_kj, "AES-GCM"_kj, "AES-KW"_kj, "HMAC"_kj, "RSASSA-PKCS1-v1_5"_kj,
        "RSA-PSS"_kj, "RSA-OAEP"_kj, "RSA-RAW"_kj, "ECDSA"_kj, "ECDH"_kj, "NODE-ED25519"_kj,
        "Ed25519"_kj, "X25519"_kj, "ML-DSA-44"_kj, "ML-DSA-65"_kj, "ML-DSA-87"_kj, "ML-KEM-768"_kj,
        "ML-KEM-1024"_kj});
}

bool supportsEncapsulate(kj::StringPtr normalizedName) {
  return isOneOf(normalizedName, {"ML-KEM-768"_kj, "ML-KEM-1024"_kj});
}

bool supportsGetPublicKey(kj::StringPtr normalizedName) {
  return isOneOf(normalizedName,
      {"RSA-OAEP"_kj, "ECDH"_kj, "X25519"_kj, "ML-KEM-768"_kj, "ML-KEM-1024"_kj, "ECDSA"_kj,
        "Ed25519"_kj, "RSA-PSS"_kj, "RSASSA-PKCS1-v1_5"_kj, "ML-DSA-44"_kj, "ML-DSA-65"_kj,
        "ML-DSA-87"_kj});
}

void validateEncryptAlgorithm(
    jsg::Lock& js, kj::StringPtr normalizedName, const SubtleCrypto::EncryptAlgorithm& algorithm) {
  if (normalizedName == "AES-GCM") {
    auto iv = JSG_REQUIRE_NONNULL(algorithm.iv, TypeError, "Missing field \"iv\" in \"algorithm\".")
                  .getHandle(js);
    JSG_REQUIRE(iv.size() != 0, DOMOperationError, "AES-GCM IV must not be empty.");
    validateAesGcmTagLength(algorithm.tagLength.orDefault(128));
  } else if (normalizedName == "AES-CBC") {
    auto iv = JSG_REQUIRE_NONNULL(algorithm.iv, TypeError, "Missing field \"iv\" in \"algorithm\".")
                  .getHandle(js);
    JSG_REQUIRE(iv.size() == 16, DOMOperationError, "AES-CBC IV must be 16 bytes long.");
  } else if (normalizedName == "AES-CTR") {
    auto counter = JSG_REQUIRE_NONNULL(
        algorithm.counter, TypeError, "Missing \"counter\" member in \"algorithm\".")
                       .getHandle(js);
    JSG_REQUIRE(counter.size() == 16, DOMOperationError, "Counter must have length of 16 bytes.");
    auto counterLength = JSG_REQUIRE_NONNULL(
        algorithm.length, TypeError, "Missing \"length\" member in \"algorithm\".");
    JSG_REQUIRE(counterLength > 0 && counterLength <= 128, DOMOperationError, "Invalid counter of ",
        counterLength, " bits length provided.");
  }
}

void validateSignAlgorithm(
    kj::StringPtr normalizedName, const SubtleCrypto::SignAlgorithm& algorithm) {
  if (normalizedName == "ECDSA") {
    validateHashAlgorithm(algorithm.hash, "AlgorithmIdentifier");
  } else if (normalizedName == "RSA-PSS") {
    auto saltLength = JSG_REQUIRE_NONNULL(algorithm.saltLength, TypeError,
        "Failed to provide salt for RSA-PSS key operation which requires a salt");
    JSG_REQUIRE(saltLength >= 0, DOMDataError, "SaltLength for RSA-PSS must be non-negative.");
  }
}

void validateDeriveBitsAlgorithm(jsg::Lock& js,
    kj::StringPtr normalizedName,
    const SubtleCrypto::DeriveKeyAlgorithm& algorithm,
    kj::Maybe<uint32_t> maybeLength) {
  if (normalizedName == "HKDF" || normalizedName == "PBKDF2") {
    validateHashAlgorithm(algorithm.hash);
    JSG_REQUIRE_NONNULL(algorithm.salt, TypeError, "Missing field \"salt\" in \"algorithm\".");
    auto length = JSG_REQUIRE_NONNULL(maybeLength, DOMOperationError, normalizedName,
        " cannot derive a key "
        "with null length.");
    JSG_REQUIRE(length % 8 == 0, DOMOperationError, normalizedName,
        " requires a derived key length that is a multiple of eight.");

    if (normalizedName == "HKDF") {
      JSG_REQUIRE_NONNULL(algorithm.info, TypeError, "Missing field \"info\" in \"algorithm\".");
    } else {
      auto iterations = JSG_REQUIRE_NONNULL(
          algorithm.iterations, TypeError, "Missing field \"iterations\" in \"algorithm\".");
      JSG_REQUIRE(iterations > 0, DOMOperationError,
          "PBKDF2 requires a positive iteration count (requested ", iterations, ").");
      checkPbkdfLimits(js, iterations);
    }
  } else if (normalizedName == "ECDH" || normalizedName == "X25519") {
    auto& publicKey = JSG_REQUIRE_NONNULL(
        algorithm.$public, TypeError, "Missing field \"public\" in \"derivedKeyParams\".");
    JSG_REQUIRE(publicKey->getType() == "public"_kj, DOMInvalidAccessError,
        "The provided key has type \"", publicKey->getType(), "\", not \"public\"");
    JSG_REQUIRE(strcasecmp(publicKey->getAlgorithmName().cStr(), normalizedName.cStr()) == 0,
        DOMInvalidAccessError, "Public key algorithm does not match the requested algorithm.");
    KJ_IF_SOME(length, maybeLength) {
      auto maxDeriveBits = getEcdhMaxDeriveBits(js, *publicKey);
      JSG_REQUIRE(length <= maxDeriveBits, DOMOperationError, "Derived key length (", length,
          " bits) is too long (should be at most ", maxDeriveBits, " bits).");
    }
  }
}

kj::Maybe<uint32_t> readSupportsLength(jsg::Lock& js, v8::Local<v8::Value> value) {
  if (value->IsUndefined() || value->IsNull()) return kj::none;
  if (!value->IsNumber()) return kj::none;

  auto number = jsg::check(value->NumberValue(js.v8Context()));
  JSG_REQUIRE(std::isfinite(number) && number >= 0 &&
          number <= std::numeric_limits<uint32_t>::max() && number == std::floor(number),
      TypeError, "length must be an unsigned long integer.");
  return static_cast<uint32_t>(number);
}

bool isAdditionalAlgorithmArgument(v8::Local<v8::Value> value) {
  return !value->IsUndefined() && !value->IsNull() && !value->IsNumber();
}

auto webCryptoOperationBegin(
    const char* operation, kj::StringPtr algorithm, kj::Maybe<kj::StringPtr> context = kj::none) {
  // This clears all OpenSSL errors & errno at the start & returns a deferred evaluation to make
  // sure that, when the WebCrypto entrypoint completes, there are no errors hanging around.
  // Context is used for adding contextual information (e.g. the algorithm name of the key being
  // wrapped, the import/export format being processed etc).
  ERR_clear_error();
  ERR_clear_system_error();

  // Ok to capture pointers by value because this will only be used for the duration of the parent's
  // scope which is passing in these arguments.
  return kj::defer([=] {
    if (ERR_peek_error() != 0) {
      auto allErrors = KJ_MAP(e, consumeAllOpensslErrors()) {
        KJ_SWITCH_ONEOF(e) {
          KJ_CASE_ONEOF(friendly, kj::StringPtr) {
            return kj::str(friendly);
          }
          KJ_CASE_ONEOF(raw, OpensslUntranslatedError) {
            return kj::str(raw.library, "::", raw.reasonName);
          }
        }

        KJ_UNREACHABLE;
      };

      kj::String stringifiedOperation;
      KJ_IF_SOME(c, context) {
        stringifiedOperation = kj::str(operation, "(", c, ")");
      } else {
        stringifiedOperation = kj::str(operation);
      }
      KJ_LOG(WARNING, "WebCrypto didn't handle all BoringSSL errors", stringifiedOperation,
          algorithm, allErrors);
    }
  });
}

auto webCryptoOperationBegin(
    const char* operation, kj::StringPtr algorithm, const kj::String& context) {
  return webCryptoOperationBegin(operation, algorithm, context.slice(0));
}

template <typename T,
    typename = kj::EnableIf<kj::isSameType<kj::String, decltype(kj::instance<T>().name)>()>>
[[gnu::always_inline]] auto webCryptoOperationBegin(
    const char* operation, const T& algorithm, kj::Maybe<kj::StringPtr> context = kj::none) {
  return kj::defer([operation, algorithm = kj::str(algorithm.name), context] {
    // We need a copy of the algorithm name as this defer runs after the EncryptAlgorithm struct
    // is destroyed.
    (void)webCryptoOperationBegin(operation, algorithm, context);
  });
}

}  // namespace

// =======================================================================================
// CryptoKey / SubtleCrypto implementations

CryptoKey::CryptoKey(kj::Own<Impl> impl): impl(kj::mv(impl)) {}
CryptoKey::~CryptoKey() noexcept(false) {}
kj::StringPtr CryptoKey::getAlgorithmName() const {
  return impl->getAlgorithmName();
}
CryptoKey::AlgorithmVariant CryptoKey::getAlgorithm(jsg::Lock& js) const {
  return impl->getAlgorithm(js);
}
kj::StringPtr CryptoKey::getType() const {
  return impl->getType();
}
bool CryptoKey::getExtractable() const {
  return impl->isExtractable();
}
kj::Array<kj::StringPtr> CryptoKey::getUsages() const {
  return getUsageSet().map([](auto singleton) { return singleton.name(); });
}
CryptoKeyUsageSet CryptoKey::getUsageSet() const {
  return impl->getUsages();
}

bool CryptoKey::operator==(const CryptoKey& other) const {
  // We check this first because we don't want any comparison to happen if
  // either key is not extractable, even if they are the same object.
  if (!getExtractable() || !other.getExtractable()) {
    return false;
  }
  return this == &other || (getType() == other.getType() && impl->equals(*other.impl));
}

CryptoKey::AsymmetricKeyDetails CryptoKey::getAsymmetricKeyDetails(jsg::Lock& js) const {
  return impl->getAsymmetricKeyDetail(js);
}

bool CryptoKey::verifyX509Public(const X509* cert) const {
  if (this->getType() != "public"_kj) return false;
  return impl->verifyX509Public(cert);
}

bool CryptoKey::verifyX509Private(const X509* cert) const {
  if (this->getType() != "private"_kj) return false;
  return impl->verifyX509Private(cert);
}

void CryptoKey::visitForGc(jsg::GcVisitor& visitor) {
  if (impl.get() == nullptr) return;
  impl->visitForGc(visitor);
}

jsg::Promise<jsg::JsRef<jsg::JsArrayBuffer>> SubtleCrypto::encrypt(jsg::Lock& js,
    kj::OneOf<kj::String, EncryptAlgorithm> algorithmParam,
    const CryptoKey& key,
    jsg::JsBufferSource plainText) {
  auto algorithm = interpretAlgorithmParam(kj::mv(algorithmParam));

  auto checkErrorsOnFinish = webCryptoOperationBegin(__func__, algorithm);

  return js.evalNow([&] {
    validateOperation(key, algorithm.name, CryptoKeyUsageSet::encrypt());
    return key.impl->encrypt(js, kj::mv(algorithm), nonNullBytes(plainText.asArrayPtr()))
        .addRef(js);
  });
}

jsg::Promise<jsg::JsRef<jsg::JsArrayBuffer>> SubtleCrypto::decrypt(jsg::Lock& js,
    kj::OneOf<kj::String, EncryptAlgorithm> algorithmParam,
    const CryptoKey& key,
    jsg::JsBufferSource cipherText) {
  auto algorithm = interpretAlgorithmParam(kj::mv(algorithmParam));

  auto checkErrorsOnFinish = webCryptoOperationBegin(__func__, algorithm);

  return js.evalNow([&] {
    validateOperation(key, algorithm.name, CryptoKeyUsageSet::decrypt());
    return key.impl->decrypt(js, kj::mv(algorithm), nonNullBytes(cipherText.asArrayPtr()))
        .addRef(js);
  });
}

jsg::Promise<jsg::JsRef<jsg::JsArrayBuffer>> SubtleCrypto::sign(jsg::Lock& js,
    kj::OneOf<kj::String, SignAlgorithm> algorithmParam,
    const CryptoKey& key,
    jsg::JsBufferSource data) {
  auto algorithm = interpretAlgorithmParam(kj::mv(algorithmParam));

  auto checkErrorsOnFinish = webCryptoOperationBegin(__func__, algorithm);

  return js.evalNow([&] {
    validateOperation(key, algorithm.name, CryptoKeyUsageSet::sign());
    return key.impl->sign(js, kj::mv(algorithm), nonNullBytes(data.asArrayPtr())).addRef(js);
  });
}

jsg::Promise<bool> SubtleCrypto::verify(jsg::Lock& js,
    kj::OneOf<kj::String, SignAlgorithm> algorithmParam,
    const CryptoKey& key,
    jsg::JsBufferSource signature,
    jsg::JsBufferSource data) {
  auto algorithm = interpretAlgorithmParam(kj::mv(algorithmParam));

  auto checkErrorsOnFinish = webCryptoOperationBegin(__func__, algorithm);

  return js.evalNow([&] {
    validateOperation(key, algorithm.name, CryptoKeyUsageSet::verify());
    return key.impl->verify(js, kj::mv(algorithm), nonNullBytes(signature.asArrayPtr()),
        nonNullBytes(data.asArrayPtr()));
  });
}

jsg::Promise<jsg::JsRef<jsg::JsArrayBuffer>> SubtleCrypto::digest(
    jsg::Lock& js, kj::OneOf<kj::String, HashAlgorithm> algorithmParam, jsg::JsBufferSource data) {
  auto algorithm = interpretAlgorithmParam(kj::mv(algorithmParam));

  auto checkErrorsOnFinish = webCryptoOperationBegin(__func__, algorithm);

  return js.evalNow([&] {
    auto ptr = nonNullBytes(data.asArrayPtr());
    auto type = lookupDigestAlgorithm(algorithm.name).second;

    auto digestCtx = kj::disposeWith<EVP_MD_CTX_free>(EVP_MD_CTX_new());
    KJ_ASSERT(digestCtx.get() != nullptr);

    OSSLCALL(EVP_DigestInit_ex(digestCtx.get(), type, nullptr));
    OSSLCALL(EVP_DigestUpdate(digestCtx.get(), ptr.begin(), ptr.size()));

    auto buf = jsg::JsArrayBuffer::create(js, EVP_MD_CTX_size(digestCtx.get()));
    uint messageDigestSize = 0;
    OSSLCALL(EVP_DigestFinal_ex(digestCtx.get(), buf.asArrayPtr().begin(), &messageDigestSize));

    KJ_ASSERT(messageDigestSize == buf.size());
    return buf.addRef(js);
  });
}

jsg::Promise<kj::OneOf<jsg::Ref<CryptoKey>, CryptoKeyPair>> SubtleCrypto::generateKey(jsg::Lock& js,
    kj::OneOf<kj::String, GenerateKeyAlgorithm> algorithmParam,
    bool extractable,
    kj::Array<kj::String> keyUsages) {

  auto algorithm = interpretAlgorithmParam(kj::mv(algorithmParam));

  auto checkErrorsOnFinish = webCryptoOperationBegin(__func__, algorithm);

  return js.evalNow([&] {
    CryptoAlgorithm algoImpl = lookupAlgorithm(algorithm.name).orDefault({});
    JSG_REQUIRE(algoImpl.generateFunc != nullptr, DOMNotSupportedError,
        "Unrecognized key generation algorithm \"", algorithm.name, "\" requested.");

    auto cryptoKeyOrPair =
        algoImpl.generateFunc(js, algoImpl.name, kj::mv(algorithm), extractable, keyUsages);
    KJ_SWITCH_ONEOF(cryptoKeyOrPair) {
      KJ_CASE_ONEOF(cryptoKey, jsg::Ref<CryptoKey>) {
        if (keyUsages.size() == 0) {
          auto type = cryptoKey->getType();
          JSG_REQUIRE(type != "secret" && type != "private", DOMSyntaxError,
              "Secret/private CryptoKeys must have at least one usage.");
        }
      }
      KJ_CASE_ONEOF(keyPair, CryptoKeyPair) {
        JSG_REQUIRE(keyPair.privateKey->getUsageSet().size() != 0, DOMSyntaxError,
            "Attempt to generate asymmetric keys with no valid private key usages.");
      }
    }
    return cryptoKeyOrPair;
  });
}

jsg::Promise<jsg::Ref<CryptoKey>> SubtleCrypto::deriveKey(jsg::Lock& js,
    kj::OneOf<kj::String, DeriveKeyAlgorithm> algorithmParam,
    const CryptoKey& baseKey,
    kj::OneOf<kj::String, ImportKeyAlgorithm> derivedKeyAlgorithmParam,
    bool extractable,
    kj::Array<kj::String> keyUsages) {
  auto algorithm = interpretAlgorithmParam(kj::mv(algorithmParam));
  auto derivedKeyAlgorithm = interpretAlgorithmParam(kj::mv(derivedKeyAlgorithmParam));

  auto checkErrorsOnFinish = webCryptoOperationBegin(__func__, algorithm);

  return js.evalNow([&] {
    validateOperation(baseKey, algorithm.name, CryptoKeyUsageSet::deriveKey());

    auto length = getKeyLength(derivedKeyAlgorithm);

    auto secret = jsg::JsBufferSource(baseKey.impl->deriveBits(js, kj::mv(algorithm), length));

    // TODO(perf): For conformance, importKey() makes a copy of `secret`. In this case we really
    //   don't need to, but rather we ought to call the appropriate CryptoKey::Impl::import*()
    //   function directly.
    return importKeySync(js, "raw-secret", secret.addRef(js), kj::mv(derivedKeyAlgorithm),
        extractable, kj::mv(keyUsages));
  });
}

jsg::Promise<jsg::JsRef<jsg::JsArrayBuffer>> SubtleCrypto::deriveBits(jsg::Lock& js,
    kj::OneOf<kj::String, DeriveKeyAlgorithm> algorithmParam,
    const CryptoKey& baseKey,
    jsg::Optional<kj::Maybe<int>> lengthParam) {
  auto algorithm = interpretAlgorithmParam(kj::mv(algorithmParam));

  auto checkErrorsOnFinish = webCryptoOperationBegin(__func__, algorithm);

  kj::Maybe<uint32_t> length = kj::none;
  KJ_IF_SOME(maybeLength, lengthParam) {
    KJ_IF_SOME(l, maybeLength) {
      JSG_REQUIRE(l >= 0, TypeError, "deriveBits length must be an unsigned long integer.");
      length = static_cast<uint32_t>(l);
    }
  }

  return js.evalNow([&] {
    validateOperation(baseKey, algorithm.name, CryptoKeyUsageSet::deriveBits());
    return baseKey.impl->deriveBits(js, kj::mv(algorithm), length).addRef(js);
  });
}

jsg::Promise<jsg::JsRef<jsg::JsArrayBuffer>> SubtleCrypto::wrapKey(jsg::Lock& js,
    kj::String format,
    const CryptoKey& key,
    const CryptoKey& wrappingKey,
    kj::OneOf<kj::String, EncryptAlgorithm> wrapAlgorithm,
    const jsg::TypeHandler<JsonWebKey>& jwkHandler) {
  auto checkErrorsOnFinish =
      webCryptoOperationBegin(__func__, wrappingKey.getAlgorithmName(), key.getAlgorithmName());

  return js.evalNow([&] {
    auto algorithm = interpretAlgorithmParam(kj::mv(wrapAlgorithm));

    validateOperation(wrappingKey, algorithm.name, CryptoKeyUsageSet::wrapKey());

    struct PaddingDetails {
      kj::byte byteAlignment;
      size_t minimumLength;
    };

    JSG_REQUIRE(key.getExtractable(), DOMInvalidAccessError, "Attempt to export non-extractable ",
        key.getAlgorithmName(), " key.");

    auto exportedKey = key.impl->exportKey(js, kj::mv(format));

    KJ_SWITCH_ONEOF(exportedKey) {
      KJ_CASE_ONEOF(k, jsg::JsRef<jsg::JsArrayBuffer>) {
        auto handle = k.getHandle(js);
        return wrappingKey.impl->wrapKey(js, kj::mv(algorithm), handle.asArrayPtr().asConst())
            .addRef(js);
      }
      KJ_CASE_ONEOF(jwk, JsonWebKey) {
        auto stringified = js.serializeJson(jwkHandler.wrap(js, kj::mv(jwk)));
        return wrappingKey.impl->wrapKey(js, kj::mv(algorithm), stringified.asBytes().asConst())
            .addRef(js);
      }
    }

    KJ_UNREACHABLE;
  });
}

jsg::Promise<jsg::Ref<CryptoKey>> SubtleCrypto::unwrapKey(jsg::Lock& js,
    kj::String format,
    jsg::JsBufferSource wrappedKey,
    const CryptoKey& unwrappingKey,
    kj::OneOf<kj::String, EncryptAlgorithm> unwrapAlgorithm,
    kj::OneOf<kj::String, ImportKeyAlgorithm> unwrappedKeyAlgorithm,
    bool extractable,
    kj::Array<kj::String> keyUsages,
    const jsg::TypeHandler<JsonWebKey>& jwkHandler) {
  auto operation = __func__;
  return js.evalNow([&]() -> jsg::Ref<CryptoKey> {
    auto normalizedAlgorithm = interpretAlgorithmParam(kj::mv(unwrapAlgorithm));
    auto normalizedUnwrapAlgorithm = interpretAlgorithmParam(kj::mv(unwrappedKeyAlgorithm));

    // Need a copy of the algorithm name to live in this scope, because we later kj::mv() it out.
    auto context = kj::str(normalizedUnwrapAlgorithm.name);
    auto checkErrorsOnFinish =
        webCryptoOperationBegin(operation, unwrappingKey.getAlgorithmName(), context);

    validateOperation(unwrappingKey, normalizedAlgorithm.name, CryptoKeyUsageSet::unwrapKey());

    auto bytes = unwrappingKey.impl->unwrapKey(
        js, kj::mv(normalizedAlgorithm), nonNullBytes(wrappedKey.asArrayPtr()));

    ImportKeyData importData;

    if (format == "jwk") {
      auto jwkDict = js.parseJson(bytes.asArrayPtr().asChars());

      importData = JSG_REQUIRE_NONNULL(jwkHandler.tryUnwrap(js, jwkDict.getHandle(js)),
          DOMDataError, "Missing \"kty\" field or corrupt JSON unwrapping key?");
    } else {
      importData = jsg::JsBufferSource(bytes).addRef(js);
    }

    auto imported = importKeySync(js, format, kj::mv(importData), kj::mv(normalizedUnwrapAlgorithm),
        extractable, keyUsages.asPtr());

    if (imported->getType() == "secret" || imported->getType() == "private") {
      JSG_REQUIRE(imported->getUsageSet().size() != 0, DOMSyntaxError,
          "Secret/private CryptoKeys must have at least one usage.");
    }

    return imported;
  });
}

jsg::Promise<jsg::Ref<CryptoKey>> SubtleCrypto::importKey(jsg::Lock& js,
    kj::String format,
    ImportKeyData keyData,
    kj::OneOf<kj::String, ImportKeyAlgorithm> algorithmParam,
    bool extractable,
    kj::Array<kj::String> keyUsages) {
  auto algorithm = interpretAlgorithmParam(kj::mv(algorithmParam));

  auto checkErrorsOnFinish = webCryptoOperationBegin(__func__, algorithm, format.asPtr());

  return js.evalNow([&] {
    return importKeySync(js, format, kj::mv(keyData), kj::mv(algorithm), extractable, keyUsages);
  });
}

jsg::Ref<CryptoKey> SubtleCrypto::importKeySync(jsg::Lock& js,
    kj::StringPtr format,
    ImportKeyData keyData,
    ImportKeyAlgorithm algorithm,
    bool extractable,
    kj::ArrayPtr<const kj::String> keyUsages) {
  if (format == "raw" || format == "pkcs8" || format == "spki" || format == "raw-public" ||
      format == "raw-private" || format == "raw-seed" || format == "raw-secret") {
    auto& key = JSG_REQUIRE_NONNULL(keyData.tryGet<jsg::JsRef<jsg::JsBufferSource>>(), TypeError,
        "Import data provided for buffer-based import formats must be a buffer source.");
    auto keyHandle = key.getHandle(js);

    // Make a copy of the key import data.
    auto copy = jsg::JsUint8Array::create(js, keyHandle.asArrayPtr());
    keyData = jsg::JsBufferSource(copy).addRef(js);
  } else if (format == "jwk") {
    JSG_REQUIRE(keyData.is<JsonWebKey>(), TypeError,
        "Import data provided for \"jwk\" import format must be a JsonWebKey.");
    KJ_IF_SOME(ext, keyData.get<JsonWebKey>().ext) {
      JSG_REQUIRE(ext || !extractable, DOMDataError, "JWK ext field for \"", algorithm.name,
          "\" is set to false but "
          "extractable is true");
    }
  } else {
    // Not prescribed by the spec here, but we might as well bail out here by return. Otherwise,
    // the import function implementations will eventually result in this error.
    JSG_FAIL_REQUIRE(DOMNotSupportedError, "Unrecognized key import format \"", format, "\".");
  }

  CryptoAlgorithm algoImpl = lookupAlgorithm(algorithm.name).orDefault({});
  JSG_REQUIRE(algoImpl.importFunc != nullptr, DOMNotSupportedError,
      "Unrecognized key import algorithm \"", algorithm.name, "\" requested.");

  // Note: we pass in the algorithm name (algoImpl.name) because we know it is uppercase, which
  //   the `name` member of the `algorithm` value itself is not required to be. The individual
  //   implementation functions don't necessarily know the name of the algorithm whose key they're
  //   importing (importKeyAesImpl handles AES-CTR, -CBC, and -GCM, for instance), so they should
  //   rely on this value to set the imported CryptoKey's name.
  auto cryptoKey = js.alloc<CryptoKey>(algoImpl.importFunc(
      js, algoImpl.name, format, kj::mv(keyData), kj::mv(algorithm), extractable, keyUsages));

  if (cryptoKey->getUsageSet().size() == 0) {
    auto type = cryptoKey->getType();
    JSG_REQUIRE(type != "secret" && type != "private", DOMSyntaxError,
        "Secret/private CryptoKeys must have at least one usage.");
  }

  return cryptoKey;
}

jsg::Promise<SubtleCrypto::ExportKeyData> SubtleCrypto::exportKey(
    jsg::Lock& js, kj::String format, const CryptoKey& key) {
  auto checkErrorsOnFinish = webCryptoOperationBegin(__func__, key.getAlgorithmName());

  return js.evalNow([&] {
    // TODO(someday): Throw a NotSupportedError? The Web Crypto API spec says InvalidAccessError,
    //   but Web IDL says that's deprecated.
    JSG_REQUIRE(key.getExtractable(), DOMInvalidAccessError, "Attempt to export non-extractable ",
        key.getAlgorithmName(), " key.");

    return key.impl->exportKey(js, format);
  });
}

jsg::Promise<SubtleCrypto::EncapsulatedBits> SubtleCrypto::encapsulateBits(jsg::Lock& js,
    kj::OneOf<kj::String, ImportKeyAlgorithm> encapsulationAlgorithmParam,
    const CryptoKey& encapsulationKey) {
  auto algorithm = interpretAlgorithmParam(kj::mv(encapsulationAlgorithmParam));

  auto checkErrorsOnFinish = webCryptoOperationBegin(__func__, algorithm);

  return js.evalNow([&]() -> EncapsulatedBits {
    validateOperation(encapsulationKey, algorithm.name, CryptoKeyUsageSet::encapsulateBits());
    auto [sharedKey, ciphertext] = encapsulationKey.impl->encapsulate(js);
    return EncapsulatedBits{
      .sharedKey = sharedKey.addRef(js),
      .ciphertext = ciphertext.addRef(js),
    };
  });
}

jsg::Promise<SubtleCrypto::EncapsulatedKey> SubtleCrypto::encapsulateKey(jsg::Lock& js,
    kj::OneOf<kj::String, ImportKeyAlgorithm> encapsulationAlgorithmParam,
    const CryptoKey& encapsulationKey,
    kj::OneOf<kj::String, ImportKeyAlgorithm> sharedKeyAlgorithmParam,
    bool extractable,
    kj::Array<kj::String> keyUsages) {
  auto algorithm = interpretAlgorithmParam(kj::mv(encapsulationAlgorithmParam));
  auto sharedKeyAlgorithm = interpretAlgorithmParam(kj::mv(sharedKeyAlgorithmParam));

  auto checkErrorsOnFinish = webCryptoOperationBegin(__func__, algorithm);

  return js.evalNow([&]() -> EncapsulatedKey {
    validateOperation(encapsulationKey, algorithm.name, CryptoKeyUsageSet::encapsulateKey());
    auto [sharedKey, ciphertext] = encapsulationKey.impl->encapsulate(js);
    auto sharedKeyBuffer = jsg::JsUint8Array::create(js, sharedKey.asArrayPtr());
    auto sharedKeyRef =
        importKeySync(js, "raw-secret", jsg::JsBufferSource(sharedKeyBuffer).addRef(js),
            kj::mv(sharedKeyAlgorithm), extractable, kj::mv(keyUsages));
    return EncapsulatedKey{
      .sharedKey = kj::mv(sharedKeyRef),
      .ciphertext = ciphertext.addRef(js),
    };
  });
}

jsg::Promise<jsg::JsRef<jsg::JsArrayBuffer>> SubtleCrypto::decapsulateBits(jsg::Lock& js,
    kj::OneOf<kj::String, ImportKeyAlgorithm> decapsulationAlgorithmParam,
    const CryptoKey& decapsulationKey,
    kj::Array<const kj::byte> ciphertext) {
  auto algorithm = interpretAlgorithmParam(kj::mv(decapsulationAlgorithmParam));

  auto checkErrorsOnFinish = webCryptoOperationBegin(__func__, algorithm);

  return js.evalNow([&] {
    validateOperation(decapsulationKey, algorithm.name, CryptoKeyUsageSet::decapsulateBits());
    return decapsulationKey.impl->decapsulate(js, ciphertext).addRef(js);
  });
}

jsg::Promise<jsg::Ref<CryptoKey>> SubtleCrypto::decapsulateKey(jsg::Lock& js,
    kj::OneOf<kj::String, ImportKeyAlgorithm> decapsulationAlgorithmParam,
    const CryptoKey& decapsulationKey,
    kj::Array<const kj::byte> ciphertext,
    kj::OneOf<kj::String, ImportKeyAlgorithm> sharedKeyAlgorithmParam,
    bool extractable,
    kj::Array<kj::String> keyUsages) {
  auto algorithm = interpretAlgorithmParam(kj::mv(decapsulationAlgorithmParam));
  auto sharedKeyAlgorithm = interpretAlgorithmParam(kj::mv(sharedKeyAlgorithmParam));

  auto checkErrorsOnFinish = webCryptoOperationBegin(__func__, algorithm);

  return js.evalNow([&] {
    validateOperation(decapsulationKey, algorithm.name, CryptoKeyUsageSet::decapsulateKey());
    auto secret = decapsulationKey.impl->decapsulate(js, ciphertext);
    auto secretBuffer = jsg::JsUint8Array::create(js, secret.asArrayPtr());
    return importKeySync(js, "raw-secret", jsg::JsBufferSource(secretBuffer).addRef(js),
        kj::mv(sharedKeyAlgorithm), extractable, kj::mv(keyUsages));
  });
}

jsg::Promise<jsg::Ref<CryptoKey>> SubtleCrypto::getPublicKey(
    jsg::Lock& js, const CryptoKey& key, kj::Array<kj::String> keyUsages) {
  auto checkErrorsOnFinish = webCryptoOperationBegin(__func__, key.getAlgorithmName());

  return js.evalNow([&] {
    JSG_REQUIRE(key.getType() != "secret"_kj, DOMNotSupportedError,
        "The getPublicKey operation is not supported for symmetric keys.");
    JSG_REQUIRE(key.getType() == "private"_kj, DOMInvalidAccessError,
        "The getPublicKey operation requires a private key.");

    // Determine the algorithm-specific allowed public key usages.
    auto algorithmName = key.getAlgorithmName();
    CryptoKeyUsageSet allowedUsages;
    if (algorithmName == "RSA-OAEP") {
      allowedUsages = CryptoKeyUsageSet::encrypt() | CryptoKeyUsageSet::wrapKey();
    } else if (algorithmName == "ECDH" || algorithmName == "X25519") {
      allowedUsages = CryptoKeyUsageSet();
    } else if (algorithmName == "ML-KEM-768" || algorithmName == "ML-KEM-1024") {
      allowedUsages = CryptoKeyUsageSet::encapsulateKey() | CryptoKeyUsageSet::encapsulateBits();
    } else if (algorithmName == "ECDSA" || algorithmName == "Ed25519" ||
        algorithmName == "RSA-PSS" || algorithmName == "RSASSA-PKCS1-v1_5" ||
        algorithmName == "ML-DSA-44" || algorithmName == "ML-DSA-65" ||
        algorithmName == "ML-DSA-87") {
      allowedUsages = CryptoKeyUsageSet::verify();
    } else {
      JSG_FAIL_REQUIRE(DOMNotSupportedError, "The getPublicKey operation is not supported for \"",
          algorithmName, "\".");
    }

    auto usages = CryptoKeyUsageSet::validate(
        algorithmName, CryptoKeyUsageSet::Context::importPublic, keyUsages, allowedUsages);

    auto publicKeyImpl = key.impl->getPublicKey(js, usages);
    return js.alloc<CryptoKey>(kj::mv(publicKeyImpl));
  });
}

bool SubtleCrypto::supports(jsg::Lock& js,
    kj::String operation,
    v8::Local<v8::Value> algorithm,
    jsg::Optional<v8::Local<v8::Value>> lengthOrAdditionalAlgorithm,
    const jsg::TypeHandler<kj::OneOf<kj::String, EncryptAlgorithm>>& encryptAlgorithmHandler,
    const jsg::TypeHandler<kj::OneOf<kj::String, SignAlgorithm>>& signAlgorithmHandler,
    const jsg::TypeHandler<kj::OneOf<kj::String, HashAlgorithm>>& digestAlgorithmHandler,
    const jsg::TypeHandler<kj::OneOf<kj::String, GenerateKeyAlgorithm>>&
        generateKeyAlgorithmHandler,
    const jsg::TypeHandler<kj::OneOf<kj::String, DeriveKeyAlgorithm>>& deriveKeyAlgorithmHandler,
    const jsg::TypeHandler<kj::OneOf<kj::String, ImportKeyAlgorithm>>& importKeyAlgorithmHandler) {
  KJ_IF_SOME(parsedOperation, parseSubtleOperation(operation)) {
    return js.tryCatch([&]() -> bool {
      auto checkSupportForAlgorithm = [&](SubtleOperation checkedOperation,
                                          v8::Local<v8::Value> algorithmValue,
                                          kj::Maybe<uint32_t> maybeLength) -> bool {
        switch (checkedOperation) {
          case SubtleOperation::ENCRYPT:
            [[fallthrough]];
          case SubtleOperation::DECRYPT: {
            auto normalizedAlgorithm =
                normalizeSupportAlgorithm(js, algorithmValue, encryptAlgorithmHandler);
            KJ_IF_SOME(algoImpl, lookupAlgorithm(normalizedAlgorithm.name)) {
              if (!supportsEncryptDecrypt(algoImpl.name)) return false;
              validateEncryptAlgorithm(js, algoImpl.name, normalizedAlgorithm);
              return true;
            }
            return false;
          }

          case SubtleOperation::SIGN: {
            auto normalizedAlgorithm =
                normalizeSupportAlgorithm(js, algorithmValue, signAlgorithmHandler);
            KJ_IF_SOME(algoImpl, lookupAlgorithm(normalizedAlgorithm.name)) {
              if (!supportsSign(algoImpl.name)) return false;
              validateSignAlgorithm(algoImpl.name, normalizedAlgorithm);
              return true;
            }
            return false;
          }

          case SubtleOperation::VERIFY: {
            auto normalizedAlgorithm =
                normalizeSupportAlgorithm(js, algorithmValue, signAlgorithmHandler);
            KJ_IF_SOME(algoImpl, lookupAlgorithm(normalizedAlgorithm.name)) {
              if (!supportsVerify(algoImpl.name)) return false;
              validateSignAlgorithm(algoImpl.name, normalizedAlgorithm);
              return true;
            }
            return false;
          }

          case SubtleOperation::DIGEST: {
            auto normalizedAlgorithm =
                normalizeSupportAlgorithm(js, algorithmValue, digestAlgorithmHandler);
            KJ_IF_SOME(exception, kj::runCatchingExceptions([&] {
              lookupDigestAlgorithm(normalizedAlgorithm.name);
            })) {
              (void)exception;
              return false;
            }
            return true;
          }

          case SubtleOperation::GENERATE_KEY: {
            auto normalizedAlgorithm =
                normalizeSupportAlgorithm(js, algorithmValue, generateKeyAlgorithmHandler);
            KJ_IF_SOME(algoImpl, lookupAlgorithm(normalizedAlgorithm.name)) {
              if (algoImpl.generateFunc == nullptr) return false;
              validateGenerateKeyAlgorithm(js, algoImpl.name, normalizedAlgorithm);
              return true;
            }
            return false;
          }

          case SubtleOperation::DERIVE_KEY:
            return false;

          case SubtleOperation::DERIVE_BITS: {
            auto normalizedAlgorithm =
                normalizeSupportAlgorithm(js, algorithmValue, deriveKeyAlgorithmHandler);
            KJ_IF_SOME(algoImpl, lookupAlgorithm(normalizedAlgorithm.name)) {
              if (!supportsDeriveBits(algoImpl.name)) return false;
              validateDeriveBitsAlgorithm(js, algoImpl.name, normalizedAlgorithm, maybeLength);
              return true;
            }
            return false;
          }

          case SubtleOperation::IMPORT_KEY: {
            auto normalizedAlgorithm =
                normalizeSupportAlgorithm(js, algorithmValue, importKeyAlgorithmHandler);
            KJ_IF_SOME(algoImpl, lookupAlgorithm(normalizedAlgorithm.name)) {
              if (algoImpl.importFunc == nullptr) return false;
              validateImportKeyAlgorithm(algoImpl.name, normalizedAlgorithm);
              return true;
            }
            return false;
          }

          case SubtleOperation::EXPORT_KEY: {
            auto normalizedAlgorithm =
                normalizeSupportAlgorithm(js, algorithmValue, importKeyAlgorithmHandler);
            KJ_IF_SOME(algoImpl, lookupAlgorithm(normalizedAlgorithm.name)) {
              return supportsExportKey(algoImpl.name);
            }
            return false;
          }

          case SubtleOperation::WRAP_KEY:
            [[fallthrough]];
          case SubtleOperation::UNWRAP_KEY: {
            auto normalizedAlgorithm =
                normalizeSupportAlgorithm(js, algorithmValue, encryptAlgorithmHandler);
            KJ_IF_SOME(algoImpl, lookupAlgorithm(normalizedAlgorithm.name)) {
              if (algoImpl.name == "AES-KW") return true;
              if (!supportsEncryptDecrypt(algoImpl.name)) return false;
              validateEncryptAlgorithm(js, algoImpl.name, normalizedAlgorithm);
              return true;
            }
            return false;
          }

          case SubtleOperation::ENCAPSULATE_KEY:
            [[fallthrough]];
          case SubtleOperation::ENCAPSULATE_BITS:
            [[fallthrough]];
          case SubtleOperation::DECAPSULATE_KEY:
            [[fallthrough]];
          case SubtleOperation::DECAPSULATE_BITS: {
            auto normalizedAlgorithm =
                normalizeSupportAlgorithm(js, algorithmValue, importKeyAlgorithmHandler);
            KJ_IF_SOME(algoImpl, lookupAlgorithm(normalizedAlgorithm.name)) {
              return supportsEncapsulate(algoImpl.name);
            }
            return false;
          }

          case SubtleOperation::GET_PUBLIC_KEY: {
            auto normalizedAlgorithm =
                normalizeSupportAlgorithm(js, algorithmValue, importKeyAlgorithmHandler);
            KJ_IF_SOME(algoImpl, lookupAlgorithm(normalizedAlgorithm.name)) {
              return supportsGetPublicKey(algoImpl.name);
            }
            return false;
          }
        }

        KJ_UNREACHABLE;
      };

      KJ_IF_SOME(thirdArgument, lengthOrAdditionalAlgorithm) {
        if (isAdditionalAlgorithmArgument(thirdArgument)) {
          auto additionalAlgorithm =
              normalizeSupportAlgorithm(js, thirdArgument, importKeyAlgorithmHandler);

          switch (parsedOperation) {
            case SubtleOperation::DERIVE_KEY: {
              if (!checkSupportForAlgorithm(SubtleOperation::IMPORT_KEY, thirdArgument, kj::none)) {
                return false;
              }
              auto length = getKeyLength(additionalAlgorithm);
              return checkSupportForAlgorithm(SubtleOperation::DERIVE_BITS, algorithm, length);
            }

            case SubtleOperation::UNWRAP_KEY:
              [[fallthrough]];
            case SubtleOperation::ENCAPSULATE_KEY:
              [[fallthrough]];
            case SubtleOperation::DECAPSULATE_KEY:
              if (!checkSupportForAlgorithm(SubtleOperation::IMPORT_KEY, thirdArgument, kj::none)) {
                return false;
              }
              return checkSupportForAlgorithm(parsedOperation, algorithm, kj::none);

            case SubtleOperation::WRAP_KEY:
              if (!checkSupportForAlgorithm(SubtleOperation::EXPORT_KEY, thirdArgument, kj::none)) {
                return false;
              }
              return checkSupportForAlgorithm(parsedOperation, algorithm, kj::none);

            default:
              return checkSupportForAlgorithm(parsedOperation, algorithm, kj::none);
          }
        }
      }

      kj::Maybe<uint32_t> maybeLength = kj::none;
      KJ_IF_SOME(thirdArgument, lengthOrAdditionalAlgorithm) {
        maybeLength = readSupportsLength(js, thirdArgument);
      }
      return checkSupportForAlgorithm(parsedOperation, algorithm, maybeLength);
    }, [](jsg::Value&&) { return false; });
  }

  return false;
}

bool SubtleCrypto::timingSafeEqual(jsg::JsBufferSource a, jsg::JsBufferSource b) {
  JSG_REQUIRE(a.size() == b.size(), TypeError, "Input buffers must have the same byte length.");

  // The implementation here depends entirely on the characteristics of the CRYPTO_memcmp
  // implementation. We do not perform any additional verification that the operation is
  // actually timing safe other than checking the input types and lengths.

  return CRYPTO_memcmp(a.asArrayPtr().begin(), b.asArrayPtr().begin(), a.size()) == 0;
}

// =======================================================================================
// Crypto implementation

jsg::JsArrayBufferView Crypto::getRandomValues(jsg::JsArrayBufferView buffer) {
  // NOTE: TypeMismatchError is deprecated (obviated by TypeError), but the spec and W3C tests still
  //   expect a TypeMismatchError here.
  JSG_REQUIRE(buffer.isIntegerType(), DOMTypeMismatchError,
      "ArrayBufferView argument to getRandomValues() must be an integer-typed view.");
  JSG_REQUIRE(buffer.size() <= 0x10000, DOMQuotaExceededError,
      "getRandomValues() only accepts buffers of size <= 64K but provided ", buffer.size(),
      " bytes.");
  IoContext::current().getEntropySource().generate(buffer.asArrayPtr());
  return buffer;
}

kj::String Crypto::randomUUID() {
  return ::workerd::randomUUID(IoContext::current().getEntropySource());
}

// =======================================================================================
// Crypto Streams implementation

class CRC32DigestContext final: public DigestContext {
 public:
  CRC32DigestContext(): value(crc32(0, Z_NULL, 0)) {}
  ~CRC32DigestContext() noexcept override = default;

  void write(kj::ArrayPtr<kj::byte> buffer) override {
    value = crc32(value, buffer.begin(), buffer.size());
  }

  jsg::JsArrayBuffer close(jsg::Lock& js) override {
    auto beValue = htobe32(value);
    static_assert(sizeof(value) == sizeof(beValue), "CRC32 digest is not 32 bits?");
    kj::ArrayPtr<kj::byte> be(reinterpret_cast<kj::byte*>(&beValue), sizeof(beValue));
    return jsg::JsArrayBuffer::create(js, be);
  }

 private:
  uint32_t value;
};

class CRC32CDigestContext final: public DigestContext {
 public:
  CRC32CDigestContext(): value(crc32c(0, nullptr, 0)) {}
  ~CRC32CDigestContext() noexcept override = default;

  void write(kj::ArrayPtr<kj::byte> buffer) override {
    value = crc32c(value, buffer.begin(), buffer.size());
  }

  jsg::JsArrayBuffer close(jsg::Lock& js) override {
    auto beValue = htobe32(value);
    static_assert(sizeof(value) == sizeof(beValue), "CRC32 digest is not 32 bits?");
    kj::ArrayPtr<kj::byte> be(reinterpret_cast<kj::byte*>(&beValue), sizeof(beValue));
    return jsg::JsArrayBuffer::create(js, be);
  }

 private:
  uint32_t value;
};

class CRC64NVMEDigestContext final: public DigestContext {
 public:
  CRC64NVMEDigestContext(): value(crc64nvme(0, nullptr, 0)) {}
  ~CRC64NVMEDigestContext() noexcept override = default;

  void write(kj::ArrayPtr<kj::byte> buffer) override {
    value = crc64nvme(value, buffer.begin(), buffer.size());
  }

  jsg::JsArrayBuffer close(jsg::Lock& js) override {
    auto beValue = htobe64(value);
    static_assert(sizeof(value) == sizeof(beValue), "CRC64 digest is not 64 bits?");
    kj::ArrayPtr<kj::byte> be(reinterpret_cast<kj::byte*>(&beValue), sizeof(beValue));
    return jsg::JsArrayBuffer::create(js, be);
  }

 private:
  uint64_t value;
};

class OpenSSLDigestContext final: public DigestContext {
 public:
  OpenSSLDigestContext(kj::StringPtr algorithm): algorithm(kj::str(algorithm)) {
    auto checkErrorsOnFinish = webCryptoOperationBegin(__func__, algorithm);
    auto type = lookupDigestAlgorithm(algorithm).second;
    auto opensslContext = kj::disposeWith<EVP_MD_CTX_free>(EVP_MD_CTX_new());
    KJ_ASSERT(opensslContext.get() != nullptr);
    OSSLCALL(EVP_DigestInit_ex(opensslContext.get(), type, nullptr));
    context = kj::mv(opensslContext);
  }
  ~OpenSSLDigestContext() noexcept override = default;

  void write(kj::ArrayPtr<kj::byte> buffer) override {
    auto checkErrorsOnFinish = webCryptoOperationBegin(__func__, algorithm);
    OSSLCALL(EVP_DigestUpdate(context.get(), buffer.begin(), buffer.size()));
  }

  jsg::JsArrayBuffer close(jsg::Lock& js) override {
    auto checkErrorsOnFinish = webCryptoOperationBegin(__func__, algorithm);
    uint size = 0;
    auto buf = jsg::JsArrayBuffer::create(js, EVP_MD_CTX_size(context.get()));
    OSSLCALL(EVP_DigestFinal_ex(context.get(), buf.asArrayPtr().begin(), &size));
    KJ_ASSERT(size == buf.size());
    return buf;
  }

 private:
  kj::String algorithm;
  kj::Own<EVP_MD_CTX> context;
};

DigestStream::DigestContextPtr DigestStream::initContext(SubtleCrypto::HashAlgorithm& algorithm) {
  if (algorithm.name == "crc32") {
    return kj::heap<CRC32DigestContext>();
  } else if (algorithm.name == "crc32c") {
    return kj::heap<CRC32CDigestContext>();
  } else if (algorithm.name == "crc64nvme") {
    return kj::heap<CRC64NVMEDigestContext>();
  } else {
    return kj::heap<OpenSSLDigestContext>(algorithm.name);
  }
}

DigestStream::DigestStream(kj::Own<WritableStreamController> controller,
    SubtleCrypto::HashAlgorithm algorithm,
    jsg::Promise<jsg::JsRef<jsg::JsArrayBuffer>>::Resolver resolver,
    jsg::Promise<jsg::JsRef<jsg::JsArrayBuffer>> promise)
    : WritableStream(kj::mv(controller)),
      promise(kj::mv(promise)),
      state(Ready(kj::mv(algorithm), kj::mv(resolver))) {}

void DigestStream::dispose(jsg::Lock& js) {
  JSG_TRY(js) {
    KJ_IF_SOME(ready, state.tryGet<Ready>()) {
      auto reason = js.typeError("The DigestStream was disposed.");
      ready.resolver.reject(js, reason);
      state.init<StreamStates::Errored>(reason.addRef(js));
    }
  }
  JSG_CATCH(exception) {
    js.throwException(kj::mv(exception));
  }
}

void DigestStream::visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
  tracker.trackField("promise", promise);
  KJ_IF_SOME(ready, state.tryGet<Ready>()) {
    tracker.trackField("resolver", ready.resolver);
  }
}

void DigestStream::visitForGc(jsg::GcVisitor& visitor) {
  visitor.visit(promise);
  KJ_IF_SOME(ready, state.tryGet<Ready>()) {
    visitor.visit(ready.resolver);
  }
}

kj::Maybe<StreamStates::Errored> DigestStream::write(jsg::Lock& js, kj::ArrayPtr<kj::byte> buffer) {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      return kj::none;
    }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) {
      return errored.addRef(js);
    }
    KJ_CASE_ONEOF(ready, Ready) {
      ready.context->write(buffer);
      return kj::none;
    }
  }
  KJ_UNREACHABLE;
}

kj::Maybe<StreamStates::Errored> DigestStream::close(jsg::Lock& js) {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      return kj::none;
    }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) {
      return errored.addRef(js);
    }
    KJ_CASE_ONEOF(ready, Ready) {
      ready.resolver.resolve(js, ready.context->close(js).addRef(js));
      state.init<StreamStates::Closed>();
      return kj::none;
    }
  }
  KJ_UNREACHABLE;
}

void DigestStream::abort(jsg::Lock& js, jsg::JsValue reason) {
  // If the state is already closed or errored, then this is a non-op
  KJ_IF_SOME(ready, state.tryGet<Ready>()) {
    ready.resolver.reject(js, reason);
    state.init<StreamStates::Errored>(reason.addRef(js));
  }
}

jsg::Ref<DigestStream> DigestStream::constructor(jsg::Lock& js, Algorithm algorithm) {
  auto paf = js.newPromiseAndResolver<jsg::JsRef<jsg::JsArrayBuffer>>();

  auto stream = js.alloc<DigestStream>(newWritableStreamJsController(),
      interpretAlgorithmParam(kj::mv(algorithm)), kj::mv(paf.resolver), kj::mv(paf.promise));

  // clang-format off
  stream->getController().setup(js, UnderlyingSink{
    .write = [&stream = *stream](jsg::Lock& js, v8::Local<v8::Value> chunk, auto c) mutable {
      return js.tryCatch([&] {
        // Make sure what we got can be interpreted as bytes...
        if (chunk->IsArrayBuffer() || chunk->IsArrayBufferView()) {
          jsg::JsBufferSource source(chunk);
          if (source.size() == 0) return js.resolvedPromise();

          KJ_IF_SOME(error, stream.write(js, source.asArrayPtr())) {
            return js.rejectedPromise<void>(kj::mv(error));
          } else {
          }  // Here to silence a compiler warning
          stream.bytesWritten += source.size();
          return js.resolvedPromise();
        } else if (chunk->IsString()) {
          // If we receive a string, we'll convert that to UTF-8 bytes and digest that.
          auto str = js.toString(chunk);
          if (str.size() == 0) return js.resolvedPromise();
          KJ_IF_SOME(error, stream.write(js, str.asBytes())) {
            return js.rejectedPromise<void>(kj::mv(error));
          }
          stream.bytesWritten += str.size();
          return js.resolvedPromise();
        }
        return js.rejectedPromise<void>(
            js.typeError("DigestStream is a byte stream but received an object of "
                        "non-ArrayBuffer/ArrayBufferView/string type on its writable side."));
      }, [&](jsg::Value exception) { return js.rejectedPromise<void>(kj::mv(exception)); });
    },
    .abort = [&stream = *stream](jsg::Lock& js, auto reason) mutable {
      return js.tryCatch([&] {
        stream.abort(js, jsg::JsValue(reason));
        return js.resolvedPromise();
      }, [&](jsg::Value exception) { return js.rejectedPromise<void>(kj::mv(exception)); });
    },
    .close = [&stream = *stream](jsg::Lock& js) mutable {
      return js.tryCatch([&] {
        // If sink.close returns a non kj::none value, that means the sink was errored
        // and we return a rejected promise here. Otherwise, we return resolved.
        KJ_IF_SOME(error, stream.close(js)) {
          return js.rejectedPromise<void>(kj::mv(error));
        } else {
        }  // Here to silence a compiler warning
        return js.resolvedPromise();
      }, [&](jsg::Value exception) { return js.rejectedPromise<void>(kj::mv(exception)); });
    }
  }, kj::none);
  // clang-format on

  return kj::mv(stream);
}

}  // namespace workerd::api
