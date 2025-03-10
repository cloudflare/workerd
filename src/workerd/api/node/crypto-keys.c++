// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "crypto.h"
#include "util.h"

#include <workerd/api/crypto/impl.h>
#include <workerd/api/crypto/jwk.h>

#include <ncrypto.h>
#include <openssl/crypto.h>

#include <map>

// TODO(soon): This implements most of node:crypto key import, export, and
// generation with a number of notable exceptions.
//
// 1. While it is possible to import DSA keys, it is currently not possible
//    to generate a new DSA key pair. This is due entirely to limitations
//    currently in boringssl+fips that we use in production.
// 2. It is currently not possible to generate or import diffie-hellman
//    keys or use the stateless diffie-hellman API. The older DH apis are
//    still functional, but the stateless DH and DH keys currently rely on
//    the EVP DH APIs that are not implementing by boringssl+fips. An
//    alternative approach is possible but requires a bit more effort.
// 3.
namespace workerd::api::node {

namespace {
// An algorithm-independent secret key. Used as the underlying
// implementation of Node.js SecretKey objects. Unlike Web Crypto,
// a Node.js secret key is not algorithm specific. For instance, a
// single secret key can be used for both AES and HMAC, where as
// Web Crypto requires a separate key for each algorithm.
class SecretKey final: public CryptoKey::Impl {
 public:
  explicit SecretKey(jsg::BufferSource keyData)
      : Impl(true, CryptoKeyUsageSet::privateKeyMask() | CryptoKeyUsageSet::publicKeyMask()),
        keyData(kj::mv(keyData)) {}
  ~SecretKey() noexcept(false) {
    keyData.setToZero();
  }

  kj::StringPtr getAlgorithmName() const override {
    return "secret"_kj;
  }
  CryptoKey::AlgorithmVariant getAlgorithm(jsg::Lock& js) const override {
    return CryptoKey::ArbitraryKeyAlgorithm{
      .name = getAlgorithmName(),
      .length = keyData.size(),
    };
  }

  bool equals(const CryptoKey::Impl& other) const override final {
    return this == &other || (other.getType() == "secret"_kj && other.equals(keyData));
  }

  bool equalsImpl(kj::ArrayPtr<const kj::byte> other) const {
    return keyData.size() == other.size() &&
        CRYPTO_memcmp(keyData.asArrayPtr().begin(), other.begin(), keyData.size()) == 0;
  }

  bool equals(const kj::Array<kj::byte>& other) const override final {
    return equalsImpl(other.asPtr());
  }

  bool equals(const jsg::BufferSource& other) const override final {
    return equalsImpl(other.asArrayPtr());
  }

  SubtleCrypto::ExportKeyData exportKey(jsg::Lock& js, kj::StringPtr format) const override final {
    JSG_REQUIRE(format == "raw" || format == "jwk", DOMNotSupportedError, getAlgorithmName(),
        " key only supports exporting \"raw\" & \"jwk\", not \"", format, "\".");

    if (format == "jwk") {
      SubtleCrypto::JsonWebKey jwk;
      jwk.kty = kj::str("oct");
      jwk.k = fastEncodeBase64Url(keyData.asArrayPtr());
      jwk.ext = true;
      return jwk;
    }

    auto backing = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, keyData.size());
    backing.asArrayPtr().copyFrom(keyData.asArrayPtr());
    return jsg::BufferSource(js, kj::mv(backing));
  }

  kj::StringPtr jsgGetMemoryName() const override {
    return "SecretKey";
  }
  size_t jsgGetMemorySelfSize() const override {
    return sizeof(SecretKey);
  }
  void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const override {
    tracker.trackField("keyData", keyData);
  }

  void visitForGc(jsg::GcVisitor& visitor) override {
    visitor.visit(keyData);
  }

  const kj::ArrayPtr<const kj::byte> rawKeyData() const {
    return keyData.asArrayPtr().asConst();
  }

