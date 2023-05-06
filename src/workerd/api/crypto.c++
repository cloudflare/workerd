// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "crypto.h"
#include "crypto-impl.h"
#include <array>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <workerd/jsg/jsg.h>
#include "util.h"
#include <workerd/io/io-context.h>
#include <workerd/util/uuid.h>
#include <set>
#include <algorithm>
#include <limits>
#include <typeinfo>

namespace workerd::api {

kj::StringPtr CryptoKeyUsageSet::name() const {
  if (*this == encrypt())    return "encrypt";
  if (*this == decrypt())    return "decrypt";
  if (*this == sign())       return "sign";
  if (*this == verify())     return "verify";
  if (*this == deriveKey())  return "deriveKey";
  if (*this == deriveBits()) return "deriveBits";
  if (*this == wrapKey())    return "wrapKey";
  if (*this == unwrapKey())  return "unwrapKey";
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
    encrypt(), decrypt(), sign(), verify(), deriveKey(), deriveBits(), wrapKey(), unwrapKey()
  };
  return singletons;
}

CryptoKeyUsageSet CryptoKeyUsageSet::validate(kj::StringPtr normalizedName, Context ctx,
    kj::ArrayPtr<const kj::String> actual, CryptoKeyUsageSet mask) {
  const auto op = (ctx == Context::generate)     ? "generate" :
                  (ctx == Context::importSecret) ? "import secret" :
                  (ctx == Context::importPublic) ? "import public" :
                                                   "import private";
  CryptoKeyUsageSet usages;
  for (const auto& usage: actual) {
    CryptoKeyUsageSet match = byName(usage);
    JSG_REQUIRE(match.isSingleton() && match <= mask, DOMSyntaxError,
        "Attempt to ", op, " ", normalizedName, " key with invalid usage \"", usage, "\".");
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
// OpenSSL shims

std::unique_ptr<EVP_MD_CTX, void(*)(EVP_MD_CTX*)> makeDigestContext() {
  return {EVP_MD_CTX_new(), EVP_MD_CTX_free};
}

// =======================================================================================
// Registered algorithms

static kj::Maybe<const CryptoAlgorithm&> lookupAlgorithm(kj::StringPtr name) {
  static const std::set<CryptoAlgorithm> ALGORITHMS = {
    {"AES-CTR"_kj,           &CryptoKey::Impl::importAes, &CryptoKey::Impl::generateAes},
    {"AES-CBC"_kj,           &CryptoKey::Impl::importAes, &CryptoKey::Impl::generateAes},
    {"AES-GCM"_kj,           &CryptoKey::Impl::importAes, &CryptoKey::Impl::generateAes},
    {"AES-KW"_kj,            &CryptoKey::Impl::importAes, &CryptoKey::Impl::generateAes},
    {"HMAC"_kj,              &CryptoKey::Impl::importHmac, &CryptoKey::Impl::generateHmac},
    {"PBKDF2"_kj,            &CryptoKey::Impl::importPbkdf2},
    {"HKDF"_kj,              &CryptoKey::Impl::importHkdf},
    {"RSASSA-PKCS1-v1_5"_kj, &CryptoKey::Impl::importRsa, &CryptoKey::Impl::generateRsa},
    {"RSA-PSS"_kj,           &CryptoKey::Impl::importRsa, &CryptoKey::Impl::generateRsa},
    {"RSA-OAEP"_kj,          &CryptoKey::Impl::importRsa, &CryptoKey::Impl::generateRsa},
    {"ECDSA"_kj,             &CryptoKey::Impl::importEcdsa, &CryptoKey::Impl::generateEcdsa},
    {"ECDH"_kj,              &CryptoKey::Impl::importEcdh, &CryptoKey::Impl::generateEcdh},
    {"NODE-ED25519"_kj,      &CryptoKey::Impl::importEddsa, &CryptoKey::Impl::generateEddsa},
    {"Ed25519"_kj,           &CryptoKey::Impl::importEddsa, &CryptoKey::Impl::generateEddsa},
    {"X25519"_kj,            &CryptoKey::Impl::importEddsa, &CryptoKey::Impl::generateEddsa},
    {"RSA-RAW"_kj,           &CryptoKey::Impl::importRsaRaw},
  };

  auto iter = ALGORITHMS.find(CryptoAlgorithm {name});
  if (iter == ALGORITHMS.end()) {
    // No such built-in algorithm, so fall back to checking if the ApiIsolate has a custom
    // algorithm registered.
    return Worker::ApiIsolate::current().getCryptoAlgorithm(name);
  } else {
    return *iter;
  }
}

// =======================================================================================
// Helper functions

void validateOperation(
    const CryptoKey& key,
    kj::StringPtr requestedName,
    CryptoKeyUsageSet usage) {
  // Throws InvalidAccessError if the key is incompatible with the given normalized algorithm name,
  // or if it doesn't support the given usage.
  //
  // TODO(someday): Throw a NotSupportedError? The Web Crypto API spec says InvalidAccessError, but
  //   Web IDL says that's deprecated.
  //
  // TODO(cleanup): Make this function go away. Maybe this can be rolled into the default
  //   implementations of the CryptoKey::Impl::<crypto operation>() functions.

  JSG_REQUIRE(strcasecmp(requestedName.cStr(), key.getAlgorithmName().cStr()) == 0,
      DOMInvalidAccessError,
      "Requested algorithm \"", requestedName, "\" does not match this CryptoKey's algorithm \"",
      key.getAlgorithmName() ,"\".");
  JSG_REQUIRE(usage <= key.getUsageSet(), DOMInvalidAccessError, "Requested key usage \"",
      usage.name(), "\" does not match any usage listed in this CryptoKey.");
}

kj::Maybe<uint32_t> getKeyLength(const SubtleCrypto::ImportKeyAlgorithm& derivedKeyAlgorithm) {
  // Helper for `deriveKey()`. This private crypto operation is actually defined by the spec as
  // the "get key length" operation.

  kj::StringPtr algName = derivedKeyAlgorithm.name;

  // TODO(cleanup): This should be a method of CryptoKey::Impl so it can be abstracted. Currently
  //   we ad-hoc match various algorithms below, so the set of supported algorithms must be
  //   hard-coded.
  static const std::set<kj::StringPtr, CiLess> registeredAlgorithms{
    {"AES-CTR"}, {"AES-CBC"}, {"AES-GCM"}, {"AES-KW"}, {"HMAC"}, {"HKDF"}, {"PBKDF2"},
  };
  auto algIter = registeredAlgorithms.find(algName);
  JSG_REQUIRE(algIter != registeredAlgorithms.end(), DOMNotSupportedError,
      "Unrecognized derived key type \"", algName, "\" requested.");

  // We could implement getKeyLength() with the same map-of-strings-to-implementation-functions
  // strategy as the rest of the crypto operations, but this function is so simple that it hardly
  // seems worth the bother. The spec only identifies three cases: the AES family, HMAC, and the KDF
  // algorithms.
  if (algIter->startsWith("AES-")) {
    int length = JSG_REQUIRE_NONNULL(derivedKeyAlgorithm.length, TypeError,
        "Missing field \"length\" in \"derivedKeyParams\".");
    switch (length) {
      case 128: [[fallthrough]];
      case 192: [[fallthrough]];
      case 256: break;
      default:
        JSG_FAIL_REQUIRE(DOMOperationError,
            "Derived AES key must be 128, 192, or 256 bits in length but provided ", length, ".");
    }
    return length;
  } else if (*algIter == "HMAC") {
    KJ_IF_MAYBE(length, derivedKeyAlgorithm.length) {
      // If the user requested a specific HMAC key length, honor it.
      if (*length > 0) {
        return *length;
      }
      JSG_FAIL_REQUIRE(TypeError, "HMAC key length must be a non-zero unsigned long integer.");
    }
    // Otherwise, assume the user wants the default HMAC key size.
    auto digestAlg = getAlgorithmName(JSG_REQUIRE_NONNULL(derivedKeyAlgorithm.hash, TypeError,
        "Missing field \"hash\" in \"derivedKeyParams\"."));
    return EVP_MD_block_size(lookupDigestAlgorithm(digestAlg).second) * 8;
  } else {
    // HKDF or PBKDF2. I'm not not sure what it means to derive a HKDF/PBKDF2 key from a base key
    // (are you deriving a password from a password?) but based on my reading of the spec, this code
    // path will become meaningful once we support ECDH, which handles null-length deriveBits()
    // operations. This is the entire reason getKeyLength() returns a Maybe<uint32_t> rather than a
    // uint32_t (and also why we do not throw an OperationError here but rather later on in
    // deriveBitsPbkdf2Impl()).
    return nullptr;
  }
}

auto webCryptoOperationBegin(
    const char *operation, kj::StringPtr algorithm, kj::Maybe<kj::StringPtr> context = nullptr) {
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
      KJ_IF_MAYBE(c, context) {
        stringifiedOperation = kj::str(operation, "(", *c, ")");
      } else {
        stringifiedOperation = kj::str(operation);
      }
      KJ_LOG(WARNING, "WebCrypto didn't handle all BoringSSL errors", stringifiedOperation,
          algorithm, allErrors);
    }
  });
}

