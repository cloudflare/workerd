// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// WebCrypto API

#include <bit>
#include <workerd/io/features.h>
#include <workerd/jsg/buffersource.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/buffersource.h>
#include <openssl/err.h>
#include "../streams.h"

namespace workerd::api {
namespace node {
class CryptoImpl;
}
namespace {
class EdDsaKey;
class EllipticKey;
}

// Subset of recognized key usage values.
//
// https://w3c.github.io/webcrypto/#dfn-RecognizedKeyUsage
class CryptoKeyUsageSet {
public:
  static constexpr CryptoKeyUsageSet encrypt()    { return 1 << 0; }
  static constexpr CryptoKeyUsageSet decrypt()    { return 1 << 1; }
  static constexpr CryptoKeyUsageSet sign()       { return 1 << 2; }
  static constexpr CryptoKeyUsageSet verify()     { return 1 << 3; }
  static constexpr CryptoKeyUsageSet deriveKey()  { return 1 << 4; }
  static constexpr CryptoKeyUsageSet deriveBits() { return 1 << 5; }
  static constexpr CryptoKeyUsageSet wrapKey()    { return 1 << 6; }
  static constexpr CryptoKeyUsageSet unwrapKey()  { return 1 << 7; }

  static constexpr CryptoKeyUsageSet publicKeyMask() {
    return encrypt() | verify() | wrapKey();
  }

  static constexpr CryptoKeyUsageSet privateKeyMask() {
    return decrypt() | sign() | unwrapKey() | deriveKey() | deriveBits();
  }

  static constexpr CryptoKeyUsageSet derivationKeyMask() {
    return deriveKey() | deriveBits();
  }

  CryptoKeyUsageSet() : set(0) {}

  CryptoKeyUsageSet operator&(CryptoKeyUsageSet other) const { return set & other.set; }
  CryptoKeyUsageSet operator|(CryptoKeyUsageSet other) const { return set | other.set; }

  CryptoKeyUsageSet& operator&=(CryptoKeyUsageSet other) {
    set &= other.set;
    return *this;
  }

  CryptoKeyUsageSet& operator|=(CryptoKeyUsageSet other) {
    set |= other.set;
    return *this;
  }

  // True if and only if this is a subset of the given set.
  inline bool operator<=(CryptoKeyUsageSet superset) const { return (superset & *this) == *this; }

  inline bool operator==(CryptoKeyUsageSet other) const { return set == other.set; }

  unsigned int size() const { return std::popcount(set); }
  bool isSingleton() const { return size() == 1; }

  // The recognized name. This must be a singleton.
  kj::StringPtr name() const;

  // A singleton with the given name.
  static CryptoKeyUsageSet byName(kj::StringPtr name);

  // All singletons, in the order defined by the spec (encrypt, decrypt, sign, verify, ...).
  static kj::ArrayPtr<const CryptoKeyUsageSet> singletons();

  enum class Context { generate, importSecret, importPublic, importPrivate };

  // Parses a list of key usage strings. Throws if any are not recognized or not in mask.
  static CryptoKeyUsageSet validate(kj::StringPtr normalizedName, Context ctx,
      kj::ArrayPtr<const kj::String> actual, CryptoKeyUsageSet mask);

  template <typename Func>
  auto map(Func f) const -> kj::Array<decltype(f(*this))> {
    auto strings = kj::heapArrayBuilder<decltype(f(*this))>(size());
    for (auto& singleton: singletons()) {
      if (singleton <= *this) strings.add(f(singleton));
    }
    return strings.finish();
  }

private:
  constexpr CryptoKeyUsageSet(uint8_t set) : set(set) {}
  uint8_t set;
};

// =======================================================================================
// SubtleCrypto and CryptoKey

// Represents keying material. Users get an object of this type by calling SubtleCrypto's
// `importKey()`, `generateKey()`, or `deriveKey()` methods. The user can then use the object by
// passing it as a parameter to other SubtleCrypto methods.
class CryptoKey: public jsg::Object {
public:
  // KeyAlgorithm dictionaries
  //
  // These dictionaries implement CryptoKey's `algorithm` property. They allow user code to inspect
  // which algorithm a particular CryptoKey is used for, and what algorithm-specific parameters it
  // might have. These are similar to the Algorithm-derived dictionaries used as parameters to
  // SubtleCrypto's interface (see the SubtleCrypto class below), but they are specific to
  // CryptoKey. Like Algorithm, all of these dictionaries notionally derive from a KeyAlgorithm base
  // class.
  //
  // One difference between CryptoKey::KeyAlgorithm dictionaries and SubtleCrypto::Algorithm
  // dictionaries is that KeyAlgorithms use a kj::StringPtr to store their algorithm names, because
  // we know that they will only ever point to internal static strings of normalized algorithm
  // names.