 private:
  jsg::BufferSource keyData;
};

CryptoKey::AsymmetricKeyDetails getRsaKeyDetails(const ncrypto::EVPKeyPointer& key) {
  ncrypto::Rsa rsa = key;

  // BoringSSL does not currently support the id-RSASSA-PSS key encoding and
  // does not support getting the PSS param details using RSA_get0_pss_params.
  // Therefore there's nothing else to do here currently.
  // TODO(later): If/When BoringSSL supports getting the pss params, we will
  // need to update this.
  KJ_ASSERT(!rsa.getPssParams().has_value());

  return CryptoKey::AsymmetricKeyDetails{
    .modulusLength = key.bits(),
    .publicExponent = JSG_REQUIRE_NONNULL(
        bignumToArrayPadded(*rsa.getPublicKey().e), Error, "Failed to extract public exponent"),
  };
}

CryptoKey::AsymmetricKeyDetails getDsaKeyDetails(const ncrypto::EVPKeyPointer& key) {
  ncrypto::Dsa dsa = key;

  return CryptoKey::AsymmetricKeyDetails{
    .modulusLength = static_cast<uint32_t>(dsa.getModulusLength()),
    .divisorLength = static_cast<uint32_t>(dsa.getDivisorLength()),
  };
}

CryptoKey::AsymmetricKeyDetails getEcKeyDetails(const ncrypto::EVPKeyPointer& key) {
  ncrypto::Ec ec = key;

  return CryptoKey::AsymmetricKeyDetails{
    .namedCurve = kj::str(OBJ_nid2sn(EC_GROUP_get_curve_name(ec.getGroup()))),
  };
}

kj::Maybe<ncrypto::EVPKeyPointer::PKFormatType> trySelectKeyFormat(kj::StringPtr format) {
  if (format == "pem"_kj) return ncrypto::EVPKeyPointer::PKFormatType::PEM;
  if (format == "der"_kj) return ncrypto::EVPKeyPointer::PKFormatType::DER;
  if (format == "jwk"_kj) return ncrypto::EVPKeyPointer::PKFormatType::JWK;
  return kj::none;
}

kj::Maybe<ncrypto::EVPKeyPointer::PKEncodingType> trySelectKeyEncoding(kj::StringPtr enc) {
  if (enc == "pkcs1"_kj) return ncrypto::EVPKeyPointer::PKEncodingType::PKCS1;
  if (enc == "pkcs8"_kj) return ncrypto::EVPKeyPointer::PKEncodingType::PKCS8;
  if (enc == "sec1"_kj) return ncrypto::EVPKeyPointer::PKEncodingType::SEC1;
  if (enc == "spki"_kj) return ncrypto::EVPKeyPointer::PKEncodingType::SPKI;
  return kj::none;
}

class AsymmetricKey final: public CryptoKey::Impl {
 public:
  static kj::Own<AsymmetricKey> NewPrivate(ncrypto::EVPKeyPointer&& key) {
    return kj::heap<AsymmetricKey>(kj::mv(key), true);
  }

  static kj::Own<AsymmetricKey> NewPublic(ncrypto::EVPKeyPointer&& key) {
    return kj::heap<AsymmetricKey>(kj::mv(key), false);
  }

  AsymmetricKey(ncrypto::EVPKeyPointer&& key, bool isPrivate)
      : CryptoKey::Impl(true, CryptoKeyUsageSet::privateKeyMask()),
        key(kj::mv(key)),
        isPrivate(isPrivate) {}

  kj::StringPtr getAlgorithmName() const override {
    if (!key) return nullptr;
    switch (key.id()) {
      case EVP_PKEY_RSA:
        return "rsa"_kj;
      case EVP_PKEY_RSA2:
        return "rsa"_kj;
      case EVP_PKEY_RSA_PSS:
        return "rsa"_kj;
      case EVP_PKEY_EC:
        return "ec"_kj;
      case EVP_PKEY_ED25519:
        return "ed25519"_kj;
      case EVP_PKEY_ED448:
        return "ed448"_kj;
      case EVP_PKEY_X25519:
        return "x25519"_kj;
      case EVP_PKEY_DSA:
        return "dsa"_kj;
      case EVP_PKEY_DH:
        return "dh"_kj;
#ifndef NCRYPTO_NO_KDF_H
      case EVP_PKEY_HKDF:
        return "hkdf"_kj;
#endif
      default:
        return nullptr;
    }
    KJ_UNREACHABLE;
  }

  CryptoKey::AlgorithmVariant getAlgorithm(jsg::Lock& js) const override {
    CryptoKey::ArbitraryKeyAlgorithm alg;
    if (key) [[likely]] {
      switch (key.id()) {
        case EVP_PKEY_RSA:
          alg.name = "RSASSA-PKCS1-v1_5"_kj;
          break;
        case EVP_PKEY_RSA2:
          alg.name = "RSASSA-PKCS1-v1_5"_kj;
          break;
        case EVP_PKEY_RSA_PSS:
          alg.name = "RSA-PSS"_kj;
          break;
        case EVP_PKEY_EC:
          alg.name = "ECDSA"_kj;
          break;
        case EVP_PKEY_ED25519:
          alg.name = "Ed25519"_kj;
          break;
        case EVP_PKEY_ED448:
          alg.name = "Ed448"_kj;
          break;
        case EVP_PKEY_X25519:
          alg.name = "X25519"_kj;
          break;
        case EVP_PKEY_DSA:
          alg.name = "NODE-DSA"_kj;
          break;
        case EVP_PKEY_DH:
          alg.name = "NODE-DH"_kj;
          break;
#ifndef NCRYPTO_NO_KDF_H
        case EVP_PKEY_HKDF:
          alg.name = "NODE-HKDF"_kj;
          break;
#endif
      }
    }
    return alg;
  }