auto webCryptoOperationBegin(
    const char *operation, kj::StringPtr algorithm, const kj::String& context) {
  return webCryptoOperationBegin(operation, algorithm, context.slice(0));
}

template <typename T, typename = kj::EnableIf<kj::isSameType<
    kj::String, decltype(kj::instance<T>().name)>()>>
[[gnu::always_inline]] auto webCryptoOperationBegin(const char *operation, const T& algorithm,
    kj::Maybe<kj::StringPtr> context = nullptr) {
  return kj::defer([operation, algorithm = kj::str(algorithm.name), context] {
    // We need a copy of the algorithm name as this defer runs after the EncryptAlgorithm struct
    // is destroyed.
    (void) webCryptoOperationBegin(operation, algorithm, context);
  });
}

}  // namespace

// =======================================================================================
// CryptoKey / SubtleCrypto implementations

CryptoKey::CryptoKey(kj::Own<Impl> impl): impl(kj::mv(impl)) {}
CryptoKey::~CryptoKey() noexcept(false) {}
kj::StringPtr CryptoKey::getAlgorithmName() const { return impl->getAlgorithmName(); }
CryptoKey::AlgorithmVariant CryptoKey::getAlgorithm() const { return impl->getAlgorithm(); }
kj::StringPtr CryptoKey::getType() const { return impl->getType(); }
bool CryptoKey::getExtractable() const { return impl->isExtractable(); }
kj::Array<kj::StringPtr> CryptoKey::getUsages() const {
  return getUsageSet().map([](auto singleton) { return singleton.name(); });
}
CryptoKeyUsageSet CryptoKey::getUsageSet() const { return impl->getUsages(); }

