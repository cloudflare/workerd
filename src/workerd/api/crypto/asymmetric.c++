// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "impl.h"
#include "keys.h"
#include <openssl/rsa.h>
#include <openssl/ec_key.h>
#include <openssl/bn.h>
#include <openssl/x509.h>
#include <openssl/crypto.h>
#include <openssl/curve25519.h>
#include <map>
#include <kj/function.h>
#include <type_traits>
#include <workerd/api/util.h>
#include <workerd/io/features.h>

namespace workerd::api {

// =====================================================================================
// RSASSA-PKCS1-V1_5, RSA-PSS, RSA-OEAP, RSA-RAW

namespace {

class RsaBase: public AsymmetricKeyCryptoKeyImpl {
public:
  explicit RsaBase(AsymmetricKeyData keyData,
                   CryptoKey::RsaKeyAlgorithm keyAlgorithm,
                   bool extractable)
    : AsymmetricKeyCryptoKeyImpl(kj::mv(keyData), extractable),
      keyAlgorithm(kj::mv(keyAlgorithm)) {}

  kj::StringPtr jsgGetMemoryName() const override { return "AsymmetricKey"; }
  size_t jsgGetMemorySelfSize() const override { return sizeof(AsymmetricKeyCryptoKeyImpl); }
  void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const override {
    AsymmetricKeyCryptoKeyImpl::jsgGetMemoryInfo(tracker);
    tracker.trackField("keyAlgorithm", keyAlgorithm);
  }

protected:
  CryptoKey::RsaKeyAlgorithm keyAlgorithm;

private:
  SubtleCrypto::JsonWebKey exportJwk() const override final {
    const auto& rsa = JSG_REQUIRE_NONNULL(EVP_PKEY_get0_RSA(getEvpPkey()), DOMOperationError,
        "No RSA data backing key", tryDescribeOpensslErrors());

    SubtleCrypto::JsonWebKey jwk;
    jwk.kty = kj::str("RSA");
    jwk.alg = jwkHashAlgorithmName();

    jwk.n = kj::encodeBase64Url(KJ_REQUIRE_NONNULL(bignumToArray(KJ_REQUIRE_NONNULL(rsa.n))));
    jwk.e = kj::encodeBase64Url(KJ_REQUIRE_NONNULL(bignumToArray(KJ_REQUIRE_NONNULL(rsa.e))));

    if (getTypeEnum() == KeyType::PRIVATE) {
      jwk.d = kj::encodeBase64Url(KJ_REQUIRE_NONNULL(bignumToArray(KJ_REQUIRE_NONNULL(rsa.d))));
      jwk.p = kj::encodeBase64Url(KJ_REQUIRE_NONNULL(bignumToArray(KJ_REQUIRE_NONNULL(rsa.p))));
      jwk.q = kj::encodeBase64Url(KJ_REQUIRE_NONNULL(bignumToArray(KJ_REQUIRE_NONNULL(rsa.q))));
      jwk.dp = kj::encodeBase64Url(KJ_REQUIRE_NONNULL(bignumToArray(KJ_REQUIRE_NONNULL(rsa.dmp1))));
      jwk.dq = kj::encodeBase64Url(KJ_REQUIRE_NONNULL(bignumToArray(KJ_REQUIRE_NONNULL(rsa.dmq1))));
      jwk.qi = kj::encodeBase64Url(KJ_REQUIRE_NONNULL(bignumToArray(KJ_REQUIRE_NONNULL(rsa.iqmp))));
    }

    return jwk;
  }

  kj::Array<kj::byte> exportRaw() const override final {
    JSG_FAIL_REQUIRE(DOMInvalidAccessError, "Cannot export \"", getAlgorithmName(),
        "\" in \"raw\" format.");
  }

  CryptoKey::AsymmetricKeyDetails getAsymmetricKeyDetail() const override {
    // Adapted from the Node.js implementation of GetRsaKeyDetail
    const BIGNUM* e;  // Public Exponent
    const BIGNUM* n;  // Modulus

    int type = EVP_PKEY_id(getEvpPkey());
    KJ_REQUIRE(type == EVP_PKEY_RSA || type == EVP_PKEY_RSA_PSS);

    const RSA* rsa = EVP_PKEY_get0_RSA(getEvpPkey());
    KJ_ASSERT(rsa != nullptr);
    RSA_get0_key(rsa, &n, &e, nullptr);

    CryptoKey::AsymmetricKeyDetails details;
    details.modulusLength = BN_num_bits(n);

    details.publicExponent = JSG_REQUIRE_NONNULL(bignumToArrayPadded(*e), Error,
        "Failed to extract public exponent");

    // TODO(soon): Does BoringSSL not support retrieving RSA_PSS params?
    // if (type == EVP_PKEY_RSA_PSS) {
    //   // Due to the way ASN.1 encoding works, default values are omitted when
    //   // encoding the data structure. However, there are also RSA-PSS keys for
    //   // which no parameters are set. In that case, the ASN.1 RSASSA-PSS-params
    //   // sequence will be missing entirely and RSA_get0_pss_params will return
    //   // nullptr. If parameters are present but all parameters are set to their
    //   // default values, an empty sequence will be stored in the ASN.1 structure.
    //   // In that case, RSA_get0_pss_params does not return nullptr but all fields
    //   // of the returned RSA_PSS_PARAMS will be set to nullptr.

    //   const RSA_PSS_PARAMS* params = RSA_get0_pss_params(rsa);
    //   if (params != nullptr) {
    //     int hash_nid = NID_sha1;
    //     int mgf_nid = NID_mgf1;
    //     int mgf1_hash_nid = NID_sha1;
    //     int64_t salt_length = 20;

    //     if (params->hashAlgorithm != nullptr) {
    //       hash_nid = OBJ_obj2nid(params->hashAlgorithm->algorithm);
    //     }
    //     details.hashAlgorithm = kj::str(OBJ_nid2ln(hash_nid));

    //     if (params->maskGenAlgorithm != nullptr) {
    //       mgf_nid = OBJ_obj2nid(params->maskGenAlgorithm->algorithm);
    //       if (mgf_nid == NID_mgf1) {
    //         mgf1_hash_nid = OBJ_obj2nid(params->maskHash->algorithm);
    //       }
    //     }

    //     // If, for some reason, the MGF is not MGF1, then the MGF1 hash function
    //     // is intentionally not added to the object.
    //     if (mgf_nid == NID_mgf1) {
    //       details.mgf1HashAlgorithm = kj::str(OBJ_nid2ln(mgf1_hash_nid));
    //     }

    //     if (params->saltLength != nullptr) {
    //       JSG_REQUIRE(ASN1_INTEGER_get_int64(&salt_length, params->saltLength) == 1,
    //                   Error, "Unable to get salt length from RSA-PSS parameters");
    //     }
    //     details.saltLength = static_cast<double>(salt_length);
    //   }
    // }

    return kj::mv(details);
  }

  virtual kj::String jwkHashAlgorithmName() const = 0;
};

class RsassaPkcs1V15Key final: public RsaBase {
public:
  explicit RsassaPkcs1V15Key(AsymmetricKeyData keyData,
                             CryptoKey::RsaKeyAlgorithm keyAlgorithm,
                             bool extractable)
      : RsaBase(kj::mv(keyData), kj::mv(keyAlgorithm), extractable) {}

  CryptoKey::AlgorithmVariant getAlgorithm(jsg::Lock& js) const override { return keyAlgorithm.clone(js); }
  kj::StringPtr getAlgorithmName() const override { return "RSASSA-PKCS1-v1_5"; }

  kj::StringPtr chooseHash(
      const kj::Maybe<kj::OneOf<kj::String, SubtleCrypto::HashAlgorithm>>& callTimeHash)
      const override {
    // RSASSA-PKCS1-v1_5 attaches the hash to the key, ignoring whatever is specified at call time.
    return KJ_REQUIRE_NONNULL(keyAlgorithm.hash).name;
  }

private:
  kj::String jwkHashAlgorithmName() const override {
    const auto& hashName = KJ_REQUIRE_NONNULL(keyAlgorithm.hash).name;
    JSG_REQUIRE(hashName.startsWith("SHA"), DOMNotSupportedError,
        "JWK export not supported for hash algorithm \"", hashName, "\".");
    return kj::str("RS", hashName.slice(4, hashName.size()));
  }
};

class RsaPssKey final: public RsaBase {
public:
  explicit RsaPssKey(AsymmetricKeyData keyData,
                     CryptoKey::RsaKeyAlgorithm keyAlgorithm,
                     bool extractable)
      : RsaBase(kj::mv(keyData), kj::mv(keyAlgorithm), extractable) {}

  CryptoKey::AlgorithmVariant getAlgorithm(jsg::Lock& js) const override { return keyAlgorithm.clone(js); }
  kj::StringPtr getAlgorithmName() const override { return keyAlgorithm.name; }

  kj::StringPtr chooseHash(
      const kj::Maybe<kj::OneOf<kj::String, SubtleCrypto::HashAlgorithm>>& callTimeHash)
      const override {
    // RSA-PSS attaches the hash to the key, ignoring whatever is specified at call time.
    return KJ_REQUIRE_NONNULL(keyAlgorithm.hash).name;
  }

  void addSalt(EVP_PKEY_CTX* pctx, const SubtleCrypto::SignAlgorithm& algorithm) const override {
    auto salt = JSG_REQUIRE_NONNULL(algorithm.saltLength, TypeError,
        "Failed to provide salt for RSA-PSS key operation which requires a salt");
    JSG_REQUIRE(salt >= 0, DOMDataError, "SaltLength for RSA-PSS must be non-negative (provided ",
        salt, ").");
    OSSLCALL(EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING));
    OSSLCALL(EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, salt));
  }

private:
  kj::String jwkHashAlgorithmName() const override {
    const auto& hashName = KJ_REQUIRE_NONNULL(keyAlgorithm.hash).name;
    JSG_REQUIRE(hashName.startsWith("SHA"), DOMNotSupportedError,
        "JWK export not supported for hash algorithm \"", hashName, "\".");
    return kj::str("PS", hashName.slice(4, hashName.size()));
  }
};

class RsaOaepKey final : public RsaBase {
  using InitFunction = decltype(EVP_PKEY_encrypt_init);
  using EncryptDecryptFunction = decltype(EVP_PKEY_encrypt);

public:
  explicit RsaOaepKey(AsymmetricKeyData keyData,
                      CryptoKey::RsaKeyAlgorithm keyAlgorithm,
                      bool extractable)
      : RsaBase(kj::mv(keyData), kj::mv(keyAlgorithm), extractable) {}

  CryptoKey::AlgorithmVariant getAlgorithm(jsg::Lock& js) const override { return keyAlgorithm.clone(js); }
  kj::StringPtr getAlgorithmName() const override { return keyAlgorithm.name; }

  kj::StringPtr chooseHash(
      const kj::Maybe<kj::OneOf<kj::String, SubtleCrypto::HashAlgorithm>>& callTimeHash)
      const override {
    // RSA-OAEP is for encryption/decryption, not signing, but this method is called by the
    // parent class when performing sign() or verify().
    JSG_FAIL_REQUIRE(DOMNotSupportedError,
        "The sign and verify operations are not implemented for \"", keyAlgorithm.name, "\".");
  }

  kj::Array<kj::byte> encrypt(
      SubtleCrypto::EncryptAlgorithm&& algorithm,
      kj::ArrayPtr<const kj::byte> plainText) const override {
    JSG_REQUIRE(getTypeEnum() == KeyType::PUBLIC, DOMInvalidAccessError,
        "Encryption/key wrapping only works with public keys, not \"", getType(), "\".");
    return commonEncryptDecrypt(kj::mv(algorithm), plainText, EVP_PKEY_encrypt_init,
        EVP_PKEY_encrypt);
  }

