// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "crypto.h"

#include "impl.h"

#include <workerd/api/streams/standard.h>
#include <workerd/api/util.h>
#include <workerd/io/io-context.h>
#include <workerd/jsg/jsg.h>
#include <workerd/util/uuid.h>

#include <openssl/digest.h>
#include <openssl/mem.h>

#include <algorithm>
#include <array>
#include <limits>
#include <set>
#include <typeinfo>

namespace workerd::api {

kj::StringPtr CryptoKeyUsageSet::name() const {
  if (*this == encrypt()) return "encrypt";
  if (*this == decrypt()) return "decrypt";
  if (*this == sign()) return "sign";
  if (*this == verify()) return "verify";
  if (*this == deriveKey()) return "deriveKey";
  if (*this == deriveBits()) return "deriveBits";
  if (*this == wrapKey()) return "wrapKey";
  if (*this == unwrapKey()) return "unwrapKey";
  KJ_FAIL_REQUIRE("CryptoKeyUsageSet does not contain exactly one key usage");
}

CryptoKeyUsageSet CryptoKeyUsageSet::byName(kj::StringPtr name) {
  for (auto& usage: singletons()) {
    if (name == usage.name()) return usage;
  }
  return {};
}

kj::ArrayPtr<const CryptoKeyUsageSet> CryptoKeyUsageSet::singletons() {
  static const workerd::api::CryptoKeyUsageSet singletons[] = {
    encrypt(), decrypt(), sign(), verify(), deriveKey(), deriveBits(), wrapKey(), unwrapKey()};
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
  };

