#include "impl.h"

#include <workerd/api/util.h>
#include <workerd/io/io-context.h>
#include <workerd/jsg/jsvalue.h>

#include <openssl/aead.h>
#include <openssl/mem.h>

namespace workerd::api {
namespace {

// ChaCha20-Poly1305 constants
constexpr size_t CHACHA20_POLY1305_KEY_SIZE = 32;    // 256 bits
constexpr size_t CHACHA20_POLY1305_NONCE_SIZE = 12;  // 96 bits
constexpr size_t CHACHA20_POLY1305_TAG_SIZE = 16;    // 128 bits

class ChaCha20Poly1305Key final: public CryptoKey::Impl {
 public:
  explicit ChaCha20Poly1305Key(kj::Array<kj::byte> keyData,
      kj::StringPtr algorithmName,
      bool extractable,
      CryptoKeyUsageSet usages)
      : CryptoKey::Impl(extractable, usages),
        keyData(kj::mv(keyData)),
        algorithmName(algorithmName) {}

 private:
  kj::StringPtr getAlgorithmName() const override {
    return algorithmName;
  }

  CryptoKey::AlgorithmVariant getAlgorithm(jsg::Lock& js) const override {
    return CryptoKey::KeyAlgorithm{algorithmName};
  }

  bool equals(const CryptoKey::Impl& other) const override {
    return this == &other || (other.getType() == "secret"_kj && other.equals(keyData));
  }

  bool equals(const kj::Array<kj::byte>& other) const override {
    return keyData.size() == other.size() &&
        CRYPTO_memcmp(keyData.begin(), other.begin(), keyData.size()) == 0;
  }

  SubtleCrypto::ExportKeyData exportKey(jsg::Lock& js, kj::StringPtr format) const override {
    JSG_REQUIRE(format == "raw-secret" || format == "jwk", DOMNotSupportedError,
        "ChaCha20-Poly1305 key only supports exporting \"raw-secret\" & \"jwk\", not \"", format,
        "\".");

    if (format == "jwk") {
      SubtleCrypto::JsonWebKey jwk;
      jwk.kty = kj::str("oct");
      jwk.k = fastEncodeBase64Url(keyData);
      jwk.alg = kj::str("C20P");
      jwk.key_ops = getUsages().map([](auto usage) { return kj::str(usage.name()); });
      jwk.ext = true;
      return jwk;
    }

    return jsg::JsArrayBuffer::create(js, keyData).addRef(js);
  }

  jsg::JsArrayBuffer encrypt(jsg::Lock& js,
      SubtleCrypto::EncryptAlgorithm&& algorithm,
      kj::ArrayPtr<const kj::byte> plainText) const override {
    auto iv = JSG_REQUIRE_NONNULL(algorithm.iv, TypeError, "Missing field \"iv\" in \"algorithm\".")
                  .getHandle(js);
    JSG_REQUIRE(iv.size() == CHACHA20_POLY1305_NONCE_SIZE, DOMOperationError,
        "ChaCha20-Poly1305 IV must be 12 bytes (provided ", iv.size(), ").");

    KJ_IF_SOME(tagLength, algorithm.tagLength) {
      JSG_REQUIRE(tagLength == 128, DOMOperationError,
          "ChaCha20-Poly1305 tag length must be 128 (provided ", tagLength, ").");
    }

    kj::ArrayPtr<const kj::byte> additionalData = nullptr;
    KJ_IF_SOME(sourceRef, algorithm.additionalData) {
      auto source = sourceRef.getHandle(js);
      additionalData = source.asArrayPtr();
    }

    auto aeadCtx = kj::disposeWith<EVP_AEAD_CTX_free>(EVP_AEAD_CTX_new(
        EVP_aead_chacha20_poly1305(), keyData.begin(), keyData.size(), CHACHA20_POLY1305_TAG_SIZE));
    KJ_ASSERT(aeadCtx.get() != nullptr);

    auto maxOutLen = plainText.size() + CHACHA20_POLY1305_TAG_SIZE;
    auto cipherText = jsg::JsArrayBuffer::create(js, maxOutLen);

    size_t outLen = 0;
    JSG_REQUIRE(EVP_AEAD_CTX_seal(aeadCtx.get(), cipherText.asArrayPtr().begin(), &outLen,
                    maxOutLen, iv.asArrayPtr().begin(), iv.size(), plainText.begin(),
                    plainText.size(), additionalData.begin(), additionalData.size()),
        DOMOperationError, "ChaCha20-Poly1305 encryption failed", internalDescribeOpensslErrors());
    KJ_ASSERT(outLen == maxOutLen);

    return cipherText;
  }

