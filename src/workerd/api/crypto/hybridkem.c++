// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "impl.h"

#include <openssl/bytestring.h>
#include <openssl/mem.h>
#include <openssl/xwing.h>

#include <algorithm>

namespace workerd::api {
namespace {

const auto PUBLIC_USAGES =
    CryptoKeyUsageSet::encapsulateKey() | CryptoKeyUsageSet::encapsulateBits();
const auto PRIVATE_USAGES =
    CryptoKeyUsageSet::decapsulateKey() | CryptoKeyUsageSet::decapsulateBits();

class HybridKemKey final: public CryptoKey::Impl {
 public:
  HybridKemKey(kj::Array<kj::byte> seed,
      XWING_private_key privateKey,
      kj::Array<kj::byte> publicKey,
      bool extractable,
      CryptoKeyUsageSet usages)
      : CryptoKey::Impl(extractable, usages),
        keyType(KeyType::PRIVATE),
        seed(kj::mv(seed)),
        privateKey(kj::mv(privateKey)),
        publicKey(kj::mv(publicKey)) {}

  HybridKemKey(kj::Array<kj::byte> publicKey, bool extractable, CryptoKeyUsageSet usages)
      : CryptoKey::Impl(extractable, usages),
        keyType(KeyType::PUBLIC),
        publicKey(kj::mv(publicKey)) {}

  ~HybridKemKey() noexcept(false) {
    KJ_IF_SOME(s, seed) OPENSSL_cleanse(s.begin(), s.size());
    KJ_IF_SOME(k, privateKey) OPENSSL_cleanse(&k, sizeof(k));
  }

  std::pair<jsg::JsArrayBuffer, jsg::JsArrayBuffer> encapsulate(jsg::Lock& js) const override {
    JSG_REQUIRE(
        keyType == KeyType::PUBLIC, DOMInvalidAccessError, "Encapsulation requires a public key.");
    auto sharedSecret = jsg::JsArrayBuffer::create(js, XWING_SHARED_SECRET_BYTES);
    auto ciphertext = jsg::JsArrayBuffer::create(js, XWING_CIPHERTEXT_BYTES);
    JSG_REQUIRE(XWING_encap(ciphertext.asArrayPtr().begin(), sharedSecret.asArrayPtr().begin(),
                    publicKey.begin()) == 1,
        DOMOperationError, "MLKEM768-X25519 encapsulation failed.");
    return {kj::mv(sharedSecret), kj::mv(ciphertext)};
  }

  jsg::JsArrayBuffer decapsulate(
      jsg::Lock& js, kj::ArrayPtr<const kj::byte> ciphertext) const override {
    JSG_REQUIRE(keyType == KeyType::PRIVATE, DOMInvalidAccessError,
        "Decapsulation requires a private key.");
    JSG_REQUIRE(ciphertext.size() == XWING_CIPHERTEXT_BYTES, DOMOperationError,
        "Invalid ciphertext length for MLKEM768-X25519.");
    auto sharedSecret = jsg::JsArrayBuffer::create(js, XWING_SHARED_SECRET_BYTES);
    JSG_REQUIRE(XWING_decap(sharedSecret.asArrayPtr().begin(), ciphertext.begin(),
                    &KJ_ASSERT_NONNULL(privateKey)) == 1,
        DOMOperationError, "MLKEM768-X25519 decapsulation failed.");
    return sharedSecret;
  }

  kj::Own<CryptoKey::Impl> getPublicKey(jsg::Lock&, CryptoKeyUsageSet usages) const override {
    JSG_REQUIRE(
        keyType == KeyType::PRIVATE, DOMInvalidAccessError, "getPublicKey requires a private key.");
    return kj::heap<HybridKemKey>(kj::heapArray<kj::byte>(publicKey), true, usages);
  }

