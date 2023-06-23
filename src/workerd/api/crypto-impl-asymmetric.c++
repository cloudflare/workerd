// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "crypto-impl.h"
#include <openssl/rsa.h>
#include <openssl/pem.h>
#include <openssl/ec_key.h>
#include <openssl/bn.h>
#include <openssl/x509.h>
#include <openssl/crypto.h>
#include <openssl/curve25519.h>
#include <map>
#include <kj/function.h>
#include <type_traits>
#include "util.h"
#include <workerd/io/features.h>

namespace workerd::api {
namespace {

static char EMPTY_PASSPHRASE[] = "";

class AsymmetricKey: public CryptoKey::Impl {
public:
  explicit AsymmetricKey(kj::Own<EVP_PKEY> keyData, kj::StringPtr keyType, bool extractable,
                         CryptoKeyUsageSet usages)
      : CryptoKey::Impl(extractable, usages), keyData(kj::mv(keyData)), keyType(keyType) {}

  // ---------------------------------------------------------------------------
  // Subclasses must implement these

  // virtual CryptoKey::AlgorithmVariant getAlgorithm() = 0;
  // kj::StringPtr getAlgorithmName() const = 0;
  // (inheritted from CryptoKey::Impl, needs to be implemented by subclass)

  virtual kj::StringPtr chooseHash(
      const kj::Maybe<kj::OneOf<kj::String, SubtleCrypto::HashAlgorithm>>& callTimeHash) const = 0;
  // Determine the hash function to use. Some algorithms choose this at key import time while
  // others choose it at sign() or verify() time. `callTimeHash` is the hash name passed to the
  // call.

  virtual kj::Array<kj::byte> signatureSslToWebCrypto(kj::Array<kj::byte> signature) const {
    // Convert OpenSSL-format signature to WebCrypto-format signature, if different.
    return kj::mv(signature);
  }
  virtual kj::Array<const kj::byte> signatureWebCryptoToSsl(
      kj::ArrayPtr<const kj::byte> signature) const {
    // Convert WebCrypto-format signature to OpenSSL-format signature, if different.
    return { signature.begin(), signature.size(), kj::NullArrayDisposer::instance };
  }
  virtual void addSalt(EVP_PKEY_CTX* digestCtx, const SubtleCrypto::SignAlgorithm& algorithm) const {}
  // Add salt to digest context in order to generate or verify salted signature.
  // Currently only used for RSA-PSS sign and verify operations.

  // ---------------------------------------------------------------------------
  // Implementation of CryptoKey

  SubtleCrypto::ExportKeyData exportKey(kj::StringPtr format) const override final {
    // EVP_marshal_{public,private}_key() functions are BoringSSL
    // extensions which export asymmetric keys in DER format.
    // DER is the binary format which *should* work to export any EVP_PKEY.

    uint8_t *der = nullptr;
    KJ_DEFER(if (der != nullptr) { OPENSSL_free(der); });
    size_t derLen;
    bssl::ScopedCBB cbb;
    if (format == "pkcs8"_kj) {
      JSG_REQUIRE(keyType == "private"_kj, DOMInvalidAccessError,
          "Asymmetric pkcs8 export requires private key (not \"", keyType, "\").");
      if (!CBB_init(cbb.get(), 0) ||
          !EVP_marshal_private_key(cbb.get(), keyData.get()) ||
          !CBB_finish(cbb.get(), &der, &derLen)) {
        JSG_FAIL_REQUIRE(DOMOperationError, "Private key export failed.");
      }
    } else if (format == "spki"_kj) {
       JSG_REQUIRE(keyType == "public"_kj, DOMInvalidAccessError,
          "Asymmetric spki export requires public key (not \"", keyType, "\").");
      if (!CBB_init(cbb.get(), 0) ||
          !EVP_marshal_public_key(cbb.get(), keyData.get()) ||
          !CBB_finish(cbb.get(), &der, &derLen)) {
        JSG_FAIL_REQUIRE(DOMOperationError, "Public key export failed.");
      }
    } else if (format == "jwk"_kj) {
      auto jwk = exportJwk();
      // Implicitly extractable since the normative part of the implementation validates that
      // already.
      jwk.ext = true;
      jwk.key_ops = getUsages().map([](auto usage) { return kj::str(usage.name()); });
      return jwk;
    } else if (format == "raw"_kj) {
      return exportRaw();
    } else {
      JSG_FAIL_REQUIRE(DOMInvalidAccessError, "Cannot export \"", getAlgorithmName(), "\" in \"",
          format, "\" format.");
    }

    return kj::heapArray(der, derLen);
  }

  virtual kj::Array<kj::byte> exportKeyExt(
      kj::StringPtr format,
      kj::StringPtr type,
      jsg::Optional<kj::String> cipher = nullptr,
      jsg::Optional<kj::Array<kj::byte>> passphrase = nullptr) const override final {
    KJ_REQUIRE(isExtractable(), "Key is not extractable.");
    MarkPopErrorOnReturn mark_pop_error_on_return;
    KJ_REQUIRE(format != "jwk", "jwk export not supported for exportKeyExt");
    auto pkey = getEvpPkey();
    auto bio = OSSL_BIO_MEM();

    struct EncDetail {
      char* pass = &EMPTY_PASSPHRASE[0];
      size_t pass_len = 0;
      const EVP_CIPHER* cipher = nullptr;
    };

    const auto getEncDetail = [&] {
      EncDetail detail;
      KJ_IF_MAYBE(pw, passphrase) {
        detail.pass = reinterpret_cast<char*>(pw->begin());
        detail.pass_len = pw->size();
      }
      KJ_IF_MAYBE(ciph, cipher) {
        detail.cipher = EVP_get_cipherbyname(ciph->cStr());
        JSG_REQUIRE(detail.cipher != nullptr, TypeError, "Unknown cipher ", *ciph);
        KJ_REQUIRE(detail.pass != nullptr);
      }
      return detail;
    };

    const auto fromBio = [&](kj::StringPtr format) {
      BUF_MEM* bptr;
      BIO_get_mem_ptr(bio.get(), &bptr);
      auto result = kj::heapArray<kj::byte>(bptr->length);
      memcpy(result.begin(), bptr->data, bptr->length);
      return kj::mv(result);
    };

    if (getType() == "public"_kj) {
      // Here we only care about the format and the type.
      if (type == "pkcs1"_kj) {
        // PKCS#1 is only for RSA keys.
        JSG_REQUIRE(EVP_PKEY_id(pkey) == EVP_PKEY_RSA, TypeError,
            "The pkcs1 type is only valid for RSA keys.");
        auto rsa = EVP_PKEY_get1_RSA(pkey);
        KJ_DEFER(RSA_free(rsa));
        if (format == "pem"_kj) {
          if (PEM_write_bio_RSAPublicKey(bio.get(), rsa) == 1) {
            return fromBio(format);
          }
        } else if (format == "der"_kj) {
          if (i2d_RSAPublicKey_bio(bio.get(), rsa) == 1) {
            return fromBio(format);
          }
        }
      } else if (type == "spki"_kj) {
        if (format == "pem"_kj) {
          if (PEM_write_bio_PUBKEY(bio.get(), pkey) == 1) {
            return fromBio(format);
          }
        } else if (format == "der"_kj) {
          if (i2d_PUBKEY_bio(bio.get(), pkey) == 1) {
            return fromBio(format);
          }
        }
      }
      JSG_FAIL_REQUIRE(TypeError, "Failed to encode public key");
    }

    // Otherwise it's a private key.
    KJ_REQUIRE(getType() == "private"_kj);

    if (type == "pkcs1"_kj) {
      // PKCS#1 is only for RSA keys.
      JSG_REQUIRE(EVP_PKEY_id(pkey) == EVP_PKEY_RSA, TypeError,
          "The pkcs1 type is only valid for RSA keys.");
      auto rsa = EVP_PKEY_get1_RSA(pkey);
      KJ_DEFER(RSA_free(rsa));
      if (format == "pem"_kj) {
        auto enc = getEncDetail();
        if (PEM_write_bio_RSAPrivateKey(
                bio.get(),
                rsa,
                enc.cipher,
                reinterpret_cast<unsigned char*>(enc.pass),
                enc.pass_len,
                nullptr, nullptr) == 1) {
          return fromBio(format);
        }
      } else if (format == "der"_kj) {
        // The cipher and passphrase are ignored for DER with PKCS#1.
        if (i2d_RSAPrivateKey_bio(bio.get(), rsa) == 1) {
          return fromBio(format);
        }
      }
    } else if (type == "pkcs8"_kj) {
      auto enc = getEncDetail();
      if (format == "pem"_kj) {
        if (PEM_write_bio_PKCS8PrivateKey(
                bio.get(), pkey,
                enc.cipher,
                enc.pass,
                enc.pass_len,
                nullptr, nullptr) == 1) {
          return fromBio(format);
        }
      } else if (format == "der"_kj) {
        if (i2d_PKCS8PrivateKey_bio(
                bio.get(), pkey,
                enc.cipher,
                enc.pass,
                enc.pass_len,
                nullptr, nullptr) == 1) {
          return fromBio(format);
        }
      }
    } else if (type == "sec1"_kj) {
      // SEC1 is only used for EC keys.
      JSG_REQUIRE(EVP_PKEY_id(pkey) == EVP_PKEY_EC, TypeError,
          "The sec1 type is only valid for EC keys.");
      auto ec = EVP_PKEY_get1_EC_KEY(pkey);
      KJ_DEFER(EC_KEY_free(ec));
      if (format == "pem"_kj) {
        auto enc = getEncDetail();
        if (PEM_write_bio_ECPrivateKey(
                  bio.get(), ec,
                  enc.cipher,
                  reinterpret_cast<unsigned char*>(enc.pass),
                  enc.pass_len,
                  nullptr, nullptr) == 1) {
          return fromBio(format);
        }
      } else if (format == "der"_kj) {
        // The cipher and passphrase are ignored for DER with SEC1
        if (i2d_ECPrivateKey_bio(bio.get(), ec) == 1) {
          return fromBio(format);
        }
      }
    }

    JSG_FAIL_REQUIRE(TypeError, "Failed to encode private key");
  }