  jsg::JsArrayBuffer decrypt(jsg::Lock& js,
      SubtleCrypto::EncryptAlgorithm&& algorithm,
      kj::ArrayPtr<const kj::byte> cipherText) const override {
    auto iv = JSG_REQUIRE_NONNULL(algorithm.iv, TypeError, "Missing field \"iv\" in \"algorithm\".")
                  .getHandle(js);
    JSG_REQUIRE(iv.size() == CHACHA20_POLY1305_NONCE_SIZE, DOMOperationError,
        "ChaCha20-Poly1305 IV must be 12 bytes (provided ", iv.size(), ").");

    KJ_IF_SOME(tagLength, algorithm.tagLength) {
      JSG_REQUIRE(tagLength == 128, DOMOperationError,
          "ChaCha20-Poly1305 tag length must be 128 (provided ", tagLength, ").");
    }

    JSG_REQUIRE(cipherText.size() >= CHACHA20_POLY1305_TAG_SIZE, DOMOperationError,
        "Ciphertext is too short to contain a valid ChaCha20-Poly1305 authentication tag.");

    kj::ArrayPtr<const kj::byte> additionalData = nullptr;
    KJ_IF_SOME(sourceRef, algorithm.additionalData) {
      auto source = sourceRef.getHandle(js);
      additionalData = source.asArrayPtr();
    }

    auto aeadCtx = kj::disposeWith<EVP_AEAD_CTX_free>(EVP_AEAD_CTX_new(
        EVP_aead_chacha20_poly1305(), keyData.begin(), keyData.size(), CHACHA20_POLY1305_TAG_SIZE));
    KJ_ASSERT(aeadCtx.get() != nullptr);

    auto maxOutLen = cipherText.size();
    auto plainText = jsg::JsArrayBuffer::create(js, maxOutLen);

    size_t outLen = 0;
    JSG_REQUIRE(EVP_AEAD_CTX_open(aeadCtx.get(), plainText.asArrayPtr().begin(), &outLen, maxOutLen,
                    iv.asArrayPtr().begin(), iv.size(), cipherText.begin(), cipherText.size(),
                    additionalData.begin(), additionalData.size()),
        DOMOperationError, "ChaCha20-Poly1305 decryption failed.");

    return jsg::JsArrayBuffer::create(js, plainText.asArrayPtr().first(outLen));
  }

  kj::StringPtr jsgGetMemoryName() const override {
    return "ChaCha20Poly1305Key";
  }
  size_t jsgGetMemorySelfSize() const override {
    return sizeof(ChaCha20Poly1305Key);
  }
  void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const override {
    tracker.trackFieldWithSize("keyData", keyData.size());
  }

