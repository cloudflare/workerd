// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "crypto-impl.h"
#include <openssl/hkdf.h>

namespace workerd::api {
namespace {

class HkdfKey final: public CryptoKey::Impl {
public:
  explicit HkdfKey(kj::Array<kj::byte> keyData, CryptoKey::KeyAlgorithm keyAlgorithm,
                   bool extractable, CryptoKeyUsageSet usages)
      : CryptoKey::Impl(extractable, usages),
        keyData(kj::mv(keyData)), keyAlgorithm(kj::mv(keyAlgorithm)) {}

private:
  kj::Array<kj::byte> deriveBits(
      SubtleCrypto::DeriveKeyAlgorithm&& algorithm, kj::Maybe<uint32_t> maybeLength) const override {
    kj::StringPtr hashName = api::getAlgorithmName(JSG_REQUIRE_NONNULL(algorithm.hash, TypeError,
        "Missing field \"hash\" in \"algorithm\"."));
    const EVP_MD* hashType = lookupDigestAlgorithm(hashName).second;

    const auto& salt = JSG_REQUIRE_NONNULL(algorithm.salt, TypeError,
        "Missing field \"salt\" in \"algorithm\".");
    const auto& info = JSG_REQUIRE_NONNULL(algorithm.info, TypeError,
        "Missing field \"info\" in \"algorithm\".");

    uint32_t length = JSG_REQUIRE_NONNULL(maybeLength, DOMOperationError,
        "HKDF cannot derive a key with null length.");

    JSG_REQUIRE(length != 0 && (length % 8) == 0, DOMOperationError,
        "HKDF requires a derived key length that is a non-zero multiple of eight (requested ",
        length, ").");

    auto derivedLengthBytes = length / 8;

    kj::Vector<kj::byte> result(derivedLengthBytes);
    result.resize(derivedLengthBytes);

    auto operationSucceed = HKDF(result.begin(), result.size(), hashType, keyData.begin(),
        keyData.size(), salt.begin(), salt.size(), info.begin(), info.size());

    if (operationSucceed != 1) {
      JSG_FAIL_REQUIRE(DOMOperationError, "HKDF deriveBits failed.");
    }

    return result.releaseAsArray();
  }

  kj::StringPtr getAlgorithmName() const override { return "HKDF"; }
  CryptoKey::AlgorithmVariant getAlgorithm() const override { return keyAlgorithm; }

  kj::Array<kj::byte> keyData;
  CryptoKey::KeyAlgorithm keyAlgorithm;
};

}  // namespace

kj::Own<CryptoKey::Impl> CryptoKey::Impl::importHkdf(
    kj::StringPtr normalizedName, kj::StringPtr format,
    SubtleCrypto::ImportKeyData keyData,
    SubtleCrypto::ImportKeyAlgorithm&& algorithm, bool extractable,
    kj::ArrayPtr<const kj::String> keyUsages) {
  auto usages =
      CryptoKeyUsageSet::validate(normalizedName, CryptoKeyUsageSet::Context::importSecret,
          keyUsages, CryptoKeyUsageSet::derivationKeyMask());

  JSG_REQUIRE(!extractable, DOMSyntaxError, "HKDF key cannot be extractable.");
  JSG_REQUIRE(format == "raw", DOMNotSupportedError, "HKDF key must be imported "
      "in \"raw\" format (requested \"", format, "\")");

  // NOTE: Checked in SubtleCrypto::importKey().
  auto keyDataArray = kj::mv(keyData.get<kj::Array<kj::byte>>());

  auto keyAlgorithm = CryptoKey::KeyAlgorithm{normalizedName};
  return kj::heap<HkdfKey>(kj::mv(keyDataArray), kj::mv(keyAlgorithm), extractable, usages);
}

}  // namespace workerd::api
