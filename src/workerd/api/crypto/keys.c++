#include "keys.h"

#include <openssl/crypto.h>
#include <openssl/ec_key.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

namespace workerd::api {

namespace {
static const char EMPTY_PASSPHRASE[] = "";
}  // namespace

kj::StringPtr toStringPtr(KeyType type) {
  switch (type) {
    case KeyType::SECRET:
      return "secret"_kj;
    case KeyType::PUBLIC:
      return "public"_kj;
    case KeyType::PRIVATE:
      return "private"_kj;
  }
  KJ_UNREACHABLE;
}

AsymmetricKeyCryptoKeyImpl::AsymmetricKeyCryptoKeyImpl(AsymmetricKeyData&& key, bool extractable)
    : CryptoKey::Impl(extractable, key.usages),
      keyData(kj::mv(key.evpPkey)),
      keyType(key.keyType) {
  KJ_DASSERT(keyType != KeyType::SECRET);
}

jsg::BufferSource AsymmetricKeyCryptoKeyImpl::signatureSslToWebCrypto(
    jsg::Lock& js, kj::ArrayPtr<kj::byte> signature) const {
  auto backing = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, signature.size());
  backing.asArrayPtr().copyFrom(signature);
  return jsg::BufferSource(js, kj::mv(backing));
}

jsg::BufferSource AsymmetricKeyCryptoKeyImpl::signatureWebCryptoToSsl(
    jsg::Lock& js, kj::ArrayPtr<const kj::byte> signature) const {
  auto backing = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, signature.size());
  backing.asArrayPtr().copyFrom(signature);
  return jsg::BufferSource(js, kj::mv(backing));
}

SubtleCrypto::ExportKeyData AsymmetricKeyCryptoKeyImpl::exportKey(
    jsg::Lock& js, kj::StringPtr format) const {
  // EVP_marshal_{public,private}_key() functions are BoringSSL
  // extensions which export asymmetric keys in DER format.
  // DER is the binary format which *should* work to export any EVP_PKEY.

  uint8_t* der = nullptr;
  KJ_DEFER(if (der != nullptr) { OPENSSL_free(der); });
  size_t derLen;
  bssl::ScopedCBB cbb;
  if (format == "pkcs8"_kj) {
    JSG_REQUIRE(keyType == KeyType::PRIVATE, DOMInvalidAccessError,
        "Asymmetric pkcs8 export requires private key (not \"", toStringPtr(keyType), "\").");
    if (!CBB_init(cbb.get(), 0) || !EVP_marshal_private_key(cbb.get(), keyData.get()) ||
        !CBB_finish(cbb.get(), &der, &derLen)) {
      JSG_FAIL_REQUIRE(DOMOperationError, "Private key export failed.");
    }
  } else if (format == "spki"_kj) {
    JSG_REQUIRE(keyType == KeyType::PUBLIC, DOMInvalidAccessError,
        "Asymmetric spki export requires public key (not \"", toStringPtr(keyType), "\").");
    if (!CBB_init(cbb.get(), 0) || !EVP_marshal_public_key(cbb.get(), keyData.get()) ||
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
    return exportRaw(js);
  } else {
    JSG_FAIL_REQUIRE(DOMInvalidAccessError, "Cannot export \"", getAlgorithmName(), "\" in \"",
        format, "\" format.");
  }

  auto backing = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, derLen);
  auto src = kj::arrayPtr(der, derLen);
  backing.asArrayPtr().copyFrom(src);
  return jsg::BufferSource(js, kj::mv(backing));
}