  SubtleCrypto::ExportKeyData exportKey(jsg::Lock& js, kj::StringPtr format) const override {
    if (format == "raw-public") {
      JSG_REQUIRE(keyType == KeyType::PUBLIC, DOMInvalidAccessError,
          "raw-public export requires a public key.");
      return jsg::JsArrayBuffer::create(js, publicKey).addRef(js);
    }
    if (format == "raw-seed") {
      JSG_REQUIRE(keyType == KeyType::PRIVATE, DOMInvalidAccessError,
          "raw-seed export requires a private key.");
      return jsg::JsArrayBuffer::create(js, KJ_ASSERT_NONNULL(seed)).addRef(js);
    }
    if (format == "jwk") {
      SubtleCrypto::JsonWebKey jwk;
      jwk.kty = kj::str("AKP");
      jwk.alg = kj::str("MLKEM768-X25519");
      jwk.pub = kj::encodeBase64Url(publicKey);
      jwk.ext = isExtractable();
      jwk.key_ops = getUsages().map([](auto usage) { return kj::str(usage.name()); });
      if (keyType == KeyType::PRIVATE) jwk.priv = kj::encodeBase64Url(KJ_ASSERT_NONNULL(seed));
      return jwk;
    }
    JSG_FAIL_REQUIRE(
        DOMNotSupportedError, "Unrecognized export format \"", format, "\" for MLKEM768-X25519.");
  }

  kj::StringPtr getAlgorithmName() const override {
    return "MLKEM768-X25519";
  }
  bool publicKeyEquals(kj::ArrayPtr<const kj::byte> value) const {
    return value.size() == publicKey.size() &&
        CRYPTO_memcmp(value.begin(), publicKey.begin(), value.size()) == 0;
  }
  CryptoKey::AlgorithmVariant getAlgorithm(jsg::Lock&) const override {
    return CryptoKey::KeyAlgorithm{"MLKEM768-X25519"};
  }
  kj::StringPtr getType() const override {
    return keyType == KeyType::PRIVATE ? "private" : "public";
  }
  bool equals(const Impl& other) const override {
    auto* key = dynamic_cast<const HybridKemKey*>(&other);
    return key != nullptr && keyType == key->keyType && publicKey.size() == key->publicKey.size() &&
        CRYPTO_memcmp(publicKey.begin(), key->publicKey.begin(), publicKey.size()) == 0;
  }
  kj::StringPtr jsgGetMemoryName() const override {
    return "HybridKemKey";
  }
  size_t jsgGetMemorySelfSize() const override {
    return sizeof(HybridKemKey);
  }
  void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const override {
    tracker.trackFieldWithSize("publicKey", publicKey.size());
    KJ_IF_SOME(s, seed) tracker.trackFieldWithSize("seed", s.size());
  }

  static kj::Own<HybridKemKey> fromSeed(
      kj::Array<kj::byte> seed, bool extractable, CryptoKeyUsageSet usages) {
    XWING_private_key privateKey;
    CBS cbs;
    CBS_init(&cbs, seed.begin(), seed.size());
    JSG_REQUIRE(XWING_parse_private_key(&privateKey, &cbs) == 1, DOMDataError,
        "Invalid MLKEM768-X25519 seed.");
    KJ_DEFER(OPENSSL_cleanse(&privateKey, sizeof(privateKey)));
    auto publicKey = kj::heapArray<kj::byte>(XWING_PUBLIC_KEY_BYTES);
    JSG_REQUIRE(XWING_public_from_private(publicKey.begin(), &privateKey) == 1,
        InternalDOMOperationError, "Failed to derive MLKEM768-X25519 public key.");
    return kj::heap<HybridKemKey>(
        kj::mv(seed), kj::mv(privateKey), kj::mv(publicKey), extractable, usages);
  }