  ZeroOnFree keyData;
  kj::StringPtr algorithmName;
};

}  // namespace

kj::OneOf<jsg::Ref<CryptoKey>, CryptoKeyPair> CryptoKey::Impl::generateChaCha20Poly1305(
    jsg::Lock& js,
    kj::StringPtr normalizedName,
    SubtleCrypto::GenerateKeyAlgorithm&& algorithm,
    bool extractable,
    kj::ArrayPtr<const kj::String> keyUsages) {
  CryptoKeyUsageSet validUsages = CryptoKeyUsageSet::encrypt() | CryptoKeyUsageSet::decrypt() |
      CryptoKeyUsageSet::wrapKey() | CryptoKeyUsageSet::unwrapKey();
  auto usages = CryptoKeyUsageSet::validate(
      normalizedName, CryptoKeyUsageSet::Context::generate, keyUsages, validUsages);

  auto keyDataArray = kj::heapArray<kj::byte>(CHACHA20_POLY1305_KEY_SIZE);
  IoContext::current().getEntropySource().generate(keyDataArray);

  return js.alloc<CryptoKey>(
      kj::heap<ChaCha20Poly1305Key>(kj::mv(keyDataArray), normalizedName, extractable, usages));
}

kj::Own<CryptoKey::Impl> CryptoKey::Impl::importChaCha20Poly1305(jsg::Lock& js,
    kj::StringPtr normalizedName,
    kj::StringPtr format,
    SubtleCrypto::ImportKeyData keyData,
    SubtleCrypto::ImportKeyAlgorithm&& algorithm,
    bool extractable,
    kj::ArrayPtr<const kj::String> keyUsages) {
  CryptoKeyUsageSet validUsages = CryptoKeyUsageSet::encrypt() | CryptoKeyUsageSet::decrypt() |
      CryptoKeyUsageSet::wrapKey() | CryptoKeyUsageSet::unwrapKey();
  auto usages = CryptoKeyUsageSet::validate(
      normalizedName, CryptoKeyUsageSet::Context::importSecret, keyUsages, validUsages);

  kj::Array<kj::byte> keyDataArray;

  if (format == "raw-secret") {
    keyDataArray = kj::mv(keyData.get<kj::Array<kj::byte>>());
    JSG_REQUIRE(keyDataArray.size() == CHACHA20_POLY1305_KEY_SIZE, DOMDataError,
        "ChaCha20-Poly1305 key must be 256 bits (provided ", keyDataArray.size() * 8, ").");
  } else if (format == "jwk") {
    auto& keyDataJwk = keyData.get<SubtleCrypto::JsonWebKey>();
    JSG_REQUIRE(keyDataJwk.kty == "oct", DOMDataError,
        "Symmetric \"jwk\" key import requires a JSON Web Key with Key Type parameter "
        "\"kty\" equal to \"oct\" (encountered \"",
        keyDataJwk.kty, "\").");

    keyDataArray = UNWRAP_JWK_BIGNUM(kj::mv(keyDataJwk.k), DOMDataError,
        "Symmetric \"jwk\" key import requires a base64Url encoding of the key.");

    JSG_REQUIRE(keyDataArray.size() == CHACHA20_POLY1305_KEY_SIZE, DOMDataError,
        "ChaCha20-Poly1305 key must be 256 bits (provided ", keyDataArray.size() * 8, ").");

    KJ_IF_SOME(alg, keyDataJwk.alg) {
      JSG_REQUIRE(alg == "C20P", DOMDataError,
          "Symmetric \"jwk\" key contains invalid \"alg\" value \"", alg, "\", expected \"C20P\".");
    }

    if (keyUsages.size() != 0) {
      KJ_IF_SOME(u, keyDataJwk.use) {
        JSG_REQUIRE(u == "enc", DOMDataError,
            "Symmetric \"jwk\" key must have a \"use\" of \"enc\", not \"", u, "\".");
      }
    }

    KJ_IF_SOME(ops, keyDataJwk.key_ops) {
      std::sort(ops.begin(), ops.end());
      auto duplicate = std::adjacent_find(ops.begin(), ops.end());
      JSG_REQUIRE(duplicate == ops.end(), DOMDataError,
          "Symmetric \"jwk\" key contains duplicate value \"", *duplicate, "\", in \"key_op\".");

      for (const auto& usage: keyUsages) {
        JSG_REQUIRE(std::binary_search(ops.begin(), ops.end(), usage), DOMDataError,
            "\"jwk\" key missing usage \"", usage, "\", in \"key_ops\".");
      }
    }

    KJ_IF_SOME(e, keyDataJwk.ext) {
      JSG_REQUIRE(e || !extractable, DOMDataError, "\"jwk\" key has value \"", e ? "true" : "false",
          "\", for \"ext\" that is incompatible "
          "with import extractability value \"",
          extractable ? "true" : "false", "\".");
    }
  } else {
    JSG_FAIL_REQUIRE(DOMNotSupportedError, "Unrecognized key import format \"", format, "\".");
  }

  return kj::heap<ChaCha20Poly1305Key>(kj::mv(keyDataArray), normalizedName, extractable, usages);
}

}  // namespace workerd::api