  struct KeyAlgorithm {
    kj::StringPtr name;
    JSG_STRUCT(name);
    JSG_MEMORY_INFO(KeyAlgorithm) {}
  };

  struct AesKeyAlgorithm {
    // "AES-CTR", "AES-GCM", "AES-CBC", "AES-KW"
    kj::StringPtr name;

    // Length in bits of the key.
    uint16_t length;

    JSG_STRUCT(name, length);

    JSG_MEMORY_INFO(AesKeyAlgorithm) {}
  };

  struct HmacKeyAlgorithm {
    // "HMAC"
    kj::StringPtr name;

    // The inner hash function to use.
    KeyAlgorithm hash;

    // Length in bits of the key. The spec wants this to be an unsigned long, but whatever.
    // TODO(someday): Reexamine use of uint16_t in these algorithm structures.
    // We picked uint16_t to work around ambiguous bindings for uint32_t in
    // jsg::PrimitiveWrapper::wrap().  HMAC, at least, allows very long keys.
    uint16_t length;

    JSG_STRUCT(name, hash, length);
    JSG_MEMORY_INFO(HmacKeyAlgorithm) {}
  };

  using BigInteger = kj::Array<kj::byte>;

  struct RsaKeyAlgorithm {
    // "RSASSA-PKCS1-v1_5", "RSA-PSS", "RSA-OAEP"
    kj::StringPtr name;

    // The length, in bits, of the RSA modulus. The spec would have this be an unsigned long.
    uint16_t modulusLength;

     // The RSA public exponent (in unsigned big-endian form)
    kj::OneOf<BigInteger, jsg::BufferSource> publicExponent;

    // The hash algorithm that is used with this key.
    jsg::Optional<KeyAlgorithm> hash;

    RsaKeyAlgorithm clone(jsg::Lock& js) const {
      auto fixPublicExp = FeatureFlags::get(js).getCryptoPreservePublicExponent();
      KJ_SWITCH_ONEOF(publicExponent) {
        KJ_CASE_ONEOF(array, BigInteger) {
          if (fixPublicExp) {
            auto expCopy = kj::heapArray<kj::byte>(array.asPtr());
            jsg::BackingStore expBack = jsg::BackingStore::from(kj::mv(expCopy));
            return { name, modulusLength, jsg::BufferSource(js, kj::mv(expBack)), hash };
          } else {
            return { name, modulusLength, kj::heapArray(array.asPtr()), hash };
          }
        }
        KJ_CASE_ONEOF(source, jsg::BufferSource) {
          // Should only happen if the flag is enabled and an algorithm field is cloned twice.
          KJ_ASSERT(fixPublicExp == true);
          auto expCopy = kj::heapArray<kj::byte>(source.asArrayPtr());
          jsg::BackingStore expBack = jsg::BackingStore::from(kj::mv(expCopy));
          return { name, modulusLength, jsg::BufferSource(js, kj::mv(expBack)), hash };
        }
      }
      KJ_UNREACHABLE;
    }

    JSG_STRUCT(name, modulusLength, publicExponent, hash);

    JSG_MEMORY_INFO(RsaKeyAlgorithm) {}
  };

  struct EllipticKeyAlgorithm {
    // "ECDSA" or "ECDH"
    kj::StringPtr name;

    // "P-256", "P-384", or "P-521"
    kj::StringPtr namedCurve;

    JSG_STRUCT(name, namedCurve);

    JSG_MEMORY_INFO(EllipticKeyAlgorithm) {}
  };

  // Catch-all that can be used for extension algorithms. Combines fields of several known types.
  struct ArbitraryKeyAlgorithm {
    // TODO(cleanup): Should we just replace AlgorithmVariant with this? Note we'd have to add
    //   `publicExponent` which is currently a problem because it makes the type non-copyable...
    //   Alternatively, should we create some better way to abstract this?

