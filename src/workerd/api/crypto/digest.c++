// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "digest.h"

#include "impl.h"
#include "util.h"

#include <workerd/api/crypto/crypto.h>
#include <workerd/io/io-context.h>

#include <openssl/hmac.h>
#include <openssl/mem.h>

namespace workerd::api {
namespace {

class HmacKey final: public CryptoKey::Impl {
public:
  explicit HmacKey(kj::Array<kj::byte> keyData,
      CryptoKey::HmacKeyAlgorithm keyAlgorithm,
      bool extractable,
      CryptoKeyUsageSet usages)
      : CryptoKey::Impl(extractable, usages),
        keyData(kj::mv(keyData)),
        keyAlgorithm(kj::mv(keyAlgorithm)) {}

  kj::StringPtr jsgGetMemoryName() const override {
    return "HmacKey";
  }
  size_t jsgGetMemorySelfSize() const override {
    return sizeof(HmacKey);
  }
  void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const override {
    tracker.trackFieldWithSize("keyData", keyData.size());
    tracker.trackField("keyAlgorithm", keyAlgorithm);
  }

private:
  kj::Array<kj::byte> sign(
      SubtleCrypto::SignAlgorithm&& algorithm, kj::ArrayPtr<const kj::byte> data) const override {
    return computeHmac(kj::mv(algorithm), data);
  }

  bool verify(SubtleCrypto::SignAlgorithm&& algorithm,
      kj::ArrayPtr<const kj::byte> signature,
      kj::ArrayPtr<const kj::byte> data) const override {
    auto messageDigest = computeHmac(kj::mv(algorithm), data);
    return messageDigest.size() == signature.size() &&
        CRYPTO_memcmp(messageDigest.begin(), signature.begin(), signature.size()) == 0;
  }

  kj::Array<kj::byte> computeHmac(
      SubtleCrypto::SignAlgorithm&& algorithm, kj::ArrayPtr<const kj::byte> data) const {
    // For HMAC, the hash is specified when creating the key, not at call time.
    auto type = lookupDigestAlgorithm(keyAlgorithm.hash.name).second;
    auto messageDigest = kj::heapArray<kj::byte>(EVP_MD_size(type));

    uint messageDigestSize = 0;
    auto ptr = HMAC(type, keyData.begin(), keyData.size(), data.begin(), data.size(),
        messageDigest.begin(), &messageDigestSize);
    JSG_REQUIRE(ptr != nullptr, DOMOperationError, "HMAC computation failed.");

    KJ_ASSERT(messageDigestSize == messageDigest.size());
    return kj::mv(messageDigest);
  }

  SubtleCrypto::ExportKeyData exportKey(kj::StringPtr format) const override {
    JSG_REQUIRE(format == "raw" || format == "jwk", DOMNotSupportedError,
        "Unimplemented key export format \"", format, "\".");

    if (format == "jwk") {
      // This assert enforces that the slice logic to fill in `.alg` below is safe.
      JSG_REQUIRE(keyAlgorithm.hash.name.first(4) == "SHA-"_kj, DOMNotSupportedError,
          "Unimplemented JWK key export format for key algorithm \"", keyAlgorithm.hash.name,
          "\".");

      SubtleCrypto::JsonWebKey jwk;
      jwk.kty = kj::str("oct");
      jwk.k = fastEncodeBase64Url(keyData);
      jwk.alg = kj::str("HS", keyAlgorithm.hash.name.slice(4));
      jwk.key_ops = getUsages().map([](auto usage) { return kj::str(usage.name()); });
      // I don't know why the spec says:
      //   Set the ext attribute of jwk to equal the [[extractable]] internal slot of key.
      // Earlier in the normative part of the spec it says:
      //   6. If the [[extractable]] internal slot of key is false, then throw an InvalidAccessError.
      //   7. Let result be the result of performing the export key operation specified by the
      //      [[algorithm]] internal slot of key using key and format.
      // So there's not really any other value that `ext` can have here since this code is the
      // implementation of step 7 (see SubtleCrypto::exportKey where you can confirm it is
      // enforcing step 6).
      jwk.ext = true;

      return jwk;
    }

    return kj::heapArray(keyData.asPtr());
  }