  CryptoKey::AsymmetricKeyDetails getAsymmetricKeyDetail() const override {
    if (!key) [[unlikely]]
      return {};

    if (key.isRsaVariant()) {
      return getRsaKeyDetails(key);
    }

    if (key.id() == EVP_PKEY_DSA) {
      return getDsaKeyDetails(key);
    }

    if (key.id() == EVP_PKEY_EC) {
      return getEcKeyDetails(key);
    }

    return {};
  }

  jsg::BufferSource exportKeyExt(jsg::Lock& js,
      kj::StringPtr format,
      kj::StringPtr type,
      jsg::Optional<kj::String> cipher = kj::none,
      jsg::Optional<kj::Array<kj::byte>> passphrase = kj::none) const override {
    if (!key) {
      return JSG_REQUIRE_NONNULL(jsg::BufferSource::tryAlloc(js, 0), Error, "Failed to export key");
    }

    auto formatType = JSG_REQUIRE_NONNULL(trySelectKeyFormat(format), Error, "Invalid key format");
    auto encType = JSG_REQUIRE_NONNULL(trySelectKeyEncoding(type), Error, "Invalid key encoding");

    if (!key.isRsaVariant()) {
      JSG_REQUIRE(encType != ncrypto::EVPKeyPointer::PKEncodingType::PKCS1, Error,
          "PKCS1 can only be used for RSA keys");
    }

    if (encType == ncrypto::EVPKeyPointer::PKEncodingType::SEC1) {
      JSG_REQUIRE(key.id() == EVP_PKEY_EC, Error, "SEC1 can only be used for EC keys");
    }

    // This branch should never be taken for JWK
    KJ_ASSERT(formatType != ncrypto::EVPKeyPointer::PKFormatType::JWK);

    auto maybeBio = ([&] {
      if (isPrivate) {
        return key.writePrivateKey(
            ncrypto::EVPKeyPointer::PrivateKeyEncodingConfig(false, formatType, encType));
      }
      return key.writePublicKey(
          ncrypto::EVPKeyPointer::PublicKeyEncodingConfig(false, formatType, encType));
    })();
    if (maybeBio.has_value) {
      BUF_MEM* mem = maybeBio.value;
      kj::ArrayPtr<kj::byte> source(reinterpret_cast<kj::byte*>(mem->data), mem->length);
      if (source.size() > 0) {
        auto backing = jsg::BackingStore::alloc(js, source.size());
        backing.asArrayPtr().copyFrom(source);
        return jsg::BufferSource(js, kj::mv(backing));
      } else {
        return JSG_REQUIRE_NONNULL(
            jsg::BufferSource::tryAlloc(js, 0), Error, "Failed to export key");
      }
    }

    JSG_FAIL_REQUIRE(Error, "Failed to export key");
  }

  SubtleCrypto::ExportKeyData exportKey(jsg::Lock& js, kj::StringPtr format) const override final {
    if (format == "jwk") {
      auto res = toJwk(key, isPrivate ? KeyType::PRIVATE : KeyType::PUBLIC);
      JSG_REQUIRE(res.kty != "INVALID"_kj, Error, "Key type is invalid for JWK export");
      return kj::mv(res);
    }

    return exportKeyExt(js, format, "pkcs8"_kj);
  }

  bool equals(const CryptoKey::Impl& other) const override final {
    KJ_IF_SOME(o, kj::dynamicDowncastIfAvailable<const AsymmetricKey>(other)) {
      return EVP_PKEY_cmp(key.get(), o.key.get());
    }
    // TODO(later): Currently, this only compares keys using the ncrypto::EVPKeyPointer.
    // If the "other" impl happens to be from the web crypto impl that does not use
    // this AsymmetricKey impl then the comparison will be false. We can support both
    // cases but for now, skip it.
    return false;
  }

  kj::StringPtr getType() const override {
    return isPrivate ? "private"_kj : "public"_kj;
  }

  kj::Own<AsymmetricKey> cloneAsPublicKey() {
    if (!key) return kj::Own<AsymmetricKey>();
    auto cloned = key.clone();
    if (!cloned) return kj::Own<AsymmetricKey>();
    return NewPublic(kj::mv(cloned));
  }

  operator const ncrypto::EVPKeyPointer&() const {
    return key;
  }

 private:
  ncrypto::EVPKeyPointer key;
  bool isPrivate;
};

int getCurveFromName(kj::StringPtr name) {
  int nid = EC_curve_nist2nid(name.begin());
  if (nid == NID_undef) nid = OBJ_sn2nid(name.begin());
  return nid;
}
}  // namespace