    kj::StringPtr name;
    jsg::Optional<KeyAlgorithm> hash;
    jsg::Optional<kj::StringPtr> namedCurve;
    jsg::Optional<uint16_t> length;

    JSG_STRUCT(name, hash, namedCurve, length);
  };

  // Used as part of the Node.js crypto implementation of KeyObject.
  // Defined here instead of api/node/crypto.h because it it is needed
  // by CryptoKey::Impl to provide the actual implementation.
  struct AsymmetricKeyDetails {
    jsg::Optional<uint32_t> modulusLength;
    jsg::Optional<kj::Array<kj::byte>> publicExponent;
    jsg::Optional<kj::String> hashAlgorithm;
    jsg::Optional<kj::String> mgf1HashAlgorithm;
    jsg::Optional<uint32_t> saltLength;
    jsg::Optional<uint32_t> divisorLength;
    jsg::Optional<kj::String> namedCurve;
    JSG_STRUCT(modulusLength,
                publicExponent,
                hashAlgorithm,
                mgf1HashAlgorithm,
                saltLength,
                divisorLength,
                namedCurve);
  };
  AsymmetricKeyDetails getAsymmetricKeyDetails() const;

  ~CryptoKey() noexcept(false);

  // Returns the name of this CryptoKey's algorithm in a normalized, statically-allocated string.
  kj::StringPtr getAlgorithmName() const;

  // JS API

  using AlgorithmVariant = kj::OneOf<
      KeyAlgorithm, AesKeyAlgorithm, HmacKeyAlgorithm, RsaKeyAlgorithm,
      EllipticKeyAlgorithm, ArbitraryKeyAlgorithm>;

  AlgorithmVariant getAlgorithm(jsg::Lock& js) const;
  kj::StringPtr getType() const;
  bool getExtractable() const;
  kj::Array<kj::StringPtr> getUsages() const;
  CryptoKeyUsageSet getUsageSet() const;

  JSG_RESOURCE_TYPE(CryptoKey) {
    JSG_READONLY_INSTANCE_PROPERTY(type, getType);
    JSG_READONLY_INSTANCE_PROPERTY(extractable, getExtractable);
    JSG_READONLY_INSTANCE_PROPERTY(algorithm, getAlgorithm);
    JSG_READONLY_INSTANCE_PROPERTY(usages, getUsages);
  }

  // HACK: Needs to be public so derived classes can inherit from it.
  class Impl;

  // Treat as private -- needs to be public for jsg::alloc<T>()...
  explicit CryptoKey(kj::Own<Impl> impl);

  // Compare the contents of this key with the other. Will return false if
  // either key is not extractable or if the keys are a different type.
  // For secret keys, we will compare only the actual key material and not
  // the algorithm parameters or the algorithm name. We will also ensure
  // that a timing-safe comparison is used for the key material.
  bool operator==(const CryptoKey& other) const;

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const;

private:
  kj::Own<Impl> impl;

  friend class SubtleCrypto;
  friend class EllipticKey;
  friend class EdDsaKey;
  friend class node::CryptoImpl;
};

struct CryptoKeyPair {
  jsg::Ref<CryptoKey> publicKey;
  jsg::Ref<CryptoKey> privateKey;

  JSG_STRUCT(publicKey, privateKey);
};

class SubtleCrypto: public jsg::Object {
public:
  // Algorithm dictionaries
  //
  // Every method of SubtleCrypto except `exportKey()` takes an `algorithm` parameter, usually as the
  // first argument. This can usually be a raw string algorithm name, or an object with a `name`
  // field and other fields. The other fields differ based on which algorithm is named and which
  // function is being called. We achieve polymorphism here by making all the fields except `name`
  // be `jsg::Optional`... ugly, but it works.

  // Type of the `algorithm` parameter passed to `digest()`. Also used as the type of the `hash`
  // parameter of many other algorithm structs.
  struct HashAlgorithm {
    kj::String name;

    JSG_STRUCT(name);
  };

  // Type of the `algorithm` parameter passed to `encrypt()` and `decrypt()`. Different
  // algorithms call for different fields.
  struct EncryptAlgorithm {
    // E.g. "AES-GCM"
    kj::String name;

    // For AES: The initialization vector use. May be up to 2^64-1 bytes long.
    jsg::Optional<kj::Array<kj::byte>> iv;

    // The additional authentication data to include.
    jsg::Optional<kj::Array<kj::byte>> additionalData;