  kj::StringPtr getAlgorithmName() const override {
    return "HMAC";
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
  CryptoKey::HmacKeyAlgorithm keyAlgorithm;
};

void zeroOutTrailingKeyBits(kj::Array<kj::byte>& keyDataArray, int keyBitLength) {
  // We zero out the least-significant bits of the last byte, matching Chrome's
  // big-endian behavior when generating keys.
  int arrayBitLength = keyDataArray.size() * 8;
  KJ_REQUIRE(arrayBitLength >= keyBitLength);
  KJ_REQUIRE(arrayBitLength - 8 < keyBitLength);

  if (auto difference = keyBitLength - (arrayBitLength - 8); difference > 0) {
    keyDataArray.back() &= 0xff00 >> difference;
  }
}

kj::Own<HMAC_CTX> initHmacContext(kj::StringPtr algorithm, HmacContext::KeyData& key) {
  static constexpr auto handle = [](kj::StringPtr algorithm, kj::ArrayPtr<kj::byte> key) {
    ClearErrorOnReturn clearErrorOnReturn;
    JSG_REQUIRE(key.size() <= INT_MAX, RangeError, "key is too long");
    const EVP_MD* md = EVP_get_digestbyname(algorithm.begin());
    JSG_REQUIRE(md != nullptr, Error, "Digest method not supported");
    static constexpr auto mt = ""_kjc;
    auto hmac_ctx = OSSL_NEW(HMAC_CTX);
    JSG_REQUIRE(HMAC_Init_ex(hmac_ctx.get(), key.size() ? key.asChars().begin() : mt.begin(),
                    key.size(), md, nullptr),
        Error, "Failed to initalize HMAC");
    return kj::mv(hmac_ctx);
  };

  KJ_SWITCH_ONEOF(key) {
    KJ_CASE_ONEOF(buf, kj::ArrayPtr<kj::byte>) {
      return handle(algorithm, buf);
    }
    KJ_CASE_ONEOF(key2, CryptoKey::Impl*) {
      // We already checked that the key is a secret key, so the following should succeed.
      SubtleCrypto::ExportKeyData keyData = key2->exportKey("raw"_kj);

      KJ_SWITCH_ONEOF(keyData) {
        KJ_CASE_ONEOF(key_data, kj::Array<kj::byte>) {
          return handle(algorithm, key_data);
        }
        KJ_CASE_ONEOF(jwk, SubtleCrypto::JsonWebKey) {
          KJ_UNREACHABLE;
        }
      }
    }
  }
  KJ_UNREACHABLE;
}
}  // namespace

HmacContext::HmacContext(kj::StringPtr algorithm, KeyData key)
    : state(initHmacContext(algorithm, key)) {}

void HmacContext::update(kj::ArrayPtr<kj::byte> data) {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(ctx, kj::Own<HMAC_CTX>) {
      JSG_REQUIRE(data.size() <= INT_MAX, RangeError, "data is too long");
      KJ_ASSERT(HMAC_Update(ctx.get(), data.begin(), data.size()) == 1);
    }
    KJ_CASE_ONEOF(digest, kj::Array<kj::byte>) {
      JSG_FAIL_REQUIRE(DOMOperationError, "HMAC context has already been finalized.");
    }
  }
}

kj::ArrayPtr<kj::byte> HmacContext::digest() {
  kj::ArrayPtr<kj::byte> ret = nullptr;
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(ctx, kj::Own<HMAC_CTX>) {
      auto theCtx = kj::mv(ctx);
      unsigned len;
      auto digest = kj::heapArray<kj::byte>(HMAC_size(theCtx.get()));
      JSG_REQUIRE(HMAC_Final(theCtx.get(), digest.begin(), &len), Error, "Failed to finalize HMAC");
      KJ_ASSERT(len == digest.size());
      ret = digest.asPtr();
      state = kj::mv(digest);
    }
    KJ_CASE_ONEOF(digest, kj::Array<kj::byte>) {
      ret = digest.asPtr();
    }
  }
  return ret;
}

size_t HmacContext::size() const {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(ctx, kj::Own<HMAC_CTX>) {
      return 0;
    }
    KJ_CASE_ONEOF(digest, kj::Array<kj::byte>) {
      return digest.size();
    }
  }
  KJ_UNREACHABLE;
}

kj::OneOf<jsg::Ref<CryptoKey>, CryptoKeyPair> CryptoKey::Impl::generateHmac(jsg::Lock& js,
    kj::StringPtr normalizedName,
    SubtleCrypto::GenerateKeyAlgorithm&& algorithm,
    bool extractable,
    kj::ArrayPtr<const kj::String> keyUsages) {
  KJ_REQUIRE(normalizedName == "HMAC");
  kj::StringPtr hash = api::getAlgorithmName(
      JSG_REQUIRE_NONNULL(algorithm.hash, TypeError, "Missing field \"hash\" in \"algorithm\"."));

  auto [normalizedHashName, hashEvpMd] = lookupDigestAlgorithm(hash);
  auto usages = CryptoKeyUsageSet::validate(normalizedName, CryptoKeyUsageSet::Context::generate,
      keyUsages, CryptoKeyUsageSet::sign() | CryptoKeyUsageSet::verify());

  // If the user requested a specific HMAC key length, honor it.
  auto length = algorithm.length.orDefault(EVP_MD_block_size(hashEvpMd) * 8);
  JSG_REQUIRE(length > 0, DOMOperationError,
      "HMAC key length must be a non-zero unsigned long integer (requested ", length, ").");

  auto keyDataArray = kj::heapArray<kj::byte>(
      integerCeilDivision<std::make_unsigned<decltype(length)>::type>(length, 8u));
  IoContext::current().getEntropySource().generate(keyDataArray);
  zeroOutTrailingKeyBits(keyDataArray, length);

  auto keyAlgorithm = CryptoKey::HmacKeyAlgorithm{
    normalizedName, {normalizedHashName}, static_cast<uint16_t>(length)};

  return jsg::alloc<CryptoKey>(
      kj::heap<HmacKey>(kj::mv(keyDataArray), kj::mv(keyAlgorithm), extractable, usages));
}