jsg::Promise<kj::Array<kj::byte>> SubtleCrypto::encrypt(
    jsg::Lock& js,
    kj::OneOf<kj::String, EncryptAlgorithm> algorithmParam,
    const CryptoKey& key,
    kj::Array<const kj::byte> plainText) {
  auto algorithm = interpretAlgorithmParam(kj::mv(algorithmParam));

  auto checkErrorsOnFinish = webCryptoOperationBegin(__func__, algorithm);

  return js.evalNow([&] {
    validateOperation(key, algorithm.name, CryptoKeyUsageSet::encrypt());
    return key.impl->encrypt(kj::mv(algorithm), plainText);
  });
}

jsg::Promise<kj::Array<kj::byte>> SubtleCrypto::decrypt(
    jsg::Lock& js,
    kj::OneOf<kj::String, EncryptAlgorithm> algorithmParam,
    const CryptoKey& key,
    kj::Array<const kj::byte> cipherText) {
  auto algorithm = interpretAlgorithmParam(kj::mv(algorithmParam));

  auto checkErrorsOnFinish = webCryptoOperationBegin(__func__, algorithm);

  return js.evalNow([&] {
    validateOperation(key, algorithm.name, CryptoKeyUsageSet::decrypt());
    return key.impl->decrypt(kj::mv(algorithm), cipherText);
  });
}