  kj::Array<kj::byte> sign(
      SubtleCrypto::SignAlgorithm&& algorithm,
      kj::ArrayPtr<const kj::byte> data) const override {
    JSG_REQUIRE(keyType == "private", DOMInvalidAccessError,
        "Asymmetric signing requires a private key.");

    auto type = lookupDigestAlgorithm(chooseHash(algorithm.hash)).second;
    if (getAlgorithmName() == "RSASSA-PKCS1-v1_5") {
      // RSASSA-PKCS1-v1_5 requires the RSA key to be at least as big as the digest size
      // plus a 15 to 19 byte digest-specific prefix (see boringssl's RSA_add_pkcs1_prefix) plus 11
      // bytes for padding (see RSA_PKCS1_PADDING_SIZE). For simplicity, require the key to be at
      // least 32 bytes larger than the hash digest.
      // Similar checks could also be adopted for more detailed error handling in verify(), but the
      // current approach should be sufficient to avoid internal errors.
      RSA& rsa = JSG_REQUIRE_NONNULL(EVP_PKEY_get0_RSA(getEvpPkey()), DOMDataError,
          "Missing RSA key", tryDescribeOpensslErrors());

      JSG_REQUIRE(EVP_MD_size(type) + 32 <= RSA_size(&rsa), DOMOperationError,
          "key too small for signing with given digest, need at least ",
          8 * (EVP_MD_size(type) + 32), "bits.");
    } else if (getAlgorithmName() == "RSA-PSS") {
      // Similarly, RSA-PSS requires keys to be at least the size of the digest and salt plus 2
      // bytes, see https://developer.mozilla.org/en-US/docs/Web/API/RsaPssParams for details.
      RSA& rsa = JSG_REQUIRE_NONNULL(EVP_PKEY_get0_RSA(getEvpPkey()), DOMDataError,
          "Missing RSA key", tryDescribeOpensslErrors());
      auto salt = JSG_REQUIRE_NONNULL(algorithm.saltLength, DOMDataError,
          "Failed to provide salt for RSA-PSS key operation which requires a salt");
      JSG_REQUIRE(salt >= 0, DOMDataError, "SaltLength for RSA-PSS must be non-negative ",
          "(provided ", salt, ").");
      JSG_REQUIRE(EVP_MD_size(type) + 2 <= RSA_size(&rsa), DOMOperationError,
          "key too small for signing with given digest");
      JSG_REQUIRE(salt <= RSA_size(&rsa) - EVP_MD_size(type) - 2, DOMOperationError,
          "key too small for signing with given digest and salt length");
    }

    auto digestCtx = OSSL_NEW(EVP_MD_CTX);

    OSSLCALL(EVP_DigestSignInit(digestCtx.get(), nullptr, type, nullptr, keyData.get()));
    addSalt(digestCtx->pctx, algorithm);
    // No-op call unless CryptoKey is RsaPss
    OSSLCALL(EVP_DigestSignUpdate(digestCtx.get(), data.begin(), data.size()));
    size_t signatureSize = 0;
    OSSLCALL(EVP_DigestSignFinal(digestCtx.get(), nullptr, &signatureSize));

    auto signature = kj::heapArray<kj::byte>(signatureSize);
    OSSLCALL(EVP_DigestSignFinal(digestCtx.get(), signature.begin(), &signatureSize));

    KJ_ASSERT(signatureSize <= signature.size());
    if (signatureSize < signature.size()) {
      signature = kj::heapArray<kj::byte>(signature.slice(0, signatureSize));
    }

    return signatureSslToWebCrypto(kj::mv(signature));
  }

  bool verify(
      SubtleCrypto::SignAlgorithm&& algorithm,
      kj::ArrayPtr<const kj::byte> signature, kj::ArrayPtr<const kj::byte> data) const override {
    JSG_REQUIRE(keyType == "public", DOMInvalidAccessError,
        "Asymmetric verification requires a public key.");

    auto sslSignature = signatureWebCryptoToSsl(signature);

    auto type = lookupDigestAlgorithm(chooseHash(algorithm.hash)).second;

    auto digestCtx = OSSL_NEW(EVP_MD_CTX);

    OSSLCALL(EVP_DigestVerifyInit(digestCtx.get(), nullptr, type, nullptr, keyData.get()));
    addSalt(digestCtx->pctx, algorithm);
    // No-op call unless CryptoKey is RsaPss
    OSSLCALL(EVP_DigestVerifyUpdate(digestCtx.get(), data.begin(), data.size()));
    // EVP_DigestVerifyFinal() returns 1 on success, 0 on invalid signature, and any other value
    // indicates "a more serious error".
    auto result = EVP_DigestVerifyFinal(digestCtx.get(), sslSignature.begin(), sslSignature.size());
    JSG_REQUIRE(result == 0 || result == 1, InternalDOMOperationError,
        "Unexpected return code from digest verify", getAlgorithmName());
    if (result == 0) {
      ERR_clear_error();
    }
    return !!result;
  }

  kj::StringPtr getType() const override { return keyType; }

  EVP_PKEY* getEvpPkey() const { return keyData.get(); }

  bool equals(const CryptoKey::Impl& other) const override final {
    if (this == &other) return true;
    KJ_IF_MAYBE(otherImpl, kj::dynamicDowncastIfAvailable<const AsymmetricKey>(other)) {
      // EVP_PKEY_cmp will return 1 if the inputs match, 0 if they don't match,
      // -1 if the key types are different, and -2 if the operation is not supported.
      // We only really care about the first two cases.
      return EVP_PKEY_cmp(keyData.get(), otherImpl->keyData.get()) == 1;
    }
    return false;
  }

private:
  virtual SubtleCrypto::JsonWebKey exportJwk() const = 0;
  virtual kj::Array<kj::byte> exportRaw() const = 0;

