// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "crypto.h"
#include "util.h"

#include <workerd/api/crypto/impl.h>

#include <ncrypto.h>
#include <openssl/crypto.h>

#include <map>

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
      case EVP_PKEY_HKDF:
        return "hkdf"_kj;
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
        case EVP_PKEY_HKDF:
          alg.name = "NODE-HKDF"_kj;
          break;
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

 private:
  ncrypto::EVPKeyPointer key;
  bool isPrivate;
};
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

  ncrypto::Buffer<const kj::byte> buf{
    .data = buffer.asArrayPtr().begin(),
    .len = buffer.size(),
  };

  auto result = ncrypto::EVPKeyPointer::TryParsePrivateKey(config, buf);

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
      JSG_FAIL_REQUIRE(Error, "JWK private key import is not yet implemented");
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

        ncrypto::Buffer<const kj::byte> buf{
          .data = buffer.asArrayPtr().begin(),
          .len = buffer.size(),
        };

        auto result = ncrypto::EVPKeyPointer::TryParsePublicKey(config, buf);

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
      JSG_FAIL_REQUIRE(Error, "JWK public key import is not yet implemented");
    }
    KJ_CASE_ONEOF(key, jsg::Ref<api::CryptoKey>) {
      JSG_FAIL_REQUIRE(Error, "Getting a public key from a private key is not yet implemented");
    }
  }

  KJ_UNREACHABLE;
}

}  // namespace workerd::api::node
