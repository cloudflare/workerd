// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "impl.h"

#include <workerd/io/io-context.h>

#include <openssl/aead.h>
#include <openssl/mem.h>

#include <algorithm>

namespace workerd::api {
namespace {

constexpr size_t KEY_BYTES = 32;
constexpr size_t IV_BYTES = 12;
constexpr size_t TAG_BYTES = 16;

const auto USAGES = CryptoKeyUsageSet::encrypt() | CryptoKeyUsageSet::decrypt() |
    CryptoKeyUsageSet::wrapKey() | CryptoKeyUsageSet::unwrapKey();

class Chacha20Poly1305Key final: public CryptoKey::Impl {
 public:
  Chacha20Poly1305Key(kj::Array<kj::byte> keyData, bool extractable, CryptoKeyUsageSet usages)
      : CryptoKey::Impl(extractable, usages),
        keyData(kj::mv(keyData)) {}

  ~Chacha20Poly1305Key() noexcept(false) {
    OPENSSL_cleanse(keyData.begin(), keyData.size());
  }

  jsg::JsArrayBuffer encrypt(jsg::Lock& js,
      SubtleCrypto::EncryptAlgorithm&& algorithm,
      kj::ArrayPtr<const kj::byte> plaintext) const override {
    auto iv = JSG_REQUIRE_NONNULL(algorithm.iv, TypeError, "Missing field \"iv\" in \"algorithm\".")
                  .getHandle(js);
    JSG_REQUIRE(
        iv.size() == IV_BYTES, DOMOperationError, "ChaCha20-Poly1305 IV must be 12 bytes long.");
    validateChacha20Poly1305TagLength(algorithm.tagLength.orDefault(128));

    auto ctx = kj::disposeWith<EVP_AEAD_CTX_free>(
        EVP_AEAD_CTX_new(EVP_aead_chacha20_poly1305(), keyData.begin(), keyData.size(), TAG_BYTES));
    JSG_REQUIRE(ctx.get() != nullptr, DOMOperationError, "Failed to initialize ChaCha20-Poly1305.");
    kj::ArrayPtr<const kj::byte> aad;
    KJ_IF_SOME(additionalData, algorithm.additionalData) {
      aad = additionalData.getHandle(js).asArrayPtr();
    }
    auto result = jsg::JsArrayBuffer::create(js, plaintext.size() + TAG_BYTES);
    size_t written = 0;
    JSG_REQUIRE(EVP_AEAD_CTX_seal(ctx.get(), result.asArrayPtr().begin(), &written, result.size(),
                    iv.asArrayPtr().begin(), iv.size(), plaintext.begin(), plaintext.size(),
                    aad.begin(), aad.size()) == 1,
        DOMOperationError, "ChaCha20-Poly1305 encryption failed.");
    KJ_ASSERT(written == result.size());
    return result;
  }

  jsg::JsArrayBuffer decrypt(jsg::Lock& js,
      SubtleCrypto::EncryptAlgorithm&& algorithm,
      kj::ArrayPtr<const kj::byte> ciphertext) const override {
    auto iv = JSG_REQUIRE_NONNULL(algorithm.iv, TypeError, "Missing field \"iv\" in \"algorithm\".")
                  .getHandle(js);
    JSG_REQUIRE(
        iv.size() == IV_BYTES, DOMOperationError, "ChaCha20-Poly1305 IV must be 12 bytes long.");
    validateChacha20Poly1305TagLength(algorithm.tagLength.orDefault(128));
    JSG_REQUIRE(ciphertext.size() >= TAG_BYTES, DOMOperationError,
        "ChaCha20-Poly1305 ciphertext is shorter than its authentication tag.");

    auto ctx = kj::disposeWith<EVP_AEAD_CTX_free>(
        EVP_AEAD_CTX_new(EVP_aead_chacha20_poly1305(), keyData.begin(), keyData.size(), TAG_BYTES));
    JSG_REQUIRE(ctx.get() != nullptr, DOMOperationError, "Failed to initialize ChaCha20-Poly1305.");
    kj::ArrayPtr<const kj::byte> aad;
    KJ_IF_SOME(additionalData, algorithm.additionalData) {
      aad = additionalData.getHandle(js).asArrayPtr();
    }
    auto result = jsg::JsArrayBuffer::create(js, ciphertext.size() - TAG_BYTES);
    size_t written = 0;
    JSG_REQUIRE(EVP_AEAD_CTX_open(ctx.get(), result.asArrayPtr().begin(), &written, result.size(),
                    iv.asArrayPtr().begin(), iv.size(), ciphertext.begin(), ciphertext.size(),
                    aad.begin(), aad.size()) == 1,
        DOMOperationError, "ChaCha20-Poly1305 authentication failed.");
    KJ_ASSERT(written == result.size());
    return result;
  }

  SubtleCrypto::ExportKeyData exportKey(jsg::Lock& js, kj::StringPtr format) const override {
    if (format == "raw-secret") {
      return jsg::JsArrayBuffer::create(js, keyData).addRef(js);
    }
    if (format == "jwk") {
      SubtleCrypto::JsonWebKey jwk;
      jwk.kty = kj::str("oct");
      jwk.k = fastEncodeBase64Url(keyData);
      jwk.alg = kj::str("C20P");
      jwk.key_ops = getUsages().map([](auto usage) { return kj::str(usage.name()); });
      jwk.ext = isExtractable();
      return jwk;
    }
    JSG_FAIL_REQUIRE(DOMNotSupportedError,
        "ChaCha20-Poly1305 key only supports exporting \"raw-secret\" and \"jwk\", not \"", format,
        "\".");
  }