  mutable kj::Own<EVP_PKEY> keyData;
  // mutable because OpenSSL wants non-const pointers even when the object won't be modified...
  kj::StringPtr keyType;
};

struct ImportAsymmetricResult {
  kj::Own<EVP_PKEY> evpPkey;
  kj::StringPtr keyType;
  CryptoKeyUsageSet usages;
};

enum class UsageFamily {
  Derivation,
  SignVerify,
  EncryptDecrypt,
};

ImportAsymmetricResult importAsymmetric(jsg::Lock& js, kj::StringPtr format,
    SubtleCrypto::ImportKeyData keyData, kj::StringPtr normalizedName, bool extractable,
    kj::ArrayPtr<const kj::String> keyUsages,
    kj::FunctionParam<kj::Own<EVP_PKEY>(SubtleCrypto::JsonWebKey)> readJwk,
    CryptoKeyUsageSet allowedUsages) {
  CryptoKeyUsageSet usages;
  if (format == "jwk") {
    // I found jww's SO answer immeasurably helpful while writing this:
    // https://stackoverflow.com/questions/24093272/how-to-load-a-private-key-from-a-jwk-into-openssl

    auto& keyDataJwk = JSG_REQUIRE_NONNULL(keyData.tryGet<SubtleCrypto::JsonWebKey>(),
        DOMDataError, "JSON Web Key import requires a JSON Web Key object.");

    kj::StringPtr keyType;
    if (keyDataJwk.d != nullptr) {
      // Private key (`d` is the private exponent, per RFC 7518).
      keyType = "private";
      usages =
          CryptoKeyUsageSet::validate(normalizedName, CryptoKeyUsageSet::Context::importPrivate,
              keyUsages, allowedUsages & CryptoKeyUsageSet::privateKeyMask());

      // https://tools.ietf.org/html/rfc7518#section-6.3.2.7
      // We don't support keys with > 2 primes, so error out.
      JSG_REQUIRE(keyDataJwk.oth == nullptr, DOMNotSupportedError,
          "Multi-prime private keys not supported.");
    } else {
      // Public key.
      keyType = "public";
      auto strictCrypto = FeatureFlags::get(js).getStrictCrypto();
      // restrict key usages to public key usages. In the case of ECDH, usages must be empty, but
      // if the strict crypto compat flag is not enabled allow the same usages as with private ECDH
      // keys, i.e. derivationKeyMask().
      usages =
          CryptoKeyUsageSet::validate(normalizedName, CryptoKeyUsageSet::Context::importPublic,
                                      keyUsages, allowedUsages & (normalizedName == "ECDH" ?
                                      strictCrypto ? CryptoKeyUsageSet():
                                      CryptoKeyUsageSet::derivationKeyMask() :
                                      CryptoKeyUsageSet::publicKeyMask()));
    }

    auto [expectedUse, op0, op1] = [&, normalizedName] {
      if (normalizedName == "RSA-OAEP") {return std::make_tuple("enc", "encrypt", "wrapKey");}
      if (normalizedName == "ECDH" || normalizedName == "X25519") {
        return std::make_tuple("enc", "unused", "unused");
      }
      return std::make_tuple("sig", "sign", "verify");
    }();

    if (keyUsages.size() > 0) {
      KJ_IF_MAYBE(use, keyDataJwk.use) {
        JSG_REQUIRE(*use == expectedUse, DOMDataError,
            "Asymmetric \"jwk\" key import with usages requires a JSON Web Key with "
            "Public Key Use parameter \"use\" (\"", *use, "\") equal to \"sig\".");
      }
    }

    KJ_IF_MAYBE(ops, keyDataJwk.key_ops) {
      // TODO(cleanup): When we implement other JWK import functions, factor this part out into a
      //   JWK validation function.

      // "The key operation values are case-sensitive strings.  Duplicate key operation values MUST
      // NOT be present in the array." -- RFC 7517, section 4.3
      std::sort(ops->begin(), ops->end());
      JSG_REQUIRE(std::adjacent_find(ops->begin(), ops->end()) == ops->end(), DOMDataError,
          "A JSON Web Key's Key Operations parameter (\"key_ops\") "
          "must not contain duplicates.");

      KJ_IF_MAYBE(use, keyDataJwk.use) {
        // "The "use" and "key_ops" JWK members SHOULD NOT be used together; however, if both are
        // used, the information they convey MUST be consistent." -- RFC 7517, section 4.3.

        JSG_REQUIRE(*use == expectedUse, DOMDataError, "Asymmetric \"jwk\" import requires a JSON "
            "Web Key with Public Key Use \"use\" (\"", *use, "\") equal to \"", expectedUse, "\".");

        for (const auto& op: *ops) {
          JSG_REQUIRE(normalizedName != "ECDH" && normalizedName != "X25519", DOMDataError,
              "A JSON Web Key should have either a Public Key Use parameter (\"use\") or a Key "
              "Operations parameter (\"key_ops\"); otherwise, the parameters must be consistent "
              "with each other. For public ", normalizedName, " keys, there are no valid usages,"
              "so keys with a non-empty \"key_ops\" parameter are not allowed.");

          // TODO(conform): Can a JWK private key actually be used to verify? Not
          //   using the Web Crypto API...
          JSG_REQUIRE(op == op0 || op == op1, DOMDataError,
              "A JSON Web Key should have either a Public Key Use parameter (\"use\") or a Key "
              "Operations parameter (\"key_ops\"); otherwise, the parameters must be consistent "
              "with each other. A Public Key Use for ", normalizedName, " would allow a Key "
              "Operations array with only \"", op0, "\" and/or \"", op1, "\" values (not \"", op,
              "\").");
        }
      }

      // We're supposed to verify that `ops` contains all the values listed in `keyUsages`. For any
      // of the supported algorithms, a key may have at most two distinct usages ('sig' type keys
      // have at most one valid usage, but there may be two for e.g. ECDH). Test the first usage
      // and the next usages. Test the first usage and the first usage distinct from the first, if
      // present (i.e. the second allowed usage, even if there are duplicates).
      if (keyUsages.size() > 0) {
        JSG_REQUIRE(std::find(ops->begin(), ops->end(), keyUsages.front()) != ops->end(),
            DOMDataError, "All specified key usages must be present in the JSON "
            "Web Key's Key Operations parameter (\"key_ops\").");
        auto secondUsage = std::find_end(keyUsages.begin(), keyUsages.end(), keyUsages.begin(),
            keyUsages.begin() + 1) + 1;
        if (secondUsage != keyUsages.end()) {
          JSG_REQUIRE(std::find(ops->begin(), ops->end(), *secondUsage) != ops->end(),
              DOMDataError, "All specified key usages must be present in the JSON "
              "Web Key's Key Operations parameter (\"key_ops\").");
        }
      }
    }

    KJ_IF_MAYBE(ext, keyDataJwk.ext) {
      // If the user requested this key to be extractable, make sure the JWK does not disallow it.
      JSG_REQUIRE(!extractable || *ext, DOMDataError,
          "Cannot create an extractable CryptoKey from an unextractable JSON Web Key.");
    }

    return { readJwk(kj::mv(keyDataJwk)), keyType, usages };
  } else if (format == "spki") {
    kj::ArrayPtr<const kj::byte> keyBytes = JSG_REQUIRE_NONNULL(
        keyData.tryGet<kj::Array<kj::byte>>(), DOMDataError,
        "SPKI import requires an ArrayBuffer.");
    const kj::byte* ptr = keyBytes.begin();
    auto evpPkey = OSSLCALL_OWN(EVP_PKEY, d2i_PUBKEY(nullptr, &ptr, keyBytes.size()), DOMDataError,
        "Invalid SPKI input.");
    if (ptr != keyBytes.end()) {
      JSG_FAIL_REQUIRE(DOMDataError, "Invalid ", keyBytes.end() - ptr,
          " trailing bytes after SPKI input.");
    }

    // usages must be empty for ECDH public keys, so use CryptoKeyUsageSet() when validating the
    // usage set.
    usages =
        CryptoKeyUsageSet::validate(normalizedName, CryptoKeyUsageSet::Context::importPublic,
                                    keyUsages, allowedUsages & (normalizedName == "ECDH" ?
                                    CryptoKeyUsageSet() : CryptoKeyUsageSet::publicKeyMask()));
    return { kj::mv(evpPkey), "public"_kj, usages };
  } else if (format == "pkcs8") {
    kj::ArrayPtr<const kj::byte> keyBytes = JSG_REQUIRE_NONNULL(
        keyData.tryGet<kj::Array<kj::byte>>(), DOMDataError,
        "PKCS8 import requires an ArrayBuffer.");
    const kj::byte* ptr = keyBytes.begin();
    auto evpPkey = OSSLCALL_OWN(EVP_PKEY, d2i_AutoPrivateKey(nullptr, &ptr, keyBytes.size()),
        DOMDataError, "Invalid PKCS8 input.");
    if (ptr != keyBytes.end()) {
      JSG_FAIL_REQUIRE(DOMDataError, "Invalid ", keyBytes.end() - ptr,
          " trailing bytes after PKCS8 input.");
    }
    usages =
        CryptoKeyUsageSet::validate(normalizedName, CryptoKeyUsageSet::Context::importPrivate,
                                    keyUsages, allowedUsages & CryptoKeyUsageSet::privateKeyMask());
    return { kj::mv(evpPkey), "private"_kj, usages };
  } else {
    JSG_FAIL_REQUIRE(DOMNotSupportedError, "Unrecognized key import format \"", format, "\".");
  }
}

}  // namespace

// =====================================================================================
// RSASSA-PKCS1-V1_5, RSA-PSS, RSA-OEAP, RSA-RAW

namespace {

class RsaBase: public AsymmetricKey {
public:
  explicit RsaBase(kj::Own<EVP_PKEY> keyData, CryptoKey::RsaKeyAlgorithm keyAlgorithm,
      kj::StringPtr keyType, bool extractable, CryptoKeyUsageSet usages)
    : AsymmetricKey(kj::mv(keyData), keyType, extractable, usages),
      keyAlgorithm(kj::mv(keyAlgorithm)) {}

protected:
  CryptoKey::RsaKeyAlgorithm keyAlgorithm;

private:
  static kj::Array<kj::byte> bigNumToArray(const BIGNUM& n) {
    kj::Vector<kj::byte> result(BN_num_bytes(&n));
    result.resize(result.capacity());
    BN_bn2bin(&n, result.begin());
    return result.releaseAsArray();
  }