kj::OneOf<kj::String, jsg::BufferSource, SubtleCrypto::JsonWebKey> CryptoImpl::exportKey(
    jsg::Lock& js, jsg::Ref<CryptoKey> key, jsg::Optional<KeyExportOptions> options) {
  JSG_REQUIRE(key->getExtractable(), TypeError, "Unable to export non-extractable key");
  auto& opts = JSG_REQUIRE_NONNULL(options, TypeError, "Options must be an object");

  kj::StringPtr format = JSG_REQUIRE_NONNULL(opts.format, TypeError, "Missing format option");
  if (format == "jwk"_kj) {
    // When format is jwk, all other options are ignored.
    return key->impl->exportKey(js, format);
  }

  if (key->getType() == "secret"_kj) {
    // For secret keys, we only pay attention to the format option, which will be
    // one of either "buffer" or "jwk". The "buffer" option correlates to the "raw"
    // format in Web Crypto. The "jwk" option is handled above.
    JSG_REQUIRE(format == "buffer"_kj, TypeError, "Invalid format for secret key export: ", format);
    return key->impl->exportKey(js, "raw"_kj);
  }

  kj::StringPtr type = JSG_REQUIRE_NONNULL(opts.type, TypeError, "Missing type option");
  auto data =
      key->impl->exportKeyExt(js, format, type, kj::mv(opts.cipher), kj::mv(opts.passphrase));
  if (format == "pem"_kj) {
    // TODO(perf): As a later performance optimization, change this so that it doesn't copy.
    return kj::str(data.asArrayPtr().asChars());
  }
  return kj::mv(data);
}

bool CryptoImpl::equals(jsg::Lock& js, jsg::Ref<CryptoKey> key, jsg::Ref<CryptoKey> otherKey) {
  return *key == *otherKey;
}

CryptoKey::AsymmetricKeyDetails CryptoImpl::getAsymmetricKeyDetail(
    jsg::Lock& js, jsg::Ref<CryptoKey> key) {
  JSG_REQUIRE(key->getType() != "secret"_kj, Error, "Secret keys do not have asymmetric details");
  return key->getAsymmetricKeyDetails();
}

kj::StringPtr CryptoImpl::getAsymmetricKeyType(jsg::Lock& js, jsg::Ref<CryptoKey> key) {
  static const std::map<kj::StringPtr, kj::StringPtr> mapping{
    {"RSASSA-PKCS1-v1_5", "rsa"},
    {"RSA-PSS", "rsa"},
    {"RSA-OAEP", "rsa"},
    {"ECDSA", "ec"},
    {"Ed25519", "ed25519"},
    {"NODE-ED25519", "ed25519"},
    {"ECDH", "ecdh"},
    {"X25519", "x25519"},
  };
  JSG_REQUIRE(
      key->getType() != "secret"_kj, TypeError, "Secret key does not have an asymmetric type");
  auto name = key->getAlgorithmName();
  auto found = mapping.find(name);
  return found != mapping.end() ? found->second : name;
}

jsg::Ref<CryptoKey> CryptoImpl::createSecretKey(jsg::Lock& js, jsg::BufferSource keyData) {
  // The keyData we receive here should be an exclusive copy of the key data.
  // It will have been copied on the JS side before being passed to this function.
  // We do not detach the key data, however, because we want to ensure that it
  // remains associated with the isolate for memory accounting purposes.
  return jsg::alloc<CryptoKey>(kj::heap<SecretKey>(kj::mv(keyData)));
}

namespace {
std::optional<ncrypto::EVPKeyPointer> tryParsingPrivate(
    const CryptoImpl::CreateAsymmetricKeyOptions& options, const jsg::BufferSource& buffer) {
  // As a private key the format can be either 'pem' or 'der',
  // while type can be one of `pkcs1`, `pkcs8`, or `sec1`.
  // The type is only required when format is 'der'.

  auto format =
      trySelectKeyFormat(options.format).orDefault(ncrypto::EVPKeyPointer::PKFormatType::PEM);

  auto enc = ncrypto::EVPKeyPointer::PKEncodingType::PKCS8;
  KJ_IF_SOME(type, options.type) {
    enc = trySelectKeyEncoding(type).orDefault(enc);
  }

  ncrypto::EVPKeyPointer::PrivateKeyEncodingConfig config(false, format, enc);

  KJ_IF_SOME(passphrase, options.passphrase) {
    // TODO(later): Avoid using DataPointer for passphrase... so we
    // can avoid the copy...
    auto dp = ncrypto::DataPointer::Alloc(passphrase.size());
    kj::ArrayPtr<kj::byte> ptr(dp.get<kj::byte>(), dp.size());
    ptr.copyFrom(passphrase.asArrayPtr());
    config.passphrase = kj::mv(dp);
  }

  auto result =
      ncrypto::EVPKeyPointer::TryParsePrivateKey(config, ToNcryptoBuffer(buffer.asArrayPtr()));

  if (result.has_value) return kj::mv(result.value);
  return std::nullopt;
}
}  // namespace