    // The desired length of the authentication tag. May be 0 - 128.
    // Note: the spec specifies this as a Web IDL byte (== signed char in C++), not an int, but JS
    //   has no such 8-bit integer animal.
    jsg::Optional<int> tagLength;

    // The initial value of the counter block for AES-CTR.
    // https://www.w3.org/TR/WebCryptoAPI/#aes-ctr-params
    jsg::Optional<kj::Array<kj::byte>> counter;

    // The length, in bits, of the rightmost part of the counter block that is incremented.
    // See above why we use int instead of int8_t.
    // https://www.w3.org/TR/WebCryptoAPI/#aes-ctr-params
    jsg::Optional<int> length;

    // The optional label/application data to associate with the message (for RSA-OAEP)
    jsg::Optional<kj::Array<kj::byte>> label;

    JSG_STRUCT(name, iv, additionalData, tagLength, counter, length, label);
  };

  // Type of the `algorithm` parameter passed to `sign()` and `verify()`. Different
  // algorithms call for diferent fields.
  struct SignAlgorithm {
    // E.g. "RSASSA-PKCS1-v1_5", "ECDSA"
    kj::String name;

    // ECDSA wants the hash to be specified at call time rather than import
    // time.
    jsg::Optional<kj::OneOf<kj::String, HashAlgorithm>> hash;

    // Not part of the WebCrypto spec. Used by an extension.
    jsg::Optional<int> dataLength;

    // Used for RSA-PSS
    jsg::Optional<int> saltLength;

    JSG_STRUCT(name, hash, dataLength, saltLength);
  };

  // Type of the `algorithm` parameter passed to `generateKey()`. Different algorithms call for
  // different fields.
  struct GenerateKeyAlgorithm {
    // E.g. "HMAC", "RSASSA-PKCS1-v1_5", "ECDSA", ...
    kj::String name;

    // For signing algorithms where the hash is specified at import time, identifies the hash
    // function to use, e.g. "SHA-256".
    jsg::Optional<kj::OneOf<kj::String, HashAlgorithm>> hash;

    // For RSA algorithms: The length in bits of the RSA modulus.
    jsg::Optional<int> modulusLength;

    // For RSA algorithms
    jsg::Optional<kj::Array<kj::byte>> publicExponent;

    // For AES algorithms or when name == "HMAC": The length in bits of the key.
    jsg::Optional<int> length;

    // When name == "ECDSA": "P-256", "P-384", or "P-521"
    jsg::Optional<kj::String> namedCurve;

    JSG_STRUCT(name, hash, modulusLength, publicExponent, length, namedCurve);
  };

  // Type of the `algorithm` parameter passed to `importKey()`, as well as the
  // `derivedKeyAlgorithm` parameter to `deriveKey()`. Different algorithms call for different
  // fields.
  struct ImportKeyAlgorithm {
    // E.g. "HMAC", "RSASSA-PKCS1-v1_5", "ECDSA", ...
    kj::String name;

    // For signing algorithms where the hash is specified at import time, identifies the hash
    // function to use, e.g. "SHA-256".
    jsg::Optional<kj::OneOf<kj::String, HashAlgorithm>> hash;

    // When name == "HMAC": The length in bits of the key.
    jsg::Optional<int> length;

    // When name == "ECDSA": "P-256", "P-384", or "P-521"
    jsg::Optional<kj::String> namedCurve;

    // Not part of the WebCrypto spec. Used by an extension to indicate that curve points are in
    // compressed format. (The standard algorithms do not recognize this option.)
    jsg::Optional<bool> compressed;

    JSG_STRUCT(name, hash, length, namedCurve, compressed);
  };

  // Type of the `algorithm` parameter passed to `deriveKey()`. Different algorithms call for
  // different fields.
  struct DeriveKeyAlgorithm {
    // e.g. "PBKDF2", "ECDH", etc
    kj::String name;

    // PBKDF2 parameters
    jsg::Optional<kj::Array<kj::byte>> salt;
    jsg::Optional<int> iterations;
    jsg::Optional<kj::OneOf<kj::String, HashAlgorithm>> hash;

    // ECDH parameters
    jsg::Optional<jsg::Ref<CryptoKey>> $public;

    // HKDF parameters (some shared with PBKDF2)

    // Bit string that corresponds to the context and application specific context for the derived
    // keying material
    jsg::Optional<kj::Array<kj::byte>> info;

