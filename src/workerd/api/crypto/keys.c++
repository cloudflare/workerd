#include "keys.h"
#include <openssl/crypto.h>

namespace workerd::api {

SecretKey::SecretKey(kj::Array<kj::byte> keyData)
    : Impl(true, CryptoKeyUsageSet::privateKeyMask() |
                  CryptoKeyUsageSet::publicKeyMask()),
      keyData(kj::mv(keyData)) {}

kj::StringPtr SecretKey::getAlgorithmName() const {
  return "secret"_kj;
}

CryptoKey::AlgorithmVariant SecretKey::getAlgorithm(jsg::Lock& js) const {
  return CryptoKey::KeyAlgorithm { .name = "secret"_kj };
}

bool SecretKey::equals(const CryptoKey::Impl& other) const {
  return this == &other || (other.getType() == "secret"_kj && other.equals(keyData));
}

bool SecretKey::equals(const kj::Array<kj::byte>& other) const {
  return keyData.size() == other.size() &&
          CRYPTO_memcmp(keyData.begin(), other.begin(), keyData.size()) == 0;
}

SubtleCrypto::ExportKeyData SecretKey::exportKey(kj::StringPtr format) const {
  JSG_REQUIRE(format == "raw" || format == "jwk", DOMNotSupportedError,
      getAlgorithmName(), " key only supports exporting \"raw\" & \"jwk\", not \"", format,
      "\".");

  if (format == "jwk") {
    SubtleCrypto::JsonWebKey jwk;
    jwk.kty = kj::str("oct");
    jwk.k = kj::encodeBase64Url(keyData);
    jwk.ext = true;
    return jwk;
  }

  return kj::heapArray(keyData.asPtr());
}

void SecretKey::jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const {
  tracker.trackFieldWithSize("keyData", keyData.size());
}

}  // namespace workerd::api