  kj::Array<kj::byte> decrypt(
      SubtleCrypto::EncryptAlgorithm&& algorithm,
      kj::ArrayPtr<const kj::byte> cipherText) const override {
    JSG_REQUIRE(getTypeEnum() == KeyType::PRIVATE, DOMInvalidAccessError,
        "Decryption/key unwrapping only works with private keys, not \"", getType(), "\".");
    return commonEncryptDecrypt(kj::mv(algorithm), cipherText, EVP_PKEY_decrypt_init,
        EVP_PKEY_decrypt);
  }

private:
  kj::Array<kj::byte> commonEncryptDecrypt(SubtleCrypto::EncryptAlgorithm&& algorithm,
      kj::ArrayPtr<const kj::byte> data, InitFunction init,
      EncryptDecryptFunction encryptDecrypt) const {
    auto digest = lookupDigestAlgorithm(KJ_REQUIRE_NONNULL(keyAlgorithm.hash).name).second;

    auto pkey = getEvpPkey();
    auto ctx = OSSL_NEW(EVP_PKEY_CTX, pkey, nullptr);

    JSG_REQUIRE(1 == init(ctx.get()), DOMOperationError, "RSA-OAEP failed to initialize",
        tryDescribeOpensslErrors());
    JSG_REQUIRE(1 == EVP_PKEY_CTX_set_rsa_padding(ctx.get(), RSA_PKCS1_OAEP_PADDING),
        InternalDOMOperationError, "Error doing RSA OAEP encrypt/decrypt (", "padding", ")",
        internalDescribeOpensslErrors());
    JSG_REQUIRE(1 == EVP_PKEY_CTX_set_rsa_oaep_md(ctx.get(), digest), InternalDOMOperationError,
        "Error doing RSA OAEP encrypt/decrypt (", "message digest", ")",
        internalDescribeOpensslErrors());
    JSG_REQUIRE(1 == EVP_PKEY_CTX_set_rsa_mgf1_md(ctx.get(), digest), InternalDOMOperationError,
        "Error doing RSA OAEP encrypt/decrypt (", "MGF1 digest", ")",
        internalDescribeOpensslErrors());

    KJ_IF_SOME(l, algorithm.label) {
      auto labelCopy = reinterpret_cast<uint8_t*>(OPENSSL_malloc(l.size()));
      KJ_DEFER(OPENSSL_free(labelCopy));
      // If setting the label fails we need to remember to destroy the buffer. In practice it can't
      // actually happen since we set RSA_PKCS1_OAEP_PADDING above & that appears to be the only way
      // this API call can fail.

      JSG_REQUIRE(labelCopy != nullptr, DOMOperationError,
          "Failed to allocate space for RSA-OAEP label copy",
          tryDescribeOpensslErrors());
      std::copy(l.begin(), l.end(), labelCopy);

      // EVP_PKEY_CTX_set0_rsa_oaep_label below takes ownership of the buffer passed in (must have
      // been OPENSSL_malloc-allocated).
      JSG_REQUIRE(1 == EVP_PKEY_CTX_set0_rsa_oaep_label(ctx.get(), labelCopy, l.size()),
          DOMOperationError, "Failed to set RSA-OAEP label",
          tryDescribeOpensslErrors());

      // Ownership has now been transferred. The chromium WebCrypto code technically has a potential
      // memory leak here in that they check the error for EVP_PKEY_CTX_set0_rsa_oaep_label after
      // releasing. It's not actually possible though because the padding mode is set unconditionally
      // to RSA_PKCS1_OAEP_PADDING which seems to be the only way setting the label will fail.
      labelCopy = nullptr;
    }

    size_t maxResultLength = 0;
    // First compute an upper bound on the amount of space we need to store the encrypted/decrypted
    // result. Then we actually apply the encryption & finally resize to the actual correct length.
    JSG_REQUIRE(1 == encryptDecrypt(ctx.get(), nullptr, &maxResultLength, data.begin(),
          data.size()), DOMOperationError, "Failed to compute length of RSA-OAEP result",
          tryDescribeOpensslErrors());

    kj::Vector<kj::byte> result(maxResultLength);
    auto err = encryptDecrypt(ctx.get(), result.begin(), &maxResultLength, data.begin(),
        data.size());
    JSG_REQUIRE(1 == err, DOMOperationError, "RSA-OAEP failed encrypt/decrypt",
        tryDescribeOpensslErrors());
    result.resize(maxResultLength);

    return result.releaseAsArray();
  }

  kj::String jwkHashAlgorithmName() const override {
    const auto& hashName = KJ_REQUIRE_NONNULL(keyAlgorithm.hash).name;
    JSG_REQUIRE(hashName.startsWith("SHA"), DOMNotSupportedError,
        "JWK export not supported for hash algorithm \"", hashName, "\".");
    if (hashName == "SHA-1") {
      return kj::str("RSA-OAEP");
    }
    return kj::str("RSA-OAEP-", hashName.slice(4, hashName.size()));
  }
};

class RsaRawKey final: public RsaBase {
public:
  explicit RsaRawKey(AsymmetricKeyData keyData,
                     CryptoKey::RsaKeyAlgorithm keyAlgorithm,
                     bool extractable)
      : RsaBase(kj::mv(keyData), kj::mv(keyAlgorithm), extractable) {}

  kj::Array<kj::byte> sign(
      SubtleCrypto::SignAlgorithm&& algorithm,
      kj::ArrayPtr<const kj::byte> data) const override {

    RSA* rsa = EVP_PKEY_get0_RSA(getEvpPkey());
    if (rsa == nullptr) {
      JSG_FAIL_REQUIRE(DOMDataError, "Missing RSA key");
    }

    auto size = RSA_size(rsa);

    // RSA encryption/decryption requires the key value to be strictly larger than the value to be
    // signed. Ideally we would enforce this by checking that the key size is larger than the input
    // size – having both the same size makes it highly likely that some values are higher than the
    // key value – but there are scripts and test cases that depend on signing data with keys of
    // the same size.
    JSG_REQUIRE(data.size() <= size, DOMDataError,
        "Blind Signing requires presigned data (", data.size(), " bytes) to be smaller than "
        "the key (", size, " bytes).");
    if (data.size() == size) {
      auto dataVal = JSG_REQUIRE_NONNULL(toBignum(data), InternalDOMOperationError,
          "Error converting presigned data", internalDescribeOpensslErrors());
      JSG_REQUIRE(BN_ucmp(dataVal, rsa->n) < 0, DOMDataError,
          "Blind Signing requires presigned data value to be strictly smaller than RSA key"
          "modulus, consider using a larger key size.");
    }

    auto signature = kj::heapArray<kj::byte>(size);
    size_t signatureSize = 0;

    // Use raw RSA, no padding
    OSSLCALL(RSA_decrypt(rsa, &signatureSize, signature.begin(), size, data.begin(), data.size(),
        RSA_NO_PADDING));

    KJ_ASSERT(signatureSize <= signature.size());
    if (signatureSize < signature.size()) {
      signature = kj::heapArray<kj::byte>(signature.slice(0, signatureSize));
    }

    return kj::mv(signature);
  }

  bool verify(
      SubtleCrypto::SignAlgorithm&& algorithm,
      kj::ArrayPtr<const kj::byte> signature, kj::ArrayPtr<const kj::byte> data) const override {
    KJ_UNIMPLEMENTED("RawRsa Verification currently unsupported");
  }

  CryptoKey::AlgorithmVariant getAlgorithm(jsg::Lock& js) const override { return keyAlgorithm.clone(js); }
  kj::StringPtr getAlgorithmName() const override { return keyAlgorithm.name; }

  kj::StringPtr chooseHash(
      const kj::Maybe<kj::OneOf<kj::String, SubtleCrypto::HashAlgorithm>>& callTimeHash)
      const override {
    KJ_UNIMPLEMENTED("this should not be called since we overrode sign() and verify()");
  }

private:
  kj::String jwkHashAlgorithmName() const override {
    const auto& hashName = KJ_REQUIRE_NONNULL(keyAlgorithm.hash).name;
    JSG_REQUIRE(hashName.startsWith("SHA"), DOMNotSupportedError,
        "JWK export not supported for hash algorithm \"", hashName, "\".");
    return kj::str("RS", hashName.slice(4, hashName.size()));
  }
};

CryptoKeyPair generateRsaPair(jsg::Lock& js, kj::StringPtr normalizedName,
    kj::Own<EVP_PKEY> privateEvpPKey, kj::Own<EVP_PKEY> publicEvpPKey,
    CryptoKey::RsaKeyAlgorithm&& keyAlgorithm, bool privateKeyExtractable,
    CryptoKeyUsageSet usages) {
  auto privateKeyAlgorithm = keyAlgorithm.clone(js);

  CryptoKeyUsageSet publicKeyUsages = usages & CryptoKeyUsageSet::publicKeyMask();
  CryptoKeyUsageSet privateKeyUsages = usages & CryptoKeyUsageSet::privateKeyMask();

  AsymmetricKeyData publicKeyData {
    .evpPkey = kj::mv(publicEvpPKey),
    .keyType = KeyType::PUBLIC,
    .usages = publicKeyUsages,
  };
  AsymmetricKeyData privateKeyData {
    .evpPkey = kj::mv(privateEvpPKey),
    .keyType = KeyType::PRIVATE,
    .usages = privateKeyUsages,
  };

  if (normalizedName == "RSASSA-PKCS1-v1_5") {
    return CryptoKeyPair {
      .publicKey =  jsg::alloc<CryptoKey>(kj::heap<RsassaPkcs1V15Key>(kj::mv(publicKeyData),
          kj::mv(keyAlgorithm), true)),
      .privateKey = jsg::alloc<CryptoKey>(kj::heap<RsassaPkcs1V15Key>(kj::mv(privateKeyData),
          kj::mv(privateKeyAlgorithm), privateKeyExtractable))};
  } else if (normalizedName == "RSA-PSS") {
    return CryptoKeyPair {
      .publicKey =  jsg::alloc<CryptoKey>(kj::heap<RsaPssKey>(kj::mv(publicKeyData),
          kj::mv(keyAlgorithm), true)),
      .privateKey = jsg::alloc<CryptoKey>(kj::heap<RsaPssKey>(kj::mv(privateKeyData),
          kj::mv(privateKeyAlgorithm), privateKeyExtractable))};
  } else if (normalizedName == "RSA-OAEP") {
    return CryptoKeyPair {
      .publicKey =  jsg::alloc<CryptoKey>(kj::heap<RsaOaepKey>(kj::mv(publicKeyData),
          kj::mv(keyAlgorithm), true)),
      .privateKey = jsg::alloc<CryptoKey>(kj::heap<RsaOaepKey>(kj::mv(privateKeyData),
          kj::mv(privateKeyAlgorithm), privateKeyExtractable))};
  } else {
    JSG_FAIL_REQUIRE(DOMNotSupportedError, "Unimplemented RSA generation \"", normalizedName,
        "\".");
  }
}
}  // namespace

template <typename T>
kj::Maybe<T> fromBignum(kj::ArrayPtr<kj::byte> value) {
  static_assert(std::is_unsigned_v<T>, "This can only be invoked when the return type is unsigned");

  T asUnsigned = 0;
  for (size_t i = 0; i < value.size(); ++i) {
    size_t bitShift = value.size() - i - 1;
    if (bitShift >= sizeof(T) && value[i]) {
      // Too large for desired type.
      return kj::none;
    }

    asUnsigned |= value[i] << 8 * bitShift;
  }

  return asUnsigned;
}

namespace {

// The W3C standard itself doesn't describe any parameter validation but the conformance tests
// do test "bad" exponents, likely because everyone uses OpenSSL that suffers from poor behavior
// with these bad exponents (e.g. if an exponent < 3 or 65535 generates an infinite loop, a
// library might be expected to handle such cases on its own, no?).
void validateRsaParams(jsg::Lock& js, int modulusLength, kj::ArrayPtr<kj::byte> publicExponent,
                       bool isImport = false) {
  // Use Chromium's limits for RSA keygen to avoid infinite loops:
  // * Key sizes a multiple of 8 bits.
  // * Key sizes must be in [256, 16k] bits.
  auto strictCrypto = FeatureFlags::get(js).getStrictCrypto();
  JSG_REQUIRE(!(strictCrypto || !isImport) || (modulusLength % 8 == 0 && modulusLength >= 256 &&
      modulusLength <= 16384), DOMOperationError, "The modulus length must be a multiple of 8 and "
      "between 256 and 16k, but ", modulusLength, " was requested.");

  // Now check the public exponent for allow-listed values.
  // First see if we can convert the public exponent to an unsigned number. Unfortunately OpenSSL
  // doesn't have convenient APIs to do this (since these are bignums) so we have to do it by hand.
  // Since the problematic BIGNUMs are within the range of an unsigned int (& technicall an
  // unsigned short) we can treat an out-of-range issue as valid input.
  KJ_IF_SOME(v, fromBignum<unsigned>(publicExponent)) {
    if (!isImport) {
      JSG_REQUIRE(v == 3 || v == 65537, DOMOperationError,
          "The \"publicExponent\" must be either 3 or 65537, but got ", v, ".");
    } else if (strictCrypto) {
      // While we have long required the exponent to be 3 or 65537 when generating keys, handle
      // imported keys more permissively and allow additional exponents that are considered safe
      // and commonly used.
      JSG_REQUIRE(v == 3 || v == 17 || v == 37 || v == 65537, DOMOperationError,
          "Imported RSA key has invalid publicExponent ", v, ".");
    }
  } else {
    JSG_FAIL_REQUIRE(DOMOperationError, "The \"publicExponent\" must be either 3 or 65537, but "
        "got a number larger than 2^32.");
  }
}

} // namespace