  static CryptoKeyPair generateKeyPair(jsg::Lock& js,
      bool extractable,
      CryptoKeyUsageSet privateUsages,
      CryptoKeyUsageSet publicUsages) {
    XWING_private_key privateKey;
    auto publicKey = kj::heapArray<kj::byte>(XWING_PUBLIC_KEY_BYTES);
    JSG_REQUIRE(XWING_generate_key(publicKey.begin(), &privateKey) == 1, DOMOperationError,
        "MLKEM768-X25519 key generation failed.");
    KJ_DEFER(OPENSSL_cleanse(&privateKey, sizeof(privateKey)));
    bssl::ScopedCBB cbb;
    JSG_REQUIRE(CBB_init(cbb.get(), XWING_PRIVATE_KEY_BYTES) &&
            XWING_marshal_private_key(cbb.get(), &privateKey) && CBB_flush(cbb.get()),
        InternalDOMOperationError, "Failed to marshal MLKEM768-X25519 seed.");
    auto seed = kj::heapArray<kj::byte>(CBB_data(cbb.get()), CBB_len(cbb.get()));
    auto privateImpl = kj::heap<HybridKemKey>(
        kj::mv(seed), kj::mv(privateKey), kj::mv(publicKey), extractable, privateUsages);
    auto publicImpl = privateImpl->getPublicKey(js, publicUsages);
    return CryptoKeyPair{.publicKey = js.alloc<CryptoKey>(kj::mv(publicImpl)),
      .privateKey = js.alloc<CryptoKey>(kj::mv(privateImpl))};
  }