kj::Own<CryptoKey::Impl> CryptoKey::Impl::importHmac(jsg::Lock& js,
    kj::StringPtr normalizedName,
    kj::StringPtr format,
    SubtleCrypto::ImportKeyData keyData,
    SubtleCrypto::ImportKeyAlgorithm&& algorithm,
    bool extractable,
    kj::ArrayPtr<const kj::String> keyUsages) {
  auto usages =
      CryptoKeyUsageSet::validate(normalizedName, CryptoKeyUsageSet::Context::importSecret,
          keyUsages, CryptoKeyUsageSet::sign() | CryptoKeyUsageSet::verify());

  kj::Array<kj::byte> keyDataArray;
  kj::StringPtr hash = api::getAlgorithmName(
      JSG_REQUIRE_NONNULL(algorithm.hash, TypeError, "Missing field \"hash\" in \"algorithm\"."));

  if (format == "raw") {
    // NOTE: Checked in SubtleCrypto::importKey().
    keyDataArray = kj::mv(keyData.get<kj::Array<kj::byte>>());
  } else if (format == "jwk") {
    auto& keyDataJwk = keyData.get<SubtleCrypto::JsonWebKey>();
    JSG_REQUIRE(keyDataJwk.kty == "oct", DOMDataError,
        "HMAC \"jwk\" key import requires a JSON Web Key with Key Type parameter "
        "(\"kty\") equal to \"oct\" (encountered \"",
        keyDataJwk.kty, "\").");
    // https://www.rfc-editor.org/rfc/rfc7518.txt Section 6.1
    keyDataArray = UNWRAP_JWK_BIGNUM(kj::mv(keyDataJwk.k), DOMDataError,
        "HMAC \"jwk\" key import requires a base64Url encoding of the key");

    KJ_IF_SOME(alg, keyDataJwk.alg) {
      if (hash.startsWith("SHA-")) {
        auto expectedAlg = kj::str("HS", hash.slice(4));
        JSG_REQUIRE(alg == expectedAlg, DOMDataError,
            "HMAC \"jwk\" key import specifies \"alg\" that is incompatible with the hash name "
            "(encountered \"",
            alg, "\", expected \"", expectedAlg, "\").");
      } else {
        // TODO(conform): Spec says this for non-SHA hashes:
        //     > Perform any key import steps defined by other applicable specifications, passing
        //     > format, jwk and hash and obtaining hash.
        //   What other hashes should be supported (if any)? For example, technically we support MD5
        //   below in `lookupDigestAlgorithm` for "raw" keys...
        JSG_FAIL_REQUIRE(
            DOMNotSupportedError, "Unrecognized or unimplemented hash algorithm requested", alg);
      }
    }
  } else {
    JSG_FAIL_REQUIRE(DOMNotSupportedError, "Unrecognized key import format \"", format, "\".");
  }

  // The spec claims the length of an HMAC key can be up to 7 bits less than the bit length of the
  // raw key data passed in to `importKey()`. Since the raw key data comes in bytes, that means that
  // HMAC keys can have non-multiple-of-8 bit lengths. I dutifully implemented this check, but it
  // seems rather pointless: the OpenSSL HMAC interface only supports key lengths in bytes ...
  auto keySize = keyDataArray.size() * 8;
  auto length = algorithm.length.orDefault(keySize);
  if (length == 0 || length > keySize || length <= keySize - 8) {
    JSG_FAIL_REQUIRE(DOMDataError, "Imported HMAC key length (", length,
        ") must be a non-zero value up to 7 bits less than, "
        "and no greater than, the bit length of the raw key data (",
        keySize, ").");
  }

  // Not required by the spec, but zeroing out the unused bits makes me feel better.
  zeroOutTrailingKeyBits(keyDataArray, length);

  auto normalizedHashName = lookupDigestAlgorithm(hash).first;
  auto keyAlgorithm = CryptoKey::HmacKeyAlgorithm{
    normalizedName, {normalizedHashName}, static_cast<uint16_t>(length)};
  return kj::heap<HmacKey>(kj::mv(keyDataArray), kj::mv(keyAlgorithm), extractable, usages);
}