kj::OneOf<jsg::Ref<CryptoKey>, CryptoKeyPair> CryptoKey::Impl::generateRsa(
    jsg::Lock& js, kj::StringPtr normalizedName,
    SubtleCrypto::GenerateKeyAlgorithm&& algorithm, bool extractable,
    kj::ArrayPtr<const kj::String> keyUsages) {

  KJ_ASSERT(normalizedName == "RSASSA-PKCS1-v1_5" || normalizedName == "RSA-PSS" ||
      normalizedName == "RSA-OAEP", "generateRsa called on non-RSA cryptoKey", normalizedName);

  auto publicExponent = JSG_REQUIRE_NONNULL(kj::mv(algorithm.publicExponent), TypeError,
        "Missing field \"publicExponent\" in \"algorithm\".");
  kj::StringPtr hash = api::getAlgorithmName(JSG_REQUIRE_NONNULL(algorithm.hash, TypeError,
        "Missing field \"hash\" in \"algorithm\"."));
  int modulusLength = JSG_REQUIRE_NONNULL(algorithm.modulusLength, TypeError,
      "Missing field \"modulusLength\" in \"algorithm\".");
  JSG_REQUIRE(modulusLength > 0, DOMOperationError, "modulusLength must be greater than zero "
      "(requested ", modulusLength, ").");
  auto [normalizedHashName, hashEvpMd] = lookupDigestAlgorithm(hash);

  CryptoKeyUsageSet validUsages = (normalizedName == "RSA-OAEP") ?
      (CryptoKeyUsageSet::encrypt() | CryptoKeyUsageSet::decrypt() |
       CryptoKeyUsageSet::wrapKey() | CryptoKeyUsageSet::unwrapKey()) :
      (CryptoKeyUsageSet::sign() | CryptoKeyUsageSet::verify());
  auto usages = CryptoKeyUsageSet::validate(normalizedName, CryptoKeyUsageSet::Context::generate,
                                            keyUsages, validUsages);

  validateRsaParams(js, modulusLength, publicExponent.asPtr());
  // boringssl silently uses (modulusLength & ~127) for the key size, i.e. it rounds down to the
  // closest multiple of 128 bits. This can easily cause confusion when non-standard key sizes are
  // requested.
  // The `modulusLength` field of the resulting CryptoKey will be incorrect when the compat flag
  // is disabled and the key size is rounded down, but since it is not currently used this is
  // acceptable.
  JSG_REQUIRE(!(FeatureFlags::get(js).getStrictCrypto() && (modulusLength & 127)), DOMOperationError,
      "Can't generate key: RSA key size is required to be a multiple of 128");

  auto bnExponent = JSG_REQUIRE_NONNULL(toBignum(publicExponent), InternalDOMOperationError,
      "Error setting up RSA keygen.");

  auto rsaPrivateKey = OSSL_NEW(RSA);
  OSSLCALL(RSA_generate_key_ex(rsaPrivateKey, modulusLength, bnExponent.get(), 0));
  auto privateEvpPKey = OSSL_NEW(EVP_PKEY);
  OSSLCALL(EVP_PKEY_set1_RSA(privateEvpPKey.get(), rsaPrivateKey.get()));
  kj::Own<RSA> rsaPublicKey = OSSLCALL_OWN(RSA, RSAPublicKey_dup(rsaPrivateKey.get()),
      InternalDOMOperationError, "Error finalizing RSA keygen", internalDescribeOpensslErrors());
  auto publicEvpPKey = OSSL_NEW(EVP_PKEY);
  OSSLCALL(EVP_PKEY_set1_RSA(publicEvpPKey.get(), rsaPublicKey));

  auto keyAlgorithm = CryptoKey::RsaKeyAlgorithm {
    .name = normalizedName,
    .modulusLength = static_cast<uint16_t>(modulusLength),
    .publicExponent = kj::mv(publicExponent),
    .hash = KeyAlgorithm { normalizedHashName }
  };

  return generateRsaPair(js, normalizedName, kj::mv(privateEvpPKey), kj::mv(publicEvpPKey),
      kj::mv(keyAlgorithm), extractable, usages);
}

kj::Own<EVP_PKEY> rsaJwkReader(SubtleCrypto::JsonWebKey&& keyDataJwk) {
  auto rsaKey = OSSL_NEW(RSA);

  auto modulus = UNWRAP_JWK_BIGNUM(kj::mv(keyDataJwk.n),
      DOMDataError, "Invalid RSA key in JSON Web Key; missing or invalid Modulus "
      "parameter (\"n\").");
  auto publicExponent = UNWRAP_JWK_BIGNUM(kj::mv(keyDataJwk.e),
      DOMDataError, "Invalid RSA key in JSON Web Key; missing or invalid "
      "Exponent parameter (\"e\").");

  // RSA_set0_*() transfers BIGNUM ownership to the RSA key, so we don't need to worry about
  // calling BN_free().
  OSSLCALL(RSA_set0_key(rsaKey.get(),
      toBignumUnowned(modulus),
      toBignumUnowned(publicExponent),
      nullptr));

  if (keyDataJwk.d != kj::none) {
    // This is a private key.

    auto privateExponent = UNWRAP_JWK_BIGNUM(kj::mv(keyDataJwk.d),
        DOMDataError, "Invalid RSA key in JSON Web Key; missing or invalid "
        "Private Exponent parameter (\"d\").");

    OSSLCALL(RSA_set0_key(rsaKey.get(), nullptr, nullptr, toBignumUnowned(privateExponent)));

    auto presence = (keyDataJwk.p != kj::none) + (keyDataJwk.q != kj::none) +
                    (keyDataJwk.dp != kj::none) + (keyDataJwk.dq != kj::none) +
                    (keyDataJwk.qi != kj::none);

    if (presence == 5) {
      auto firstPrimeFactor = UNWRAP_JWK_BIGNUM(kj::mv(keyDataJwk.p),
          DOMDataError, "Invalid RSA key in JSON Web Key; invalid First Prime "
          "Factor parameter (\"p\").");
      auto secondPrimeFactor = UNWRAP_JWK_BIGNUM(kj::mv(keyDataJwk.q),
          DOMDataError, "Invalid RSA key in JSON Web Key; invalid Second Prime "
          "Factor parameter (\"q\").");
      auto firstFactorCrtExponent = UNWRAP_JWK_BIGNUM(kj::mv(keyDataJwk.dp),
          DOMDataError, "Invalid RSA key in JSON Web Key; invalid First Factor "
          "CRT Exponent parameter (\"dp\").");
      auto secondFactorCrtExponent = UNWRAP_JWK_BIGNUM(kj::mv(keyDataJwk.dq),
          DOMDataError, "Invalid RSA key in JSON Web Key; invalid Second Factor "
          "CRT Exponent parameter (\"dq\").");
      auto firstCrtCoefficient = UNWRAP_JWK_BIGNUM(kj::mv(keyDataJwk.qi),
          DOMDataError, "Invalid RSA key in JSON Web Key; invalid First CRT "
          "Coefficient parameter (\"qi\").");

      OSSLCALL(RSA_set0_factors(rsaKey.get(),
          toBignumUnowned(firstPrimeFactor),
          toBignumUnowned(secondPrimeFactor)));
      OSSLCALL(RSA_set0_crt_params(rsaKey.get(),
          toBignumUnowned(firstFactorCrtExponent),
          toBignumUnowned(secondFactorCrtExponent),
          toBignumUnowned(firstCrtCoefficient)));
    } else {
      JSG_REQUIRE(presence == 0, DOMDataError,
          "Invalid RSA private key in JSON Web Key; if one Prime "
          "Factor or CRT Exponent/Coefficient parameter is present, then they must all be "
          "present (\"p\", \"q\", \"dp\", \"dq\", \"qi\").");
    }
  }

  auto evpPkey = OSSL_NEW(EVP_PKEY);
  OSSLCALL(EVP_PKEY_set1_RSA(evpPkey.get(), rsaKey.get()));
  return evpPkey;
}

kj::Own<CryptoKey::Impl> CryptoKey::Impl::importRsa(
    jsg::Lock& js, kj::StringPtr normalizedName, kj::StringPtr format,
    SubtleCrypto::ImportKeyData keyData,
    SubtleCrypto::ImportKeyAlgorithm&& algorithm, bool extractable,
    kj::ArrayPtr<const kj::String> keyUsages) {
  kj::StringPtr hash = api::getAlgorithmName(JSG_REQUIRE_NONNULL(algorithm.hash, TypeError,
      "Missing field \"hash\" in \"algorithm\"."));

  CryptoKeyUsageSet allowedUsages = (normalizedName == "RSA-OAEP") ?
      (CryptoKeyUsageSet::encrypt() | CryptoKeyUsageSet::decrypt() |
       CryptoKeyUsageSet::wrapKey() | CryptoKeyUsageSet::unwrapKey()) :
      (CryptoKeyUsageSet::sign() | CryptoKeyUsageSet::verify());

  auto [normalizedHashName, hashEvpMd] = lookupDigestAlgorithm(hash);

  auto importedKey = importAsymmetricForWebCrypto(
      js, kj::mv(format), kj::mv(keyData), normalizedName, extractable, keyUsages,
      // Verbose lambda capture needed because: https://bugs.llvm.org/show_bug.cgi?id=35984
      [hashEvpMd = hashEvpMd, &algorithm](SubtleCrypto::JsonWebKey keyDataJwk) -> kj::Own<EVP_PKEY> {
    JSG_REQUIRE(keyDataJwk.kty == "RSA", DOMDataError,
        "RSASSA-PKCS1-v1_5 \"jwk\" key import requires a JSON Web Key with Key Type parameter "
        "\"kty\" (\"", keyDataJwk.kty, "\") equal to \"RSA\".");

    KJ_IF_SOME(alg, keyDataJwk.alg) {
      // If this JWK specifies an algorithm, make sure it jives with the hash we were passed via
      // importKey().
      static std::map<kj::StringPtr, const EVP_MD*> rsaShaAlgorithms{
        {"RS1", EVP_sha1()},
        {"RS256", EVP_sha256()},
        {"RS384", EVP_sha384()},
        {"RS512", EVP_sha512()},
      };
      static std::map<kj::StringPtr, const EVP_MD*> rsaPssAlgorithms{
        {"PS1", EVP_sha1()},
        {"PS256", EVP_sha256()},
        {"PS384", EVP_sha384()},
        {"PS512", EVP_sha512()},
      };
      static std::map<kj::StringPtr, const EVP_MD*> rsaOaepAlgorithms{
        {"RSA-OAEP", EVP_sha1()},
        {"RSA-OAEP-256", EVP_sha256()},
        {"RSA-OAEP-384", EVP_sha384()},
        {"RSA-OAEP-512", EVP_sha512()},
      };
      const auto& validAlgorithms = [&] {
        if (algorithm.name == "RSASSA-PKCS1-v1_5") {
          return rsaShaAlgorithms;
        } else if (algorithm.name == "RSA-PSS") {
          return rsaPssAlgorithms;
        } else if (algorithm.name == "RSA-OAEP") {
          return rsaOaepAlgorithms;
        } else {
          JSG_FAIL_REQUIRE(DOMNotSupportedError, "Unrecognized RSA variant \"", algorithm.name,
              "\".");
        }
      }();
      auto jwkHash = validAlgorithms.find(alg);
      JSG_REQUIRE(jwkHash != rsaPssAlgorithms.end(), DOMNotSupportedError,
          "Unrecognized or unimplemented algorithm \"", alg, "\" listed in JSON Web Key Algorithm "
          "parameter.");

      JSG_REQUIRE(jwkHash->second == hashEvpMd, DOMDataError,
          "JSON Web Key Algorithm parameter \"alg\" (\"", alg, "\") does not match requested hash "
          "algorithm \"", jwkHash->first, "\".");
    }

    return rsaJwkReader(kj::mv(keyDataJwk));
  }, allowedUsages);

  // get0 avoids adding a refcount...
  RSA& rsa = JSG_REQUIRE_NONNULL(EVP_PKEY_get0_RSA(importedKey.evpPkey.get()), DOMDataError,
      "Input was not an RSA key", tryDescribeOpensslErrors());

  // TODO(conform): We're supposed to check if PKCS8/SPKI input specified a hash and, if so,
  //   compare it against the hash requested in `algorithm`. But, I can't find the OpenSSL
  //   interface to extract the hash from the ASN.1. Oh well...

  auto modulusLength = RSA_size(&rsa) * 8;
  KJ_ASSERT(modulusLength <= ~uint16_t(0));

  const BIGNUM *n, *e, *d;
  RSA_get0_key(&rsa, &n, &e, &d);

  auto publicExponent = KJ_REQUIRE_NONNULL(bignumToArray(*e));

  // Validate modulus and exponent, reject imported RSA keys that may be unsafe.
  validateRsaParams(js, modulusLength, publicExponent, true);

  auto keyAlgorithm = CryptoKey::RsaKeyAlgorithm {
    .name = normalizedName,
    .modulusLength = static_cast<uint16_t>(modulusLength),
    .publicExponent = kj::mv(publicExponent),
    .hash = KeyAlgorithm { normalizedHashName }
  };
  if (normalizedName == "RSASSA-PKCS1-v1_5") {
    return kj::heap<RsassaPkcs1V15Key>(
        kj::mv(importedKey), kj::mv(keyAlgorithm), extractable);
  } else if (normalizedName == "RSA-PSS") {
    return kj::heap<RsaPssKey>(kj::mv(importedKey), kj::mv(keyAlgorithm), extractable);
  } else if (normalizedName == "RSA-OAEP") {
    return kj::heap<RsaOaepKey>(kj::mv(importedKey), kj::mv(keyAlgorithm), extractable);
  } else {
    JSG_FAIL_REQUIRE(DOMNotSupportedError, "Unrecognized RSA variant \"", normalizedName, "\".");
  }
}