jsg::Promise<kj::Array<kj::byte>> SubtleCrypto::sign(
    jsg::Lock& js,
    kj::OneOf<kj::String, SignAlgorithm> algorithmParam,
    const CryptoKey& key,
    kj::Array<const kj::byte> data) {
  auto algorithm = interpretAlgorithmParam(kj::mv(algorithmParam));

  auto checkErrorsOnFinish = webCryptoOperationBegin(__func__, algorithm);

  return js.evalNow([&] {
    validateOperation(key, algorithm.name, CryptoKeyUsageSet::sign());
    return key.impl->sign(kj::mv(algorithm), data);
  });
}

jsg::Promise<bool> SubtleCrypto::verify(
    jsg::Lock& js,
    kj::OneOf<kj::String, SignAlgorithm> algorithmParam,
    const CryptoKey& key,
    kj::Array<const kj::byte> signature,
    kj::Array<const kj::byte> data) {
  auto algorithm = interpretAlgorithmParam(kj::mv(algorithmParam));

  auto checkErrorsOnFinish = webCryptoOperationBegin(__func__, algorithm);

  return js.evalNow([&] {
    validateOperation(key, algorithm.name, CryptoKeyUsageSet::verify());
    return key.impl->verify(kj::mv(algorithm), signature, data);
  });
}

jsg::Promise<kj::Array<kj::byte>> SubtleCrypto::digest(
    jsg::Lock& js,
    kj::OneOf<kj::String, HashAlgorithm> algorithmParam,
    kj::Array<const kj::byte> data) {
  auto algorithm = interpretAlgorithmParam(kj::mv(algorithmParam));

  auto checkErrorsOnFinish = webCryptoOperationBegin(__func__, algorithm);

  return js.evalNow([&] {
    auto type = lookupDigestAlgorithm(algorithm.name).second;

    auto digestCtx = makeDigestContext();
    KJ_ASSERT(digestCtx != nullptr);

    OSSLCALL(EVP_DigestInit_ex(digestCtx.get(), type, nullptr));
    OSSLCALL(EVP_DigestUpdate(digestCtx.get(), data.begin(), data.size()));
    auto messageDigest = kj::heapArray<kj::byte>(EVP_MD_CTX_size(digestCtx.get()));
    uint messageDigestSize = 0;
    OSSLCALL(EVP_DigestFinal_ex(digestCtx.get(), messageDigest.begin(), &messageDigestSize));

    KJ_ASSERT(messageDigestSize == messageDigest.size());
    return kj::mv(messageDigest);
  });
}