  auto iter = ALGORITHMS.find(CryptoAlgorithm{name});
  if (iter == ALGORITHMS.end()) {
    // No such built-in algorithm, so fall back to checking if the Api has a custom
    // algorithm registered.
    return Worker::Api::current().getCryptoAlgorithm(name);
  } else {
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

CryptoKey::AsymmetricKeyDetails CryptoKey::getAsymmetricKeyDetails() const {
  return impl->getAsymmetricKeyDetail();
}

bool CryptoKey::verifyX509Public(const X509* cert) const {
  if (this->getType() != "public"_kj) return false;
  return impl->verifyX509Public(cert);
}

bool CryptoKey::verifyX509Private(const X509* cert) const {
  if (this->getType() != "private"_kj) return false;
  return impl->verifyX509Private(cert);
}

jsg::Promise<jsg::BufferSource> SubtleCrypto::encrypt(jsg::Lock& js,
    kj::OneOf<kj::String, EncryptAlgorithm> algorithmParam,
    const CryptoKey& key,
    kj::Array<const kj::byte> plainText) {
  auto algorithm = interpretAlgorithmParam(kj::mv(algorithmParam));

  auto checkErrorsOnFinish = webCryptoOperationBegin(__func__, algorithm);

  return js.evalNow([&] {
    validateOperation(key, algorithm.name, CryptoKeyUsageSet::encrypt());
    return key.impl->encrypt(js, kj::mv(algorithm), plainText);
  });
}

jsg::Promise<jsg::BufferSource> SubtleCrypto::decrypt(jsg::Lock& js,
    kj::OneOf<kj::String, EncryptAlgorithm> algorithmParam,
    const CryptoKey& key,
    kj::Array<const kj::byte> cipherText) {
  auto algorithm = interpretAlgorithmParam(kj::mv(algorithmParam));

  auto checkErrorsOnFinish = webCryptoOperationBegin(__func__, algorithm);

  return js.evalNow([&] {
    validateOperation(key, algorithm.name, CryptoKeyUsageSet::decrypt());
    return key.impl->decrypt(js, kj::mv(algorithm), cipherText);
  });
}

jsg::Promise<jsg::BufferSource> SubtleCrypto::sign(jsg::Lock& js,
    kj::OneOf<kj::String, SignAlgorithm> algorithmParam,
    const CryptoKey& key,
    kj::Array<const kj::byte> data) {
  auto algorithm = interpretAlgorithmParam(kj::mv(algorithmParam));

  auto checkErrorsOnFinish = webCryptoOperationBegin(__func__, algorithm);

  return js.evalNow([&] {
    validateOperation(key, algorithm.name, CryptoKeyUsageSet::sign());
    return key.impl->sign(js, kj::mv(algorithm), data);
  });
}

jsg::Promise<bool> SubtleCrypto::verify(jsg::Lock& js,
    kj::OneOf<kj::String, SignAlgorithm> algorithmParam,
    const CryptoKey& key,
    kj::Array<const kj::byte> signature,
    kj::Array<const kj::byte> data) {
  auto algorithm = interpretAlgorithmParam(kj::mv(algorithmParam));

  auto checkErrorsOnFinish = webCryptoOperationBegin(__func__, algorithm);

  return js.evalNow([&] {
    validateOperation(key, algorithm.name, CryptoKeyUsageSet::verify());
    return key.impl->verify(js, kj::mv(algorithm), signature, data);
  });
}

jsg::Promise<jsg::BufferSource> SubtleCrypto::digest(jsg::Lock& js,
    kj::OneOf<kj::String, HashAlgorithm> algorithmParam,
    kj::Array<const kj::byte> data) {
  auto algorithm = interpretAlgorithmParam(kj::mv(algorithmParam));

  auto checkErrorsOnFinish = webCryptoOperationBegin(__func__, algorithm);

  return js.evalNow([&] {
    auto type = lookupDigestAlgorithm(algorithm.name).second;

    auto digestCtx = kj::disposeWith<EVP_MD_CTX_free>(EVP_MD_CTX_new());
    KJ_ASSERT(digestCtx.get() != nullptr);

    OSSLCALL(EVP_DigestInit_ex(digestCtx.get(), type, nullptr));
    OSSLCALL(EVP_DigestUpdate(digestCtx.get(), data.begin(), data.size()));

    auto messageDigest =
        jsg::BackingStore::alloc<v8::ArrayBuffer>(js, EVP_MD_CTX_size(digestCtx.get()));
    uint messageDigestSize = 0;
    OSSLCALL(EVP_DigestFinal_ex(
        digestCtx.get(), messageDigest.asArrayPtr().begin(), &messageDigestSize));

    KJ_ASSERT(messageDigestSize == messageDigest.size());
    return jsg::BufferSource(js, kj::mv(messageDigest));
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

    auto secret = baseKey.impl->deriveBits(js, kj::mv(algorithm), length);

    // TODO(perf): For conformance, importKey() makes a copy of `secret`. In this case we really
    //   don't need to, but rather we ought to call the appropriate CryptoKey::Impl::import*()
    //   function directly.
    auto data = kj::heapArray<kj::byte>(secret);
    return importKeySync(
        js, "raw", kj::mv(data), kj::mv(derivedKeyAlgorithm), extractable, kj::mv(keyUsages));
  });
}

jsg::Promise<jsg::BufferSource> SubtleCrypto::deriveBits(jsg::Lock& js,
    kj::OneOf<kj::String, DeriveKeyAlgorithm> algorithmParam,
    const CryptoKey& baseKey,
    jsg::Optional<kj::Maybe<int>> lengthParam) {
  auto algorithm = interpretAlgorithmParam(kj::mv(algorithmParam));

  auto checkErrorsOnFinish = webCryptoOperationBegin(__func__, algorithm);

  kj::Maybe<uint32_t> length = kj::none;
  KJ_IF_SOME(maybeLength, lengthParam) {
    KJ_IF_SOME(l, maybeLength) {
      JSG_REQUIRE(l >= 0, TypeError, "deriveBits length must be an unsigned long integer.");
      length = uint32_t(l);
    }
  }

  return js.evalNow([&] {
    validateOperation(baseKey, algorithm.name, CryptoKeyUsageSet::deriveBits());
    return baseKey.impl->deriveBits(js, kj::mv(algorithm), length);
  });
}

jsg::Promise<jsg::BufferSource> SubtleCrypto::wrapKey(jsg::Lock& js,
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
      KJ_CASE_ONEOF(k, jsg::BufferSource) {
        return wrappingKey.impl->wrapKey(js, kj::mv(algorithm), k.asArrayPtr().asConst());
      }
      KJ_CASE_ONEOF(jwk, JsonWebKey) {
        auto stringified = js.serializeJson(jwkHandler.wrap(js, kj::mv(jwk)));
        return wrappingKey.impl->wrapKey(js, kj::mv(algorithm), stringified.asBytes().asConst());
      }
    }

    KJ_UNREACHABLE;
  });
}