// ======================================================================================

namespace {
kj::Own<EVP_MD_CTX> initDigestCtx(kj::StringPtr algorithm) {
  const EVP_MD* md = EVP_get_digestbyname(algorithm.begin());
  JSG_REQUIRE(md != nullptr, Error, "Digest method not supported");
  auto ctx = OSSL_NEW(EVP_MD_CTX);
  OSSLCALL(EVP_DigestInit(ctx.get(), md));
  return kj::mv(ctx);
}

void checkXofLen(EVP_MD_CTX* ctx, kj::Maybe<uint32_t>& maybeXof) {
  KJ_IF_SOME(xof, maybeXof) {
    auto md = EVP_MD_CTX_md(ctx);
    if (xof != EVP_MD_size(md)) {
      JSG_REQUIRE((EVP_MD_flags(md) & EVP_MD_FLAG_XOF) != 0, Error, "invalid digest size");
    }
  }
}
}  // namespace

HashContext::HashContext(
    kj::OneOf<kj::Own<EVP_MD_CTX>, kj::Array<kj::byte>> state, kj::Maybe<uint32_t> maybeXof)
    : state(kj::mv(state)),
      maybeXof(kj::mv(maybeXof)) {
  checkXofLen(this->state.get<kj::Own<EVP_MD_CTX>>().get(), this->maybeXof);
}

HashContext::HashContext(kj::StringPtr algorithm, kj::Maybe<uint32_t> maybeXof)
    : HashContext(initDigestCtx(algorithm), kj::mv(maybeXof)) {}

void HashContext::update(kj::ArrayPtr<kj::byte> data) {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(ctx, kj::Own<EVP_MD_CTX>) {
      JSG_REQUIRE(data.size() <= INT_MAX, RangeError, "data is too long");
      OSSLCALL(EVP_DigestUpdate(ctx.get(), data.begin(), data.size()));
    }
    KJ_CASE_ONEOF(digest, kj::Array<kj::byte>) {
      JSG_FAIL_REQUIRE(DOMOperationError, "Hash context has already been finalized.");
    }
  }
}

kj::ArrayPtr<kj::byte> HashContext::digest() {
  kj::ArrayPtr<kj::byte> ret = nullptr;
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(ctx, kj::Own<EVP_MD_CTX>) {
      auto theCtx = kj::mv(ctx);
      uint32_t len = EVP_MD_size(EVP_MD_CTX_md(theCtx.get()));
      KJ_IF_SOME(xof, maybeXof) {
        if (xof == len) {
          auto digest = kj::heapArray<kj::byte>(len);
          JSG_REQUIRE(EVP_DigestFinal_ex(theCtx.get(), digest.begin(), &len) == 1, Error,
              "Failed to compute hash digest");
          KJ_ASSERT(len == digest.size());
          ret = digest.asPtr();
          state = kj::mv(digest);
        } else {
          auto digest = kj::heapArray<kj::byte>(xof);
          JSG_REQUIRE(EVP_DigestFinalXOF(theCtx.get(), digest.begin(), xof) == 1, Error,
              "Failed to compute XOF hash digest");
          ret = digest.asPtr();
          state = kj::mv(digest);
        }
      } else {
        uint32_t len = EVP_MD_size(EVP_MD_CTX_md(theCtx.get()));
        auto digest = kj::heapArray<kj::byte>(len);
        JSG_REQUIRE(EVP_DigestFinal_ex(theCtx.get(), digest.begin(), &len) == 1, Error,
            "Failed to compute hash digest");
        KJ_ASSERT(len == digest.size());
        ret = digest.asPtr();
        state = kj::mv(digest);
      }
    }
    KJ_CASE_ONEOF(digest, kj::Array<kj::byte>) {
      ret = digest.asPtr();
    }
  }
  return ret;
}

HashContext HashContext::clone(kj::Maybe<uint32_t> xofLen) {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(ctx, kj::Own<EVP_MD_CTX>) {
      auto newCtx = OSSL_NEW(EVP_MD_CTX);
      OSSLCALL(EVP_MD_CTX_copy_ex(newCtx, ctx.get()));
      return HashContext(kj::mv(newCtx), kj::mv(xofLen));
    }
    KJ_CASE_ONEOF(digest, kj::Array<kj::byte>) {
      return HashContext(kj::mv(digest), kj::mv(xofLen));
    }
  }
  KJ_UNREACHABLE;
}

size_t HashContext::size() const {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(ctx, kj::Own<EVP_MD_CTX>) {
      return 0;
    }
    KJ_CASE_ONEOF(digest, kj::Array<kj::byte>) {
      return digest.size();
    }
  }
  KJ_UNREACHABLE;
}

}  // namespace workerd::api