jsg::BufferSource AsymmetricKeyCryptoKeyImpl::exportKeyExt(jsg::Lock& js,
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
    // const_cast is acceptable, pass will be reassigned before it is written to.
    char* pass = const_cast<char*>(&EMPTY_PASSPHRASE[0]);
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
    auto result = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, bptr->length);
    auto src = kj::arrayPtr(bptr->data, bptr->length);
    result.asArrayPtr().copyFrom(src.asBytes());
    return jsg::BufferSource(js, kj::mv(result));
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
    JSG_REQUIRE(
        EVP_PKEY_id(pkey) == EVP_PKEY_RSA, TypeError, "The pkcs1 type is only valid for RSA keys.");
    auto rsa = EVP_PKEY_get1_RSA(pkey);
    KJ_DEFER(RSA_free(rsa));
    if (format == "pem"_kj) {
      auto enc = getEncDetail();
      if (PEM_write_bio_RSAPrivateKey(bio.get(), rsa, enc.cipher,
              reinterpret_cast<unsigned char*>(enc.pass), enc.pass_len, nullptr, nullptr) == 1) {
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
              bio.get(), pkey, enc.cipher, enc.pass, enc.pass_len, nullptr, nullptr) == 1) {
        return fromBio(format);
      }
    } else if (format == "der"_kj) {
      if (i2d_PKCS8PrivateKey_bio(
              bio.get(), pkey, enc.cipher, enc.pass, enc.pass_len, nullptr, nullptr) == 1) {
        return fromBio(format);
      }
    }
  } else if (type == "sec1"_kj) {
    // SEC1 is only used for EC keys.
    JSG_REQUIRE(
        EVP_PKEY_id(pkey) == EVP_PKEY_EC, TypeError, "The sec1 type is only valid for EC keys.");
    auto ec = EVP_PKEY_get1_EC_KEY(pkey);
    KJ_DEFER(EC_KEY_free(ec));
    if (format == "pem"_kj) {
      auto enc = getEncDetail();
      if (PEM_write_bio_ECPrivateKey(bio.get(), ec, enc.cipher,
              reinterpret_cast<unsigned char*>(enc.pass), enc.pass_len, nullptr, nullptr) == 1) {
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

jsg::BufferSource AsymmetricKeyCryptoKeyImpl::sign(jsg::Lock& js,
    SubtleCrypto::SignAlgorithm&& algorithm,
    kj::ArrayPtr<const kj::byte> data) const {
  JSG_REQUIRE(keyType == KeyType::PRIVATE, DOMInvalidAccessError,
      "Asymmetric signing requires a private key.");

  auto type = lookupDigestAlgorithm(chooseHash(algorithm.hash)).second;
  if (getAlgorithmName() == "RSASSA-PKCS1-v1_5") {
    // RSASSA-PKCS1-v1_5 requires the RSA key to be at least as big as the digest size
    // plus a 15 to 19 byte digest-specific prefix (see BoringSSL's RSA_add_pkcs1_prefix) plus 11
    // bytes for padding (see RSA_PKCS1_PADDING_SIZE). For simplicity, require the key to be at
    // least 32 bytes larger than the hash digest.
    // Similar checks could also be adopted for more detailed error handling in verify(), but the
    // current approach should be sufficient to avoid internal errors.
    RSA& rsa = JSG_REQUIRE_NONNULL(EVP_PKEY_get0_RSA(getEvpPkey()), DOMDataError, "Missing RSA key",
        tryDescribeOpensslErrors());

    JSG_REQUIRE(EVP_MD_size(type) + 32 <= RSA_size(&rsa), DOMOperationError,
        "key too small for signing with given digest, need at least ", 8 * (EVP_MD_size(type) + 32),
        "bits.");
  } else if (getAlgorithmName() == "RSA-PSS") {
    // Similarly, RSA-PSS requires keys to be at least the size of the digest and salt plus 2
    // bytes, see https://developer.mozilla.org/en-US/docs/Web/API/RsaPssParams for details.
    RSA& rsa = JSG_REQUIRE_NONNULL(EVP_PKEY_get0_RSA(getEvpPkey()), DOMDataError, "Missing RSA key",
        tryDescribeOpensslErrors());
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

  KJ_STACK_ARRAY(kj::byte, signature, signatureSize, 256, 256);
  OSSLCALL(EVP_DigestSignFinal(digestCtx.get(), signature.begin(), &signatureSize));

  KJ_ASSERT(signatureSize <= signature.size());
  if (signatureSize < signature.size()) {
    return signatureSslToWebCrypto(js, signature.first(signatureSize));
  }

  return signatureSslToWebCrypto(js, signature);
}

bool AsymmetricKeyCryptoKeyImpl::verify(jsg::Lock& js,
    SubtleCrypto::SignAlgorithm&& algorithm,
    kj::ArrayPtr<const kj::byte> signature,
    kj::ArrayPtr<const kj::byte> data) const {
  ClearErrorOnReturn clearErrorOnReturn;

  JSG_REQUIRE(keyType == KeyType::PUBLIC, DOMInvalidAccessError,
      "Asymmetric verification requires a public key.");

  auto sslSignature = signatureWebCryptoToSsl(js, signature);

  auto type = lookupDigestAlgorithm(chooseHash(algorithm.hash)).second;

  auto digestCtx = OSSL_NEW(EVP_MD_CTX);

  OSSLCALL(EVP_DigestVerifyInit(digestCtx.get(), nullptr, type, nullptr, keyData.get()));
  addSalt(digestCtx->pctx, algorithm);
  // No-op call unless CryptoKey is RsaPss
  OSSLCALL(EVP_DigestVerifyUpdate(digestCtx.get(), data.begin(), data.size()));
  // EVP_DigestVerifyFinal() returns 1 on success, 0 on invalid signature, and any other value
  // indicates "a more serious error".
  auto result = EVP_DigestVerifyFinal(
      digestCtx.get(), sslSignature.asArrayPtr().begin(), sslSignature.size());
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

kj::StringPtr AsymmetricKeyCryptoKeyImpl::getType() const {
  return toStringPtr(keyType);
}

bool AsymmetricKeyCryptoKeyImpl::verifyX509Public(const X509* cert) const {
  ClearErrorOnReturn clearErrorOnReturn;
  return X509_verify(const_cast<X509*>(cert), getEvpPkey()) > 0;
}

bool AsymmetricKeyCryptoKeyImpl::verifyX509Private(const X509* cert) const {
  ClearErrorOnReturn clearErrorOnReturn;
  return X509_check_private_key(const_cast<X509*>(cert), getEvpPkey()) == 1;
}

// ======================================================================================

AsymmetricKeyData importAsymmetricForWebCrypto(jsg::Lock& js,
    kj::StringPtr format,
    SubtleCrypto::ImportKeyData keyData,
    kj::StringPtr normalizedName,
    bool extractable,
    kj::ArrayPtr<const kj::String> keyUsages,
    kj::FunctionParam<kj::Own<EVP_PKEY>(SubtleCrypto::JsonWebKey)> readJwk,
    CryptoKeyUsageSet allowedUsages) {
  CryptoKeyUsageSet usages;
  if (format == "jwk") {
    // I found jww's SO answer immeasurably helpful while writing this:
    // https://stackoverflow.com/questions/24093272/how-to-load-a-private-key-from-a-jwk-into-openssl

    auto& keyDataJwk = JSG_REQUIRE_NONNULL(keyData.tryGet<SubtleCrypto::JsonWebKey>(), DOMDataError,
        "JSON Web Key import requires a JSON Web Key object.");

    KeyType keyType = KeyType::PRIVATE;
    if (keyDataJwk.d != kj::none) {
      // Private key (`d` is the private exponent, per RFC 7518).
      keyType = KeyType::PRIVATE;
      usages =
          CryptoKeyUsageSet::validate(normalizedName, CryptoKeyUsageSet::Context::importPrivate,
              keyUsages, allowedUsages & CryptoKeyUsageSet::privateKeyMask());

      // https://tools.ietf.org/html/rfc7518#section-6.3.2.7
      // We don't support keys with > 2 primes, so error out.
      JSG_REQUIRE(keyDataJwk.oth == kj::none, DOMNotSupportedError,
          "Multi-prime private keys not supported.");
    } else {
      // Public key.
      keyType = KeyType::PUBLIC;
      auto strictCrypto = FeatureFlags::get(js).getStrictCrypto();
      // restrict key usages to public key usages. In the case of ECDH, usages must be empty, but
      // if the strict crypto compat flag is not enabled allow the same usages as with private ECDH
      // keys, i.e. derivationKeyMask().
      usages = CryptoKeyUsageSet::validate(normalizedName, CryptoKeyUsageSet::Context::importPublic,
          keyUsages,
          allowedUsages &
              (normalizedName == "ECDH"
                      ? strictCrypto ? CryptoKeyUsageSet() : CryptoKeyUsageSet::derivationKeyMask()
                      : CryptoKeyUsageSet::publicKeyMask()));
    }

    auto [expectedUse, op0, op1] = [&, normalizedName] {
      if (normalizedName == "RSA-OAEP") {
        return std::make_tuple("enc", "encrypt", "wrapKey");
      }
      if (normalizedName == "ECDH" || normalizedName == "X25519") {
        return std::make_tuple("enc", "unused", "unused");
      }
      return std::make_tuple("sig", "sign", "verify");
    }();

    if (keyUsages.size() > 0) {
      KJ_IF_SOME(use, keyDataJwk.use) {
        JSG_REQUIRE(use == expectedUse, DOMDataError,
            "Asymmetric \"jwk\" key import with usages requires a JSON Web Key with "
            "Public Key Use parameter \"use\" (\"",
            use, "\") equal to \"sig\".");
      }
    }

    KJ_IF_SOME(ops, keyDataJwk.key_ops) {
      // TODO(cleanup): When we implement other JWK import functions, factor this part out into a
      //   JWK validation function.

      // "The key operation values are case-sensitive strings.  Duplicate key operation values MUST
      // NOT be present in the array." -- RFC 7517, section 4.3
      std::sort(ops.begin(), ops.end());
      JSG_REQUIRE(std::adjacent_find(ops.begin(), ops.end()) == ops.end(), DOMDataError,
          "A JSON Web Key's Key Operations parameter (\"key_ops\") "
          "must not contain duplicates.");

      KJ_IF_SOME(use, keyDataJwk.use) {
        // "The "use" and "key_ops" JWK members SHOULD NOT be used together; however, if both are
        // used, the information they convey MUST be consistent." -- RFC 7517, section 4.3.

        JSG_REQUIRE(use == expectedUse, DOMDataError,
            "Asymmetric \"jwk\" import requires a JSON "
            "Web Key with Public Key Use \"use\" (\"",
            use, "\") equal to \"", expectedUse, "\".");

        for (const auto& op: ops) {
          JSG_REQUIRE(normalizedName != "ECDH" && normalizedName != "X25519", DOMDataError,
              "A JSON Web Key should have either a Public Key Use parameter (\"use\") or a Key "
              "Operations parameter (\"key_ops\"); otherwise, the parameters must be consistent "
              "with each other. For public ",
              normalizedName,
              " keys, there are no valid usages,"
              "so keys with a non-empty \"key_ops\" parameter are not allowed.");

          // TODO(conform): Can a JWK private key actually be used to verify? Not
          //   using the Web Crypto API...
          JSG_REQUIRE(op == op0 || op == op1, DOMDataError,
              "A JSON Web Key should have either a Public Key Use parameter (\"use\") or a Key "
              "Operations parameter (\"key_ops\"); otherwise, the parameters must be consistent "
              "with each other. A Public Key Use for ",
              normalizedName,
              " would allow a Key "
              "Operations array with only \"",
              op0, "\" and/or \"", op1, "\" values (not \"", op, "\").");
        }
      }

      // We're supposed to verify that `ops` contains all the values listed in `keyUsages`. For any
      // of the supported algorithms, a key may have at most two distinct usages ('sig' type keys
      // have at most one valid usage, but there may be two for e.g. ECDH). Test the first usage
      // and the next usages. Test the first usage and the first usage distinct from the first, if
      // present (i.e. the second allowed usage, even if there are duplicates).
      if (keyUsages.size() > 0) {
        JSG_REQUIRE(std::find(ops.begin(), ops.end(), keyUsages.front()) != ops.end(), DOMDataError,
            "All specified key usages must be present in the JSON "
            "Web Key's Key Operations parameter (\"key_ops\").");
        auto secondUsage = std::find_end(keyUsages.begin(), keyUsages.end(), keyUsages.begin(),
                               keyUsages.begin() + 1) +
            1;
        if (secondUsage != keyUsages.end()) {
          JSG_REQUIRE(std::find(ops.begin(), ops.end(), *secondUsage) != ops.end(), DOMDataError,
              "All specified key usages must be present in the JSON "
              "Web Key's Key Operations parameter (\"key_ops\").");
        }
      }
    }

    KJ_IF_SOME(ext, keyDataJwk.ext) {
      // If the user requested this key to be extractable, make sure the JWK does not disallow it.
      JSG_REQUIRE(!extractable || ext, DOMDataError,
          "Cannot create an extractable CryptoKey from an unextractable JSON Web Key.");
    }

    return {readJwk(kj::mv(keyDataJwk)), keyType, usages};
  } else if (format == "spki") {
    kj::ArrayPtr<const kj::byte> keyBytes =
        JSG_REQUIRE_NONNULL(keyData.tryGet<kj::Array<kj::byte>>(), DOMDataError,
            "SPKI import requires an ArrayBuffer.");
    const kj::byte* ptr = keyBytes.begin();
    auto evpPkey = OSSLCALL_OWN(
        EVP_PKEY, d2i_PUBKEY(nullptr, &ptr, keyBytes.size()), DOMDataError, "Invalid SPKI input.");
    if (ptr != keyBytes.end()) {
      JSG_FAIL_REQUIRE(
          DOMDataError, "Invalid ", keyBytes.end() - ptr, " trailing bytes after SPKI input.");
    }

    // usages must be empty for ECDH public keys, so use CryptoKeyUsageSet() when validating the
    // usage set.
    usages = CryptoKeyUsageSet::validate(normalizedName, CryptoKeyUsageSet::Context::importPublic,
        keyUsages,
        allowedUsages &
            (normalizedName == "ECDH" ? CryptoKeyUsageSet() : CryptoKeyUsageSet::publicKeyMask()));
    return {kj::mv(evpPkey), KeyType::PUBLIC, usages};
  } else if (format == "pkcs8") {
    kj::ArrayPtr<const kj::byte> keyBytes =
        JSG_REQUIRE_NONNULL(keyData.tryGet<kj::Array<kj::byte>>(), DOMDataError,
            "PKCS8 import requires an ArrayBuffer.");
    const kj::byte* ptr = keyBytes.begin();
    auto evpPkey = OSSLCALL_OWN(EVP_PKEY, d2i_AutoPrivateKey(nullptr, &ptr, keyBytes.size()),
        DOMDataError, "Invalid PKCS8 input.");
    if (ptr != keyBytes.end()) {
      JSG_FAIL_REQUIRE(
          DOMDataError, "Invalid ", keyBytes.end() - ptr, " trailing bytes after PKCS8 input.");
    }
    usages = CryptoKeyUsageSet::validate(normalizedName, CryptoKeyUsageSet::Context::importPrivate,
        keyUsages, allowedUsages & CryptoKeyUsageSet::privateKeyMask());
    return {kj::mv(evpPkey), KeyType::PRIVATE, usages};
  } else {
    JSG_FAIL_REQUIRE(DOMNotSupportedError, "Unrecognized key import format \"", format, "\".");
  }
}

}  // namespace workerd::api