    JSG_STRUCT(name, salt, iterations, hash, $public, info);
  };

  // https://www.w3.org/TR/WebCryptoAPI/#JsonWebKey-dictionary
  struct JsonWebKey {

    struct RsaOtherPrimesInfo {
      // The following fields are defined in Section 6.3.2.7 of JSON Web Algorithms
      jsg::Optional<kj::String> r;
      jsg::Optional<kj::String> d;
      jsg::Optional<kj::String> t;

      JSG_STRUCT(r, d, t);
      JSG_STRUCT_TS_OVERRIDE(RsaOtherPrimesInfo); // Rename from SubtleCryptoJsonWebKeyRsaOtherPrimesInfo
    };

    // The following fields are defined in Section 3.1 of JSON Web Key (RFC 7517).
    // NOTE: The Web Crypto spec's IDL for JsonWebKey considers `kty` optional, yet the RFC lists it
    //   as required.
    kj::String kty;
    jsg::Optional<kj::String> use;
    jsg::Optional<kj::Array<kj::String>> key_ops;
    jsg::Optional<kj::String> alg;

    // The following fields are defined in JSON Web Key Parameters Registration
    jsg::Optional<bool> ext;

    // The following fields are defined in Section 6 of JSON Web Algorithms
    jsg::Optional<kj::String> crv;
    jsg::Optional<kj::String> x;
    jsg::Optional<kj::String> y;
    jsg::Optional<kj::String> d;
    jsg::Optional<kj::String> n;
    jsg::Optional<kj::String> e;
    jsg::Optional<kj::String> p;
    jsg::Optional<kj::String> q;
    jsg::Optional<kj::String> dp;
    jsg::Optional<kj::String> dq;
    jsg::Optional<kj::String> qi;
    jsg::Optional<kj::Array<RsaOtherPrimesInfo>> oth;
    // TODO(conform): Support multiprime RSA keys. This used to be jsg::Unimplemented but needs to
    //   be properly defined for exporting JWK of other keys. On the other hand, are we even going
    //   to bother adding support for multiprime RSA keys? Chromium doesn't AFAICT...
    jsg::Optional<kj::String> k;

    JSG_STRUCT(kty, use, key_ops, alg, ext, crv, x, y, d, n, e, p, q, dp, dq, qi, oth, k);
    JSG_STRUCT_TS_OVERRIDE(JsonWebKey); // Rename from SubtleCryptoJsonWebKey
  };

  using ImportKeyData = kj::OneOf<kj::Array<kj::byte>, JsonWebKey>;
  using ExportKeyData = kj::OneOf<kj::Array<kj::byte>, JsonWebKey>;

  jsg::Promise<kj::Array<kj::byte>> encrypt(
      jsg::Lock& js,
      kj::OneOf<kj::String, EncryptAlgorithm> algorithm,
      const CryptoKey& key,
      kj::Array<const kj::byte> plainText);
  jsg::Promise<kj::Array<kj::byte>> decrypt(
      jsg::Lock& js,
      kj::OneOf<kj::String, EncryptAlgorithm> algorithm,
      const CryptoKey& key,
      kj::Array<const kj::byte> cipherText);

  jsg::Promise<kj::Array<kj::byte>> sign(
      jsg::Lock& js,
      kj::OneOf<kj::String, SignAlgorithm> algorithm,
      const CryptoKey& key,
      kj::Array<const kj::byte> data);
  jsg::Promise<bool> verify(
      jsg::Lock& js,
      kj::OneOf<kj::String, SignAlgorithm> algorithm,
      const CryptoKey& key,
      kj::Array<const kj::byte> signature,
      kj::Array<const kj::byte> data);

  jsg::Promise<kj::Array<kj::byte>> digest(
      jsg::Lock& js,
      kj::OneOf<kj::String, HashAlgorithm> algorithm,
      kj::Array<const kj::byte> data);

  jsg::Promise<kj::OneOf<jsg::Ref<CryptoKey>, CryptoKeyPair>> generateKey(
      jsg::Lock& js,
      kj::OneOf<kj::String, GenerateKeyAlgorithm> algorithm,
      bool extractable,
      kj::Array<kj::String> keyUsages);