jsg::Promise<kj::OneOf<jsg::Ref<CryptoKey>, CryptoKeyPair>> SubtleCrypto::generateKey(
    jsg::Lock& js,
    kj::OneOf<kj::String, GenerateKeyAlgorithm> algorithmParam,
    bool extractable, kj::Array<kj::String> keyUsages) {

  auto algorithm = interpretAlgorithmParam(kj::mv(algorithmParam));

  auto checkErrorsOnFinish = webCryptoOperationBegin(__func__, algorithm);

  return js.evalNow([&] {
    CryptoAlgorithm algoImpl = lookupAlgorithm(algorithm.name).orDefault({});
    JSG_REQUIRE(algoImpl.generateFunc != nullptr, DOMNotSupportedError,
        "Unrecognized key generation algorithm \"", algorithm.name, "\" requested.");

    auto cryptoKeyOrPair = algoImpl.generateFunc(algoImpl.name, kj::mv(algorithm), extractable,
                                                 keyUsages);
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

jsg::Promise<jsg::Ref<CryptoKey>> SubtleCrypto::deriveKey(
    jsg::Lock& js,
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

    auto secret = baseKey.impl->deriveBits(kj::mv(algorithm), length);

    // TODO(perf): For conformance, importKey() makes a copy of `secret`. In this case we really
    //   don't need to, but rather we ought to call the appropriate CryptoKey::Impl::import*()
    //   function directly.
    return importKeySync(
        js, "raw", kj::mv(secret), kj::mv(derivedKeyAlgorithm), extractable, kj::mv(keyUsages));
  });
}

jsg::Promise<kj::Array<kj::byte>> SubtleCrypto::deriveBits(
    jsg::Lock& js,
    kj::OneOf<kj::String, DeriveKeyAlgorithm> algorithmParam,
    const CryptoKey& baseKey,
    kj::Maybe<int> lengthParam) {
  auto algorithm = interpretAlgorithmParam(kj::mv(algorithmParam));

  auto checkErrorsOnFinish = webCryptoOperationBegin(__func__, algorithm);

  auto length = lengthParam.map([&](int l) {
    // TODO(cleanup): Use the type jsg wrapper for uint32_t?
    JSG_REQUIRE(l >= 0, TypeError, "deriveBits length must be an unsigned long integer.");
    return uint32_t(l);
  });

  return js.evalNow([&] {
    validateOperation(baseKey, algorithm.name, CryptoKeyUsageSet::deriveBits());
    return baseKey.impl->deriveBits(kj::mv(algorithm), length);
  });
}

jsg::Promise<kj::Array<kj::byte>> SubtleCrypto::wrapKey(jsg::Lock& js,
    kj::String format, const CryptoKey& key,
    const CryptoKey& wrappingKey, kj::OneOf<kj::String, EncryptAlgorithm> wrapAlgorithm,
    const jsg::TypeHandler<JsonWebKey>& jwkHandler) {
  auto checkErrorsOnFinish = webCryptoOperationBegin(__func__, wrappingKey.getAlgorithmName(),
      key.getAlgorithmName());

  return js.evalNow([&] {
    auto isolate = js.v8Isolate;
    auto algorithm = interpretAlgorithmParam(kj::mv(wrapAlgorithm));

    validateOperation(wrappingKey, algorithm.name, CryptoKeyUsageSet::wrapKey());

    struct PaddingDetails {
      kj::byte byteAlignment;
      size_t minimumLength;
    };

    JSG_REQUIRE(key.getExtractable(), DOMInvalidAccessError, "Attempt to export non-extractable ",
        key.getAlgorithmName(), " key.");

    auto exportedKey = key.impl->exportKey(kj::mv(format));

    kj::Array<kj::byte> bytes;
    KJ_SWITCH_ONEOF(exportedKey) {
      KJ_CASE_ONEOF(k, kj::Array<kj::byte>) {
        bytes = kj::mv(k);
      }
      KJ_CASE_ONEOF(jwk, JsonWebKey) {
        auto jwkValue = jwkHandler.wrap(js, kj::mv(jwk));
        auto stringified = jsg::check(v8::JSON::Stringify(js.v8Context(), jwkValue));
        kj::Vector<kj::byte> converted;

        auto serializedLength = stringified->Utf8Length(isolate);
        // The WebCrypto spec would seem to indicate we need to pad AES-KW here. However, I can't
        // find any conformance test that fails if we don't pad. I can't find anywhere within
        // Chromium that has padding either.

        converted.resize(serializedLength);

        auto written = stringified->WriteUtf8(isolate, converted.asPtr().asChars().begin(),
            serializedLength, nullptr, v8::String::NO_NULL_TERMINATION);

        converted.resize(written);

        bytes = converted.releaseAsArray();
      }
    }

    auto unwrappedKey = bytes.asPtr().asConst();
    return wrappingKey.impl->wrapKey(kj::mv(algorithm), unwrappedKey);
  });
}

jsg::Promise<jsg::Ref<CryptoKey>> SubtleCrypto::unwrapKey(jsg::Lock& js, kj::String format,
    kj::Array<const kj::byte> wrappedKey, const CryptoKey& unwrappingKey,
    kj::OneOf<kj::String, EncryptAlgorithm> unwrapAlgorithm,
    kj::OneOf<kj::String, ImportKeyAlgorithm> unwrappedKeyAlgorithm, bool extractable,
    kj::Array<kj::String> keyUsages,
    const jsg::TypeHandler<JsonWebKey>& jwkHandler) {
  auto operation = __func__;
  return js.evalNow([&]() -> jsg::Ref<CryptoKey> {
    auto isolate = js.v8Isolate;
    auto normalizedAlgorithm = interpretAlgorithmParam(kj::mv(unwrapAlgorithm));
    auto normalizedUnwrapAlgorithm = interpretAlgorithmParam(kj::mv(unwrappedKeyAlgorithm));

    // Need a copy of the algorithm name to live in this scope, because we later kj::mv() it out.
    auto context = kj::str(normalizedUnwrapAlgorithm.name);
    auto checkErrorsOnFinish = webCryptoOperationBegin(operation, unwrappingKey.getAlgorithmName(),
        context);

    validateOperation(unwrappingKey, normalizedAlgorithm.name, CryptoKeyUsageSet::unwrapKey());

    kj::Array<kj::byte> bytes =
        unwrappingKey.impl->unwrapKey(kj::mv(normalizedAlgorithm), wrappedKey);

    ImportKeyData importData;

    if (format == "jwk") {
      auto jsonJwk = jsg::v8Str(isolate, bytes.asChars());

      auto jwkDict = jsg::check(v8::JSON::Parse(js.v8Context(), jsonJwk));

      importData = JSG_REQUIRE_NONNULL(jwkHandler.tryUnwrap(js, jwkDict), DOMDataError,
          "Missing \"kty\" field or corrupt JSON unwrapping key?");
    } else {
      importData = kj::mv(bytes);
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

jsg::Promise<jsg::Ref<CryptoKey>> SubtleCrypto::importKey(
    jsg::Lock& js,
    kj::String format,
    ImportKeyData keyData,
    kj::OneOf<kj::String, ImportKeyAlgorithm> algorithmParam,
    bool extractable,
    kj::Array<kj::String> keyUsages) {
  auto algorithm = interpretAlgorithmParam(kj::mv(algorithmParam));

  auto checkErrorsOnFinish = webCryptoOperationBegin(__func__, algorithm, format.asPtr());

  return js.evalNow([&] {
    return importKeySync(js, format, kj::mv(keyData), kj::mv(algorithm), extractable,
                         keyUsages);
  });
}

jsg::Ref<CryptoKey> SubtleCrypto::importKeySync(
    jsg::Lock& js,
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
    KJ_IF_MAYBE(ext, keyData.get<JsonWebKey>().ext) {
      JSG_REQUIRE(*ext || !extractable,
          DOMDataError, "JWK ext field for \"", algorithm.name, "\" is set to false but "
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
  auto cryptoKey = jsg::alloc<CryptoKey>(
      algoImpl.importFunc(algoImpl.name, format, kj::mv(keyData),
                          kj::mv(algorithm), extractable, keyUsages));

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
    JSG_REQUIRE(key.getExtractable(), DOMInvalidAccessError,
        "Attempt to export non-extractable ", key.getAlgorithmName(), " key.");

    return key.impl->exportKey(format);
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

v8::Local<v8::ArrayBufferView> Crypto::getRandomValues(v8::Local<v8::ArrayBufferView> bufferView) {
  // NOTE: TypeMismatchError is deprecated (obviated by TypeError), but the spec and W3C tests still
  //   expect a TypeMismatchError here.
  JSG_REQUIRE(
      bufferView->IsInt8Array() || bufferView->IsUint8Array() || bufferView->IsUint8ClampedArray()
          || bufferView->IsInt16Array() || bufferView->IsUint16Array()
          || bufferView->IsInt32Array() || bufferView->IsUint32Array()
          || bufferView->IsBigInt64Array() || bufferView->IsBigUint64Array(),
      DOMTypeMismatchError, "ArrayBufferView argument to getRandomValues() must be an "
      "integer-typed view.");

  auto buffer = jsg::asBytes(bufferView);
  JSG_REQUIRE(buffer.size() <= 0x10000, DOMQuotaExceededError,
      "getRandomValues() only accepts buffers of size <= 64K but provided ", buffer.size(),
      " bytes.");
  IoContext::current().getEntropySource().generate(buffer);
  return bufferView;
}

kj::String Crypto::randomUUID() {
  return ::workerd::randomUUID(IoContext::current().getEntropySource());
}

// =======================================================================================
// Crypto Streams implementation

namespace {
DigestStreamSink::DigestContextPtr initContext(DigestStreamSink::HashAlgorithm& algorithm) {
  auto checkErrorsOnFinish = webCryptoOperationBegin(__func__, algorithm.name);
  auto type = lookupDigestAlgorithm(algorithm.name).second;
  auto context = makeDigestContext();
  KJ_ASSERT(context != nullptr);
  OSSLCALL(EVP_DigestInit_ex(context.get(), type, nullptr));
  return kj::mv(context);
}
}  // namespace

DigestStreamSink::DigestStreamSink(
    HashAlgorithm algorithm,
    kj::Own<kj::PromiseFulfiller<kj::Array<kj::byte>>> fulfiller)
    : algorithm(kj::mv(algorithm)),
      state(initContext(this->algorithm)),
      fulfiller(kj::mv(fulfiller)) {}

DigestStreamSink::~DigestStreamSink() {
  if (fulfiller && fulfiller->isWaiting()) {
    fulfiller->reject(JSG_KJ_EXCEPTION(FAILED, Error,
        "The digest was never completed. The DigestStream was created but possibly never "
        "used or finished."));
  }
}

kj::Promise<void> DigestStreamSink::write(const void* buffer, size_t size) {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, Closed) {
      return kj::READY_NOW;
    }
    KJ_CASE_ONEOF(errored, Errored) {
      return kj::cp(errored);
    }
    KJ_CASE_ONEOF(context, DigestContextPtr) {
      auto checkErrorsOnFinish = webCryptoOperationBegin(__func__, algorithm.name);
      OSSLCALL(EVP_DigestUpdate(context.get(), buffer, size));
      return kj::READY_NOW;
    }
  }
  KJ_UNREACHABLE;
}

kj::Promise<void> DigestStreamSink::write(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) {
  for (auto& piece : pieces) {
    write(piece.begin(), piece.size());
  }
  return kj::READY_NOW;
}

kj::Promise<void> DigestStreamSink::end() {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, Closed) {
      return kj::READY_NOW;
    }
    KJ_CASE_ONEOF(errored, Errored) {
      return kj::cp(errored);
    }
    KJ_CASE_ONEOF(context, DigestContextPtr) {
      auto checkErrorsOnFinish = webCryptoOperationBegin(__func__, algorithm.name);
      uint size = 0;
      auto digest = kj::heapArray<kj::byte>(EVP_MD_CTX_size(context.get()));
      OSSLCALL(EVP_DigestFinal_ex(context.get(), digest.begin(), &size));
      KJ_ASSERT(size, digest.size());
      state.init<Closed>();
      fulfiller->fulfill(kj::mv(digest));
      return kj::READY_NOW;
    }
  }
  KJ_UNREACHABLE;
}

void DigestStreamSink::abort(kj::Exception reason) {
  fulfiller->reject(kj::cp(reason));
  state.init<Errored>(kj::mv(reason));
}

DigestStream::DigestStream(
    HashAlgorithm algorithm,
    kj::Own<kj::PromiseFulfiller<kj::Array<kj::byte>>> fulfiller,
    jsg::Promise<kj::Array<kj::byte>> promise)
    : WritableStream(IoContext::current(),
        kj::heap<DigestStreamSink>(kj::mv(algorithm), kj::mv(fulfiller))),
      promise(kj::mv(promise)) {}

jsg::Ref<DigestStream> DigestStream::constructor(Algorithm algorithm) {
  auto paf = kj::newPromiseAndFulfiller<kj::Array<kj::byte>>();

  auto jsPromise = IoContext::current().awaitIoLegacy(kj::mv(paf.promise));
  jsPromise.markAsHandled();

  return jsg::alloc<DigestStream>(
      interpretAlgorithmParam(kj::mv(algorithm)),
      kj::mv(paf.fulfiller),
      kj::mv(jsPromise));
}

}  // namespace workerd::api
