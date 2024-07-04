#include "keys.h"
#include <openssl/evp.h>
#include <openssl/ec_key.h>
#include <openssl/crypto.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

KJ_DECLARE_NON_POLYMORPHIC(X509);
KJ_DECLARE_NON_POLYMORPHIC(PKCS8_PRIV_KEY_INFO);

namespace workerd::api {

kj::Maybe<KeyEncoding> tryGetKeyEncoding(const kj::Maybe<kj::String>& encoding) {
  KJ_IF_SOME(enc, encoding) {
    if (enc == "pkcs1"_kj) return KeyEncoding::PKCS1;
    else if (enc == "pkcs8"_kj) return KeyEncoding::PKCS8;
    else if (enc == "spki"_kj) return KeyEncoding::SPKI;
    else if (enc == "sec1"_kj) return KeyEncoding::SEC1;
  }
  return kj::none;
}

kj::Maybe<KeyFormat> tryGetKeyFormat(const kj::Maybe<kj::String>& format) {
  KJ_IF_SOME(form, format) {
    if (form == "pem"_kj) return KeyFormat::PEM;
    else if (form == "der"_kj) return KeyFormat::DER;
    else if (form == "jwk"_kj) return KeyFormat::JWK;
  }
  return kj::none;
}

kj::StringPtr toStringPtr(KeyType type) {
  switch (type) {
    case KeyType::SECRET: return "secret"_kj;
    case KeyType::PUBLIC: return "public"_kj;
    case KeyType::PRIVATE: return "private"_kj;
  }
  KJ_UNREACHABLE;
}

AsymmetricKeyCryptoKeyImpl::AsymmetricKeyCryptoKeyImpl(kj::Rc<AsymmetricKeyData> key,
                                                       bool extractable)
    : CryptoKey::Impl(extractable, key->usages),
      keyData(kj::mv(key)) {
  KJ_DASSERT(keyData->keyType != KeyType::SECRET);
}

kj::Array<kj::byte> AsymmetricKeyCryptoKeyImpl::signatureSslToWebCrypto(
    kj::Array<kj::byte> signature) const {
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
  auto keyType = keyData->keyType;
  if (format == "pkcs8"_kj) {
    JSG_REQUIRE(keyType == KeyType::PRIVATE, DOMInvalidAccessError,
        "Asymmetric pkcs8 export requires private key (not \"", toStringPtr(keyType), "\").");
    if (!CBB_init(cbb.get(), 0) ||
        !EVP_marshal_private_key(cbb.get(), getEvpPkey()) ||
        !CBB_finish(cbb.get(), &der, &derLen)) {
      JSG_FAIL_REQUIRE(DOMOperationError, "Private key export failed.");
    }
  } else if (format == "spki"_kj) {
      JSG_REQUIRE(keyType == KeyType::PUBLIC, DOMInvalidAccessError,
        "Asymmetric spki export requires public key (not \"", toStringPtr(keyType), "\").");
    if (!CBB_init(cbb.get(), 0) ||
        !EVP_marshal_public_key(cbb.get(), getEvpPkey()) ||
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

kj::Array<kj::byte> AsymmetricKeyCryptoKeyImpl::sign(
    SubtleCrypto::SignAlgorithm&& algorithm,
    kj::ArrayPtr<const kj::byte> data) const {
  JSG_REQUIRE(getTypeEnum() == KeyType::PRIVATE, DOMInvalidAccessError,
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

  OSSLCALL(EVP_DigestSignInit(digestCtx.get(), nullptr, type, nullptr, getEvpPkey()));
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

  JSG_REQUIRE(getTypeEnum() == KeyType::PUBLIC, DOMInvalidAccessError,
      "Asymmetric verification requires a public key.");

  auto sslSignature = signatureWebCryptoToSsl(signature);

  auto type = lookupDigestAlgorithm(chooseHash(algorithm.hash)).second;

  auto digestCtx = OSSL_NEW(EVP_MD_CTX);

  OSSLCALL(EVP_DigestVerifyInit(digestCtx.get(), nullptr, type, nullptr, getEvpPkey()));
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
    return EVP_PKEY_cmp(getEvpPkey(), otherImpl.getEvpPkey()) == 1;
  }
  return false;
}

kj::StringPtr AsymmetricKeyCryptoKeyImpl::getType() const {
  return toStringPtr(keyData->keyType);
}
AsymmetricKeyData::Kind AsymmetricKeyData::getKind() const {
  switch (EVP_PKEY_id(evpPkey.get())) {
    case EVP_PKEY_RSA: return Kind::RSA;
    case EVP_PKEY_EC: return Kind::EC;
    case EVP_PKEY_DSA: return Kind::DSA;
    case EVP_PKEY_DH: return Kind::DH;
    case EVP_PKEY_ED25519: return Kind::ED25519;
    case EVP_PKEY_X25519: return Kind::X25519;
  }
  return Kind::UNKNOWN;
}

kj::StringPtr AsymmetricKeyData::getKindName() const {
  switch (getKind()) {
    case Kind::RSA: return "rsa"_kj;
    case Kind::RSA_PSS: return "rsa-pss"_kj;
    case Kind::DSA: return "dsa"_kj;
    case Kind::EC: return "ec"_kj;
    case Kind::X25519: return "x25519"_kj;
    case Kind::ED25519: return "ed25519"_kj;
    case Kind::DH: return "dh"_kj;
    default: return nullptr;
  }
  KJ_UNREACHABLE;
}

bool AsymmetricKeyData::equals(const kj::Rc<AsymmetricKeyData>& other) const {
  ClearErrorOnReturn clearErrorOnReturn;
  auto ret = EVP_PKEY_cmp(evpPkey.get(), other->evpPkey.get());
  if (ret < 0) throwOpensslError(__FILE__, __LINE__, "Asymmetric key comparison");
  return ret == 1;
}

bool AsymmetricKeyCryptoKeyImpl::verifyX509Public(const X509* cert) const {
  ClearErrorOnReturn clearErrorOnReturn;
  return X509_verify(const_cast<X509*>(cert), getEvpPkey()) > 0;
}

bool AsymmetricKeyCryptoKeyImpl::verifyX509Private(const X509* cert) const{
  ClearErrorOnReturn clearErrorOnReturn;
  return X509_check_private_key(const_cast<X509*>(cert), getEvpPkey()) == 1;
}

// ======================================================================================

kj::Rc<AsymmetricKeyData> importAsymmetricForWebCrypto(
    jsg::Lock& js,
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

    auto& keyDataJwk = JSG_REQUIRE_NONNULL(keyData.tryGet<SubtleCrypto::JsonWebKey>(),
        DOMDataError, "JSON Web Key import requires a JSON Web Key object.");

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
      KJ_IF_SOME(use, keyDataJwk.use) {
        JSG_REQUIRE(use == expectedUse, DOMDataError,
            "Asymmetric \"jwk\" key import with usages requires a JSON Web Key with "
            "Public Key Use parameter \"use\" (\"", use, "\") equal to \"sig\".");
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

        JSG_REQUIRE(use == expectedUse, DOMDataError, "Asymmetric \"jwk\" import requires a JSON "
            "Web Key with Public Key Use \"use\" (\"", use, "\") equal to \"", expectedUse, "\".");

        for (const auto& op: ops) {
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
        JSG_REQUIRE(std::find(ops.begin(), ops.end(), keyUsages.front()) != ops.end(),
            DOMDataError, "All specified key usages must be present in the JSON "
            "Web Key's Key Operations parameter (\"key_ops\").");
        auto secondUsage = std::find_end(keyUsages.begin(), keyUsages.end(), keyUsages.begin(),
            keyUsages.begin() + 1) + 1;
        if (secondUsage != keyUsages.end()) {
          JSG_REQUIRE(std::find(ops.begin(), ops.end(), *secondUsage) != ops.end(),
              DOMDataError, "All specified key usages must be present in the JSON "
              "Web Key's Key Operations parameter (\"key_ops\").");
        }
      }
    }

    KJ_IF_SOME(ext, keyDataJwk.ext) {
      // If the user requested this key to be extractable, make sure the JWK does not disallow it.
      JSG_REQUIRE(!extractable || ext, DOMDataError,
          "Cannot create an extractable CryptoKey from an unextractable JSON Web Key.");
    }

    return kj::rc<AsymmetricKeyData>(readJwk(kj::mv(keyDataJwk)), keyType, usages);
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
    return kj::rc<AsymmetricKeyData>(kj::mv(evpPkey), KeyType::PUBLIC, usages);
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
    return kj::rc<AsymmetricKeyData>(kj::mv(evpPkey), KeyType::PRIVATE, usages);
  } else {
    JSG_FAIL_REQUIRE(DOMNotSupportedError, "Unrecognized key import format \"", format, "\".");
  }
}

// ======================================================================================
namespace {
int passwordCallback(char* buf, int size, int rwflag, void* u) {
  KJ_IF_SOME(passphrase, *reinterpret_cast<kj::Maybe<kj::Array<kj::byte>>*>(u)) {
    auto dest = kj::arrayPtr<char>(buf, size);
    if (dest.size() >= passphrase.size()) {
      dest.first(passphrase.size()).copyFrom(passphrase.asChars());
      return passphrase.size();
    }
  }
  return -1;
}

kj::Maybe<kj::ArrayPtr<const kj::byte>> isASN1Sequence(kj::ArrayPtr<const kj::byte> data) {
  if (data.size() < 2 || data[0] != 0x30)
    return kj::none;

  if (data[1] & 0x80) {
    // Long form.
    size_t n_bytes = data[1] & ~0x80;
    if (n_bytes + 2 > data.size() || n_bytes > sizeof(size_t))
      return kj::none;
    size_t length = 0;
    for (size_t i = 0; i < n_bytes; i++)
      length = (length << 8) | data[i + 2];
    size_t start = 2 + n_bytes;
    size_t end = start + kj::min(data.size() - start, length);
    return data.slice(start, end);
  }

  // Short form.
  size_t start = 2;
  size_t end = start + std::min<size_t>(data.size() - 2, data[1]);
  return data.slice(start, end);
}

bool isRSAPrivateKey(kj::ArrayPtr<const kj::byte> data) {
  // Both RSAPrivateKey and RSAPublicKey structures start with a SEQUENCE.
  KJ_IF_SOME(view, isASN1Sequence(data)) {
    // An RSAPrivateKey sequence always starts with a single-byte integer whose
    // value is either 0 or 1, whereas an RSAPublicKey starts with the modulus
    // (which is the product of two primes and therefore at least 4), so we can
    // decide the type of the structure based on the first three bytes of the
    // sequence.
    return view.size() >= 3 && data[0] == 2 && data[1] == 1 && !(data[2] & 0xfe);
  }
  return false;
}

bool isEncryptedPrivateKeyInfo(kj::ArrayPtr<const kj::byte> data) {
  // Both PrivateKeyInfo and EncryptedPrivateKeyInfo start with a SEQUENCE.
  KJ_IF_SOME(view, isASN1Sequence(data)) {
    // An EncryptedPrivateKeyInfo sequence always starts with an AlgorithmIdentifier
    // whereas a PrivateKeyInfo starts with an integer.
    return view.size() >= 1 && data[0] != 2;
  }
  return false;
}

template <typename Func>
kj::Maybe<kj::Own<EVP_PKEY>> tryParsePublicKey(BIO* bp, const char* name, Func parse) {
  kj::byte* der_data;
  long der_len;  // NOLINT(runtime/int)
  KJ_DEFER(OPENSSL_clear_free(der_data, der_len));

  // This skips surrounding data and decodes PEM to DER.
  {
    MarkPopErrorOnReturn mark_pop_error_on_return;
    if (PEM_bytes_read_bio(&der_data, &der_len, nullptr, name, bp, nullptr, nullptr) != 1)
      return kj::none;
  }

  // OpenSSL might modify the pointer, so we need to make a copy before parsing.
  const kj::byte* p = der_data;
  auto ptr = parse(&p, der_len);
  if (ptr == nullptr || p != der_data + der_len) {
    return kj::none;
  }
  return kj::disposeWith<EVP_PKEY_free>(ptr);
}

kj::Maybe<kj::Own<EVP_PKEY>> parsePublicKeyPEM(kj::ArrayPtr<const kj::byte> keyData) {
  auto ptr = BIO_new_mem_buf(keyData.asChars().begin(), keyData.size());
  if (ptr == nullptr) {
    return kj::none;
  }
  auto bio = kj::disposeWith<BIO_free_all>(ptr);

  // Try parsing as a SubjectPublicKeyInfo first.
  KJ_IF_SOME(pkey, tryParsePublicKey(bio.get(), "PUBLIC KEY",
      [](const unsigned char** p, long l) -> EVP_PKEY* {
        return d2i_PUBKEY(nullptr, p, l);
      })) {
    return kj::mv(pkey);
  }

  // Maybe it is PKCS#1.
  KJ_ASSERT(BIO_reset(bio.get()));
  KJ_IF_SOME(pkey, tryParsePublicKey(bio.get(), "RSA PUBLIC KEY",
      [](const unsigned char** p, long l) -> EVP_PKEY* {
        return d2i_PublicKey(EVP_PKEY_RSA, nullptr, p, l);
      })) {
    return kj::mv(pkey);
  }

  // X.509 fallback.
  KJ_ASSERT(BIO_reset(bio.get()));
  return tryParsePublicKey(bio.get(), "CERTIFICATE",
      [](const unsigned char** p, long l) -> EVP_PKEY* {
        auto ptr = d2i_X509(nullptr, p, l);
        if (ptr == nullptr) {
          return nullptr;
        }
        auto x509 = kj::disposeWith<X509_free>(ptr);
        return x509 ? X509_get_pubkey(x509.get()) : nullptr;
      });
}

kj::Maybe<kj::Own<EVP_PKEY>> parsePublicKey(kj::ArrayPtr<const kj::byte> keyData,
                                            KeyFormat format,
                                            KeyEncoding encoding) {
  if (format == KeyFormat::PEM) {
    return parsePublicKeyPEM(keyData);
  }

  KJ_ASSERT(format == KeyFormat::DER);

  auto p = keyData.begin();
  if (encoding == KeyEncoding::PKCS1) {
    auto ptr = d2i_PublicKey(EVP_PKEY_RSA, nullptr, &p, keyData.size());
    if (ptr == nullptr) {
      return kj::none;
    }
  }

  KJ_ASSERT(encoding == KeyEncoding::SPKI);
  auto ptr = d2i_PUBKEY(nullptr, &p, keyData.size());
  if (ptr == nullptr) {
    return kj::none;
  }
  return kj::disposeWith<EVP_PKEY_free>(ptr);
}
}  // namespace

kj::Maybe<kj::Rc<AsymmetricKeyData>> importAsymmetricPrivateKeyForNodeJs(
    kj::ArrayPtr<const kj::byte> keyData,
    KeyFormat format,
    const kj::Maybe<KeyEncoding>& maybeEncoding,
    kj::Maybe<kj::Array<kj::byte>>& passphrase) {
  ClearErrorOnReturn clearErrorOnReturn;

  const auto checkAndReturn = [&](EVP_PKEY* pkey) {
    JSG_REQUIRE(pkey != nullptr, Error, "Failed to create private key");
        int err = clearErrorOnReturn.peekError();
    if (pkey == nullptr || err != 0) {
      if (ERR_GET_LIB(err) == ERR_LIB_PEM && ERR_GET_REASON(err) == PEM_R_BAD_PASSWORD_READ) {
        JSG_FAIL_REQUIRE(Error, "Failed to read private key due to incorrect passphrase");
      }
      JSG_FAIL_REQUIRE(Error, "Failed to read private key");
    }
    return kj::rc<AsymmetricKeyData>(kj::disposeWith<EVP_PKEY_free>(pkey),
                                    KeyType::PRIVATE,
                                    CryptoKeyUsageSet::privateKeyMask());
  };

  if (format == KeyFormat::PEM) {
    auto ptr = BIO_new_mem_buf(keyData.begin(), keyData.size());
    JSG_REQUIRE(ptr != nullptr, Error, "Failed to create private key");
    auto bio = kj::disposeWith<BIO_free_all>(ptr);
    return checkAndReturn(PEM_read_bio_PrivateKey(bio.get(), nullptr,
                          passwordCallback, &passphrase));
  }

  KJ_ASSERT(format == KeyFormat::DER);
  switch (KJ_ASSERT_NONNULL(maybeEncoding)) {
    case KeyEncoding::PKCS1: {
      auto p = keyData.begin();
      return checkAndReturn(d2i_PrivateKey(EVP_PKEY_RSA, nullptr, &p, keyData.size()));
    }
    case KeyEncoding::PKCS8: {
      auto ptr = BIO_new_mem_buf(keyData.begin(), keyData.size());
      JSG_REQUIRE(ptr != nullptr, Error, "Failed to create private key");
      auto bio = kj::disposeWith<BIO_free_all>(ptr);

      if (isEncryptedPrivateKeyInfo(keyData)) {
        return checkAndReturn(d2i_PKCS8PrivateKey_bio(bio.get(), nullptr,
                                                      passwordCallback, &passphrase));
      } else {
        auto ptr = d2i_PKCS8_PRIV_KEY_INFO_bio(bio.get(), nullptr);
        JSG_REQUIRE(ptr != nullptr, Error, "Failed to create private key");
        auto p8inf = kj::disposeWith<PKCS8_PRIV_KEY_INFO_free>(ptr);
        return checkAndReturn(EVP_PKCS82PKEY(p8inf.get()));
      }
    }
    case KeyEncoding::SEC1: {
      auto p = keyData.begin();
      return checkAndReturn(d2i_PrivateKey(EVP_PKEY_EC, nullptr, &p, keyData.size()));
    }
    default: JSG_FAIL_REQUIRE(Error, "Failed to read private key due to unsupported format");
  }

  return kj::none;
}

kj::Maybe<kj::Rc<AsymmetricKeyData>> importAsymmetricPublicKeyForNodeJs(
    kj::ArrayPtr<const kj::byte> keyData,
    KeyFormat format,
    const kj::Maybe<KeyEncoding>& maybeEncoding,
    kj::Maybe<kj::Array<kj::byte>>& passphrase) {
  if (format == KeyFormat::PEM) {
      // For PEM, we can easily determine whether it is a public or private key
      // by looking for the respective PEM tags.
      KJ_IF_SOME(pkey, parsePublicKeyPEM(keyData)) {
        return kj::rc<AsymmetricKeyData>(kj::mv(pkey), KeyType::PUBLIC,
                                         CryptoKeyUsageSet::publicKeyMask());
      }
      return importAsymmetricPrivateKeyForNodeJs(keyData, format, maybeEncoding, passphrase);
  }

  // For DER, the type determines how to parse it. SPKI, PKCS#8 and SEC1 are
  // easy, but PKCS#1 can be a public key or a private key.
  auto key = ([&]() -> kj::Maybe<kj::Rc<AsymmetricKeyData>> {
    switch (KJ_ASSERT_NONNULL(maybeEncoding)) {
      case KeyEncoding::PKCS1: {
        if (isRSAPrivateKey(keyData)) {
          return importAsymmetricPrivateKeyForNodeJs(keyData, format, maybeEncoding, passphrase);
        }
        KJ_IF_SOME(pkey, parsePublicKey(keyData, format, KeyEncoding::PKCS1)) {
          return kj::rc<AsymmetricKeyData>(kj::mv(pkey), KeyType::PUBLIC,
                                           CryptoKeyUsageSet::publicKeyMask());
        }
        return kj::none;
      }
      case KeyEncoding::SPKI: {
        KJ_IF_SOME(pkey, parsePublicKey(keyData, format, KeyEncoding::SPKI)) {
          return kj::rc<AsymmetricKeyData>(kj::mv(pkey), KeyType::PUBLIC,
                                           CryptoKeyUsageSet::publicKeyMask());
        }
        return kj::none;
      }
      case KeyEncoding::PKCS8: {
        return importAsymmetricPrivateKeyForNodeJs(keyData, format, maybeEncoding, passphrase);
      }
      case KeyEncoding::SEC1: {
        return importAsymmetricPrivateKeyForNodeJs(keyData, format, maybeEncoding, passphrase);
      }
    }
    KJ_UNREACHABLE;
  })();

  KJ_IF_SOME(k, key) {
    return derivePublicKeyFromPrivateKey(kj::mv(k));
  }
  return kj::none;
}

kj::Maybe<kj::Rc<AsymmetricKeyData>> derivePublicKeyFromPrivateKey(
    kj::Rc<AsymmetricKeyData> privateKeyData) {
  return kj::mv(privateKeyData);
}

}  // namespace workerd::api
