// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "impl.h"

#include <workerd/api/crypto/kdf.h>

#include <openssl/evp.h>
#include <openssl/mem.h>

namespace workerd::api {
namespace {

class Pbkdf2Key final: public CryptoKey::Impl {
 public:
  explicit Pbkdf2Key(kj::Array<kj::byte> keyData,
      CryptoKey::KeyAlgorithm keyAlgorithm,
      bool extractable,
      CryptoKeyUsageSet usages)
      : CryptoKey::Impl(extractable, usages),
        keyData(kj::mv(keyData)),
        keyAlgorithm(kj::mv(keyAlgorithm)) {}

  kj::StringPtr jsgGetMemoryName() const override {
    return "Pbkdf2Key";
  }
  size_t jsgGetMemorySelfSize() const override {
    return sizeof(Pbkdf2Key);
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
    auto hashType = lookupDigestAlgorithm(hashName).second;
    kj::ArrayPtr<kj::byte> salt =
        JSG_REQUIRE_NONNULL(algorithm.salt, TypeError, "Missing field \"salt\" in \"algorithm\".");
    int iterations = JSG_REQUIRE_NONNULL(
        algorithm.iterations, TypeError, "Missing field \"iterations\" in \"algorithm\".");

    uint32_t length = JSG_REQUIRE_NONNULL(
        maybeLength, DOMOperationError, "PBKDF2 cannot derive a key with null length.");

    JSG_REQUIRE(length != 0 && (length & 0b111) == 0, DOMOperationError,
        "PBKDF2 requires a derived key length that is a non-zero multiple of eight (requested ",
        length, ").");

    JSG_REQUIRE(iterations > 0, DOMOperationError,
        "PBKDF2 requires a positive iteration count (requested ", iterations, ").");

    // Note: The user could DoS us by selecting a very high iteration count. Our dead man's switch
    // would kick in, resulting in a process restart. We guard against this by limiting the
    // maximum iteration count a user can select -- this is an intentional non-conformity.
    // Another approach might be to fork OpenSSL's PKCS5_PBKDF2_HMAC() function and insert a
    // check for v8::Isolate::IsExecutionTerminating() in the loop, but for now a hard cap seems
    // wisest.
    checkPbkdfLimits(js, iterations);

    return JSG_REQUIRE_NONNULL(pbkdf2(js, length / 8, iterations, hashType, keyData, salt), Error,
        "PBKDF2 deriveBits failed.");
  }

  // TODO(bug): Possibly by mistake, PBKDF2 was historically not on the allow list of
  //   algorithms in exportKey(). Later, the allow list was removed, instead assuming that any
  //   alogorithm which implemented this method must be allowed. To maintain exactly the
  //   preexisting behavior, then, this implementation had to be commented out. If disallowing this
  //   was a mistake, we can un-comment this method, but we would need to make sure to add tests
  //   when we do.
  //   SubtleCrypto::ExportKeyData exportKey(kj::StringPtr format) const override {
  //     JSG_REQUIRE(format == "raw", DOMNotSupportedError,
  //         "Unimplemented key export format \"", format, "\".");
  //     return kj::heapArray(keyData.asPtr());
  //   }

  kj::StringPtr getAlgorithmName() const override {
    return "PBKDF2";
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

kj::Maybe<jsg::BufferSource> pbkdf2(jsg::Lock& js,
    size_t length,
    size_t iterations,
    const EVP_MD* digest,
    kj::ArrayPtr<const kj::byte> password,
    kj::ArrayPtr<const kj::byte> salt) {
  auto buf = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, length);
  if (PKCS5_PBKDF2_HMAC(password.asChars().begin(), password.size(), salt.begin(), salt.size(),
          iterations, digest, length, buf.asArrayPtr().begin()) != 1) {
    return kj::none;
  }
  return jsg::BufferSource(js, kj::mv(buf));
}

kj::Own<CryptoKey::Impl> CryptoKey::Impl::importPbkdf2(jsg::Lock& js,
    kj::StringPtr normalizedName,
    kj::StringPtr format,
    SubtleCrypto::ImportKeyData keyData,
    SubtleCrypto::ImportKeyAlgorithm&& algorithm,
    bool extractable,
    kj::ArrayPtr<const kj::String> keyUsages) {
  auto usages = CryptoKeyUsageSet::validate(normalizedName,
      CryptoKeyUsageSet::Context::importSecret, keyUsages, CryptoKeyUsageSet::derivationKeyMask());

  JSG_REQUIRE(!extractable, DOMSyntaxError, "PBKDF2 key cannot be extractable.");
  JSG_REQUIRE(format == "raw", DOMNotSupportedError,
      "PBKDF2 key must be imported in \"raw\" format (requested \"", format, "\").");

  // NOTE: Checked in SubtleCrypto::importKey().
  auto keyDataArray = kj::mv(keyData.get<kj::Array<kj::byte>>());

  auto keyAlgorithm = CryptoKey::KeyAlgorithm{normalizedName};
  return kj::heap<Pbkdf2Key>(kj::mv(keyDataArray), kj::mv(keyAlgorithm), extractable, usages);
}

}  // namespace workerd::api