  jsg::Promise<jsg::Ref<CryptoKey>> deriveKey(
      jsg::Lock& js,
      kj::OneOf<kj::String, DeriveKeyAlgorithm> algorithm,
      const CryptoKey& baseKey,
      kj::OneOf<kj::String, ImportKeyAlgorithm> derivedKeyAlgorithm,
      bool extractable,
      kj::Array<kj::String> keyUsages);
  jsg::Promise<kj::Array<kj::byte>> deriveBits(
      jsg::Lock& js,
      kj::OneOf<kj::String, DeriveKeyAlgorithm> algorithm,
      const CryptoKey& baseKey,
      kj::Maybe<int> length);

  jsg::Promise<jsg::Ref<CryptoKey>> importKey(
      jsg::Lock& js,
      kj::String format,
      ImportKeyData keyData,
      kj::OneOf<kj::String, ImportKeyAlgorithm> algorithm,
      bool extractable,
      kj::Array<kj::String> keyUsages);

  // NOT VISIBLE TO JS: like importKey() but return the key, not a promise.
  jsg::Ref<CryptoKey> importKeySync(
      jsg::Lock& js,
      kj::StringPtr format,
      ImportKeyData keyData,
      ImportKeyAlgorithm algorithm,
      bool extractable,
      kj::ArrayPtr<const kj::String> keyUsages);

  jsg::Promise<ExportKeyData> exportKey(
      jsg::Lock& js,
      kj::String format,
      const CryptoKey& key);

  jsg::Promise<kj::Array<kj::byte>> wrapKey(
      jsg::Lock& js,
      kj::String format,
      const CryptoKey& key,
      const CryptoKey& wrappingKey,
      kj::OneOf<kj::String, EncryptAlgorithm> wrapAlgorithm,
      const jsg::TypeHandler<JsonWebKey>& jwkHandler);
  jsg::Promise<jsg::Ref<CryptoKey>> unwrapKey(
      jsg::Lock& js,
      kj::String format,
      kj::Array<const kj::byte> wrappedKey,
      const CryptoKey& unwrappingKey,
      kj::OneOf<kj::String, EncryptAlgorithm> unwrapAlgorithm,
      kj::OneOf<kj::String, ImportKeyAlgorithm> unwrappedKeyAlgorithm,
      bool extractable,
      kj::Array<kj::String> keyUsages,
      const jsg::TypeHandler<JsonWebKey>& jwkHandler);

  // This is a non-standard extension based off Node.js' implementation of crypto.timingSafeEqual.
  bool timingSafeEqual(kj::Array<kj::byte> a, kj::Array<kj::byte> b);

  JSG_RESOURCE_TYPE(SubtleCrypto) {
    JSG_METHOD(encrypt);
    JSG_METHOD(decrypt);
    JSG_METHOD(sign);
    JSG_METHOD(verify);
    JSG_METHOD(digest);
    JSG_METHOD(generateKey);
    JSG_METHOD(deriveKey);
    JSG_METHOD(deriveBits);
    JSG_METHOD(importKey);
    JSG_METHOD(exportKey);
    JSG_METHOD(wrapKey);
    JSG_METHOD(unwrapKey);
    JSG_METHOD(timingSafeEqual);
  }
};

// =======================================================================================
// DigestStream is a non-standard extension that provides a way of generating
// a hash digest from streaming data. It combines Web Crypto concepts into a
// WritableStream and is compatible with both APIs.
class DigestStream: public WritableStream {
public:
  using DigestContextPtr = kj::Own<EVP_MD_CTX>;
  using Algorithm = kj::OneOf<kj::String, SubtleCrypto::HashAlgorithm>;

  explicit DigestStream(
      kj::Own<WritableStreamController> controller,
      SubtleCrypto::HashAlgorithm algorithm,
      jsg::Promise<kj::Array<kj::byte>>::Resolver resolver,
      jsg::Promise<kj::Array<kj::byte>> promise);

  static jsg::Ref<DigestStream> constructor(jsg::Lock& js, Algorithm algorithm);

  jsg::MemoizedIdentity<jsg::Promise<kj::Array<kj::byte>>>& getDigest() { return promise; }
  void dispose(jsg::Lock& js);
  uint64_t getBytesWritten() const { return bytesWritten; }