  kj::StringPtr getAlgorithmName() const override {
    return "ChaCha20-Poly1305";
  }
  CryptoKey::AlgorithmVariant getAlgorithm(jsg::Lock&) const override {
    return CryptoKey::KeyAlgorithm{"ChaCha20-Poly1305"};
  }
  bool equals(const Impl& other) const override {
    auto* key = dynamic_cast<const Chacha20Poly1305Key*>(&other);
    return key != nullptr && CRYPTO_memcmp(keyData.begin(), key->keyData.begin(), KEY_BYTES) == 0;
  }
  bool equals(const kj::Array<kj::byte>& other) const override {
    return other.size() == KEY_BYTES &&
        CRYPTO_memcmp(keyData.begin(), other.begin(), KEY_BYTES) == 0;
  }
  kj::StringPtr jsgGetMemoryName() const override {
    return "Chacha20Poly1305Key";
  }
  size_t jsgGetMemorySelfSize() const override {
    return sizeof(Chacha20Poly1305Key);
  }
  void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const override {
    tracker.trackFieldWithSize("keyData", keyData.size());
  }

 private:
  kj::Array<kj::byte> keyData;
};

void validateJwkUsages(SubtleCrypto::JsonWebKey& jwk, kj::ArrayPtr<const kj::String> keyUsages) {
  KJ_IF_SOME(use, jwk.use) {
    JSG_REQUIRE(keyUsages.size() == 0 || use == "enc", DOMDataError,
        "ChaCha20-Poly1305 JWK must have a use of \"enc\".");
  }
  KJ_IF_SOME(ops, jwk.key_ops) {
    std::sort(ops.begin(), ops.end());
    JSG_REQUIRE(std::adjacent_find(ops.begin(), ops.end()) == ops.end(), DOMDataError,
        "ChaCha20-Poly1305 JWK key_ops contains duplicates.");
    for (const auto& usage: keyUsages) {
      JSG_REQUIRE(std::binary_search(ops.begin(), ops.end(), usage), DOMDataError,
          "ChaCha20-Poly1305 JWK key_ops does not contain ", usage, ".");
    }
  }
}

}  // namespace

kj::OneOf<jsg::Ref<CryptoKey>, CryptoKeyPair> CryptoKey::Impl::generateChacha20Poly1305(
    jsg::Lock& js,
    kj::StringPtr normalizedName,
    SubtleCrypto::GenerateKeyAlgorithm&&,
    bool extractable,
    kj::ArrayPtr<const kj::String> keyUsages) {
  auto usages = CryptoKeyUsageSet::validate(
      normalizedName, CryptoKeyUsageSet::Context::generate, keyUsages, USAGES);
  auto keyData = kj::heapArray<kj::byte>(KEY_BYTES);
  IoContext::current().getEntropySource().generate(keyData);
  return js.alloc<CryptoKey>(kj::heap<Chacha20Poly1305Key>(kj::mv(keyData), extractable, usages));
}

kj::Own<CryptoKey::Impl> CryptoKey::Impl::importChacha20Poly1305(jsg::Lock& js,
    kj::StringPtr normalizedName,
    kj::StringPtr format,
    SubtleCrypto::ImportKeyData keyData,
    SubtleCrypto::ImportKeyAlgorithm&&,
    bool extractable,
    kj::ArrayPtr<const kj::String> keyUsages) {
  auto usages = CryptoKeyUsageSet::validate(
      normalizedName, CryptoKeyUsageSet::Context::importSecret, keyUsages, USAGES);
  kj::Array<kj::byte> bytes;
  if (format == "raw-secret") {
    auto& source = keyData.get<jsg::JsRef<jsg::JsBufferSource>>();
    auto handle = source.getHandle(js);
    JSG_REQUIRE(
        handle.size() == KEY_BYTES, DOMDataError, "ChaCha20-Poly1305 key must be 256 bits.");
    bytes = handle.copy();
  } else if (format == "jwk") {
    auto& jwk = keyData.get<SubtleCrypto::JsonWebKey>();
    JSG_REQUIRE(jwk.kty == "oct", DOMDataError, "ChaCha20-Poly1305 JWK kty must be oct.");
    KJ_IF_SOME(alg, jwk.alg) {
      JSG_REQUIRE(alg == "C20P", DOMDataError, "ChaCha20-Poly1305 JWK alg must be C20P.");
    }
    validateJwkUsages(jwk, keyUsages);
    KJ_IF_SOME(ext, jwk.ext) {
      JSG_REQUIRE(ext || !extractable, DOMDataError,
          "ChaCha20-Poly1305 JWK ext is incompatible with extractability.");
    }
    bytes = UNWRAP_JWK_BIGNUM(
        kj::mv(jwk.k), DOMDataError, "ChaCha20-Poly1305 JWK requires a base64url key.");
    JSG_REQUIRE(
        bytes.size() == KEY_BYTES, DOMDataError, "ChaCha20-Poly1305 JWK key must be 256 bits.");
  } else {
    JSG_FAIL_REQUIRE(DOMNotSupportedError, "Unrecognized key import format \"", format, "\".");
  }
  return kj::heap<Chacha20Poly1305Key>(kj::mv(bytes), extractable, usages);
}

}  // namespace workerd::api