kj::Own<CryptoKey::Impl> CryptoKey::Impl::importRsaRaw(
    jsg::Lock& js, kj::StringPtr normalizedName, kj::StringPtr format,
    SubtleCrypto::ImportKeyData keyData,
    SubtleCrypto::ImportKeyAlgorithm&& algorithm, bool extractable,
    kj::ArrayPtr<const kj::String> keyUsages) {
  // Note that in this context raw refers to the RSA-RAW algorithm, not to keys represented by raw
  // data. Importing raw keys is currently not supported for this algorithm.
  CryptoKeyUsageSet allowedUsages = CryptoKeyUsageSet::sign() | CryptoKeyUsageSet::verify();
  auto importedKey = importAsymmetricForWebCrypto(
      js, kj::mv(format), kj::mv(keyData), normalizedName, extractable, keyUsages,
      // Verbose lambda capture needed because: https://bugs.llvm.org/show_bug.cgi?id=35984
      [](SubtleCrypto::JsonWebKey keyDataJwk) -> kj::Own<EVP_PKEY> {
    JSG_REQUIRE(keyDataJwk.kty == "RSA", DOMDataError,
        "RSA-RAW \"jwk\" key import requires a JSON Web Key with Key Type parameter "
        "\"kty\" (\"", keyDataJwk.kty, "\") equal to \"RSA\".");

    KJ_IF_SOME(alg, keyDataJwk.alg) {
      // If this JWK specifies an algorithm, make sure it jives with the hash we were passed via
      // importKey().
      static std::map<kj::StringPtr, const EVP_MD*> rsaAlgorithms{
        {"RS1", EVP_sha1()},
        {"RS256", EVP_sha256()},
        {"RS384", EVP_sha384()},
        {"RS512", EVP_sha512()},
      };
      auto jwkHash = rsaAlgorithms.find(alg);
      JSG_REQUIRE(jwkHash != rsaAlgorithms.end(), DOMNotSupportedError,
          "Unrecognized or unimplemented algorithm \"", alg,
          "\" listed in JSON Web Key Algorithm parameter.");
    }
    return rsaJwkReader(kj::mv(keyDataJwk));
  }, allowedUsages);

  JSG_REQUIRE(importedKey.keyType == KeyType::PRIVATE, DOMDataError,
      "RSA-RAW only supports private keys but requested \"",
      toStringPtr(importedKey.keyType), "\".");

  // get0 avoids adding a refcount...
  RSA& rsa = JSG_REQUIRE_NONNULL(EVP_PKEY_get0_RSA(importedKey.evpPkey.get()), DOMDataError,
      "Input was not an RSA key", tryDescribeOpensslErrors());

  auto modulusLength = RSA_size(&rsa) * 8;
  KJ_ASSERT(modulusLength <= ~uint16_t(0));

  const BIGNUM *n, *e, *d;
  RSA_get0_key(&rsa, &n, &e, &d);

  auto publicExponent = KJ_REQUIRE_NONNULL(bignumToArray(*e));

  // Validate modulus and exponent, reject imported RSA keys that may be unsafe.
  validateRsaParams(js, modulusLength, publicExponent, true);

  auto keyAlgorithm = CryptoKey::RsaKeyAlgorithm {
    .name = "RSA-RAW"_kj,
    .modulusLength = static_cast<uint16_t>(modulusLength),
    .publicExponent = kj::mv(publicExponent)
  };

  return kj::heap<RsaRawKey>(kj::mv(importedKey), kj::mv(keyAlgorithm), extractable);
}

// =====================================================================================
// ECDSA & ECDH

namespace {

class EllipticKey final: public AsymmetricKeyCryptoKeyImpl {
public:
  explicit EllipticKey(AsymmetricKeyData keyData,
                       CryptoKey::EllipticKeyAlgorithm keyAlgorithm,
                       uint rsSize,
                       bool extractable)
      : AsymmetricKeyCryptoKeyImpl(kj::mv(keyData), extractable),
        keyAlgorithm(kj::mv(keyAlgorithm)), rsSize(rsSize) {}

  CryptoKey::AlgorithmVariant getAlgorithm(jsg::Lock& js) const override { return keyAlgorithm; }
  kj::StringPtr getAlgorithmName() const override { return keyAlgorithm.name; }

  void requireSigningAbility() const {
    // This assert is internal to our WebCrypto implementation because we share the AsymmetricKey
    // implementation between ECDH & ECDSA (the former only supports deriveBits/deriveKey, not
    // signing which is the usage for this function).
    JSG_REQUIRE(keyAlgorithm.name == "ECDSA", DOMNotSupportedError,
        "The sign and verify operations are not implemented for \"", keyAlgorithm.name, "\".");
  }

  kj::StringPtr chooseHash(
      const kj::Maybe<kj::OneOf<kj::String, SubtleCrypto::HashAlgorithm>>& callTimeHash)
      const override {
    requireSigningAbility();

    // ECDSA infamously expects the hash to be specified at call time.
    // See: https://github.com/w3c/webcrypto/issues/111
    return api::getAlgorithmName(JSG_REQUIRE_NONNULL(callTimeHash, TypeError,
        "Missing \"hash\" in AlgorithmIdentifier. (ECDSA requires that the hash algorithm be "
        "specified at call time rather than on the key. This differs from other WebCrypto "
        "algorithms for historical reasons.)"));
  }

  kj::Array<kj::byte> deriveBits(
      jsg::Lock& js, SubtleCrypto::DeriveKeyAlgorithm&& algorithm,
      kj::Maybe<uint32_t> resultBitLength) const override final {
    JSG_REQUIRE(keyAlgorithm.name == "ECDH", DOMNotSupportedError, ""
        "The deriveBits operation is not implemented for \"", keyAlgorithm.name, "\".");

    JSG_REQUIRE(getTypeEnum() == KeyType::PRIVATE, DOMInvalidAccessError, ""
        "The deriveBits operation is only valid for a private key, not \"", getType(), "\".");

    auto& publicKey = JSG_REQUIRE_NONNULL(algorithm.$public, TypeError,
        "Missing field \"public\" in \"derivedKeyParams\".");

    JSG_REQUIRE(publicKey->getType() == "public"_kj, DOMInvalidAccessError, ""
        "The provided key has type \"", publicKey->getType(), "\", not \"public\"");

    JSG_REQUIRE(getAlgorithm(js).which() == publicKey->getAlgorithm(js).which(),
        DOMInvalidAccessError, "Base ", getAlgorithmName(), " private key cannot be used to derive"
        " a key from a peer ", publicKey->getAlgorithmName(), " public key");

    JSG_REQUIRE(getAlgorithmName() == publicKey->getAlgorithmName(), DOMInvalidAccessError,
        "Private key for derivation is using \"", getAlgorithmName(),
        "\" while public key is using \"", publicKey->getAlgorithmName(), "\".");

    auto publicCurve = publicKey->getAlgorithm(js).get<CryptoKey::EllipticKeyAlgorithm>()
        .namedCurve;
    JSG_REQUIRE(keyAlgorithm.namedCurve == publicCurve, DOMInvalidAccessError,
        "Private key for derivation is using curve \"", keyAlgorithm.namedCurve,
        "\" while public key is using \"", publicCurve, "\".");

    // The check above for the algorithm `which` equality ensures that the impl can be downcast to
    // EllipticKey (assuming we don't accidentally create a class that doesn't inherit this one that
    // for some reason returns an EllipticKey).
    auto& publicKeyImpl = kj::downcast<EllipticKey>(*publicKey->impl);

    // Adapted from https://wiki.openssl.org/index.php/Elliptic_Curve_Diffie_Hellman:
    auto& privateEcKey = JSG_REQUIRE_NONNULL(EVP_PKEY_get0_EC_KEY(getEvpPkey()),
        InternalDOMOperationError, "No elliptic curve data backing key",
        tryDescribeOpensslErrors());
    auto& publicEcKey = JSG_REQUIRE_NONNULL(EVP_PKEY_get0_EC_KEY(publicKeyImpl.getEvpPkey()),
        InternalDOMOperationError, "No elliptic curve data backing key",
        tryDescribeOpensslErrors());
    auto& publicEcPoint = JSG_REQUIRE_NONNULL(EC_KEY_get0_public_key(&publicEcKey),
        DOMOperationError, "No public elliptic curve key data in this key",
        tryDescribeOpensslErrors());
    auto fieldSize = EC_GROUP_get_degree(EC_KEY_get0_group(&privateEcKey));

    // Assuming that `fieldSize` will always be a sane value since it's related to the keys we
    // construct in C++ (i.e. not untrusted user input).

    kj::Vector<kj::byte> sharedSecret;
    sharedSecret.resize(
        integerCeilDivision<std::make_unsigned<decltype(fieldSize)>::type>(fieldSize, 8u));
    auto written = ECDH_compute_key(sharedSecret.begin(), sharedSecret.capacity(),
        &publicEcPoint, &privateEcKey, nullptr);
    JSG_REQUIRE(written > 0, DOMOperationError, "Failed to generate shared ECDH secret",
        tryDescribeOpensslErrors());

    sharedSecret.resize(written);

    auto outputBitLength = resultBitLength.orDefault(sharedSecret.size() * 8);
    JSG_REQUIRE(outputBitLength <= sharedSecret.size() * 8, DOMOperationError,
        "Derived key length (", outputBitLength, " bits) is too long (should be at most ",
        sharedSecret.size() * 8, " bits).");

    // Round up since outputBitLength may not be a perfect multiple of 8.
    // However, the last byte may now have bits that have leaked which we handle below.
    auto resultByteLength = integerCeilDivision(outputBitLength, 8u);
    sharedSecret.truncate(resultByteLength);

    // We have to remember to mask off the bits that weren't requested (if a non multiple of 8 was
    // passed in). NOTE: The conformance tests DO NOT appear to test for this. This is my reading of
    // the spec, combining:
    //   * ECDH: Return an octet string containing the first length bits of secret.
    //   * octet string: b is the octet string obtained by first appending zero or more bits of
    //                   value zero to b such that the length of the resulting bit string is minimal
    //                   and an integer multiple of 8.
    auto numBitsToMaskOff = resultByteLength * 8 - outputBitLength;
    KJ_DASSERT(numBitsToMaskOff < 8, numBitsToMaskOff);

    // The mask should have `numBitsToMaskOff` bits set to 0 from least significant to most.
    // 0 = 1 1 1 1 1 1 1 1 (0xFF)
    // 1 = 1 1 1 1 1 1 1 0 (0xFE)
    // 2 = 1 1 1 1 1 1 0 0 (0xFD)
    // 3 = 1 1 1 1 1 0 0 0 (0xFC)
    // Let's rewrite this to have the lower bits set to 1 since that's typically the easier form to
    // generate with bit twiddling.
    // 0 = 0 0 0 0 0 0 0 0 (0)
    // 1 = 0 0 0 0 0 0 0 1 (1)
    // 2 = 0 0 0 0 0 0 1 1 (3)
    // 3 = 0 0 0 0 0 1 1 1 (7)
    // The pattern seems pretty clearly ~(2^n - 1) where n is the number of bits to mask off. Let's
    // check the last one though (8 is not a possible boundary condition).
    // (2^7 - 1) = 0x7f => ~0x7f = 0x80 (when truncated to a byte)
    uint8_t mask = ~((1 << numBitsToMaskOff) - 1);

    sharedSecret.back() &= mask;

    return sharedSecret.releaseAsArray();
  }

