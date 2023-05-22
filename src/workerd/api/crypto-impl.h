// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// INTERNAL CRYPTO IMPLEMENTATION FILE
//
// Don't include this file unless your name is "crypto*.c++".

#include "crypto.h"
#include <kj/encoding.h>
#include <openssl/evp.h>
#include <openssl/bio.h>

#define OSSLCALL(...) if ((__VA_ARGS__) != 1) \
    ::workerd::api::throwOpensslError(__FILE__, __LINE__, #__VA_ARGS__)
// Wrap calls to OpenSSL's EVP_* interface (and similar APIs) in this macro to
// deal with errors.

#define UNWRAP_JWK_BIGNUM(value, ...) \
    JSG_REQUIRE_NONNULL( \
        decodeBase64Url(JSG_REQUIRE_NONNULL((value), __VA_ARGS__)), __VA_ARGS__)

namespace workerd::api {

struct OpensslUntranslatedError {
  kj::StringPtr library;
  kj::StringPtr reasonName;
};

KJ_NORETURN(void throwOpensslError(const char* file, int line, kj::StringPtr code));
// Call to throw an exception based on the OpenSSL error code. Usually, you should wrap your call
// in OSSLCALL() to have this invoked automatically.
//
// Some error codes are translated into application-visible errors of type
// `DOMException(OperationError)`, but most errors are considered internal errors.

kj::Vector<kj::OneOf<kj::StringPtr, OpensslUntranslatedError>> consumeAllOpensslErrors();
// Consumes the entire OpenSSL error queue & converts it either into friendly names or the raw
// (unfriendly) name that OpenSSL gives the error code.

kj::String tryDescribeOpensslErrors(kj::StringPtr defaultIfNoError = nullptr);
// Returns a description of the OpenSSL errors (starting with ": ") in the stack & clears them if
// there are any. The expected usage is something like:
//   JSG_REQUIRE(<some OpenSSL call succeeds>, OperationError, "This thing went wrong",
//       tryDescribeOpensslErrors());
// This way if there are any OpenSSL errors to describe it will get rendered as:
//   "jsg.DOMException(OperationError): This thing went wrong: <description>."
// and if there aren't, then this will get rendered as:
//   "jsg.DOMException(OperationError): This thing went wrong."

kj::String internalDescribeOpensslErrors();
// Like tryDescribeOpensslErrors but dumps all OpenSSL errors even if not user-facing. This is for
// use with `Internal` errors passed to JSG which automagically strip all contextual information so
// that these errors only end up in Sentry.

std::pair<kj::StringPtr, const EVP_MD*> lookupDigestAlgorithm(kj::StringPtr algorithm);
// Helper for implementing `sign()`, `digest()` and `importKey()`. Returns a pair containing a
// StringPtr to the normalized name of the given algorithm and the EVP_MD type to use with
// OpenSSL's EVP interface.
//
// Throws if the given algorithm isn't supported.

kj::EncodingResult<kj::Array<kj::byte>> decodeBase64Url(kj::String text);
// kj::decodeBase64 doesn't know how to parse URL-encoded variants.
// https://en.wikipedia.org/wiki/Base64#URL_applications
// Due to this, the input string is modified prior to passing to kj::decodeBase64. The mutation
// isn't actually required & it's possible that a non-mutating variant could be written. That's more
// complex to implement outside of kj::decodeBase64 though and the mutating variant is easier to
// implement as a wrapper. Could be sufficient to just add a "urlEncoded" boolean so that
// kj::decodeBase64 can do this in-situ for both cases.

template <typename T>
T interpretAlgorithmParam(kj::OneOf<kj::String, T>&& param) {
  // WebCrypto likes to allow algorithms to be specified as a simple string name, or as a struct
  // containing a `name` field and possibly other fields. This helper collapses that.

  if (param.template is<kj::String>()) {
    T result;
    result.name = kj::mv(param.template get<kj::String>());
    return result;
  } else {
    return kj::mv(param.template get<T>());
  }
}

template <typename T>
kj::StringPtr getAlgorithmName(const kj::OneOf<kj::String, T>& param) {
  // Like `interpretAlgorithmParam` but just get the algorithm name. Works with const input.

  if (param.template is<kj::String>()) {
    return param.template get<kj::String>();
  } else {
    return param.template get<T>().name;
  }
}

class CryptoKey::Impl {
public:
  // C++ API

  using ImportFunc = kj::Own<Impl>(
      kj::StringPtr normalizedName, kj::StringPtr format,
      SubtleCrypto::ImportKeyData keyData,
      SubtleCrypto::ImportKeyAlgorithm&& algorithm, bool extractable,
      kj::ArrayPtr<const kj::String> keyUsages);

  static ImportFunc importAes;
  static ImportFunc importHmac;
  static ImportFunc importPbkdf2;
  static ImportFunc importHkdf;
  static ImportFunc importRsa;
  static ImportFunc importEcdsa;
  static ImportFunc importEcdh;
  static ImportFunc importEddsa;
  static ImportFunc importRsaRaw;

  using GenerateFunc = kj::OneOf<jsg::Ref<CryptoKey>, CryptoKeyPair>(
      kj::StringPtr normalizedName,
      SubtleCrypto::GenerateKeyAlgorithm&& algorithm, bool extractable,
      kj::ArrayPtr<const kj::String> keyUsages);

  static GenerateFunc generateAes;
  static GenerateFunc generateHmac;
  static GenerateFunc generateRsa;
  static GenerateFunc generateEcdsa;
  static GenerateFunc generateEcdh;
  static GenerateFunc generateEddsa;

  Impl(bool extractable, CryptoKeyUsageSet usages) : extractable(extractable), usages(usages) {}

  bool isExtractable() const { return extractable; }
  CryptoKeyUsageSet getUsages() const { return usages; }

  virtual kj::Array<kj::byte> encrypt(
      SubtleCrypto::EncryptAlgorithm&& algorithm,
      kj::ArrayPtr<const kj::byte> plainText) const {
    JSG_FAIL_REQUIRE(DOMNotSupportedError, "The encrypt operation is not implemented for \"",
        getAlgorithmName(), "\".");
  }
  virtual kj::Array<kj::byte> decrypt(
      SubtleCrypto::EncryptAlgorithm&& algorithm,
      kj::ArrayPtr<const kj::byte> cipherText) const {
    JSG_FAIL_REQUIRE(DOMNotSupportedError, "The decrypt operation is not implemented for \"",
        getAlgorithmName(), "\".");
  }

  virtual kj::Array<kj::byte> sign(
      SubtleCrypto::SignAlgorithm&& algorithm,
      kj::ArrayPtr<const kj::byte> data) const {
    JSG_FAIL_REQUIRE(DOMNotSupportedError, "The sign operation is not implemented for \"",
        getAlgorithmName(), "\".");
  }
  virtual bool verify(
      SubtleCrypto::SignAlgorithm&& algorithm, kj::ArrayPtr<const kj::byte> signature,
      kj::ArrayPtr<const kj::byte> data) const {
    JSG_FAIL_REQUIRE(DOMNotSupportedError, "The verify operation is not implemented for \"",
        getAlgorithmName(), "\".");
  }

  virtual kj::Array<kj::byte> deriveBits(
      SubtleCrypto::DeriveKeyAlgorithm&& algorithm, kj::Maybe<uint32_t> length) const {
    JSG_FAIL_REQUIRE(DOMNotSupportedError,
        "The deriveKey and deriveBits operations are not implemented for \"",
        getAlgorithmName(), "\".");
  }

  virtual kj::Array<kj::byte> wrapKey(SubtleCrypto::EncryptAlgorithm&& algorithm,
      kj::ArrayPtr<const kj::byte> unwrappedKey) const {
    // For many algorithms, wrapKey() is the same as encrypt(), so as a convenience the default
    // implementation just forwards to it.
    return encrypt(kj::mv(algorithm), unwrappedKey);
  }

  virtual kj::Array<kj::byte> unwrapKey(SubtleCrypto::EncryptAlgorithm&& algorithm,
      kj::ArrayPtr<const kj::byte> wrappedKey) const {
    // For many algorithms, unwrapKey() is the same as decrypt(), so as a convenience the default
    // implementation just forwards to it.
    return decrypt(kj::mv(algorithm), wrappedKey);
  }

  virtual SubtleCrypto::ExportKeyData exportKey(kj::StringPtr format) const {
    JSG_FAIL_REQUIRE(DOMNotSupportedError,
        "Unrecognized or unsupported export of \"", getAlgorithmName(), "\" requested.");
  }

  virtual kj::Array<kj::byte> exportKeyExt(
      kj::StringPtr format,
      kj::StringPtr type,
      jsg::Optional<kj::String> cipher = nullptr,
      jsg::Optional<kj::Array<kj::byte>> passphrase = nullptr) const {
    // The exportKeyExt variant is used by the Node.js crypto module. It allows the caller to
    // specify a broader range of export formats and types that are not supported by Web
    // Crypto. For instance, Web Crypto limits the export of public keys to only the spki or
    // jwk formats, while Node.js allows pkcs1 or spki formatted as either pem, der, or jwk.
    // For private keys, Node.js allows optionally encrypting the private key using a given
    // cipher and passphrase.
    // Rather than modify the existing exportKey API, we add this new variant to support the
    // Node.js implementation without risking breaking the Web Crypto impl.
    JSG_FAIL_REQUIRE(DOMNotSupportedError,
        "Unrecognized or unsupported export of \"", getAlgorithmName(), "\" requested.");
  }

  virtual kj::StringPtr getAlgorithmName() const = 0;

  virtual CryptoKey::AsymmetricKeyDetails getAsymmetricKeyDetail() const {
    JSG_FAIL_REQUIRE(DOMNotSupportedError,
        "The getAsymmetricKeyDetail operation is not implemented for \"", getAlgorithmName(),
        "\".");
  }

  // JS API implementation

  virtual AlgorithmVariant getAlgorithm() const = 0;
  virtual kj::StringPtr getType() const { return "secret"_kj; }

  virtual bool equals(const Impl& other) const = 0;
  virtual bool equals(const kj::Array<kj::byte>& other) const;

private:
  const bool extractable;
  const CryptoKeyUsageSet usages;
};

struct CryptoAlgorithm {
  kj::StringPtr name;
  // Name, in canonical (all-uppercase) format.

  CryptoKey::Impl::ImportFunc* importFunc = nullptr;
  CryptoKey::Impl::GenerateFunc* generateFunc = nullptr;
  // Functions to import / generate keys for this algorithm. If nullptr, the respective
  // operation isn't allowed.
  //
  // TODO(cleanup): I have these as pointers instead of maybe-references because the references
  //   would have to be const in order to enable const-copying, but it turns out you cannot specify
  //   `const` on a reference-to-function (the compiler ignores it as "redundant", but then
  //   template metaprogramming cannot recognize it as const). Maybe we can fix this in KJ, by
  //   making `RemoveConstOrDisable` recognize function references are inherenly const.

  inline bool operator==(const CryptoAlgorithm& other) const {
    return strcasecmp(name.cStr(), other.name.cStr()) == 0;
  }
  inline bool operator< (const CryptoAlgorithm& other) const {
    return strcasecmp(name.cStr(), other.name.cStr()) < 0;
  }
  // Allow comparison by name, case-insensitive. This is a convenience for placing in an std::set.
  //
  // TODO(cleanup): I'd rather use kj::Table with HashIndex but we need a case-insensitive hash
  //   function, which seemed slightly too annoying to implement now.
};

class SslArrayDisposer : public kj::ArrayDisposer {
public:
  static SslArrayDisposer INSTANCE;

  void disposeImpl(void* firstElement, size_t elementSize, size_t elementCount,
                   size_t capacity, void (*destroyElement)(void*)) const;
};

template <typename T, void (*sslFree)(T*)>
class SslDisposer: public kj::Disposer {
public:
  static const SslDisposer INSTANCE;

protected:
  void disposeImpl(void* pointer) const override {
    sslFree(reinterpret_cast<T*>(pointer));
  }
};

template <typename T, void (*sslFree)(T*)>
const SslDisposer<T, sslFree> SslDisposer<T, sslFree>::INSTANCE;

#define OSSLCALL_OWN(T, code, ...) \
  ({ \
    T* result = code; \
    JSG_REQUIRE(result != nullptr, ##__VA_ARGS__); \
    kj::Own<T>(result, workerd::api::SslDisposer<T, &T##_free>::INSTANCE); \
  })

#define OSSL_NEW(T, ...) \
  OSSLCALL_OWN(T, T##_new(__VA_ARGS__), InternalDOMOperationError, "Error allocating crypto")

#define BIGNUM_new BN_new
#define BIGNUM_free BN_free
// BIGNUM obnoxiously doesn't follow the naming convention...

#define OSSL_BIO_MEM() \
  ({ \
    BIO* result = BIO_new(BIO_s_mem()); \
    JSG_REQUIRE(result != nullptr, InternalDOMOperationError, "Error allocating crypto"); \
    kj::Own<BIO>(result, workerd::api::SslDisposer<BIO, &BIO_free_all>::INSTANCE); \
  })

// Adopted from Node.js' crypto implementation. the MarkPopErrorOnReturn
// and ClearErrorOnReturn mechanisms make working with the openssl error
// stack a bit easier...
struct MarkPopErrorOnReturn {
  MarkPopErrorOnReturn() { ERR_set_mark(); }
  ~MarkPopErrorOnReturn() { ERR_pop_to_mark(); }
  KJ_DISALLOW_COPY_AND_MOVE(MarkPopErrorOnReturn);
};

struct ClearErrorOnReturn {
  ClearErrorOnReturn() = default;
  ~ClearErrorOnReturn() { ERR_clear_error(); }
  KJ_DISALLOW_COPY_AND_MOVE(ClearErrorOnReturn);
};

template <typename T>
static inline T integerCeilDivision(T a, T b) {
  // Returns ceil(a / b) for integers (std::ceil always returns a floating point result).
  static_assert(std::is_unsigned<T>::value);
  return a == 0 ? 0 : 1 + (a - 1) / b;
}

kj::Own<EVP_PKEY> ellipticJwkReader(int curveId, SubtleCrypto::JsonWebKey&& keyDataJwk);
kj::Own<EVP_PKEY> rsaJwkReader(SubtleCrypto::JsonWebKey&& keyDataJwk);

}  // namespace workerd::api

KJ_DECLARE_NON_POLYMORPHIC(EC_KEY);
KJ_DECLARE_NON_POLYMORPHIC(EC_POINT);
KJ_DECLARE_NON_POLYMORPHIC(EC_GROUP);
KJ_DECLARE_NON_POLYMORPHIC(BN_CTX);
KJ_DECLARE_NON_POLYMORPHIC(EVP_PKEY);
KJ_DECLARE_NON_POLYMORPHIC(EVP_PKEY_CTX);
// Tell KJ that these OpenSSL types are non-polymorphic so that they can be wrapped in kj::Own.
