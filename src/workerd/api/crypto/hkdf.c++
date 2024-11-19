// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "impl.h"
#include "kdf.h"

#include <openssl/crypto.h>
#include <openssl/hkdf.h>

namespace workerd::api {
namespace {

class HkdfKey final: public CryptoKey::Impl {
 public:
  explicit HkdfKey(kj::Array<kj::byte> keyData,
      CryptoKey::KeyAlgorithm keyAlgorithm,
      bool extractable,
      CryptoKeyUsageSet usages)
      : CryptoKey::Impl(extractable, usages),
        keyData(kj::mv(keyData)),
        keyAlgorithm(kj::mv(keyAlgorithm)) {}

  kj::StringPtr jsgGetMemoryName() const override {
    return "HkdfKey";
  }
  size_t jsgGetMemorySelfSize() const override {
    return sizeof(HkdfKey);
  }
  void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const override {
    tracker.trackFieldWithSize("keyData", keyData.size());
    tracker.trackField("keyAlgorithm", keyAlgorithm);
  }

 private:
  jsg::BufferSource deriveBits(jsg::Lock& js,
      SubtleCrypto::DeriveKeyAlgorithm&& algorithm,
      kj::Maybe<uint32_t> maybeLength) const override {
    kj::StringPtr hashName = api::getAlgorithmName(
        JSG_REQUIRE_NONNULL(algorithm.hash, TypeError, "Missing field \"hash\" in \"algorithm\"."));
    const EVP_MD* hashType = lookupDigestAlgorithm(hashName).second;

    const auto& salt =
        JSG_REQUIRE_NONNULL(algorithm.salt, TypeError, "Missing field \"salt\" in \"algorithm\".");
    const auto& info =
        JSG_REQUIRE_NONNULL(algorithm.info, TypeError, "Missing field \"info\" in \"algorithm\".");

    uint32_t length = JSG_REQUIRE_NONNULL(
        maybeLength, DOMOperationError, "HKDF cannot derive a key with null length.");

    JSG_REQUIRE(length != 0 && (length % 8) == 0, DOMOperationError,
        "HKDF requires a derived key length that is a non-zero multiple of eight (requested ",
        length, ").");

    auto derivedLengthBytes = length / 8;

    return JSG_REQUIRE_NONNULL(hkdf(js, derivedLengthBytes, hashType, keyData, salt, info),
        DOMOperationError, "HKDF deriveBits failed.");
  }

  kj::StringPtr getAlgorithmName() const override {
    return "HKDF";
  }
  CryptoKey::AlgorithmVariant getAlgorithm(jsg::Lock& js) const override {
    return keyAlgorithm;
  }

  bool equals(const CryptoKey::Impl& other) const override final {
    return this == &other || (other.getType() == "secret"_kj && other.equals(keyData));
  }

  bool equals(const kj::Array<kj::byte>& other) const override final {
    return keyData.size() == other.size() &&
        CRYPTO_memcmp(keyData.begin(), other.begin(), keyData.size()) == 0;
  }

  ZeroOnFree keyData;
  CryptoKey::KeyAlgorithm keyAlgorithm;
};

}  // namespace

kj::Maybe<jsg::BufferSource> hkdf(jsg::Lock& js,
    size_t length,
    const EVP_MD* digest,
    kj::ArrayPtr<const kj::byte> key,
    kj::ArrayPtr<const kj::byte> salt,
    kj::ArrayPtr<const kj::byte> info) {
  auto buf = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, length);
  if (HKDF(buf.asArrayPtr().begin(), length, digest, key.begin(), key.size(), salt.begin(),
          salt.size(), info.begin(), info.size()) != 1) {
    return kj::none;
  }
  return jsg::BufferSource(js, kj::mv(buf));
}

kj::Own<CryptoKey::Impl> CryptoKey::Impl::importHkdf(jsg::Lock& js,
    kj::StringPtr normalizedName,
    kj::StringPtr format,
    SubtleCrypto::ImportKeyData keyData,
    SubtleCrypto::ImportKeyAlgorithm&& algorithm,
    bool extractable,
    kj::ArrayPtr<const kj::String> keyUsages) {
  auto usages = CryptoKeyUsageSet::validate(normalizedName,
      CryptoKeyUsageSet::Context::importSecret, keyUsages, CryptoKeyUsageSet::derivationKeyMask());

  JSG_REQUIRE(!extractable, DOMSyntaxError, "HKDF key cannot be extractable.");
  JSG_REQUIRE(format == "raw", DOMNotSupportedError,
      "HKDF key must be imported "
      "in \"raw\" format (requested \"",
      format, "\")");

  // NOTE: Checked in SubtleCrypto::importKey().
  auto keyDataArray = kj::mv(keyData.get<kj::Array<kj::byte>>());

  auto keyAlgorithm = CryptoKey::KeyAlgorithm{normalizedName};
  return kj::heap<HkdfKey>(kj::mv(keyDataArray), kj::mv(keyAlgorithm), extractable, usages);
}

}  // namespace workerd::api