  kj::Array<kj::byte> signatureSslToWebCrypto(kj::Array<kj::byte> signature) const override {
    // An EC signature is two big integers "r" and "s". WebCrypto wants us to just concatenate both
    // integers, using a constant size of each that depends on the curve size. OpenSSL wants to
    // encode them in some ASN.1 wrapper with variable-width sizes. Ugh.

    requireSigningAbility();

    // Manually decode ASN.1 BER.
    KJ_ASSERT(signature.size() >= 6);
    KJ_ASSERT(signature[0] == 0x30);
    kj::ArrayPtr<const kj::byte> rest;
    if (signature[1] < 128) {
      KJ_ASSERT(signature[1] == signature.size() - 2);
      rest = signature.slice(2, signature.size());
    } else {
      // Size of message did not fit in 7 bits, so the first byte encodes the size-of-size, but it
      // will always fit in 8 bits so the size-of-size will always be 1 (plus 128 because top bit
      // is set).
      KJ_ASSERT(signature[1] == 129);
      KJ_ASSERT(signature[2] == signature.size() - 3);
      rest = signature.slice(3, signature.size());
    }

    KJ_ASSERT(rest.size() >= 2);
    KJ_ASSERT(rest[0] == 0x02);
    size_t rSize = rest[1];
    KJ_ASSERT(rest.size() >= 2 + rSize);
    auto r = rest.slice(2, 2 + rSize);

    rest = rest.slice(2 + rSize, rest.size());

    KJ_ASSERT(rest.size() >= 2);
    KJ_ASSERT(rest[0] == 0x02);
    size_t sSize = rest[1];
    KJ_ASSERT(rest.size() == 2 + sSize);
    auto s = rest.slice(2, 2 + sSize);

    // If the top bit is set, BER encoding will add an extra 0-byte prefix to disambiguate from a
    // negative number. Uggghhh.
    while (r.size() > rsSize && r[0] == 0) r = r.slice(1, r.size());
    while (s.size() > rsSize && s[0] == 0) s = s.slice(1, s.size());
    KJ_ASSERT(r.size() <= rsSize);
    KJ_ASSERT(s.size() <= rsSize);

    // Construct WebCrypto format.
    auto out = kj::heapArray<kj::byte>(rsSize * 2);
    out.asPtr().fill(0);

    // We're dealing with big-endian, so we have to align the copy to the right. This is exactly
    // why big-endian is the wrong edian.
    memcpy(out.begin() + rsSize - r.size(), r.begin(), r.size());
    memcpy(out.end() - s.size(), s.begin(), s.size());
    return out;
  }

  kj::Array<const kj::byte> signatureWebCryptoToSsl(
      kj::ArrayPtr<const kj::byte> signature) const override {
    requireSigningAbility();

    if (signature.size() != rsSize * 2) {
      // The signature is the wrong size. Return an empty signature, which will be judged invalid.
      return nullptr;
    }

    auto r = signature.slice(0, rsSize);
    auto s = signature.slice(rsSize, signature.size());

    // Trim leading zeros.
    while (r.size() > 1 && r[0] == 0) r = r.slice(1, r.size());
    while (s.size() > 1 && s[0] == 0) s = s.slice(1, s.size());

    // If the most significant bit is set, we have to add a zero, ugh.
    bool padR = r[0] >= 128;
    bool padS = s[0] >= 128;

    size_t bodySize = 4 + padR + padS + r.size() + s.size();
    size_t resultSize = 2 + bodySize + (bodySize >= 128);
    auto result = kj::heapArray<kj::byte>(resultSize);

    kj::byte* pos = result.begin();
    *pos++ = 0x30;
    if (bodySize < 128) {
      *pos++ = bodySize;
    } else {
      *pos++ = 129;
      *pos++ = bodySize;
    }

    *pos++ = 0x02;
    *pos++ = r.size() + padR;
    if (padR) *pos++ = 0;
    memcpy(pos, r.begin(), r.size());
    pos += r.size();

    *pos++ = 0x02;
    *pos++ = s.size() + padS;
    if (padS) *pos++ = 0;
    memcpy(pos, s.begin(), s.size());
    pos += s.size();

    KJ_ASSERT(pos == result.end());

    return result;
  }

  static kj::OneOf<jsg::Ref<CryptoKey>, CryptoKeyPair> generateElliptic(
      kj::StringPtr normalizedName,
      SubtleCrypto::GenerateKeyAlgorithm&& algorithm, bool extractable,
      CryptoKeyUsageSet privateKeyUsages, CryptoKeyUsageSet publicKeyUsages);

  kj::StringPtr jsgGetMemoryName() const override { return "EllipticKey"; }
  size_t jsgGetMemorySelfSize() const override { return sizeof(EllipticKey); }
  void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const override {
    AsymmetricKeyCryptoKeyImpl::jsgGetMemoryInfo(tracker);
    tracker.trackField("keyAlgorithm", keyAlgorithm);
  }

private:
  SubtleCrypto::JsonWebKey exportJwk() const override final {
    const EC_KEY& ec = JSG_REQUIRE_NONNULL(EVP_PKEY_get0_EC_KEY(getEvpPkey()), DOMOperationError,
        "No elliptic curve data backing key", tryDescribeOpensslErrors());

    const auto& group = JSG_REQUIRE_NONNULL(EC_KEY_get0_group(&ec), DOMOperationError,
        "No elliptic curve group in this key", tryDescribeOpensslErrors());
    const auto& point = JSG_REQUIRE_NONNULL(EC_KEY_get0_public_key(&ec), DOMOperationError,
        "No public elliptic curve key data in this key",
        tryDescribeOpensslErrors());

    auto groupDegreeInBytes = integerCeilDivision(EC_GROUP_get_degree(&group), 8u);
    // EC_GROUP_get_degree returns number of bits. We need this because x, y, & d need to match the
    // group degree according to JWK.

    BIGNUM x = {0};
    BIGNUM y = {0};

    JSG_REQUIRE(1 == EC_POINT_get_affine_coordinates_GFp(&group, &point, &x, &y, nullptr),
        InternalDOMOperationError, "Error getting affine coordinates for export",
        internalDescribeOpensslErrors());

    SubtleCrypto::JsonWebKey jwk;
    jwk.kty = kj::str("EC");
    jwk.crv = kj::str(keyAlgorithm.namedCurve);

    static constexpr auto handleBn = [](const BIGNUM& bn, size_t size) {
      return JSG_REQUIRE_NONNULL(bignumToArrayPadded(bn, size),
        InternalDOMOperationError, "Error converting EC affine co-ordinates to padded array",
        internalDescribeOpensslErrors());
    };

    auto xa = handleBn(x, groupDegreeInBytes);
    auto ya = handleBn(y, groupDegreeInBytes);

    jwk.x = kj::encodeBase64Url(xa);
    jwk.y = kj::encodeBase64Url(ya);
    if (getTypeEnum() == KeyType::PRIVATE) {
      const auto& privateKey = JSG_REQUIRE_NONNULL(EC_KEY_get0_private_key(&ec),
          InternalDOMOperationError, "Error getting private key material for JSON Web Key export",
          internalDescribeOpensslErrors());
      auto pk = handleBn(privateKey, groupDegreeInBytes);
      jwk.d = kj::encodeBase64Url(pk);
    }
    return jwk;
  }

  kj::Array<kj::byte> exportRaw() const override final {
    JSG_REQUIRE(getTypeEnum() == KeyType::PUBLIC, DOMInvalidAccessError,
        "Raw export of elliptic curve keys is only allowed for public keys.");

    const EC_KEY& ec = JSG_REQUIRE_NONNULL(EVP_PKEY_get0_EC_KEY(getEvpPkey()),
        InternalDOMOperationError, "No elliptic curve data backing key",
        tryDescribeOpensslErrors());
    const auto& group = JSG_REQUIRE_NONNULL(EC_KEY_get0_group(&ec), InternalDOMOperationError,
        "No elliptic curve group in this key", tryDescribeOpensslErrors());
    const auto& point = JSG_REQUIRE_NONNULL(EC_KEY_get0_public_key(&ec), InternalDOMOperationError,
        "No public elliptic curve key data in this key",
        tryDescribeOpensslErrors());

    // Serialize the public key as an uncompressed point in X9.62 form.
    uint8_t* raw;
    size_t raw_len;
    CBB cbb;

    JSG_REQUIRE(1 == CBB_init(&cbb, 0), InternalDOMOperationError, "Failed to init CBB",
        internalDescribeOpensslErrors());
    KJ_DEFER(CBB_cleanup(&cbb));

    JSG_REQUIRE(1 == EC_POINT_point2cbb(&cbb, &group, &point, POINT_CONVERSION_UNCOMPRESSED,
        nullptr), InternalDOMOperationError, "Failed to convert to serialize EC key",
        internalDescribeOpensslErrors());

    JSG_REQUIRE(1 == CBB_finish(&cbb, &raw, &raw_len), InternalDOMOperationError,
        "Failed to finish CBB", internalDescribeOpensslErrors());

    return kj::Array<kj::byte>(raw, raw_len, SslArrayDisposer::INSTANCE);
  }

  CryptoKey::AsymmetricKeyDetails getAsymmetricKeyDetail() const override {
    // Adapted from Node.js' GetEcKeyDetail
    KJ_REQUIRE(EVP_PKEY_id(getEvpPkey()) == EVP_PKEY_EC);
    const EC_KEY* ec = EVP_PKEY_get0_EC_KEY(getEvpPkey());
    KJ_ASSERT(ec != nullptr);

    const EC_GROUP* group = EC_KEY_get0_group(ec);
    int nid = EC_GROUP_get_curve_name(group);

    return CryptoKey::AsymmetricKeyDetails {
      .namedCurve = kj::str(OBJ_nid2sn(nid))
    };
  }