jsg::Promise<jsg::Ref<CryptoKey>> SubtleCrypto::unwrapKey(jsg::Lock& js,
    kj::String format,
    kj::Array<const kj::byte> wrappedKey,
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

    auto bytes = unwrappingKey.impl->unwrapKey(js, kj::mv(normalizedAlgorithm), wrappedKey);

    ImportKeyData importData;

    if (format == "jwk") {
      auto jwkDict = js.parseJson(bytes.asArrayPtr().asChars());

      importData = JSG_REQUIRE_NONNULL(jwkHandler.tryUnwrap(js, jwkDict.getHandle(js)),
          DOMDataError, "Missing \"kty\" field or corrupt JSON unwrapping key?");
    } else {
      importData = kj::heapArray<kj::byte>(bytes);
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
  if (format == "raw" || format == "pkcs8" || format == "spki") {
    auto& key = JSG_REQUIRE_NONNULL(keyData.tryGet<kj::Array<kj::byte>>(), TypeError,
        "Import data provided for \"raw\", \"pkcs8\", or \"spki\" import formats must be a buffer "
        "source.");

    // Make a copy of the key import data.
    keyData = kj::heapArray(key.asPtr());
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
  auto cryptoKey = jsg::alloc<CryptoKey>(algoImpl.importFunc(
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

bool SubtleCrypto::timingSafeEqual(kj::Array<kj::byte> a, kj::Array<kj::byte> b) {
  JSG_REQUIRE(a.size() == b.size(), TypeError, "Input buffers must have the same byte length.");

  // The implementation here depends entirely on the characteristics of the CRYPTO_memcmp
  // implementation. We do not perform any additional verification that the operation is
  // actually timing safe other than checking the input types and lengths.

  return CRYPTO_memcmp(a.begin(), b.begin(), a.size()) == 0;
}

// =======================================================================================
// Crypto implementation

jsg::BufferSource Crypto::getRandomValues(jsg::BufferSource buffer) {
  // NOTE: TypeMismatchError is deprecated (obviated by TypeError), but the spec and W3C tests still
  //   expect a TypeMismatchError here.
  JSG_REQUIRE(buffer.isIntegerType(), DOMTypeMismatchError,
      "ArrayBufferView argument to getRandomValues() must be an integer-typed view.");
  JSG_REQUIRE(buffer.size() <= 0x10000, DOMQuotaExceededError,
      "getRandomValues() only accepts buffers of size <= 64K but provided ", buffer.size(),
      " bytes.");
  IoContext::current().getEntropySource().generate(buffer.asArrayPtr());
  return kj::mv(buffer);
}

kj::String Crypto::randomUUID() {
  return ::workerd::randomUUID(IoContext::current().getEntropySource());
}

// =======================================================================================
// Crypto Streams implementation

DigestStream::DigestContextPtr DigestStream::initContext(SubtleCrypto::HashAlgorithm& algorithm) {
  auto checkErrorsOnFinish = webCryptoOperationBegin(__func__, algorithm.name);
  auto type = lookupDigestAlgorithm(algorithm.name).second;
  auto context = kj::disposeWith<EVP_MD_CTX_free>(EVP_MD_CTX_new());
  KJ_ASSERT(context.get() != nullptr);
  OSSLCALL(EVP_DigestInit_ex(context.get(), type, nullptr));
  return kj::mv(context);
}

DigestStream::DigestStream(kj::Own<WritableStreamController> controller,
    SubtleCrypto::HashAlgorithm algorithm,
    jsg::Promise<kj::Array<kj::byte>>::Resolver resolver,
    jsg::Promise<kj::Array<kj::byte>> promise)
    : WritableStream(kj::mv(controller)),
      promise(kj::mv(promise)),
      state(Ready(kj::mv(algorithm), kj::mv(resolver))) {}

void DigestStream::dispose(jsg::Lock& js) {
  js.tryCatch([&] {
    KJ_IF_SOME(ready, state.tryGet<Ready>()) {
      auto reason = js.typeError("The DigestStream was disposed.");
      ready.resolver.reject(js, reason);
      state.init<StreamStates::Errored>(js.v8Ref<v8::Value>(reason));
    }
  }, [&](jsg::Value exception) { js.throwException(kj::mv(exception)); });
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
      auto checkErrorsOnFinish = webCryptoOperationBegin(__func__, ready.algorithm.name);
      OSSLCALL(EVP_DigestUpdate(ready.context.get(), buffer.begin(), buffer.size()));
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
      auto checkErrorsOnFinish = webCryptoOperationBegin(__func__, ready.algorithm.name);
      uint size = 0;
      auto digest = kj::heapArray<kj::byte>(EVP_MD_CTX_size(ready.context.get()));
      OSSLCALL(EVP_DigestFinal_ex(ready.context.get(), digest.begin(), &size));
      KJ_ASSERT(size, digest.size());
      ready.resolver.resolve(js, kj::mv(digest));
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
    state.init<StreamStates::Errored>(js.v8Ref<v8::Value>(reason));
  }
}

jsg::Ref<DigestStream> DigestStream::constructor(jsg::Lock& js, Algorithm algorithm) {
  auto paf = js.newPromiseAndResolver<kj::Array<kj::byte>>();

  auto stream = jsg::alloc<DigestStream>(newWritableStreamJsController(),
      interpretAlgorithmParam(kj::mv(algorithm)), kj::mv(paf.resolver), kj::mv(paf.promise));

  stream->getController().setup(js,
      UnderlyingSink{
        .write =
            [&stream = *stream](jsg::Lock& js, v8::Local<v8::Value> chunk, auto c) mutable {
    return js.tryCatch([&] {
      // Make sure what we got can be interpreted as bytes...
      std::shared_ptr<v8::BackingStore> backing;
      if (chunk->IsArrayBuffer() || chunk->IsArrayBufferView()) {
        jsg::BufferSource source(js, chunk);
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
        .abort =
            [&stream = *stream](jsg::Lock& js, auto reason) mutable {
    return js.tryCatch([&] {
      stream.abort(js, jsg::JsValue(reason));
      return js.resolvedPromise();
    }, [&](jsg::Value exception) { return js.rejectedPromise<void>(kj::mv(exception)); });
  },
        .close =
            [&stream = *stream](jsg::Lock& js) mutable {
    return js.tryCatch([&] {
      // If sink.close returns a non kj::none value, that means the sink was errored
      // and we return a rejected promise here. Otherwise, we return resolved.
      KJ_IF_SOME(error, stream.close(js)) {
        return js.rejectedPromise<void>(kj::mv(error));
      } else {
      }  // Here to silence a compiler warning
      return js.resolvedPromise();
    }, [&](jsg::Value exception) { return js.rejectedPromise<void>(kj::mv(exception)); });
  }},
      kj::none);

  return kj::mv(stream);
}

}  // namespace workerd::api