jsg::Ref<CryptoKey> CryptoImpl::createPrivateKey(
    jsg::Lock& js, CreateAsymmetricKeyOptions options) {
  ncrypto::ClearErrorOnReturn clearErrorOnReturn;

  // Unlike with Web Crypto, where the CryptoKey being created is always
  // algorithm specific, here we will create a generic private key impl
  // that can be used for multiple kinds of operations.

  KJ_SWITCH_ONEOF(options.key) {
    KJ_CASE_ONEOF(buffer, jsg::BufferSource) {
      JSG_REQUIRE(options.format == "pem"_kj || options.format == "der"_kj, TypeError,
          "Invalid format for private key creation");

      if (auto maybePrivate = tryParsingPrivate(options, buffer)) {
        return jsg::alloc<CryptoKey>(AsymmetricKey::NewPrivate(kj::mv(maybePrivate.value())));
      }

      JSG_FAIL_REQUIRE(Error, "Failed to parse private key");
    }
    KJ_CASE_ONEOF(jwk, SubtleCrypto::JsonWebKey) {
      JSG_REQUIRE(options.format == "jwk"_kj, TypeError, "Invalid format for JWK key creation");

      if (auto key = fromJwk(jwk, KeyType::PRIVATE)) {
        return jsg::alloc<CryptoKey>(AsymmetricKey::NewPrivate(kj::mv(key)));
      }

      JSG_FAIL_REQUIRE(Error, "JWK private key import is not implemented for this key type");
    }
    KJ_CASE_ONEOF(key, jsg::Ref<api::CryptoKey>) {
      // This path shouldn't be reachable.
      JSG_FAIL_REQUIRE(TypeError, "Invalid key data");
    }
  }

  KJ_UNREACHABLE;
}

jsg::Ref<CryptoKey> CryptoImpl::createPublicKey(jsg::Lock& js, CreateAsymmetricKeyOptions options) {
  ncrypto::ClearErrorOnReturn clearErrorOnReturn;

  KJ_SWITCH_ONEOF(options.key) {
    KJ_CASE_ONEOF(buffer, jsg::BufferSource) {
      JSG_REQUIRE(options.format == "pem"_kj || options.format == "der"_kj, TypeError,
          "Invalid format for public key creation");

      // As a public key the format can be either 'pem' or 'der',
      // while type can be one of either `pkcs1` or `spki`

      {
        // It is necessary to pop the error on return before we attempt
        // to try parsing as a private key if the public key parsing fails.
        ncrypto::MarkPopErrorOnReturn markPopErrorOnReturn;

        auto format =
            trySelectKeyFormat(options.format).orDefault(ncrypto::EVPKeyPointer::PKFormatType::PEM);

        auto enc = ncrypto::EVPKeyPointer::PKEncodingType::PKCS1;
        KJ_IF_SOME(type, options.type) {
          enc = trySelectKeyEncoding(type).orDefault(enc);
        }

        ncrypto::EVPKeyPointer::PublicKeyEncodingConfig config(true, format, enc);

        auto result = ncrypto::EVPKeyPointer::TryParsePublicKey(
            config, ToNcryptoBuffer(buffer.asArrayPtr().asConst()));

        if (result.has_value) {
          return jsg::alloc<CryptoKey>(AsymmetricKey::NewPublic(kj::mv(result.value)));
        }
      }

      // Otherwise, let's try parsing as a private key...
      if (auto maybePrivate = tryParsingPrivate(options, buffer)) {
        return jsg::alloc<CryptoKey>(AsymmetricKey::NewPublic(kj::mv(maybePrivate.value())));
      }

      JSG_FAIL_REQUIRE(Error, "Failed to parse public key");
    }
    KJ_CASE_ONEOF(jwk, SubtleCrypto::JsonWebKey) {
      JSG_REQUIRE(options.format == "jwk"_kj, TypeError, "Invalid format for JWK key creation");

      if (auto key = fromJwk(jwk, KeyType::PUBLIC)) {
        return jsg::alloc<CryptoKey>(AsymmetricKey::NewPublic(kj::mv(key)));
      }

      JSG_FAIL_REQUIRE(Error, "JWK public key import is not implemented for this key type");
    }
    KJ_CASE_ONEOF(key, jsg::Ref<api::CryptoKey>) {
      JSG_REQUIRE(key->getType() == "private"_kj, TypeError,
          "Cannot create public key from secret or public key");

      // TODO(later): For now, this only works with crypto keys that are created using
      // AsymmetricKey above. Web crypto private keys won't work here.
      KJ_IF_SOME(impl, kj::dynamicDowncastIfAvailable<AsymmetricKey>(*key->impl.get())) {
        return jsg::alloc<CryptoKey>(impl.cloneAsPublicKey());
      }

      JSG_FAIL_REQUIRE(Error, "Failed to derive public key from private key");
    }
  }

  KJ_UNREACHABLE;
}