  SubtleCrypto::JsonWebKey exportJwk() const override final {
    const auto& rsa = JSG_REQUIRE_NONNULL(EVP_PKEY_get0_RSA(getEvpPkey()), DOMOperationError,
        "No RSA data backing key", tryDescribeOpensslErrors());

    SubtleCrypto::JsonWebKey jwk;
    jwk.kty = kj::str("RSA");
    jwk.alg = jwkHashAlgorithmName();

    jwk.n = kj::encodeBase64Url(bigNumToArray(KJ_REQUIRE_NONNULL(rsa.n)));
    jwk.e = kj::encodeBase64Url(bigNumToArray(KJ_REQUIRE_NONNULL(rsa.e)));

    if (getType() == "private"_kj) {
      jwk.d = kj::encodeBase64Url(bigNumToArray(KJ_REQUIRE_NONNULL(rsa.d)));
      jwk.p = kj::encodeBase64Url(bigNumToArray(KJ_REQUIRE_NONNULL(rsa.p)));
      jwk.q = kj::encodeBase64Url(bigNumToArray(KJ_REQUIRE_NONNULL(rsa.q)));
      jwk.dp = kj::encodeBase64Url(bigNumToArray(KJ_REQUIRE_NONNULL(rsa.dmp1)));
      jwk.dq = kj::encodeBase64Url(bigNumToArray(KJ_REQUIRE_NONNULL(rsa.dmq1)));
      jwk.qi = kj::encodeBase64Url(bigNumToArray(KJ_REQUIRE_NONNULL(rsa.iqmp)));
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

    auto public_exponent = kj::heapArray<kj::byte>(BN_num_bytes(e));
    KJ_ASSERT(BN_bn2binpad(e, static_cast<unsigned char*>(public_exponent.begin()),
                           public_exponent.size()) == public_exponent.size());
    details.publicExponent = kj::mv(public_exponent);

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
  explicit RsassaPkcs1V15Key(kj::Own<EVP_PKEY> keyData,
                             CryptoKey::RsaKeyAlgorithm keyAlgorithm,
                             kj::StringPtr keyType, bool extractable, CryptoKeyUsageSet usages)
      : RsaBase(kj::mv(keyData), kj::mv(keyAlgorithm), keyType, extractable, usages) {}

  CryptoKey::AlgorithmVariant getAlgorithm() const override { return keyAlgorithm.clone(); }
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
  explicit RsaPssKey(kj::Own<EVP_PKEY> keyData,
                    CryptoKey::RsaKeyAlgorithm keyAlgorithm,
                    kj::StringPtr keyType, bool extractable, CryptoKeyUsageSet usages)
      : RsaBase(kj::mv(keyData), kj::mv(keyAlgorithm), keyType, extractable, usages) {}

  CryptoKey::AlgorithmVariant getAlgorithm() const override { return keyAlgorithm.clone(); }
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
  explicit RsaOaepKey(kj::Own<EVP_PKEY> keyData,
                      CryptoKey::RsaKeyAlgorithm keyAlgorithm,
                      kj::StringPtr keyType,
                      bool extractable, CryptoKeyUsageSet usages)
      : RsaBase(kj::mv(keyData), kj::mv(keyAlgorithm), keyType, extractable, usages) {}

  CryptoKey::AlgorithmVariant getAlgorithm() const override { return keyAlgorithm.clone(); }
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
    JSG_REQUIRE(getType() == "public"_kj, DOMInvalidAccessError,
        "Encryption/key wrapping only works with public keys, not \"", getType(), "\".");
    return commonEncryptDecrypt(kj::mv(algorithm), plainText, EVP_PKEY_encrypt_init,
        EVP_PKEY_encrypt);
  }

  kj::Array<kj::byte> decrypt(
      SubtleCrypto::EncryptAlgorithm&& algorithm,
      kj::ArrayPtr<const kj::byte> cipherText) const override {
    JSG_REQUIRE(getType() == "private"_kj, DOMInvalidAccessError,
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

    KJ_IF_MAYBE(l, algorithm.label) {
      auto labelCopy = reinterpret_cast<uint8_t*>(OPENSSL_malloc(l->size()));
      KJ_DEFER(OPENSSL_free(labelCopy));
      // If setting the label fails we need to remember to destroy the buffer. In practice it can't
      // actually happen since we set RSA_PKCS1_OAEP_PADDING above & that appears to be the only way
      // this API call can fail.

      JSG_REQUIRE(labelCopy != nullptr, DOMOperationError,
          "Failed to allocate space for RSA-OAEP label copy",
          tryDescribeOpensslErrors());
      std::copy(l->begin(), l->end(), labelCopy);

      // EVP_PKEY_CTX_set0_rsa_oaep_label below takes ownership of the buffer passed in (must have
      // been OPENSSL_malloc-allocated).
      JSG_REQUIRE(1 == EVP_PKEY_CTX_set0_rsa_oaep_label(ctx.get(), labelCopy, l->size()),
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
  explicit RsaRawKey(kj::Own<EVP_PKEY> keyData, CryptoKey::RsaKeyAlgorithm keyAlgorithm,
                     bool extractable, CryptoKeyUsageSet usages)
      : RsaBase(kj::mv(keyData), kj::mv(keyAlgorithm), "private"_kj, extractable, usages) {}

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
      auto dataVal = OSSLCALL_OWN(BIGNUM, BN_bin2bn(data.begin(), data.size(), nullptr),
          InternalDOMOperationError, "Error converting presigned data",
          internalDescribeOpensslErrors());
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

  CryptoKey::AlgorithmVariant getAlgorithm() const override { return keyAlgorithm.clone(); }
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

CryptoKeyPair generateRsaPair(kj::StringPtr normalizedName, kj::Own<EVP_PKEY> privateEvpPKey,
    kj::Own<EVP_PKEY> publicEvpPKey, CryptoKey::RsaKeyAlgorithm&& keyAlgorithm,
    bool privateKeyExtractable, CryptoKeyUsageSet usages) {
  auto privateKeyAlgorithm = keyAlgorithm.clone();

  CryptoKeyUsageSet publicKeyUsages = usages & CryptoKeyUsageSet::publicKeyMask();
  CryptoKeyUsageSet privateKeyUsages = usages & CryptoKeyUsageSet::privateKeyMask();

  if (normalizedName == "RSASSA-PKCS1-v1_5") {
    return CryptoKeyPair {
      .publicKey =  jsg::alloc<CryptoKey>(kj::heap<RsassaPkcs1V15Key>(kj::mv(publicEvpPKey),
          kj::mv(keyAlgorithm), "public"_kj, true, publicKeyUsages)),
      .privateKey = jsg::alloc<CryptoKey>(kj::heap<RsassaPkcs1V15Key>(kj::mv(privateEvpPKey),
          kj::mv(privateKeyAlgorithm), "private"_kj, privateKeyExtractable, privateKeyUsages))};
  } else if (normalizedName == "RSA-PSS") {
    return CryptoKeyPair {
      .publicKey =  jsg::alloc<CryptoKey>(kj::heap<RsaPssKey>(kj::mv(publicEvpPKey),
          kj::mv(keyAlgorithm), "public"_kj, true, publicKeyUsages)),
      .privateKey = jsg::alloc<CryptoKey>(kj::heap<RsaPssKey>(kj::mv(privateEvpPKey),
          kj::mv(privateKeyAlgorithm), "private"_kj, privateKeyExtractable, privateKeyUsages))};
  } else if (normalizedName == "RSA-OAEP") {
    return CryptoKeyPair {
      .publicKey =  jsg::alloc<CryptoKey>(kj::heap<RsaOaepKey>(kj::mv(publicEvpPKey),
          kj::mv(keyAlgorithm), "public"_kj, true, publicKeyUsages)),
      .privateKey = jsg::alloc<CryptoKey>(kj::heap<RsaOaepKey>(kj::mv(privateEvpPKey),
          kj::mv(privateKeyAlgorithm), "private"_kj, privateKeyExtractable, privateKeyUsages))};
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
      return nullptr;
    }

    asUnsigned |= value[i] << 8 * bitShift;
  }

  return asUnsigned;
}

namespace {

void validateRsaParams(jsg::Lock& js, int modulusLength, kj::ArrayPtr<kj::byte> publicExponent,
                       bool isImport = false) {
  // The W3C standard itself doesn't describe any parameter validation but the conformance tests
  // do test "bad" exponents, likely because everyone uses OpenSSL that suffers from poor behavior
  // with these bad exponents (e.g. if an exponent < 3 or 65535 generates an infinite loop, a
  // library might be expected to handle such cases on its own, no?).

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
  KJ_IF_MAYBE(v, fromBignum<unsigned>(publicExponent)) {
    if (!isImport) {
      JSG_REQUIRE(*v == 3 || *v == 65537, DOMOperationError,
          "The \"publicExponent\" must be either 3 or 65537, but got ", *v, ".");
    } else if (strictCrypto) {
      // While we have long required the exponent to be 3 or 65537 when generating keys, handle
      // imported keys more permissively and allow additional exponents that are considered safe
      // and commonly used.
      JSG_REQUIRE(*v == 3 || *v == 17 || *v == 37 || *v == 65537, DOMOperationError,
          "Imported RSA key has invalid publicExponent ", *v, ".");
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

  auto bnExponent = OSSLCALL_OWN(BIGNUM, BN_bin2bn(publicExponent.begin(),
      publicExponent.size(), nullptr), InternalDOMOperationError, "Error setting up RSA keygen.");

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

  return generateRsaPair(normalizedName, kj::mv(privateEvpPKey), kj::mv(publicEvpPKey),
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
      BN_bin2bn(modulus.begin(), modulus.size(), nullptr),
      BN_bin2bn(publicExponent.begin(), publicExponent.size(), nullptr),
      nullptr));

  if (keyDataJwk.d != nullptr) {
    // This is a private key.

    auto privateExponent = UNWRAP_JWK_BIGNUM(kj::mv(keyDataJwk.d),
        DOMDataError, "Invalid RSA key in JSON Web Key; missing or invalid "
        "Private Exponent parameter (\"d\").");

    OSSLCALL(RSA_set0_key(rsaKey.get(), nullptr, nullptr,
        BN_bin2bn(privateExponent.begin(), privateExponent.size(), nullptr)));

    auto presence = (keyDataJwk.p != nullptr) + (keyDataJwk.q != nullptr) +
                    (keyDataJwk.dp != nullptr) + (keyDataJwk.dq != nullptr) +
                    (keyDataJwk.qi != nullptr);

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
          BN_bin2bn(firstPrimeFactor.begin(), firstPrimeFactor.size(), nullptr),
          BN_bin2bn(secondPrimeFactor.begin(), secondPrimeFactor.size(), nullptr)));
      OSSLCALL(RSA_set0_crt_params(rsaKey.get(),
          BN_bin2bn(firstFactorCrtExponent.begin(), firstFactorCrtExponent.size(), nullptr),
          BN_bin2bn(secondFactorCrtExponent.begin(), secondFactorCrtExponent.size(), nullptr),
          BN_bin2bn(firstCrtCoefficient.begin(), firstCrtCoefficient.size(), nullptr)));
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

  auto [evpPkey, keyType, usages] = importAsymmetric(
      js, kj::mv(format), kj::mv(keyData), normalizedName, extractable, keyUsages,
      // Verbose lambda capture needed because: https://bugs.llvm.org/show_bug.cgi?id=35984
      [hashEvpMd = hashEvpMd, &algorithm](SubtleCrypto::JsonWebKey keyDataJwk) -> kj::Own<EVP_PKEY> {
    JSG_REQUIRE(keyDataJwk.kty == "RSA", DOMDataError,
        "RSASSA-PKCS1-v1_5 \"jwk\" key import requires a JSON Web Key with Key Type parameter "
        "\"kty\" (\"", keyDataJwk.kty, "\") equal to \"RSA\".");

    KJ_IF_MAYBE(alg, keyDataJwk.alg) {
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
      auto jwkHash = validAlgorithms.find(*alg);
      JSG_REQUIRE(jwkHash != rsaPssAlgorithms.end(), DOMNotSupportedError,
          "Unrecognized or unimplemented algorithm \"", *alg, "\" listed in JSON Web Key Algorithm "
          "parameter.");

      JSG_REQUIRE(jwkHash->second == hashEvpMd, DOMDataError,
          "JSON Web Key Algorithm parameter \"alg\" (\"", *alg, "\") does not match requested hash "
          "algorithm \"", jwkHash->first, "\".");
    }

    return rsaJwkReader(kj::mv(keyDataJwk));
  }, allowedUsages);

  // get0 avoids adding a refcount...
  RSA& rsa = JSG_REQUIRE_NONNULL(EVP_PKEY_get0_RSA(evpPkey.get()), DOMDataError,
      "Input was not an RSA key", tryDescribeOpensslErrors());

  // TODO(conform): We're supposed to check if PKCS8/SPKI input specified a hash and, if so,
  //   compare it against the hash requested in `algorithm`. But, I can't find the OpenSSL
  //   interface to extract the hash from the ASN.1. Oh well...

  auto modulusLength = RSA_size(&rsa) * 8;
  KJ_ASSERT(modulusLength <= ~uint16_t(0));

  const BIGNUM *n, *e, *d;
  RSA_get0_key(&rsa, &n, &e, &d);

  auto publicExponent = kj::heapArray<kj::byte>(BN_num_bytes(e));
  KJ_ASSERT(BN_bn2bin(e, publicExponent.begin()) == publicExponent.size());

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
        kj::mv(evpPkey), kj::mv(keyAlgorithm), keyType, extractable, usages);
  } else if (normalizedName == "RSA-PSS") {
    return kj::heap<RsaPssKey>(kj::mv(evpPkey), kj::mv(keyAlgorithm), keyType, extractable, usages);
  } else if (normalizedName == "RSA-OAEP") {
    return kj::heap<RsaOaepKey>(
        kj::mv(evpPkey), kj::mv(keyAlgorithm), keyType, extractable, usages);
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
  auto [evpPkey, keyType, usages] = importAsymmetric(
      js, kj::mv(format), kj::mv(keyData), normalizedName, extractable, keyUsages,
      // Verbose lambda capture needed because: https://bugs.llvm.org/show_bug.cgi?id=35984
      [](SubtleCrypto::JsonWebKey keyDataJwk) -> kj::Own<EVP_PKEY> {
    JSG_REQUIRE(keyDataJwk.kty == "RSA", DOMDataError,
        "RSA-RAW \"jwk\" key import requires a JSON Web Key with Key Type parameter "
        "\"kty\" (\"", keyDataJwk.kty, "\") equal to \"RSA\".");

    KJ_IF_MAYBE(alg, keyDataJwk.alg) {
      // If this JWK specifies an algorithm, make sure it jives with the hash we were passed via
      // importKey().
      static std::map<kj::StringPtr, const EVP_MD*> rsaAlgorithms{
        {"RS1", EVP_sha1()},
        {"RS256", EVP_sha256()},
        {"RS384", EVP_sha384()},
        {"RS512", EVP_sha512()},
      };
      auto jwkHash = rsaAlgorithms.find(*alg);
      JSG_REQUIRE(jwkHash != rsaAlgorithms.end(), DOMNotSupportedError,
          "Unrecognized or unimplemented algorithm \"", *alg,
          "\" listed in JSON Web Key Algorithm parameter.");
    }
    return rsaJwkReader(kj::mv(keyDataJwk));
  }, allowedUsages);

  JSG_REQUIRE(keyType == "private", DOMDataError,
      "RSA-RAW only supports private keys but requested \"", keyType, "\".");

  // get0 avoids adding a refcount...
  RSA& rsa = JSG_REQUIRE_NONNULL(EVP_PKEY_get0_RSA(evpPkey.get()), DOMDataError,
      "Input was not an RSA key", tryDescribeOpensslErrors());

  auto modulusLength = RSA_size(&rsa) * 8;
  KJ_ASSERT(modulusLength <= ~uint16_t(0));

  const BIGNUM *n, *e, *d;
  RSA_get0_key(&rsa, &n, &e, &d);

  auto publicExponent = kj::heapArray<kj::byte>(BN_num_bytes(e));
  KJ_ASSERT(BN_bn2bin(e, publicExponent.begin()) == publicExponent.size());

  // Validate modulus and exponent, reject imported RSA keys that may be unsafe.
  validateRsaParams(js, modulusLength, publicExponent, true);

  auto keyAlgorithm = CryptoKey::RsaKeyAlgorithm {
    .name = "RSA-RAW"_kj,
    .modulusLength = static_cast<uint16_t>(modulusLength),
    .publicExponent = kj::mv(publicExponent)
  };

  return kj::heap<RsaRawKey>(kj::mv(evpPkey), kj::mv(keyAlgorithm), extractable, usages);
}

// =====================================================================================
// ECDSA & ECDH

namespace {

class EllipticKey final: public AsymmetricKey {
public:
  explicit EllipticKey(kj::Own<EVP_PKEY> keyData,
                       CryptoKey::EllipticKeyAlgorithm keyAlgorithm,
                       kj::StringPtr keyType,
                       uint rsSize,
                       bool extractable, CryptoKeyUsageSet usages)
      : AsymmetricKey(kj::mv(keyData), keyType, extractable, usages),
        keyAlgorithm(kj::mv(keyAlgorithm)), rsSize(rsSize) {}

  CryptoKey::AlgorithmVariant getAlgorithm() const override { return keyAlgorithm; }
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
      SubtleCrypto::DeriveKeyAlgorithm&& algorithm,
      kj::Maybe<uint32_t> resultBitLength) const override final {
    JSG_REQUIRE(keyAlgorithm.name == "ECDH", DOMNotSupportedError, ""
        "The deriveBits operation is not implemented for \"", keyAlgorithm.name, "\".");

    JSG_REQUIRE(getType() == "private"_kj, DOMInvalidAccessError, ""
        "The deriveBits operation is only valid for a private key, not \"", getType(), "\".");

    auto& publicKey = JSG_REQUIRE_NONNULL(algorithm.$public, TypeError,
        "Missing field \"public\" in \"derivedKeyParams\".");

    JSG_REQUIRE(publicKey->getType() == "public"_kj, DOMInvalidAccessError, ""
        "The provided key has type \"", publicKey->getType(), "\", not \"public\"");

    JSG_REQUIRE(getAlgorithm().which() == publicKey->getAlgorithm().which(), DOMInvalidAccessError,
        "Base ", getAlgorithmName(), " private key cannot be used to derive a "
        "key from a peer ", getAlgorithmName(), " public key");

    JSG_REQUIRE(getAlgorithmName() == publicKey->getAlgorithmName(), DOMInvalidAccessError,
        "Private key for derivation is using \"", getAlgorithmName(),
        "\" while public key is using \"", publicKey->getAlgorithmName(), "\".");

    auto publicCurve = publicKey->getAlgorithm().get<CryptoKey::EllipticKeyAlgorithm>()
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
    memset(out.begin(), 0, out.size());

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

private:
  static kj::Array<kj::byte> bigNumToPaddedArray(const BIGNUM& n, size_t paddedLength) {
    kj::Vector<kj::byte> result(paddedLength);
    result.resize(paddedLength);
    JSG_REQUIRE(1 == BN_bn2bin_padded(result.begin(), paddedLength, &n), InternalDOMOperationError,
        "Error converting EC affine co-ordinates to padded array",
        internalDescribeOpensslErrors());
    return result.releaseAsArray();

  }

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
    jwk.x = kj::encodeBase64Url(bigNumToPaddedArray(x, groupDegreeInBytes));
    jwk.y = kj::encodeBase64Url(bigNumToPaddedArray(y, groupDegreeInBytes));
    if (getType() == "private"_kj) {
      const auto& privateKey = JSG_REQUIRE_NONNULL(EC_KEY_get0_private_key(&ec),
          InternalDOMOperationError, "Error getting private key material for JSON Web Key export",
          internalDescribeOpensslErrors());
      jwk.d = kj::encodeBase64Url(bigNumToPaddedArray(privateKey, groupDegreeInBytes));
    }
    return jwk;
  }

  kj::Array<kj::byte> exportRaw() const override final {
    JSG_REQUIRE(getType() == "public"_kj, DOMInvalidAccessError,
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

  auto privateKey = jsg::alloc<CryptoKey>(kj::heap<EllipticKey>(kj::mv(privateEvpPKey),
      keyAlgorithm, "private"_kj, rsSize, extractable, privateKeyUsages));
  auto publicKey = jsg::alloc<CryptoKey>(kj::heap<EllipticKey>(kj::mv(publicEvpPKey),
      keyAlgorithm, "public"_kj, rsSize, true, publicKeyUsages));

  return CryptoKeyPair {.publicKey =  kj::mv(publicKey), .privateKey = kj::mv(privateKey)};
}

ImportAsymmetricResult importEllipticRaw(SubtleCrypto::ImportKeyData keyData, int curveId,
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
        raw.size(), internalDescribeOpensslErrors()), "public"_kj, usages };
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

  return ImportAsymmetricResult{ kj::mv(evpPkey), "public"_kj, usages };
}

}  // namespace

kj::Own<EVP_PKEY> ellipticJwkReader(int curveId, SubtleCrypto::JsonWebKey&& keyDataJwk) {
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
    KJ_IF_MAYBE(alg, keyDataJwk.alg) {
      // If this JWK specifies an algorithm, make sure it jives with the hash we were passed via
      // importKey().
      if (curveId == NID_ED25519) {
        JSG_REQUIRE(*alg == "EdDSA", DOMDataError,
            "JSON Web Key Algorithm parameter \"alg\" (\"", *alg, "\") does not match requested "
            "Ed25519 curve.");
      }
    }

    auto x = UNWRAP_JWK_BIGNUM(kj::mv(keyDataJwk.x), DOMDataError,
        "Invalid ", crv, " key in JSON WebKey; missing or invalid public key component (\"x\").");
    JSG_REQUIRE(x.size() == 32, DOMDataError, "Invalid length ", x.size(), " for public key");

    if (keyDataJwk.d == nullptr) {
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

  KJ_IF_MAYBE(alg, keyDataJwk.alg) {
    // If this JWK specifies an algorithm, make sure it jives with the hash we were passed via
    // importKey().
    static std::map<kj::StringPtr, int> ecdsaAlgorithms {
      {"ES256", NID_X9_62_prime256v1},
      {"ES384", NID_secp384r1},
      {"ES512", NID_secp521r1},
    };

    auto iter = ecdsaAlgorithms.find(*alg);
    JSG_REQUIRE(iter != ecdsaAlgorithms.end(), DOMNotSupportedError,
        "Unrecognized or unimplemented algorithm \"", *alg,
        "\" listed in JSON Web Key Algorithm parameter.");

    JSG_REQUIRE(iter->second == curveId, DOMDataError,
        "JSON Web Key Algorithm parameter \"alg\" (\"", *alg, "\") does not match requested curve.");
  }

  auto ecKey = OSSLCALL_OWN(EC_KEY, EC_KEY_new_by_curve_name(curveId), DOMOperationError,
      "Error importing EC key", tryDescribeOpensslErrors());

  auto x = UNWRAP_JWK_BIGNUM(kj::mv(keyDataJwk.x), DOMDataError,
      "Invalid EC key in JSON Web Key; missing \"x\".");
  auto y = UNWRAP_JWK_BIGNUM(kj::mv(keyDataJwk.y), DOMDataError,
      "Invalid EC key in JSON Web Key; missing \"y\".");

  auto group = EC_KEY_get0_group(ecKey);
  auto bigX = OSSLCALL_OWN(BIGNUM, BN_bin2bn(x.begin(), x.size(), nullptr),
      InternalDOMOperationError, "Error importing EC key", internalDescribeOpensslErrors());
  auto bigY = OSSLCALL_OWN(BIGNUM, BN_bin2bn(y.begin(), y.size(), nullptr),
      InternalDOMOperationError, "Error importing EC key", internalDescribeOpensslErrors());
  auto point = OSSL_NEW(EC_POINT, group);
  OSSLCALL(EC_POINT_set_affine_coordinates_GFp(group, point, bigX, bigY, nullptr));
  OSSLCALL(EC_KEY_set_public_key(ecKey, point));

  if (keyDataJwk.d != nullptr) {
    // This is a private key.

    auto d = UNWRAP_JWK_BIGNUM(kj::mv(keyDataJwk.d), DOMDataError,
        "Invalid EC key in JSON Web Key; missing or invalid private key component (\"d\").");

    auto bigD = OSSLCALL_OWN(BIGNUM, BN_bin2bn(d.begin(), d.size(), nullptr),
        InternalDOMOperationError, "Error importing EC key", internalDescribeOpensslErrors());

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

  auto [evpPkey, keyType, usages] = [&, curveId = curveId] {
    if (format != "raw") {
      return importAsymmetric(
          js, format, kj::mv(keyData), normalizedName, extractable, keyUsages,
          // Verbose lambda capture needed because: https://bugs.llvm.org/show_bug.cgi?id=35984
          [curveId = curveId](SubtleCrypto::JsonWebKey keyDataJwk) -> kj::Own<EVP_PKEY> {
        return ellipticJwkReader(curveId, kj::mv(keyDataJwk));
      }, CryptoKeyUsageSet::sign() | CryptoKeyUsageSet::verify());
    } else {
      return importEllipticRaw(kj::mv(keyData), curveId, normalizedName, keyUsages,
          CryptoKeyUsageSet::verify());
    }
  }();

  // get0 avoids adding a refcount...
  EC_KEY& ecKey = JSG_REQUIRE_NONNULL(EVP_PKEY_get0_EC_KEY(evpPkey.get()), DOMDataError,
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

  return kj::heap<EllipticKey>(kj::mv(evpPkey), kj::mv(keyAlgorithm), keyType, rsSize, extractable,
                               usages);
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

  auto [evpPkey, keyType, usages] = [&, curveId = curveId] {
    auto strictCrypto = FeatureFlags::get(js).getStrictCrypto();
    auto usageSet = strictCrypto ? CryptoKeyUsageSet() : CryptoKeyUsageSet::derivationKeyMask();

    if (format != "raw") {
      return importAsymmetric(
          js, format, kj::mv(keyData), normalizedName, extractable, keyUsages,
          // Verbose lambda capture needed because: https://bugs.llvm.org/show_bug.cgi?id=35984
          [curveId = curveId](SubtleCrypto::JsonWebKey keyDataJwk) -> kj::Own<EVP_PKEY> {
        return ellipticJwkReader(curveId, kj::mv(keyDataJwk));
      }, CryptoKeyUsageSet::derivationKeyMask());
    } else {
      // The usage set is required to be empty for public ECDH keys, including raw keys.
      return importEllipticRaw(kj::mv(keyData), curveId, normalizedName, keyUsages, usageSet);
    }
  }();

  EC_KEY& ecKey = JSG_REQUIRE_NONNULL(EVP_PKEY_get0_EC_KEY(evpPkey.get()), DOMDataError,
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

  return kj::heap<EllipticKey>(kj::mv(evpPkey), kj::mv(keyAlgorithm), keyType, rsSize, extractable,
                               usages);
}

// =====================================================================================
// EDDSA & EDDH

namespace {

// Abstract base class for EDDSA and EDDH. Unfortunately, the legacy NODE-ED25519 identifier has a
// namedCurve field whereas the algorithms in the Secure Curves spec do not, which requires having
// a base class implementing most functionality and having classes inherit from it to define the
// key algorithm struct with or without namedCurve.
class EdDsaKeyBase : public AsymmetricKey {
public:
  explicit EdDsaKeyBase(kj::Own<EVP_PKEY> keyData,
                    kj::StringPtr keyType, bool extractable, CryptoKeyUsageSet usages)
      : AsymmetricKey(kj::mv(keyData), keyType, extractable, usages) {}

  static kj::OneOf<jsg::Ref<CryptoKey>, CryptoKeyPair> generateKey(
      kj::StringPtr normalizedName, int nid, CryptoKeyUsageSet privateKeyUsages,
      CryptoKeyUsageSet publicKeyUsages, bool extractablePrivateKey);

  kj::StringPtr chooseHash(
      const kj::Maybe<kj::OneOf<kj::String,
      SubtleCrypto::HashAlgorithm>>& callTimeHash) const override {
    KJ_UNIMPLEMENTED();
  }

  kj::Array<kj::byte> sign(
      SubtleCrypto::SignAlgorithm&& algorithm,
      kj::ArrayPtr<const kj::byte> data) const override {
    JSG_REQUIRE(getType() == "private", DOMInvalidAccessError,
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
    JSG_REQUIRE(getType() == "public", DOMInvalidAccessError,
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

    if (result == 0) {
      ERR_clear_error();
    }

    return !!result;
  }

  kj::Array<kj::byte> deriveBits(
      SubtleCrypto::DeriveKeyAlgorithm&& algorithm,
      kj::Maybe<uint32_t> resultBitLength) const override final {
    JSG_REQUIRE(getAlgorithmName() == "X25519", DOMNotSupportedError, ""
        "The deriveBits operation is not implemented for \"", getAlgorithmName(), "\".");

    JSG_REQUIRE(getType() == "private"_kj, DOMInvalidAccessError, ""
        "The deriveBits operation is only valid for a private key, not \"", getType(), "\".");

    auto& publicKey = JSG_REQUIRE_NONNULL(algorithm.$public, TypeError,
        "Missing field \"public\" in \"derivedKeyParams\".");

    JSG_REQUIRE(publicKey->getType() == "public"_kj, DOMInvalidAccessError, ""
        "The provided key has type \"", publicKey->getType(), "\", not \"public\"");

    JSG_REQUIRE(getAlgorithm().which() == publicKey->getAlgorithm().which(), DOMInvalidAccessError,
        "Base ", getAlgorithmName(), " private key cannot be used to derive a "
        "key from a peer ", getAlgorithmName(), " public key");

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
    auto& publicKeyImpl = kj::downcast<EdDsaKeyBase>(*publicKey->impl);

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

private:
  SubtleCrypto::JsonWebKey exportJwk() const override final {
    KJ_ASSERT(getAlgorithmName() == "X25519"_kj || getAlgorithmName() == "Ed25519"_kj ||
        getAlgorithmName() == "NODE-ED25519"_kj);

    uint8_t rawPublicKey[ED25519_PUBLIC_KEY_LEN];
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

    if (getType() == "private"_kj) {
      // Deliberately use ED25519_PUBLIC_KEY_LEN here.
      // boringssl defines ED25519_PRIVATE_KEY_LEN as 64B since it stores the private key together
      // with public key data in some functions, but in the EVP interface only the 32B private key
      // itself is returned.
      uint8_t rawPrivateKey[ED25519_PUBLIC_KEY_LEN];
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
    JSG_REQUIRE(getType() == "public"_kj, DOMInvalidAccessError,
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

class EdDsaKey final : public EdDsaKeyBase {
public:
  explicit EdDsaKey(kj::Own<EVP_PKEY> keyData,
                    CryptoKey::KeyAlgorithm keyAlgorithm,
                    kj::StringPtr keyType, bool extractable, CryptoKeyUsageSet usages)
      : EdDsaKeyBase(kj::mv(keyData), keyType, extractable, usages),
        keyAlgorithm(kj::mv(keyAlgorithm)) {}
  CryptoKey::AlgorithmVariant getAlgorithm() const override { return keyAlgorithm; }
  kj::StringPtr getAlgorithmName() const override { return keyAlgorithm.name; }

private:
  CryptoKey::KeyAlgorithm keyAlgorithm;
};

// Class for legacy algorithm NODE-ED25519, which includes a namedCurve field in its algorithm
// unlike Ed25519.
class EdDsaCurveKey final : public EdDsaKeyBase {
public:
  explicit EdDsaCurveKey(kj::Own<EVP_PKEY> keyData,
                    CryptoKey::EllipticKeyAlgorithm keyAlgorithm,
                    kj::StringPtr keyType, bool extractable, CryptoKeyUsageSet usages)
      : EdDsaKeyBase(kj::mv(keyData), keyType, extractable, usages),
        keyAlgorithm(kj::mv(keyAlgorithm)) {}
  CryptoKey::AlgorithmVariant getAlgorithm() const override { return keyAlgorithm; }
  kj::StringPtr getAlgorithmName() const override { return keyAlgorithm.name; }

private:
  CryptoKey::EllipticKeyAlgorithm keyAlgorithm;
};

kj::OneOf<jsg::Ref<CryptoKey>, CryptoKeyPair> EdDsaKeyBase::generateKey(
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

  if (normalizedName == "NODE-ED25519") {
    auto keyAlgorithm = CryptoKey::EllipticKeyAlgorithm {
      normalizedName,
      normalizedName,
    };
    auto privateKey = jsg::alloc<CryptoKey>(kj::heap<EdDsaCurveKey>(kj::mv(privateEvpPKey),
        keyAlgorithm, "private"_kj, extractablePrivateKey, privateKeyUsages));
    auto publicKey = jsg::alloc<CryptoKey>(kj::heap<EdDsaCurveKey>(kj::mv(publicEvpPKey),
        keyAlgorithm, "public"_kj, true, publicKeyUsages));

    return CryptoKeyPair {.publicKey =  kj::mv(publicKey), .privateKey = kj::mv(privateKey)};
  }
  auto keyAlgorithm = CryptoKey::KeyAlgorithm {
    normalizedName
  };
  auto privateKey = jsg::alloc<CryptoKey>(kj::heap<EdDsaKey>(kj::mv(privateEvpPKey),
      keyAlgorithm, "private"_kj, extractablePrivateKey, privateKeyUsages));
  auto publicKey = jsg::alloc<CryptoKey>(kj::heap<EdDsaKey>(kj::mv(publicEvpPKey),
      keyAlgorithm, "public"_kj, true, publicKeyUsages));

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
  kj::StringPtr namedCurve;

  // BoringSSL doesn't support ED448.
  if (normalizedName == "NODE-ED25519") {
    namedCurve = JSG_REQUIRE_NONNULL(algorithm.namedCurve, TypeError,
        "Missing field \"namedCurve\" in \"algorithm\".");
    JSG_REQUIRE(namedCurve == "NODE-ED25519", DOMNotSupportedError,
        "EDDSA curve \"", namedCurve, "\" isn't supported.");
  }

  auto [evpPkey, keyType, usages] = [&] {
    auto nid = normalizedName == "X25519" ? NID_X25519 : NID_ED25519;
    if (format != "raw") {
      return importAsymmetric(
          js, format, kj::mv(keyData), normalizedName, extractable, keyUsages,
          [nid](SubtleCrypto::JsonWebKey keyDataJwk) -> kj::Own<EVP_PKEY> {
        return ellipticJwkReader(nid, kj::mv(keyDataJwk));
      }, normalizedName == "X25519" ? CryptoKeyUsageSet::derivationKeyMask() :
         CryptoKeyUsageSet::sign() | CryptoKeyUsageSet::verify());
    } else {
      return importEllipticRaw(
          kj::mv(keyData), nid, normalizedName, keyUsages,
          normalizedName == "X25519" ? CryptoKeyUsageSet() : CryptoKeyUsageSet::verify());
    }
  }();

  if (normalizedName == "NODE-ED25519") {
    auto keyAlgorithm = CryptoKey::EllipticKeyAlgorithm {
      normalizedName,
      normalizedName,
    };
    return kj::heap<EdDsaCurveKey>(kj::mv(evpPkey), kj::mv(keyAlgorithm), keyType, extractable,
                                   usages);
  }
  auto keyAlgorithm = CryptoKey::KeyAlgorithm {
    normalizedName
  };

  // In X25519 we ignore the id-X25519 identifier, as with id-ecDH above.
  return kj::heap<EdDsaKey>(kj::mv(evpPkey), kj::mv(keyAlgorithm), keyType, extractable, usages);
}
}  // namespace workerd::api