  JSG_RESOURCE_TYPE(DigestStream, CompatibilityFlags::Reader flags) {
    JSG_INHERIT(WritableStream);
    if (flags.getJsgPropertyOnPrototypeTemplate()) {
      JSG_READONLY_PROTOTYPE_PROPERTY(digest, getDigest);
    } else {
      JSG_READONLY_INSTANCE_PROPERTY(digest, getDigest);
    }
    JSG_READONLY_PROTOTYPE_PROPERTY(bytesWritten, getBytesWritten);
    JSG_DISPOSE(dispose);

    JSG_TS_OVERRIDE(extends WritableStream<ArrayBuffer | ArrayBufferView>);
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const;

private:
  static DigestContextPtr initContext(SubtleCrypto::HashAlgorithm& algorithm);

  struct Ready {
    SubtleCrypto::HashAlgorithm algorithm;
    jsg::Promise<kj::Array<kj::byte>>::Resolver resolver;
    DigestContextPtr context;
    Ready(SubtleCrypto::HashAlgorithm algorithm,
          jsg::Promise<kj::Array<kj::byte>>::Resolver resolver)
        : algorithm(kj::mv(algorithm)),
          resolver(kj::mv(resolver)),
          context(initContext(this->algorithm)) {}
  };
  jsg::MemoizedIdentity<jsg::Promise<kj::Array<kj::byte>>> promise;
  kj::OneOf<Ready, StreamStates::Closed, StreamStates::Errored> state;
  uint64_t bytesWritten = 0;

  kj::Maybe<StreamStates::Errored> write(jsg::Lock& js, kj::ArrayPtr<kj::byte> buffer);
  kj::Maybe<StreamStates::Errored> close(jsg::Lock& js);
  void abort(jsg::Lock& js, jsg::JsValue reason);

  void visitForGc(jsg::GcVisitor& visitor);
};

// =======================================================================================
// Crypto

// Implements the Crypto interface as prescribed by:
// https://www.w3.org/TR/WebCryptoAPI/#crypto-interface
class Crypto: public jsg::Object {
public:
  jsg::BufferSource getRandomValues(jsg::BufferSource buffer);

  kj::String randomUUID();

  jsg::Ref<SubtleCrypto> getSubtle() {
    return subtle.addRef();
  }

  JSG_RESOURCE_TYPE(Crypto, CompatibilityFlags::Reader flags) {
    if (flags.getJsgPropertyOnPrototypeTemplate()) {
      JSG_READONLY_PROTOTYPE_PROPERTY(subtle, getSubtle);
    } else {
      JSG_READONLY_INSTANCE_PROPERTY(subtle, getSubtle);
    }
    JSG_METHOD(getRandomValues);
    JSG_METHOD(randomUUID);

    JSG_NESTED_TYPE(DigestStream);

    JSG_TS_OVERRIDE({
      getRandomValues<
        T extends
          | Int8Array
          | Uint8Array
          | Int16Array
          | Uint16Array
          | Int32Array
          | Uint32Array
          | BigInt64Array
          | BigUint64Array
      >(buffer: T): T;
    });
  }

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(subtle);
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("subtle", subtle);
  }

private:
  jsg::Ref<SubtleCrypto> subtle = jsg::alloc<SubtleCrypto>();
};

#define EW_CRYPTO_ISOLATE_TYPES                       \
  api::Crypto,                                        \
  api::SubtleCrypto,                                  \
  api::CryptoKey,                                     \
  api::CryptoKeyPair,                                 \
  api::SubtleCrypto::JsonWebKey,                      \
  api::SubtleCrypto::JsonWebKey::RsaOtherPrimesInfo,  \
  api::SubtleCrypto::DeriveKeyAlgorithm,              \
  api::SubtleCrypto::EncryptAlgorithm,                \
  api::SubtleCrypto::GenerateKeyAlgorithm,            \
  api::SubtleCrypto::HashAlgorithm,                   \
  api::SubtleCrypto::ImportKeyAlgorithm,              \
  api::SubtleCrypto::SignAlgorithm,                   \
  api::CryptoKey::KeyAlgorithm,                       \
  api::CryptoKey::AesKeyAlgorithm,                    \
  api::CryptoKey::HmacKeyAlgorithm,                   \
  api::CryptoKey::RsaKeyAlgorithm,                    \
  api::CryptoKey::EllipticKeyAlgorithm,               \
  api::CryptoKey::ArbitraryKeyAlgorithm,              \
  api::CryptoKey::AsymmetricKeyDetails,               \
  api::DigestStream

}  // namespace workerd::api

KJ_DECLARE_NON_POLYMORPHIC(EVP_MD_CTX)