  CryptoKey::EllipticKeyAlgorithm keyAlgorithm;
  uint rsSize;
};

struct EllipticCurveInfo {
  kj::StringPtr normalizedName;
  int opensslCurveId;
  uint rsSize;  // size of "r" and "s" in the signature
};

EllipticCurveInfo lookupEllipticCurve(kj::StringPtr curveName) {
  static const std::map<kj::StringPtr, EllipticCurveInfo, CiLess> registeredCurves {
    {"P-256", {"P-256", NID_X9_62_prime256v1, 32}},
    {"P-384", {"P-384", NID_secp384r1, 48}},
    {"P-521", {"P-521", NID_secp521r1, 66}},
  };

  auto iter = registeredCurves.find(curveName);
  JSG_REQUIRE(iter != registeredCurves.end(), DOMNotSupportedError,
      "Unrecognized or unimplemented EC curve \"", curveName, "\" requested.");
  return iter->second;
}

kj::OneOf<jsg::Ref<CryptoKey>, CryptoKeyPair> EllipticKey::generateElliptic(
    kj::StringPtr normalizedName,
    SubtleCrypto::GenerateKeyAlgorithm&& algorithm, bool extractable,
    CryptoKeyUsageSet privateKeyUsages, CryptoKeyUsageSet publicKeyUsages) {
  kj::StringPtr namedCurve = JSG_REQUIRE_NONNULL(algorithm.namedCurve,TypeError,
      "Missing field \"namedCurve\" in \"algorithm\".");

  auto [normalizedNamedCurve, curveId, rsSize] = lookupEllipticCurve(namedCurve);

  auto keyAlgorithm = CryptoKey::EllipticKeyAlgorithm {
    normalizedName,
    normalizedNamedCurve,
  };

  // Used OpenBSD man pages starting with https://man.openbsd.org/ECDSA_SIG_new.3 for functions and
  // CryptoKey::Impl::generateRsa as a template.
  // https://stackoverflow.com/questions/18155559/how-does-one-access-the-raw-ecdh-public-key-private-key-and-params-inside-opens
  // for the reference on how to deserialize the public/private key.

  auto ecPrivateKey = OSSLCALL_OWN(EC_KEY, EC_KEY_new_by_curve_name(curveId),
      InternalDOMOperationError, "Error generating EC \"", namedCurve, "\" key",
      internalDescribeOpensslErrors());
  OSSLCALL(EC_KEY_generate_key(ecPrivateKey));

  auto privateEvpPKey = OSSL_NEW(EVP_PKEY);
  OSSLCALL(EVP_PKEY_set1_EC_KEY(privateEvpPKey.get(), ecPrivateKey.get()));

  auto ecPublicKey = OSSLCALL_OWN(EC_KEY, EC_KEY_new_by_curve_name(curveId),
      InternalDOMOperationError, "Error generating EC \"", namedCurve, "\" key",
      internalDescribeOpensslErrors());
  OSSLCALL(EC_KEY_set_public_key(ecPublicKey, EC_KEY_get0_public_key(ecPrivateKey)));
  auto publicEvpPKey = OSSL_NEW(EVP_PKEY);
  OSSLCALL(EVP_PKEY_set1_EC_KEY(publicEvpPKey.get(), ecPublicKey.get()));

  AsymmetricKeyData privateKeyData {
    .evpPkey = kj::mv(privateEvpPKey),
    .keyType = KeyType::PRIVATE,
    .usages = privateKeyUsages,
  };
  AsymmetricKeyData publicKeyData {
    .evpPkey = kj::mv(publicEvpPKey),
    .keyType = KeyType::PUBLIC,
    .usages = publicKeyUsages,
  };

  auto privateKey = jsg::alloc<CryptoKey>(kj::heap<EllipticKey>(kj::mv(privateKeyData),
      keyAlgorithm, rsSize, extractable));
  auto publicKey = jsg::alloc<CryptoKey>(kj::heap<EllipticKey>(kj::mv(publicKeyData),
      keyAlgorithm, rsSize, true));

  return CryptoKeyPair {.publicKey =  kj::mv(publicKey), .privateKey = kj::mv(privateKey)};
}

AsymmetricKeyData importEllipticRaw(SubtleCrypto::ImportKeyData keyData, int curveId,
    kj::StringPtr normalizedName, kj::ArrayPtr<const kj::String> keyUsages,
    CryptoKeyUsageSet allowedUsages) {
  // Import an elliptic key represented by raw data, only public keys are supported.
  JSG_REQUIRE(keyData.is<kj::Array<kj::byte>>(), DOMDataError,
      "Expected raw EC key but instead got a Json Web Key.");

  const auto& raw = keyData.get<kj::Array<kj::byte>>();

  auto usages = CryptoKeyUsageSet::validate(normalizedName,
      CryptoKeyUsageSet::Context::importPublic, keyUsages, allowedUsages);

  if (curveId == NID_ED25519 || curveId == NID_X25519) {
    auto evpId = curveId == NID_X25519 ? EVP_PKEY_X25519 : EVP_PKEY_ED25519;
    auto curveName = curveId == NID_X25519 ? "X25519" : "Ed25519";

    JSG_REQUIRE(raw.size() == 32, DOMDataError, curveName, " raw keys must be exactly 32-bytes "
        "(provided ", raw.size(), ").");

    return { OSSLCALL_OWN(EVP_PKEY, EVP_PKEY_new_raw_public_key(evpId, nullptr,
        raw.begin(), raw.size()), InternalDOMOperationError, "Failed to import raw public EDDSA",
        raw.size(), internalDescribeOpensslErrors()), KeyType::PUBLIC, usages };
  }

  auto ecKey = OSSLCALL_OWN(EC_KEY, EC_KEY_new_by_curve_name(curveId), DOMOperationError,
      "Error importing EC key", tryDescribeOpensslErrors());
  auto ecGroup = EC_KEY_get0_group(ecKey.get());

  auto point = OSSL_NEW(EC_POINT, ecGroup);
  JSG_REQUIRE(1 == EC_POINT_oct2point(ecGroup, point.get(), raw.begin(),
      raw.size(), nullptr), DOMDataError, "Failed to import raw EC key data",
      tryDescribeOpensslErrors());
  JSG_REQUIRE(1 == EC_KEY_set_public_key(ecKey.get(), point.get()), InternalDOMOperationError,
      "Failed to set EC raw public key", internalDescribeOpensslErrors());
  JSG_REQUIRE(1 == EC_KEY_check_key(ecKey.get()), DOMDataError, "Invalid raw EC key provided",
      tryDescribeOpensslErrors());

  auto evpPkey = OSSL_NEW(EVP_PKEY);
  OSSLCALL(EVP_PKEY_set1_EC_KEY(evpPkey.get(), ecKey.get()));

  return AsymmetricKeyData{ kj::mv(evpPkey), KeyType::PUBLIC, usages };
}

}  // namespace

kj::Own<EVP_PKEY> ellipticJwkReader(int curveId, SubtleCrypto::JsonWebKey&& keyDataJwk,
                                    kj::StringPtr normalizedName) {
  if (curveId == NID_ED25519 || curveId == NID_X25519) {
    auto evpId = curveId == NID_X25519 ? EVP_PKEY_X25519 : EVP_PKEY_ED25519;
    auto curveName = curveId == NID_X25519 ? "X25519" : "Ed25519";

    JSG_REQUIRE(keyDataJwk.kty == "OKP", DOMDataError,
        curveName, " \"jwk\" key imports requires a JSON Web Key with Key Type parameter "
        "\"kty\" (\"", keyDataJwk.kty, "\") equal to \"OKP\".");
    auto& crv = JSG_REQUIRE_NONNULL(keyDataJwk.crv, DOMDataError,
        "Missing field \"crv\" for ", curveName, " key.");
    JSG_REQUIRE(crv == curveName, DOMNotSupportedError,
        "Only ", curveName, " is supported but \"", crv, "\" was requested.");
    KJ_IF_SOME(alg, keyDataJwk.alg) {
      // If this JWK specifies an algorithm, make sure it jives with the hash we were passed via
      // importKey().
      if (curveId == NID_ED25519) {
        JSG_REQUIRE(alg == "EdDSA", DOMDataError,
            "JSON Web Key Algorithm parameter \"alg\" (\"", alg, "\") does not match requested "
            "Ed25519 curve.");
      }
    }

    auto x = UNWRAP_JWK_BIGNUM(kj::mv(keyDataJwk.x), DOMDataError,
        "Invalid ", crv, " key in JSON WebKey; missing or invalid public key component (\"x\").");
    JSG_REQUIRE(x.size() == 32, DOMDataError, "Invalid length ", x.size(), " for public key");

    if (keyDataJwk.d == kj::none) {
      // This is a public key.
      return OSSLCALL_OWN(EVP_PKEY, EVP_PKEY_new_raw_public_key(evpId, nullptr,
          x.begin(), x.size()), InternalDOMOperationError,
          "Failed to construct ", crv, " public key", internalDescribeOpensslErrors());
    }

    // This is a private key. The Section 2 of the RFC says...
    // >  The parameter "x" MUST be present and contain the public key encoded using the base64url
    // >  [RFC4648] encoding.
    // https://tools.ietf.org/html/draft-ietf-jose-cfrg-curves-06
    // ... but there's nothing really to do beside enforce that it's set? The NodeJS implementation
    // seems to throw it away when a private key is provided.

    auto d = UNWRAP_JWK_BIGNUM(kj::mv(keyDataJwk.d), DOMDataError,
        "Invalid ", curveName, " key in JSON Web Key; missing or invalid private key component (\"d\").");
    JSG_REQUIRE(d.size() == 32, DOMDataError, "Invalid length ", d.size(), " for private key");

    return OSSLCALL_OWN(EVP_PKEY, EVP_PKEY_new_raw_private_key(evpId, nullptr,
        d.begin(), d.size()), InternalDOMOperationError,
        "Failed to construct ", crv, " private key", internalDescribeOpensslErrors());
  }

  JSG_REQUIRE(keyDataJwk.kty == "EC", DOMDataError,
      "Elliptic curve \"jwk\" key import requires a JSON Web Key with Key Type parameter "
      "\"kty\" (\"", keyDataJwk.kty, "\") equal to \"EC\".");

  if (normalizedName == "ECDSA") {
    KJ_IF_SOME(alg, keyDataJwk.alg) {
      // If this JWK specifies an algorithm, make sure it jives with the hash we were passed via
      // importKey().
      static std::map<kj::StringPtr, int> ecdsaAlgorithms {
        {"ES256", NID_X9_62_prime256v1},
        {"ES384", NID_secp384r1},
        {"ES512", NID_secp521r1},
      };

      auto iter = ecdsaAlgorithms.find(alg);
      JSG_REQUIRE(iter != ecdsaAlgorithms.end(), DOMNotSupportedError,
          "Unrecognized or unimplemented algorithm \"", alg,
          "\" listed in JSON Web Key Algorithm parameter.");

      JSG_REQUIRE(iter->second == curveId, DOMDataError,
          "JSON Web Key Algorithm parameter \"alg\" (\"", alg,
          "\") does not match requested curve.");
    }
  }

  auto ecKey = OSSLCALL_OWN(EC_KEY, EC_KEY_new_by_curve_name(curveId), DOMOperationError,
      "Error importing EC key", tryDescribeOpensslErrors());

  auto x = UNWRAP_JWK_BIGNUM(kj::mv(keyDataJwk.x), DOMDataError,
      "Invalid EC key in JSON Web Key; missing \"x\".");
  auto y = UNWRAP_JWK_BIGNUM(kj::mv(keyDataJwk.y), DOMDataError,
      "Invalid EC key in JSON Web Key; missing \"y\".");

  auto group = EC_KEY_get0_group(ecKey);

  auto bigX = JSG_REQUIRE_NONNULL(toBignum(x), InternalDOMOperationError,
      "Error importing EC key", internalDescribeOpensslErrors());
  auto bigY = JSG_REQUIRE_NONNULL(toBignum(y), InternalDOMOperationError,
      "Error importing EC key", internalDescribeOpensslErrors());

  auto point = OSSL_NEW(EC_POINT, group);
  OSSLCALL(EC_POINT_set_affine_coordinates_GFp(group, point, bigX, bigY, nullptr));
  OSSLCALL(EC_KEY_set_public_key(ecKey, point));

  if (keyDataJwk.d != kj::none) {
    // This is a private key.

    auto d = UNWRAP_JWK_BIGNUM(kj::mv(keyDataJwk.d), DOMDataError,
        "Invalid EC key in JSON Web Key; missing or invalid private key component (\"d\").");

    auto bigD = JSG_REQUIRE_NONNULL(toBignum(d), InternalDOMOperationError,
        "Error importing EC key", internalDescribeOpensslErrors());

    OSSLCALL(EC_KEY_set_private_key(ecKey, bigD));
  }

  auto evpPkey = OSSL_NEW(EVP_PKEY);
  OSSLCALL(EVP_PKEY_set1_EC_KEY(evpPkey.get(), ecKey.get()));
  return evpPkey;
}

kj::OneOf<jsg::Ref<CryptoKey>, CryptoKeyPair> CryptoKey::Impl::generateEcdsa(
    jsg::Lock& js, kj::StringPtr normalizedName,
    SubtleCrypto::GenerateKeyAlgorithm&& algorithm, bool extractable,
    kj::ArrayPtr<const kj::String> keyUsages) {
  auto usages =
      CryptoKeyUsageSet::validate(normalizedName, CryptoKeyUsageSet::Context::generate, keyUsages,
                                  CryptoKeyUsageSet::sign() | CryptoKeyUsageSet::verify());
  auto privateKeyUsages = usages & CryptoKeyUsageSet::privateKeyMask();
  auto publicKeyUsages = usages & CryptoKeyUsageSet::publicKeyMask();

  return EllipticKey::generateElliptic(normalizedName, kj::mv(algorithm), extractable,
      privateKeyUsages, publicKeyUsages);
}