CryptoKeyPair CryptoImpl::generateRsaKeyPair(RsaKeyPairOptions options) {
  ncrypto::ClearErrorOnReturn clearErrorOnReturn;

  auto ctx = ncrypto::EVPKeyCtxPointer::NewFromID(
      options.type == "rsa-pss" ? EVP_PKEY_RSA_PSS : EVP_PKEY_RSA);

  JSG_REQUIRE(ctx, Error, "Failed to create keygen context");
  JSG_REQUIRE(ctx.initForKeygen(), Error, "Failed to initialize keygen context");
  JSG_REQUIRE(ctx.setRsaKeygenBits(options.modulusLength), Error, "Failed to set modulus length");

  if (options.publicExponent != ncrypto::EVPKeyCtxPointer::kDefaultRsaExponent) {
    auto bn = ncrypto::BignumPointer::New();
    JSG_REQUIRE(bn, Error, "Failed to initialize public exponent");
    JSG_REQUIRE(bn.setWord(options.publicExponent) && ctx.setRsaKeygenPubExp(kj::mv(bn)), Error,
        "Failed to set public exponent");
  }

  // TODO(later): BoringSSL does not support generating RSA-PSS this
  // way... later see if there's an alternative approach.
  // if (options.type == "rsa-pss") {

  //   KJ_IF_SOME(hash, options.hashAlgorithm) {
  //     std::string_view hashName(hash.begin(), hash.size());
  //     auto nid = ncrypto::getDigestByName(hashName);
  //     JSG_REQUIRE(nid != nullptr, Error, "Unsupported hash algorithm");
  //     JSG_REQUIRE(ctx.setRsaPssKeygenMd(nid), Error, "Failed to set hash algorithm");
  //   }

  //   KJ_IF_SOME(hash, options.mgf1HashAlgorithm) {
  //     std::string_view mgf1hashName(hash.begin(), hash.size());
  //     auto mgf1_nid = ncrypto::getDigestByName(mgf1hashName);
  //     if (mgf1_nid == nullptr) {
  //       KJ_IF_SOME(hash, options.hashAlgorithm) {
  //         std::string_view hashName(hash.begin(), hash.size());
  //         mgf1_nid = ncrypto::getDigestByName(hashName);
  //       }
  //     }
  //     if (mgf1_nid != nullptr) {
  //       JSG_REQUIRE(ctx.setRsaPssKeygenMgf1Md(mgf1_nid), Error,
  //                   "Failed to set MGF1 hash algorithm");
  //     }
  //   }

  //   KJ_IF_SOME(len, options.saltLength) {
  //     JSG_REQUIRE(ctx.setRsaPssSaltlen(len), Error, "Failed to set salt length");
  //   }
  // }

  // Generate the key
  EVP_PKEY* pkey = nullptr;
  JSG_REQUIRE(EVP_PKEY_keygen(ctx.get(), &pkey), Error, "Failed to generate key");

  auto generated = ncrypto::EVPKeyPointer(pkey);

  auto publicKey = AsymmetricKey::NewPublic(generated.clone());
  JSG_REQUIRE(publicKey, Error, "Failed to create public key");
  auto privateKey = AsymmetricKey::NewPrivate(kj::mv(generated));
  JSG_REQUIRE(privateKey, Error, "Failed to create private key");

  return CryptoKeyPair{
    .publicKey = jsg::alloc<CryptoKey>(kj::mv(publicKey)),
    .privateKey = jsg::alloc<CryptoKey>(kj::mv(privateKey)),
  };
}

CryptoKeyPair CryptoImpl::generateDsaKeyPair(DsaKeyPairOptions options) {
  // TODO(later): BoringSSL does not implement DSA key generation using
  // EVP_PKEY_keygen. We would need to implement this using the DSA-specific
  // APIs which get a bit complicated when it comes to using a user-provided
  // modulus length and divisor length. For now, leave this un-implemented.

  // auto ctx = ncrypto::EVPKeyCtxPointer::NewFromID(EVP_PKEY_DSA);

  // JSG_REQUIRE(ctx, Error, "Failed to create keygen context");
  // JSG_REQUIRE(ctx.initForKeygen(), Error, "Failed to initialize keygen context");

  // uint32_t bits = options.modulusLength;
  // std::optional<uint32_t> q_bits = std::nullopt;
  // KJ_IF_SOME(d, options.divisorLength) {
  //   q_bits = d;
  // }

  // JSG_REQUIRE(ctx.setDsaParameters(bits, q_bits), Error, "Failed to set DSA parameters");

  // // Generate the key
  // EVP_PKEY* pkey = nullptr;
  // JSG_REQUIRE(EVP_PKEY_keygen(ctx.get(), &pkey), Error, "Failed to generate key");

  // auto generated = ncrypto::EVPKeyPointer(pkey);

  // auto publicKey = AsymmetricKey::NewPublic(generated.clone());
  // JSG_REQUIRE(publicKey, Error, "Failed to create public key");
  // auto privateKey = AsymmetricKey::NewPrivate(kj::mv(generated));
  // JSG_REQUIRE(privateKey, Error, "Failed to create private key");

  // return CryptoKeyPair {
  //   .publicKey = jsg::alloc<CryptoKey>(kj::mv(publicKey)),
  //   .privateKey = jsg::alloc<CryptoKey>(kj::mv(privateKey)),
  // };

  JSG_FAIL_REQUIRE(Error, "Not yet implemented");
}