 private:
  enum class KeyType { PUBLIC, PRIVATE };
  KeyType keyType;
  kj::Maybe<kj::Array<kj::byte>> seed;
  kj::Maybe<XWING_private_key> privateKey;
  kj::Array<kj::byte> publicKey;
};

}  // namespace

kj::OneOf<jsg::Ref<CryptoKey>, CryptoKeyPair> CryptoKey::Impl::generateHybridKem(jsg::Lock& js,
    kj::StringPtr normalizedName,
    SubtleCrypto::GenerateKeyAlgorithm&&,
    bool extractable,
    kj::ArrayPtr<const kj::String> keyUsages) {
  auto usages = CryptoKeyUsageSet::validate(normalizedName, CryptoKeyUsageSet::Context::generate,
      keyUsages, PUBLIC_USAGES | PRIVATE_USAGES);
  return HybridKemKey::generateKeyPair(js, extractable,
      usages & CryptoKeyUsageSet::privateKeyMask(), usages & CryptoKeyUsageSet::publicKeyMask());
}

kj::Own<CryptoKey::Impl> CryptoKey::Impl::importHybridKem(jsg::Lock& js,
    kj::StringPtr normalizedName,
    kj::StringPtr format,
    SubtleCrypto::ImportKeyData keyData,
    SubtleCrypto::ImportKeyAlgorithm&&,
    bool extractable,
    kj::ArrayPtr<const kj::String> keyUsages) {
  if (format == "raw-seed") {
    auto usages = CryptoKeyUsageSet::validate(
        normalizedName, CryptoKeyUsageSet::Context::importPrivate, keyUsages, PRIVATE_USAGES);
    auto& source = JSG_REQUIRE_NONNULL(keyData.tryGet<jsg::JsRef<jsg::JsBufferSource>>(),
        DOMDataError, "Import data for raw-seed must be a buffer.");
    auto data = source.getHandle(js).asArrayPtr();
    JSG_REQUIRE(data.size() == XWING_PRIVATE_KEY_BYTES, DOMDataError,
        "MLKEM768-X25519 seed must be 32 bytes.");
    return HybridKemKey::fromSeed(kj::heapArray<kj::byte>(data), extractable, usages);
  }
  if (format == "jwk") {
    auto& jwk = JSG_REQUIRE_NONNULL(keyData.tryGet<SubtleCrypto::JsonWebKey>(), DOMDataError,
        "Import data for jwk must be a JsonWebKey.");
    auto usages = jwk.priv != kj::none
        ? CryptoKeyUsageSet::validate(
              normalizedName, CryptoKeyUsageSet::Context::importPrivate, keyUsages, PRIVATE_USAGES)
        : CryptoKeyUsageSet::validate(
              normalizedName, CryptoKeyUsageSet::Context::importPublic, keyUsages, PUBLIC_USAGES);
    JSG_REQUIRE(jwk.kty == "AKP", DOMDataError, "JWK kty must be AKP.");
    KJ_IF_SOME(alg, jwk.alg) {
      JSG_REQUIRE(
          alg == "MLKEM768-X25519", DOMDataError, "JWK alg does not match MLKEM768-X25519.");
    } else {
      JSG_FAIL_REQUIRE(DOMDataError, "JWK must have alg.");
    }
    KJ_IF_SOME(use, jwk.use) {
      JSG_REQUIRE(keyUsages.size() == 0 || use == "enc", DOMDataError,
          "MLKEM768-X25519 JWK use must be enc.");
    }
    KJ_IF_SOME(ops, jwk.key_ops) {
      std::sort(ops.begin(), ops.end());
      JSG_REQUIRE(std::adjacent_find(ops.begin(), ops.end()) == ops.end(), DOMDataError,
          "MLKEM768-X25519 JWK key_ops contains duplicates.");
      for (const auto& usage: keyUsages) {
        JSG_REQUIRE(std::binary_search(ops.begin(), ops.end(), usage), DOMDataError,
            "MLKEM768-X25519 JWK key_ops does not contain ", usage, ".");
      }
    }
    KJ_IF_SOME(ext, jwk.ext) {
      JSG_REQUIRE(ext || !extractable, DOMDataError,
          "MLKEM768-X25519 JWK ext is incompatible with extractability.");
    }
    KJ_IF_SOME(priv, jwk.priv) {
      auto decoded = decodeBase64Url(kj::mv(priv));
      JSG_REQUIRE(!decoded.hadErrors && decoded.size() == XWING_PRIVATE_KEY_BYTES, DOMDataError,
          "Invalid MLKEM768-X25519 JWK priv.");
      auto key = HybridKemKey::fromSeed(kj::heapArray<kj::byte>(decoded), extractable, usages);
      auto& pub = JSG_REQUIRE_NONNULL(jwk.pub, DOMDataError, "JWK private keys must have pub.");
      auto expected = decodeBase64Url(kj::mv(pub));
      JSG_REQUIRE(!expected.hadErrors && key->publicKeyEquals(expected), DOMDataError,
          "JWK pub does not match priv.");
      return kj::mv(key);
    }
    auto& pub = JSG_REQUIRE_NONNULL(jwk.pub, DOMDataError, "JWK must have pub.");
    auto decoded = decodeBase64Url(kj::mv(pub));
    JSG_REQUIRE(!decoded.hadErrors && decoded.size() == XWING_PUBLIC_KEY_BYTES, DOMDataError,
        "Invalid MLKEM768-X25519 JWK pub.");
    return kj::heap<HybridKemKey>(kj::heapArray<kj::byte>(decoded), extractable, usages);
  }
  if (format == "raw-public") {
    auto usages = CryptoKeyUsageSet::validate(
        normalizedName, CryptoKeyUsageSet::Context::importPublic, keyUsages, PUBLIC_USAGES);
    auto& source = JSG_REQUIRE_NONNULL(keyData.tryGet<jsg::JsRef<jsg::JsBufferSource>>(),
        DOMDataError, "Import data for raw-public must be a buffer.");
    auto data = source.getHandle(js).asArrayPtr();
    JSG_REQUIRE(data.size() == XWING_PUBLIC_KEY_BYTES, DOMDataError,
        "MLKEM768-X25519 public key must be 1216 bytes.");
    return kj::heap<HybridKemKey>(kj::heapArray<kj::byte>(data), extractable, usages);
  }
  JSG_FAIL_REQUIRE(
      DOMNotSupportedError, "Unsupported import format \"", format, "\" for MLKEM768-X25519.");
}

}  // namespace workerd::api