kj::Own<CryptoKey::Impl> CryptoKey::Impl::importEcdsa(
    jsg::Lock& js, kj::StringPtr normalizedName, kj::StringPtr format,
    SubtleCrypto::ImportKeyData keyData,
    SubtleCrypto::ImportKeyAlgorithm&& algorithm, bool extractable,
    kj::ArrayPtr<const kj::String> keyUsages) {
  kj::StringPtr namedCurve = JSG_REQUIRE_NONNULL(algorithm.namedCurve, TypeError,
      "Missing field \"namedCurve\" in \"algorithm\".");

  auto [normalizedNamedCurve, curveId, rsSize] = lookupEllipticCurve(namedCurve);

  auto importedKey = [&, curveId = curveId] {
    if (format != "raw") {
      return importAsymmetricForWebCrypto(
          js, format, kj::mv(keyData), normalizedName, extractable, keyUsages,
          // Verbose lambda capture needed because: https://bugs.llvm.org/show_bug.cgi?id=35984
          [curveId = curveId, normalizedName = kj::str(normalizedName)]
          (SubtleCrypto::JsonWebKey keyDataJwk) -> kj::Own<EVP_PKEY> {
        return ellipticJwkReader(curveId, kj::mv(keyDataJwk), normalizedName);
      }, CryptoKeyUsageSet::sign() | CryptoKeyUsageSet::verify());
    } else {
      return importEllipticRaw(kj::mv(keyData), curveId, normalizedName, keyUsages,
          CryptoKeyUsageSet::verify());
    }
  }();

  // get0 avoids adding a refcount...
  EC_KEY& ecKey = JSG_REQUIRE_NONNULL(EVP_PKEY_get0_EC_KEY(importedKey.evpPkey.get()), DOMDataError,
      "Input was not an EC key", tryDescribeOpensslErrors());

  // Verify namedCurve matches what was specified in the key data.
  const EC_GROUP* group = EC_KEY_get0_group(&ecKey);
  JSG_REQUIRE(group != nullptr && EC_GROUP_get_curve_name(group) == curveId, DOMDataError,
      "\"algorithm.namedCurve\" \"", namedCurve, "\" does not match the curve specified by the "
      "input key data", tryDescribeOpensslErrors());

  auto keyAlgorithm = CryptoKey::EllipticKeyAlgorithm {
    normalizedName,
    normalizedNamedCurve,
  };

  return kj::heap<EllipticKey>(kj::mv(importedKey), kj::mv(keyAlgorithm), rsSize, extractable);
}

kj::OneOf<jsg::Ref<CryptoKey>, CryptoKeyPair> CryptoKey::Impl::generateEcdh(
    jsg::Lock& js, kj::StringPtr normalizedName,
    SubtleCrypto::GenerateKeyAlgorithm&& algorithm, bool extractable,
    kj::ArrayPtr<const kj::String> keyUsages) {
  auto usages =
      CryptoKeyUsageSet::validate(normalizedName, CryptoKeyUsageSet::Context::generate, keyUsages,
                                  CryptoKeyUsageSet::derivationKeyMask());
  return EllipticKey::generateElliptic(normalizedName, kj::mv(algorithm), extractable, usages, {});
}

kj::Own<CryptoKey::Impl> CryptoKey::Impl::importEcdh(
    jsg::Lock& js, kj::StringPtr normalizedName, kj::StringPtr format,
    SubtleCrypto::ImportKeyData keyData,
    SubtleCrypto::ImportKeyAlgorithm&& algorithm, bool extractable,
    kj::ArrayPtr<const kj::String> keyUsages) {
  kj::StringPtr namedCurve = JSG_REQUIRE_NONNULL(algorithm.namedCurve, TypeError,
      "Missing field \"namedCurve\" in \"algorithm\".");

  auto [normalizedNamedCurve, curveId, rsSize] = lookupEllipticCurve(namedCurve);

  auto importedKey = [&, curveId = curveId] {
    auto strictCrypto = FeatureFlags::get(js).getStrictCrypto();
    auto usageSet = strictCrypto ? CryptoKeyUsageSet() : CryptoKeyUsageSet::derivationKeyMask();

    if (format != "raw") {
      return importAsymmetricForWebCrypto(
          js, format, kj::mv(keyData), normalizedName, extractable, keyUsages,
          // Verbose lambda capture needed because: https://bugs.llvm.org/show_bug.cgi?id=35984
          [curveId = curveId, normalizedName = kj::str(normalizedName)]
          (SubtleCrypto::JsonWebKey keyDataJwk) -> kj::Own<EVP_PKEY> {
        return ellipticJwkReader(curveId, kj::mv(keyDataJwk), normalizedName);
      }, CryptoKeyUsageSet::derivationKeyMask());
    } else {
      // The usage set is required to be empty for public ECDH keys, including raw keys.
      return importEllipticRaw(kj::mv(keyData), curveId, normalizedName, keyUsages, usageSet);
    }
  }();

  EC_KEY& ecKey = JSG_REQUIRE_NONNULL(EVP_PKEY_get0_EC_KEY(importedKey.evpPkey.get()), DOMDataError,
      "Input was not an EC public key nor a DH key",
      tryDescribeOpensslErrors());
  // get0 avoids adding a refcount...

  // We ignore id-ecDH because BoringSSL doesn't implement this.
  // https://bugs.chromium.org/p/chromium/issues/detail?id=532728
  // https://bugs.chromium.org/p/chromium/issues/detail?id=389400

  // Verify namedCurve matches what was specified in the key data.
  const EC_GROUP* group = EC_KEY_get0_group(&ecKey);
  JSG_REQUIRE(group != nullptr && EC_GROUP_get_curve_name(group) == curveId, DOMDataError,
      "\"algorithm.namedCurve\" \"", namedCurve, "\", does not match the curve specified by the "
      "input key data", tryDescribeOpensslErrors());

  auto keyAlgorithm = CryptoKey::EllipticKeyAlgorithm {
    normalizedName,
    normalizedNamedCurve,
  };

  return kj::heap<EllipticKey>(kj::mv(importedKey), kj::mv(keyAlgorithm), rsSize, extractable);
}

// =====================================================================================
// EDDSA & EDDH

namespace {

// Abstract base class for EDDSA and EDDH. The legacy NODE-ED25519 identifier for EDDSA has a
// namedCurve field whereas the algorithms in the Secure Curves spec do not. We handle this by
// keeping track of the algorithm identifier and returning an algorithm struct based on that.
class EdDsaKey final: public AsymmetricKeyCryptoKeyImpl {
public:
  explicit EdDsaKey(AsymmetricKeyData keyData,
                    kj::StringPtr keyAlgorithm,
                    bool extractable)
      : AsymmetricKeyCryptoKeyImpl(kj::mv(keyData), extractable),
        keyAlgorithm(kj::mv(keyAlgorithm)) {}

  static kj::OneOf<jsg::Ref<CryptoKey>, CryptoKeyPair> generateKey(
      kj::StringPtr normalizedName, int nid, CryptoKeyUsageSet privateKeyUsages,
      CryptoKeyUsageSet publicKeyUsages, bool extractablePrivateKey);

  CryptoKey::AlgorithmVariant getAlgorithm(jsg::Lock& js) const override {
    // For legacy node-based keys with NODE-ED25519, algorithm contains a namedCurve field.
    if (keyAlgorithm == "NODE-ED25519"){
      return CryptoKey::EllipticKeyAlgorithm {
        keyAlgorithm,
        keyAlgorithm,
      };
    } else {
      return CryptoKey::KeyAlgorithm {
        keyAlgorithm
      };
    }
  }

  kj::StringPtr getAlgorithmName() const override {
    return keyAlgorithm;
  }

  kj::StringPtr chooseHash(
      const kj::Maybe<kj::OneOf<kj::String,
      SubtleCrypto::HashAlgorithm>>& callTimeHash) const override {
    KJ_UNIMPLEMENTED();
  }

  kj::Array<kj::byte> sign(
      SubtleCrypto::SignAlgorithm&& algorithm,
      kj::ArrayPtr<const kj::byte> data) const override {
    JSG_REQUIRE(getTypeEnum() == KeyType::PRIVATE, DOMInvalidAccessError,
        "Asymmetric signing requires a private key.");

    JSG_REQUIRE(getAlgorithmName() == "Ed25519" || getAlgorithmName() == "NODE-ED25519",
        DOMOperationError, "Not implemented for algorithm \"", getAlgorithmName(), "\".");
    // Why NODE-ED25519? NodeJS uses NODE-ED25519/NODE-448 as algorithm names but that feels
    // inconsistent with the broader WebCrypto standard. Filed an issue with the standard for
    // clarification: https://github.com/tQsW/webcrypto-curve25519/issues/7

    auto signature = kj::heapArray<kj::byte>(ED25519_SIGNATURE_LEN);
    size_t signatureLength = signature.size();

    // NOTE: Even though there's a ED25519_sign/ED25519_verify methods, they don't actually seem to
    // work or are intended for some other use-case. I tried adding the verify immediately after
    // signing here & the verification failed.
    auto digestCtx = OSSL_NEW(EVP_MD_CTX);

    JSG_REQUIRE(1 == EVP_DigestSignInit(digestCtx.get(), nullptr, nullptr, nullptr, getEvpPkey()),
        InternalDOMOperationError, "Failed to initialize Ed25519 signing digest",
        internalDescribeOpensslErrors());
    JSG_REQUIRE(1 == EVP_DigestSign(digestCtx.get(), signature.begin(), &signatureLength,
        data.begin(), data.size()), InternalDOMOperationError, "Failed to sign with Ed25119 key",
        internalDescribeOpensslErrors());

    JSG_REQUIRE(signatureLength == signature.size(), InternalDOMOperationError,
        "Unexpected change in size signing Ed25519", signatureLength);

    return signature;
  }

  bool verify(
      SubtleCrypto::SignAlgorithm&& algorithm,
      kj::ArrayPtr<const kj::byte> signature, kj::ArrayPtr<const kj::byte> data) const override {
    ClearErrorOnReturn clearErrorOnReturn;

    JSG_REQUIRE(getTypeEnum() == KeyType::PUBLIC, DOMInvalidAccessError,
        "Asymmetric verification requires a public key.");

    JSG_REQUIRE(getAlgorithmName() == "Ed25519" || getAlgorithmName() == "NODE-ED25519",
        DOMOperationError, "Not implemented for this algorithm", getAlgorithmName());

    JSG_REQUIRE(signature.size() == ED25519_SIGNATURE_LEN, DOMOperationError,
        "Invalid ", getAlgorithmName(), " signature length ", signature.size());

    auto digestCtx = OSSL_NEW(EVP_MD_CTX);
    JSG_REQUIRE(1 == EVP_DigestSignInit(digestCtx.get(), nullptr, nullptr, nullptr, getEvpPkey()),
        InternalDOMOperationError, "Failed to initialize Ed25519 verification digest",
        internalDescribeOpensslErrors());

    auto result = EVP_DigestVerify(digestCtx.get(), signature.begin(), signature.size(),
        data.begin(), data.size());

    JSG_REQUIRE(result == 0 || result == 1, InternalDOMOperationError, "Unexpected return code",
        result, internalDescribeOpensslErrors());

    return !!result;
  }