CryptoKeyPair CryptoImpl::generateEcKeyPair(EcKeyPairOptions options) {
  ncrypto::ClearErrorOnReturn clearErrorOnReturn;

  auto nid = getCurveFromName(options.namedCurve);
  JSG_REQUIRE(nid != NID_undef, Error, "Invalid or unsupported curve");

  auto paramEncoding =
      options.paramEncoding == "named"_kj ? OPENSSL_EC_NAMED_CURVE : OPENSSL_EC_EXPLICIT_CURVE;

  auto ecPrivateKey = ncrypto::ECKeyPointer::NewByCurveName(nid);
  JSG_REQUIRE(ecPrivateKey, Error, "Failed to initialize key");
  JSG_REQUIRE(ecPrivateKey.generate(), Error, "Failed to generate private key");

  EC_KEY_set_enc_flags(ecPrivateKey, paramEncoding);

  auto ecPublicKey = ncrypto::ECKeyPointer::NewByCurveName(nid);
  JSG_REQUIRE(EC_KEY_set_public_key(ecPublicKey, EC_KEY_get0_public_key(ecPrivateKey)), Error,
      "Failed to derive public key");

  auto privateKey = ncrypto::EVPKeyPointer::New();
  JSG_REQUIRE(privateKey.assign(ecPrivateKey), Error, "Failed to assign private key");

  auto publicKey = ncrypto::EVPKeyPointer::New();
  JSG_REQUIRE(publicKey.assign(ecPublicKey), Error, "Failed to assign public key");

  ecPrivateKey.release();
  ecPublicKey.release();

  auto pubKey = AsymmetricKey::NewPublic(kj::mv(publicKey));
  JSG_REQUIRE(pubKey, Error, "Failed to create public key");
  auto pvtKey = AsymmetricKey::NewPrivate(kj::mv(privateKey));
  JSG_REQUIRE(pvtKey, Error, "Failed to create private key");

  return CryptoKeyPair{
    .publicKey = jsg::alloc<CryptoKey>(kj::mv(pubKey)),
    .privateKey = jsg::alloc<CryptoKey>(kj::mv(pvtKey)),
  };
}

CryptoKeyPair CryptoImpl::generateEdKeyPair(EdKeyPairOptions options) {
  ncrypto::ClearErrorOnReturn clearErrorOnReturn;

  auto nid = ([&] {
    if (options.type == "ed25519") {
      return EVP_PKEY_ED25519;
    }
    if (options.type == "x25519") {
      return EVP_PKEY_X25519;
    }
    return NID_undef;
  })();
  JSG_REQUIRE(nid != NID_undef, Error, "Invalid or unsupported curve");

  auto ctx = ncrypto::EVPKeyCtxPointer::NewFromID(nid);
  JSG_REQUIRE(ctx, Error, "Failed to create keygen context");
  JSG_REQUIRE(ctx.initForKeygen(), Error, "Failed to initialize keygen");

  // Generate the key
  EVP_PKEY* pkey = nullptr;
  JSG_REQUIRE(EVP_PKEY_keygen(ctx.get(), &pkey), Error, "Failed to generate key");

  auto generated = ncrypto::EVPKeyPointer(pkey);

  auto publicKey = AsymmetricKey::NewPublic(generated.clone());
  JSG_REQUIRE(publicKey, Error, "Failed to create public key");
  auto privateKey = AsymmetricKey::NewPrivate(kj::mv(generated));
  JSG_REQUIRE(privateKey, Error, "Failed to create private key");

  return CryptoKeyPair{
    .publicKey = jsg::alloc<CryptoKey>(kj::mv(publicKey)),
    .privateKey = jsg::alloc<CryptoKey>(kj::mv(privateKey)),
  };
}

