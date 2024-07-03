#include "keys.h"
#include <openssl/ec_key.h>
#include <openssl/crypto.h>
#include <openssl/pem.h>

namespace workerd::api {

namespace {
static char EMPTY_PASSPHRASE[] = "";
}  // namespace

AsymmetricKeyCryptoKeyImpl::AsymmetricKeyCryptoKeyImpl(kj::Own<EVP_PKEY> keyData,
                             kj::StringPtr keyType,
                             bool extractable,
                             CryptoKeyUsageSet usages)
    : CryptoKey::Impl(extractable, usages),
      keyData(kj::mv(keyData)),
      keyType(keyType) {}

kj::Array<kj::byte> AsymmetricKeyCryptoKeyImpl::signatureSslToWebCrypto(kj::Array<kj::byte> signature) const {
  return kj::mv(signature);
}

kj::Array<const kj::byte> AsymmetricKeyCryptoKeyImpl::signatureWebCryptoToSsl(
    kj::ArrayPtr<const kj::byte> signature) const {
  return { signature.begin(), signature.size(), kj::NullArrayDisposer::instance };
}

SubtleCrypto::ExportKeyData AsymmetricKeyCryptoKeyImpl::exportKey(kj::StringPtr format) const {
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

kj::Array<kj::byte> AsymmetricKeyCryptoKeyImpl::exportKeyExt(
    kj::StringPtr format,
    kj::StringPtr type,
    jsg::Optional<kj::String> cipher,
    jsg::Optional<kj::Array<kj::byte>> passphrase) const {
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
    KJ_IF_SOME(pw, passphrase) {
      detail.pass = reinterpret_cast<char*>(pw.begin());
      detail.pass_len = pw.size();
    }
    KJ_IF_SOME(ciph, cipher) {
      detail.cipher = EVP_get_cipherbyname(ciph.cStr());
      JSG_REQUIRE(detail.cipher != nullptr, TypeError, "Unknown cipher ", ciph);
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

kj::Array<kj::byte> AsymmetricKeyCryptoKeyImpl::sign(
    SubtleCrypto::SignAlgorithm&& algorithm,
    kj::ArrayPtr<const kj::byte> data) const {
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

bool AsymmetricKeyCryptoKeyImpl::verify(
    SubtleCrypto::SignAlgorithm&& algorithm,
    kj::ArrayPtr<const kj::byte> signature, kj::ArrayPtr<const kj::byte> data) const {
  ClearErrorOnReturn clearErrorOnReturn;

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
  return !!result;
}

bool AsymmetricKeyCryptoKeyImpl::equals(const CryptoKey::Impl& other) const {
  if (this == &other) return true;
  KJ_IF_SOME(otherImpl, kj::dynamicDowncastIfAvailable<const AsymmetricKeyCryptoKeyImpl>(other)) {
    // EVP_PKEY_cmp will return 1 if the inputs match, 0 if they don't match,
    // -1 if the key types are different, and -2 if the operation is not supported.
    // We only really care about the first two cases.
    return EVP_PKEY_cmp(keyData.get(), otherImpl.keyData.get()) == 1;
  }
  return false;
}

}  // namespace workerd::api