  kj::Array<kj::byte> deriveBits(
      jsg::Lock& js, SubtleCrypto::DeriveKeyAlgorithm&& algorithm,
      kj::Maybe<uint32_t> resultBitLength) const override final {
    JSG_REQUIRE(getAlgorithmName() == "X25519", DOMNotSupportedError, ""
        "The deriveBits operation is not implemented for \"", getAlgorithmName(), "\".");

    JSG_REQUIRE(getTypeEnum() == KeyType::PRIVATE, DOMInvalidAccessError, ""
        "The deriveBits operation is only valid for a private key, not \"", getType(), "\".");

    auto& publicKey = JSG_REQUIRE_NONNULL(algorithm.$public, TypeError,
        "Missing field \"public\" in \"derivedKeyParams\".");

    JSG_REQUIRE(publicKey->getType() == "public"_kj, DOMInvalidAccessError, ""
        "The provided key has type \"", publicKey->getType(), "\", not \"public\"");

    JSG_REQUIRE(getAlgorithm(js).which() == publicKey->getAlgorithm(js).which(),
        DOMInvalidAccessError, "Base ", getAlgorithmName(), " private key cannot be used to derive"
        " a key from a peer ", publicKey->getAlgorithmName(), " public key");

    JSG_REQUIRE(getAlgorithmName() == publicKey->getAlgorithmName(), DOMInvalidAccessError,
        "Private key for derivation is using \"", getAlgorithmName(),
        "\" while public key is using \"", publicKey->getAlgorithmName(), "\".");

    auto outputBitLength = resultBitLength.orDefault(X25519_SHARED_KEY_LEN * 8);
    JSG_REQUIRE(outputBitLength <= X25519_SHARED_KEY_LEN * 8, DOMOperationError,
        "Derived key length (", outputBitLength, " bits) is too long (should be at most ",
        X25519_SHARED_KEY_LEN * 8, " bits).");

    // The check above for the algorithm `which` equality ensures that the impl can be downcast to
    // EdDsaKey (assuming we don't accidentally create a class that doesn't inherit this one that
    // for some reason returns an EdDsaKey).
    auto& publicKeyImpl = kj::downcast<EdDsaKey>(*publicKey->impl);

    // EDDH code derived from https://www.openssl.org/docs/manmaster/man3/EVP_PKEY_derive.html
    auto ctx = OSSL_NEW(EVP_PKEY_CTX, getEvpPkey(), nullptr);
    JSG_REQUIRE(1 == EVP_PKEY_derive_init(ctx), InternalDOMOperationError,
        "Failed to init EDDH key derivation", internalDescribeOpensslErrors());
    JSG_REQUIRE(1 == EVP_PKEY_derive_set_peer(ctx, publicKeyImpl.getEvpPkey()),
        InternalDOMOperationError, "Failed to set EDDH peer", internalDescribeOpensslErrors());

    kj::Vector<kj::byte> sharedSecret;
    sharedSecret.resize(X25519_SHARED_KEY_LEN);
    size_t skeylen = X25519_SHARED_KEY_LEN;
    JSG_REQUIRE(1 == EVP_PKEY_derive(ctx, sharedSecret.begin(), &skeylen), DOMOperationError,
        "Failed to derive EDDH key", internalDescribeOpensslErrors());
    KJ_ASSERT(skeylen == X25519_SHARED_KEY_LEN);

    // Check for all-zero value as mandated by spec
    kj::byte isNonZeroSecret = 0;
    for (kj::byte b : sharedSecret) {
      isNonZeroSecret |= b;
    }
    JSG_REQUIRE(isNonZeroSecret, DOMOperationError,
        "Detected small order secure curve points, aborting EDDH derivation");

    // mask off bits like in ECDH's deriveBits()
    auto resultByteLength = integerCeilDivision(outputBitLength, 8u);
    sharedSecret.truncate(resultByteLength);
    auto numBitsToMaskOff = resultByteLength * 8 - outputBitLength;
    KJ_DASSERT(numBitsToMaskOff < 8, numBitsToMaskOff);
    uint8_t mask = ~((1 << numBitsToMaskOff) - 1);
    sharedSecret.back() &= mask;
    return sharedSecret.releaseAsArray();
  }

  CryptoKey::AsymmetricKeyDetails getAsymmetricKeyDetail() const override {
    // Node.js implementation for EdDsa keys currently does not provide any detail
    return CryptoKey::AsymmetricKeyDetails {};
  }

  kj::StringPtr jsgGetMemoryName() const override { return "EdDsaKey"; }
  size_t jsgGetMemorySelfSize() const override { return sizeof(EdDsaKey); }
  void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const override {
    AsymmetricKeyCryptoKeyImpl::jsgGetMemoryInfo(tracker);
  }

private:
  kj::StringPtr keyAlgorithm;

  SubtleCrypto::JsonWebKey exportJwk() const override final {
    KJ_ASSERT(getAlgorithmName() == "X25519"_kj || getAlgorithmName() == "Ed25519"_kj ||
        getAlgorithmName() == "NODE-ED25519"_kj);

    uint8_t rawPublicKey[ED25519_PUBLIC_KEY_LEN]{};
    size_t publicKeyLen = sizeof(rawPublicKey);
    JSG_REQUIRE(1 == EVP_PKEY_get_raw_public_key(getEvpPkey(), rawPublicKey, &publicKeyLen),
        InternalDOMOperationError, "Failed to retrieve public key",
        internalDescribeOpensslErrors());

    KJ_ASSERT(publicKeyLen == 32, publicKeyLen);

    SubtleCrypto::JsonWebKey jwk;
    jwk.kty = kj::str("OKP");
    jwk.crv = kj::str(getAlgorithmName() == "X25519"_kj ? "X25519"_kj : "Ed25519"_kj);
    jwk.x = kj::encodeBase64Url(kj::arrayPtr(rawPublicKey, publicKeyLen));
    if (getAlgorithmName() == "Ed25519"_kj) {
      jwk.alg = kj::str("EdDSA");
    }

    if (getTypeEnum() == KeyType::PRIVATE) {
      // Deliberately use ED25519_PUBLIC_KEY_LEN here.
      // boringssl defines ED25519_PRIVATE_KEY_LEN as 64B since it stores the private key together
      // with public key data in some functions, but in the EVP interface only the 32B private key
      // itself is returned.
      uint8_t rawPrivateKey[ED25519_PUBLIC_KEY_LEN]{};
      size_t privateKeyLen = ED25519_PUBLIC_KEY_LEN;
      JSG_REQUIRE(1 == EVP_PKEY_get_raw_private_key(getEvpPkey(), rawPrivateKey, &privateKeyLen),
          InternalDOMOperationError, "Failed to retrieve private key",
          internalDescribeOpensslErrors());

      KJ_ASSERT(privateKeyLen == 32, privateKeyLen);

      jwk.d = kj::encodeBase64Url(kj::arrayPtr(rawPrivateKey, privateKeyLen));
    }

    return jwk;
  }

  kj::Array<kj::byte> exportRaw() const override final {
    JSG_REQUIRE(getTypeEnum() == KeyType::PUBLIC, DOMInvalidAccessError,
        "Raw export of ", getAlgorithmName(), " keys is only allowed for public keys.");

    kj::Vector<kj::byte> raw(ED25519_PUBLIC_KEY_LEN);
    raw.resize(ED25519_PUBLIC_KEY_LEN);
    size_t exportedLength = raw.size();

    JSG_REQUIRE(1 == EVP_PKEY_get_raw_public_key(getEvpPkey(), raw.begin(), &exportedLength),
        InternalDOMOperationError, "Failed to retrieve public key",
        internalDescribeOpensslErrors());

    JSG_REQUIRE(exportedLength == raw.size(), InternalDOMOperationError,
        "Unexpected change in size", raw.size(), exportedLength);

    return raw.releaseAsArray();
  }

};

kj::OneOf<jsg::Ref<CryptoKey>, CryptoKeyPair> EdDsaKey::generateKey(
    kj::StringPtr normalizedName, int nid, CryptoKeyUsageSet privateKeyUsages,
    CryptoKeyUsageSet publicKeyUsages, bool extractablePrivateKey) {
  auto [curveName, keypair, keylen] = [nid, normalizedName] {
    switch (nid) {
      // BoringSSL doesn't support ED448/X448.
      case NID_ED25519:
        return std::make_tuple("Ed25519"_kj, ED25519_keypair, ED25519_PUBLIC_KEY_LEN);
      case NID_X25519:
        return std::make_tuple("X25519"_kj, X25519_keypair, X25519_PUBLIC_VALUE_LEN);
    }

    KJ_FAIL_REQUIRE("ED ", normalizedName, " unimplemented", nid);
  }();

  uint8_t rawPublicKey[keylen];
  uint8_t rawPrivateKey[keylen * 2];
  keypair(rawPublicKey, rawPrivateKey);

  // The private key technically also contains the public key. Why does the keypair function bother
  // writing out the public key to a separate buffer?

  auto privateEvpPKey = OSSLCALL_OWN(EVP_PKEY, EVP_PKEY_new_raw_private_key(nid, nullptr,
      rawPrivateKey, keylen), InternalDOMOperationError, "Error constructing ", curveName,
      " private key", internalDescribeOpensslErrors());

  auto publicEvpPKey = OSSLCALL_OWN(EVP_PKEY, EVP_PKEY_new_raw_public_key(nid, nullptr,
      rawPublicKey, keylen), InternalDOMOperationError, "Internal error construct ", curveName,
      "public key", internalDescribeOpensslErrors());

  AsymmetricKeyData privateKeyData {
    .evpPkey = kj::mv(privateEvpPKey),
    .keyType = KeyType::PRIVATE,
    .usages = privateKeyUsages,
  };
  AsymmetricKeyData publicKeyData {
    .evpPkey = kj::mv(publicEvpPKey),
    .keyType = KeyType::PUBLIC,
    .usages = publicKeyUsages,
  };

  auto privateKey = jsg::alloc<CryptoKey>(kj::heap<EdDsaKey>(kj::mv(privateKeyData),
      normalizedName, extractablePrivateKey));
  auto publicKey = jsg::alloc<CryptoKey>(kj::heap<EdDsaKey>(kj::mv(publicKeyData),
      normalizedName, true));

  return CryptoKeyPair {.publicKey =  kj::mv(publicKey), .privateKey = kj::mv(privateKey)};
}

}  // namespace

kj::OneOf<jsg::Ref<CryptoKey>, CryptoKeyPair> CryptoKey::Impl::generateEddsa(
    jsg::Lock& js, kj::StringPtr normalizedName,
    SubtleCrypto::GenerateKeyAlgorithm&& algorithm, bool extractable,
    kj::ArrayPtr<const kj::String> keyUsages) {
  auto usages =
      CryptoKeyUsageSet::validate(normalizedName, CryptoKeyUsageSet::Context::generate, keyUsages,
                                  normalizedName == "X25519" ?
                                  CryptoKeyUsageSet::derivationKeyMask() :
                                  CryptoKeyUsageSet::sign() | CryptoKeyUsageSet::verify());
  auto privateKeyUsages = usages & CryptoKeyUsageSet::privateKeyMask();
  auto publicKeyUsages = usages & CryptoKeyUsageSet::publicKeyMask();

  if (normalizedName == "NODE-ED25519") {
    kj::StringPtr namedCurve = JSG_REQUIRE_NONNULL(algorithm.namedCurve, TypeError,
        "Missing field \"namedCurve\" in \"algorithm\".");
    JSG_REQUIRE(namedCurve == "NODE-ED25519", DOMNotSupportedError,
        "EDDSA curve \"", namedCurve, "\" isn't supported.");
  }

  return EdDsaKey::generateKey(normalizedName, normalizedName == "X25519" ? NID_X25519 :
                               NID_ED25519, privateKeyUsages, publicKeyUsages, extractable);
}

kj::Own<CryptoKey::Impl> CryptoKey::Impl::importEddsa(
    jsg::Lock& js, kj::StringPtr normalizedName, kj::StringPtr format,
    SubtleCrypto::ImportKeyData keyData,
    SubtleCrypto::ImportKeyAlgorithm&& algorithm, bool extractable,
    kj::ArrayPtr<const kj::String> keyUsages) {

  // BoringSSL doesn't support ED448.
  if (normalizedName == "NODE-ED25519") {
    // TODO: I prefer this style (declaring variables within the scope where they are needed) –
    // does KJ style want this to be done differently?
    kj::StringPtr namedCurve = JSG_REQUIRE_NONNULL(algorithm.namedCurve, TypeError,
        "Missing field \"namedCurve\" in \"algorithm\".");
    JSG_REQUIRE(namedCurve == "NODE-ED25519", DOMNotSupportedError,
        "EDDSA curve \"", namedCurve, "\" isn't supported.");
  }

  auto importedKey = [&] {
    auto nid = normalizedName == "X25519" ? NID_X25519 : NID_ED25519;
    if (format != "raw") {
      return importAsymmetricForWebCrypto(
          js, format, kj::mv(keyData), normalizedName, extractable, keyUsages,
          [nid, normalizedName = kj::str(normalizedName)]
          (SubtleCrypto::JsonWebKey keyDataJwk) -> kj::Own<EVP_PKEY> {
        return ellipticJwkReader(nid, kj::mv(keyDataJwk), normalizedName);
      }, normalizedName == "X25519" ? CryptoKeyUsageSet::derivationKeyMask() :
         CryptoKeyUsageSet::sign() | CryptoKeyUsageSet::verify());
    } else {
      return importEllipticRaw(
          kj::mv(keyData), nid, normalizedName, keyUsages,
          normalizedName == "X25519" ? CryptoKeyUsageSet() : CryptoKeyUsageSet::verify());
    }
  }();

  // In X25519 we ignore the id-X25519 identifier, as with id-ecDH above.
  return kj::heap<EdDsaKey>(kj::mv(importedKey), normalizedName, extractable);
}
}  // namespace workerd::api