CryptoKeyPair CryptoImpl::generateDhKeyPair(DhKeyPairOptions options) {

  // TODO(soon): Older versions of boringssl+fips do not support EVP with
  // DH key pairs that are required to make the following work. A compile
  // flag is used to disable the mechanism in ncrypto, causing the calls
  // to `ncrypto::EVPKeyPointer::NewDH to return an empty EVPKeyPointer.
  // While the ideal situation would be for us to adopt a newer version
  // of boringssl+fips that *does* support EVP+DH, we can possibly work
  // around the issue by implementing an alternative that uses the older
  // DH_* specific APIs like the rest of our DH implementation does.

  ncrypto::ClearErrorOnReturn clearErrorOnReturn;

  static constexpr uint32_t kStandardizedGenerator = 2;

  ncrypto::EVPKeyPointer key_params;
  auto generator = options.generator.orDefault(kStandardizedGenerator);

  KJ_SWITCH_ONEOF(options.primeOrGroup) {
    KJ_CASE_ONEOF(group, kj::String) {
      std::string_view group_name(group.begin(), group.size());
      auto found = ncrypto::DHPointer::FindGroup(group_name);
      JSG_REQUIRE(found, Error, "Invalid or unsupported group");

      auto bn_g = ncrypto::BignumPointer::New();
      JSG_REQUIRE(bn_g && bn_g.setWord(generator), Error, "Failed to set generator");

      auto dh = ncrypto::DHPointer::New(kj::mv(found), kj::mv(bn_g));
      JSG_REQUIRE(dh, Error, "Failed to create DH key");

      key_params = ncrypto::EVPKeyPointer::NewDH(kj::mv(dh));
    }
    KJ_CASE_ONEOF(prime, jsg::BufferSource) {
      ncrypto::BignumPointer bn(prime.asArrayPtr().begin(), prime.size());

      auto bn_g = ncrypto::BignumPointer::New();
      JSG_REQUIRE(bn_g && bn_g.setWord(generator), Error, "Failed to set generator");

      auto dh = ncrypto::DHPointer::New(kj::mv(bn), kj::mv(bn_g));
      JSG_REQUIRE(dh, Error, "Failed to create DH key");

      key_params = ncrypto::EVPKeyPointer::NewDH(kj::mv(dh));
    }
    KJ_CASE_ONEOF(length, uint32_t) {
      // TODO(later): BoringSSL appears to not implement DH key generation
      // from a prime length the same way Node.js does. For now, defer this
      // and come back to implement later.
      JSG_FAIL_REQUIRE(Error, "Generating DH keys from a prime length is not yet implemented");
    }
  }

  JSG_REQUIRE(key_params, Error, "Failed to create keygen context");
  auto ctx = key_params.newCtx();
  JSG_REQUIRE(ctx, Error, "Failed to create keygen context");
  JSG_REQUIRE(ctx.initForKeygen(), Error, "Failed to initialize keygen context");

  // Generate the key
  EVP_PKEY* pkey = nullptr;
  JSG_REQUIRE(EVP_PKEY_keygen(ctx.get(), &pkey), Error, "Failed to generate key");

  auto generated = ncrypto::EVPKeyPointer(pkey);

  auto publicKey = AsymmetricKey::NewPublic(generated.clone());
  JSG_REQUIRE(publicKey, Error, "Failed to create public key");
  auto privateKey = AsymmetricKey::NewPrivate(kj::mv(generated));
  JSG_REQUIRE(privateKey, Error, "Failed to create private key");

  return CryptoKeyPair{
    .publicKey = jsg::alloc<CryptoKey>(kj::mv(publicKey)),
    .privateKey = jsg::alloc<CryptoKey>(kj::mv(privateKey)),
  };
}

jsg::BufferSource CryptoImpl::statelessDH(
    jsg::Lock& js, jsg::Ref<CryptoKey> privateKey, jsg::Ref<CryptoKey> publicKey) {
  auto privateKeyAlg = privateKey->getAlgorithmName();
  auto publicKeyAlg = publicKey->getAlgorithmName();
  KJ_ASSERT(privateKeyAlg == "dh"_kj || privateKeyAlg == "ec"_kj || privateKeyAlg == "x25519"_kj,
      "Invalid private key algorithm");
  KJ_ASSERT(publicKeyAlg == "dh"_kj || publicKeyAlg == "ec"_kj || publicKeyAlg == "x25519"_kj,
      "Invalid public key algorithm");
  KJ_ASSERT(privateKeyAlg == publicKeyAlg, "Mismatched public and private key types");
  KJ_IF_SOME(pubKey, kj::dynamicDowncastIfAvailable<AsymmetricKey>(*publicKey->impl)) {
    KJ_IF_SOME(pvtKey, kj::dynamicDowncastIfAvailable<AsymmetricKey>(*privateKey->impl)) {
      auto data = ncrypto::DHPointer::stateless(pubKey, pvtKey);
      JSG_REQUIRE(data, Error, "Failed to derive shared diffie-hellman secret");
      auto backing = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, data.size());
      kj::ArrayPtr<kj::byte> ptr(static_cast<kj::byte*>(data.get()), data.size());
      backing.asArrayPtr().copyFrom(ptr);
      return jsg::BufferSource(js, kj::mv(backing));
    }
  }
  JSG_FAIL_REQUIRE(Error, "Unsupported keys for stateless diffie-hellman");
}

kj::Maybe<const ncrypto::EVPKeyPointer&> CryptoImpl::tryGetKey(jsg::Ref<CryptoKey>& key) {
  KJ_IF_SOME(key, kj::dynamicDowncastIfAvailable<AsymmetricKey>(*key->impl)) {
    const ncrypto::EVPKeyPointer& evp = key;
    return evp;
  }
  return kj::none;
}

kj::Maybe<kj::ArrayPtr<const kj::byte>> CryptoImpl::tryGetSecretKeyData(jsg::Ref<CryptoKey>& key) {
  KJ_IF_SOME(secret, kj::dynamicDowncastIfAvailable<SecretKey>(*key->impl)) {
    return secret.rawKeyData();
  }
  return kj::none;
}

}  // namespace workerd::api::node
