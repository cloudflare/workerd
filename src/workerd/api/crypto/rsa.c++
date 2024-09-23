#include "rsa.h"

#include "impl.h"
#include "keys.h"
#include "simdutf.h"
#include "util.h"

#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <kj/array.h>
#include <kj/common.h>

#include <map>

namespace workerd::api {

namespace {
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

kj::Array<kj::byte> bioToArray(BIO* bio) {
  BUF_MEM* bptr;
  BIO_get_mem_ptr(bio, &bptr);
  auto buf = kj::heapArray<char>(bptr->length);
  auto aptr = kj::arrayPtr(bptr->data, bptr->length);
  buf.asPtr().copyFrom(aptr);
  return buf.releaseAsBytes();
}

kj::Maybe<kj::Array<kj::byte>> simdutfBase64UrlDecode(kj::StringPtr input) {
  auto size = simdutf::maximal_binary_length_from_base64(input.begin(), input.size());
  auto buf = kj::heapArray<kj::byte>(size);
  auto result = simdutf::base64_to_binary(
      input.begin(), input.size(), buf.asChars().begin(), simdutf::base64_url);
  if (result.error != simdutf::SUCCESS) return kj::none;
  KJ_ASSERT(result.count <= size);
  return buf.first(result.count).attach(kj::mv(buf));
}

kj::Array<kj::byte> simdutfBase64UrlDecodeChecked(kj::StringPtr input, kj::StringPtr error) {
  return JSG_REQUIRE_NONNULL(simdutfBase64UrlDecode(input), Error, error);
}
}  // namespace

kj::Maybe<Rsa> Rsa::tryGetRsa(const EVP_PKEY* key) {
  int type = EVP_PKEY_id(key);
  if (type != EVP_PKEY_RSA && type != EVP_PKEY_RSA_PSS) return kj::none;
  auto rsa = EVP_PKEY_get0_RSA(key);
  if (rsa == nullptr) return kj::none;
  return Rsa(rsa);
}

Rsa::Rsa(RSA* rsa): rsa(rsa) {
  RSA_get0_key(rsa, &n, &e, &d);
}

size_t Rsa::getModulusBits() const {
  return getModulusSize() * 8;
}

size_t Rsa::getModulusSize() const {
  return RSA_size(rsa);
}

kj::Array<kj::byte> Rsa::getPublicExponent() {
  return KJ_REQUIRE_NONNULL(bignumToArray(*e));
}

CryptoKey::AsymmetricKeyDetails Rsa::getAsymmetricKeyDetail() const {
  CryptoKey::AsymmetricKeyDetails details;

  details.modulusLength = BN_num_bits(n);
  details.publicExponent =
      JSG_REQUIRE_NONNULL(bignumToArrayPadded(*e), Error, "Failed to extract public exponent");

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

kj::Array<kj::byte> Rsa::sign(const kj::ArrayPtr<const kj::byte> data) const {
  size_t size = getModulusSize();

  // RSA encryption/decryption requires the key value to be strictly larger than the value to be
  // signed. Ideally we would enforce this by checking that the key size is larger than the input
  // size – having both the same size makes it highly likely that some values are higher than the
  // key value – but there are scripts and test cases that depend on signing data with keys of
  // the same size.
  JSG_REQUIRE(data.size() <= size, DOMDataError, "Blind Signing requires presigned data (",
      data.size(),
      " bytes) to be smaller than "
      "the key (",
      size, " bytes).");
  if (data.size() == size) {
    auto dataVal = JSG_REQUIRE_NONNULL(toBignum(data), InternalDOMOperationError,
        "Error converting presigned data", internalDescribeOpensslErrors());
    JSG_REQUIRE(BN_ucmp(dataVal, getN()) < 0, DOMDataError,
        "Blind Signing requires presigned data value to be strictly smaller than RSA key"
        "modulus, consider using a larger key size.");
  }

  auto signature = kj::heapArray<kj::byte>(size);
  size_t signatureSize = 0;
  OSSLCALL(RSA_decrypt(rsa, &signatureSize, signature.begin(), signature.size(), data.begin(),
      data.size(), RSA_NO_PADDING));
  KJ_ASSERT(signatureSize <= signature.size());
  if (signatureSize < signature.size()) {
    // We did not fill the entire buffer, let's make sure we zero
    // out the rest of it so we don't leak any uninitialized data.
    signature.slice(signatureSize).fill(0);
    return signature.slice(0, signatureSize).attach(kj::mv(signature));
  }

  return kj::mv(signature);
}

kj::Array<kj::byte> Rsa::cipher(EVP_PKEY_CTX* ctx,
    SubtleCrypto::EncryptAlgorithm&& algorithm,
    kj::ArrayPtr<const kj::byte> data,
    EncryptDecryptFunction encryptDecrypt,
    const EVP_MD* digest) const {

  JSG_REQUIRE(1 == EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_OAEP_PADDING),
      InternalDOMOperationError, "Error doing RSA OAEP encrypt/decrypt (", "padding", ")",
      internalDescribeOpensslErrors());
  JSG_REQUIRE(1 == EVP_PKEY_CTX_set_rsa_oaep_md(ctx, digest), InternalDOMOperationError,
      "Error doing RSA OAEP encrypt/decrypt (", "message digest", ")",
      internalDescribeOpensslErrors());
  JSG_REQUIRE(1 == EVP_PKEY_CTX_set_rsa_mgf1_md(ctx, digest), InternalDOMOperationError,
      "Error doing RSA OAEP encrypt/decrypt (", "MGF1 digest", ")",
      internalDescribeOpensslErrors());

  KJ_IF_SOME(l, algorithm.label) {
    auto labelCopy = reinterpret_cast<uint8_t*>(OPENSSL_malloc(l.size()));
    KJ_DEFER(OPENSSL_free(labelCopy));
    // If setting the label fails we need to remember to destroy the buffer. In practice it can't
    // actually happen since we set RSA_PKCS1_OAEP_PADDING above & that appears to be the only way
    // this API call can fail.

    JSG_REQUIRE(labelCopy != nullptr, DOMOperationError,
        "Failed to allocate space for RSA-OAEP label copy", tryDescribeOpensslErrors());
    std::copy(l.begin(), l.end(), labelCopy);

    // EVP_PKEY_CTX_set0_rsa_oaep_label below takes ownership of the buffer passed in (must have
    // been OPENSSL_malloc-allocated).
    JSG_REQUIRE(1 == EVP_PKEY_CTX_set0_rsa_oaep_label(ctx, labelCopy, l.size()), DOMOperationError,
        "Failed to set RSA-OAEP label", tryDescribeOpensslErrors());

    // Ownership has now been transferred. The chromium WebCrypto code technically has a potential
    // memory leak here in that they check the error for EVP_PKEY_CTX_set0_rsa_oaep_label after
    // releasing. It's not actually possible though because the padding mode is set unconditionally
    // to RSA_PKCS1_OAEP_PADDING which seems to be the only way setting the label will fail.
    labelCopy = nullptr;
  }

  size_t maxResultLength = 0;
  // First compute an upper bound on the amount of space we need to store the encrypted/decrypted
  // result. Then we actually apply the encryption & finally resize to the actual correct length.
  JSG_REQUIRE(1 == encryptDecrypt(ctx, nullptr, &maxResultLength, data.begin(), data.size()),
      DOMOperationError, "Failed to compute length of RSA-OAEP result", tryDescribeOpensslErrors());

  kj::Vector<kj::byte> result(maxResultLength);
  auto err = encryptDecrypt(ctx, result.begin(), &maxResultLength, data.begin(), data.size());
  JSG_REQUIRE(
      1 == err, DOMOperationError, "RSA-OAEP failed encrypt/decrypt", tryDescribeOpensslErrors());
  result.resize(maxResultLength);

  return result.releaseAsArray();
}

SubtleCrypto::JsonWebKey Rsa::toJwk(
    KeyType keyType, kj::Maybe<kj::String> maybeHashAlgorithm) const {
  SubtleCrypto::JsonWebKey jwk;
  jwk.kty = kj::str("RSA");
  KJ_IF_SOME(name, maybeHashAlgorithm) {
    jwk.alg = kj::mv(name);
  }

  jwk.n = fastEncodeBase64Url(KJ_REQUIRE_NONNULL(bignumToArray(KJ_REQUIRE_NONNULL(n))));
  jwk.e = fastEncodeBase64Url(KJ_REQUIRE_NONNULL(bignumToArray(KJ_REQUIRE_NONNULL(e))));

  if (keyType == KeyType::PRIVATE) {
    jwk.d = fastEncodeBase64Url(KJ_REQUIRE_NONNULL(bignumToArray(KJ_REQUIRE_NONNULL(d))));
    jwk.p =
        fastEncodeBase64Url(KJ_REQUIRE_NONNULL(bignumToArray(KJ_REQUIRE_NONNULL(RSA_get0_p(rsa)))));
    jwk.q =
        fastEncodeBase64Url(KJ_REQUIRE_NONNULL(bignumToArray(KJ_REQUIRE_NONNULL(RSA_get0_q(rsa)))));
    jwk.dp = fastEncodeBase64Url(
        KJ_REQUIRE_NONNULL(bignumToArray(KJ_REQUIRE_NONNULL(RSA_get0_dmp1(rsa)))));
    jwk.dq = fastEncodeBase64Url(
        KJ_REQUIRE_NONNULL(bignumToArray(KJ_REQUIRE_NONNULL(RSA_get0_dmq1(rsa)))));
    jwk.qi = fastEncodeBase64Url(
        KJ_REQUIRE_NONNULL(bignumToArray(KJ_REQUIRE_NONNULL(RSA_get0_iqmp(rsa)))));
  }

  return jwk;
}

kj::Maybe<AsymmetricKeyData> Rsa::fromJwk(KeyType keyType, const SubtleCrypto::JsonWebKey& jwk) {
  ClearErrorOnReturn clearErrorOnReturn;

  if (jwk.kty != "RSA"_kj) return kj::none;
  auto n = JSG_REQUIRE_NONNULL(jwk.n.map([](auto& str) { return str.asPtr(); }), Error,
      "Invalid RSA key in JSON Web Key; missing or invalid "
      "Modulus parameter (\"n\").");
  auto e = JSG_REQUIRE_NONNULL(jwk.e.map([](auto& str) { return str.asPtr(); }), Error,
      "Invalid RSA key in JSON Web Key; missing or invalid "
      "Exponent parameter (\"e\").");

  auto rsa = OSSL_NEW(RSA);

  static constexpr auto kInvalidBase64Error = "Invalid RSA key in JSON Web Key; invalid base64."_kj;

  auto nDecoded = toBignumUnowned(simdutfBase64UrlDecodeChecked(n, kInvalidBase64Error));
  auto eDecoded = toBignumUnowned(simdutfBase64UrlDecodeChecked(e, kInvalidBase64Error));
  JSG_REQUIRE(RSA_set0_key(rsa.get(), nDecoded, eDecoded, nullptr) == 1, Error,
      "Invalid RSA key in JSON Web Key; failed to set key parameters");

  if (keyType == KeyType::PRIVATE) {
    auto d = JSG_REQUIRE_NONNULL(jwk.d.map([](auto& str) { return str.asPtr(); }), Error,
        "Invalid RSA key in JSON Web Key; missing or invalid "
        "Private Exponent parameter (\"d\").");
    auto p = JSG_REQUIRE_NONNULL(jwk.p.map([](auto& str) { return str.asPtr(); }), Error,
        "Invalid RSA key in JSON Web Key; missing or invalid "
        "First Prime Factor parameter (\"p\").");
    auto q = JSG_REQUIRE_NONNULL(jwk.q.map([](auto& str) { return str.asPtr(); }), Error,
        "Invalid RSA key in JSON Web Key; missing or invalid "
        "Second Prime Factor parameter (\"q\").");
    auto dp = JSG_REQUIRE_NONNULL(jwk.dp.map([](auto& str) { return str.asPtr(); }), Error,
        "Invalid RSA key in JSON Web Key; missing or invalid "
        "First Factor CRT Exponent parameter (\"dp\").");
    auto dq = JSG_REQUIRE_NONNULL(jwk.dq.map([](auto& str) { return str.asPtr(); }), Error,
        "Invalid RSA key in JSON Web Key; missing or invalid "
        "Second Factor CRT Exponent parameter (\"dq\").");
    auto qi = JSG_REQUIRE_NONNULL(jwk.qi.map([](auto& str) { return str.asPtr(); }), Error,
        "Invalid RSA key in JSON Web Key; missing or invalid "
        "First CRT Coefficient parameter (\"qi\").");
    auto dDecoded =
        toBignumUnowned(simdutfBase64UrlDecodeChecked(d, "Invalid RSA key in JSON Web Key"_kj));
    auto pDecoded = toBignumUnowned(simdutfBase64UrlDecodeChecked(p, kInvalidBase64Error));
    auto qDecoded = toBignumUnowned(simdutfBase64UrlDecodeChecked(q, kInvalidBase64Error));
    auto dpDecoded = toBignumUnowned(simdutfBase64UrlDecodeChecked(dp, kInvalidBase64Error));
    auto dqDecoded = toBignumUnowned(simdutfBase64UrlDecodeChecked(dq, kInvalidBase64Error));
    auto qiDecoded = toBignumUnowned(simdutfBase64UrlDecodeChecked(qi, kInvalidBase64Error));

    JSG_REQUIRE(RSA_set0_key(rsa.get(), nullptr, nullptr, dDecoded) == 1, Error,
        "Invalid RSA key in JSON Web Key; failed to set private exponent");
    JSG_REQUIRE(RSA_set0_factors(rsa.get(), pDecoded, qDecoded) == 1, Error,
        "Invalid RSA key in JSON Web Key; failed to set prime factors");
    JSG_REQUIRE(RSA_set0_crt_params(rsa.get(), dpDecoded, dqDecoded, qiDecoded) == 1, Error,
        "Invalid RSA key in JSON Web Key; failed to set CRT parameters");
  }

  auto evpPkey = OSSL_NEW(EVP_PKEY);
  KJ_ASSERT(EVP_PKEY_set1_RSA(evpPkey.get(), rsa.get()) == 1);

  auto usages = keyType == KeyType::PRIVATE ? CryptoKeyUsageSet::privateKeyMask()
                                            : CryptoKeyUsageSet::publicKeyMask();
  return AsymmetricKeyData{kj::mv(evpPkey), keyType, usages};
}

kj::String Rsa::toPem(
    KeyEncoding encoding, KeyType keyType, kj::Maybe<CipherOptions> options) const {
  ClearErrorOnReturn clearErrorOnReturn;
  auto bio = OSSL_BIO_MEM();
  switch (keyType) {
    case KeyType::PUBLIC: {
      switch (encoding) {
        case KeyEncoding::PKCS1: {
          JSG_REQUIRE(PEM_write_bio_RSAPublicKey(bio.get(), rsa) == 1, Error,
              "Failed to write RSA public key to PEM", tryDescribeOpensslErrors());
          break;
        }
        case workerd::api::KeyEncoding::SPKI: {
          JSG_REQUIRE(PEM_write_bio_RSA_PUBKEY(bio.get(), rsa) == 1, Error,
              "Failed to write RSA public key to PEM", tryDescribeOpensslErrors());
          break;
        }
        default: {
          JSG_FAIL_REQUIRE(Error, "Unsupported RSA public key encoding: ", encoding);
        }
      }
      break;
    }
    case KeyType::PRIVATE: {
      kj::byte* passphrase = nullptr;
      size_t passLen = 0;
      const EVP_CIPHER* cipher = nullptr;
      KJ_IF_SOME(opts, options) {
        passphrase = const_cast<kj::byte*>(opts.passphrase.begin());
        passLen = opts.passphrase.size();
        cipher = opts.cipher;
      }
      switch (encoding) {
        case KeyEncoding::PKCS1: {
          JSG_REQUIRE(PEM_write_bio_RSAPrivateKey(
                          bio.get(), rsa, cipher, passphrase, passLen, nullptr, nullptr) == 1,
              Error, "Failed to write RSA private key to PEM", tryDescribeOpensslErrors());
          break;
        }
        case KeyEncoding::PKCS8: {
          auto evpPkey = OSSL_NEW(EVP_PKEY);
          EVP_PKEY_set1_RSA(evpPkey.get(), rsa);
          JSG_REQUIRE(PEM_write_bio_PKCS8PrivateKey(bio.get(), evpPkey.get(), cipher,
                          reinterpret_cast<char*>(passphrase), passLen, nullptr, nullptr) == 1,
              Error, "Failed to write RSA private key to PKCS8 PEM", tryDescribeOpensslErrors());
          break;
        }
        default: {
          JSG_FAIL_REQUIRE(Error, "Unsupported RSA private key encoding: ", encoding);
        }
      }
      break;
    }
    default:
      KJ_UNREACHABLE;
  }
  return kj::String(bioToArray(bio.get()).releaseAsChars());
}

kj::Array<const kj::byte> Rsa::toDer(
    KeyEncoding encoding, KeyType keyType, kj::Maybe<CipherOptions> options) const {
  ClearErrorOnReturn clearErrorOnReturn;
  auto bio = OSSL_BIO_MEM();
  switch (keyType) {
    case KeyType::PUBLIC: {
      switch (encoding) {
        case KeyEncoding::PKCS1: {
          JSG_REQUIRE(i2d_RSAPublicKey_bio(bio.get(), rsa) == 1, Error,
              "Failed to write RSA public key to DER", tryDescribeOpensslErrors());
          break;
        }
        case workerd::api::KeyEncoding::SPKI: {
          auto evpPkey = OSSL_NEW(EVP_PKEY);
          EVP_PKEY_set1_RSA(evpPkey.get(), rsa);
          JSG_REQUIRE(i2d_PUBKEY_bio(bio.get(), evpPkey.get()) == 1, Error,
              "Failed to write RSA public key to SPKI", tryDescribeOpensslErrors());
          break;
        }
        default: {
          JSG_FAIL_REQUIRE(Error, "Unsupported RSA public key encoding: ", encoding);
        }
      }
      break;
    }
    case KeyType::PRIVATE: {
      kj::byte* passphrase = nullptr;
      size_t passLen = 0;
      const EVP_CIPHER* cipher = nullptr;
      KJ_IF_SOME(opts, options) {
        passphrase = const_cast<kj::byte*>(opts.passphrase.begin());
        passLen = opts.passphrase.size();
        cipher = opts.cipher;
      }
      switch (encoding) {
        case KeyEncoding::PKCS1: {
          // Does not permit encryption
          JSG_REQUIRE(i2d_RSAPrivateKey_bio(bio.get(), rsa), Error,
              "Failed to write RSA private key to PEM", tryDescribeOpensslErrors());
          break;
        }
        case KeyEncoding::PKCS8: {
          auto evpPkey = OSSL_NEW(EVP_PKEY);
          EVP_PKEY_set1_RSA(evpPkey.get(), rsa);
          JSG_REQUIRE(i2d_PKCS8PrivateKey_bio(bio.get(), evpPkey.get(), cipher,
                          reinterpret_cast<char*>(passphrase), passLen, nullptr, nullptr) == 1,
              Error, "Failed to write RSA private key to PKCS8 PEM", tryDescribeOpensslErrors());
          break;
        }
        default: {
          JSG_FAIL_REQUIRE(Error, "Unsupported RSA private key encoding: ", encoding);
        }
      }
      break;
    }
    default:
      KJ_UNREACHABLE;
  }
  return bioToArray(bio.get());
}

void Rsa::validateRsaParams(
    jsg::Lock& js, size_t modulusLength, kj::ArrayPtr<kj::byte> publicExponent, bool isImport) {
  KJ_ASSERT(modulusLength <= ~uint16_t(0));
  // Use Chromium's limits for RSA keygen to avoid infinite loops:
  // * Key sizes a multiple of 8 bits.
  // * Key sizes must be in [256, 16k] bits.
  auto strictCrypto = FeatureFlags::get(js).getStrictCrypto();
  JSG_REQUIRE(!(strictCrypto || !isImport) ||
          (modulusLength % 8 == 0 && modulusLength >= 256 && modulusLength <= 16384),
      DOMOperationError,
      "The modulus length must be a multiple of 8 and "
      "between 256 and 16k, but ",
      modulusLength, " was requested.");

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
    JSG_FAIL_REQUIRE(DOMOperationError,
        "The \"publicExponent\" must be either 3 or 65537, but "
        "got a number larger than 2^32.");
  }
}

bool Rsa::isRSAPrivateKey(kj::ArrayPtr<const kj::byte> keyData) {
  KJ_IF_SOME(rem, tryGetAsn1Sequence(keyData)) {
    return rem.size() >= 3 && rem[0] == 2 && rem[1] == 1 && !(rem[2] & 0xfe);
  }
  return false;
}

// ======================================================================================
// Web Crypto Impl: RSASSA-PKCS1-V1_5, RSA-PSS, RSA-OEAP, RSA-RAW

namespace {
class RsaBase: public AsymmetricKeyCryptoKeyImpl {
public:
  explicit RsaBase(
      AsymmetricKeyData keyData, CryptoKey::RsaKeyAlgorithm keyAlgorithm, bool extractable)
      : AsymmetricKeyCryptoKeyImpl(kj::mv(keyData), extractable),
        keyAlgorithm(kj::mv(keyAlgorithm)) {}

  kj::StringPtr jsgGetMemoryName() const override {
    return "AsymmetricKey";
  }
  size_t jsgGetMemorySelfSize() const override {
    return sizeof(AsymmetricKeyCryptoKeyImpl);
  }
  void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const override {
    AsymmetricKeyCryptoKeyImpl::jsgGetMemoryInfo(tracker);
    tracker.trackField("keyAlgorithm", keyAlgorithm);
  }

protected:
  CryptoKey::RsaKeyAlgorithm keyAlgorithm;

private:
  SubtleCrypto::JsonWebKey exportJwk() const override final {
    auto rsa = JSG_REQUIRE_NONNULL(Rsa::tryGetRsa(getEvpPkey()), DOMDataError,
        "No RSA data backing key", tryDescribeOpensslErrors());
    return rsa.toJwk(getTypeEnum(), jwkHashAlgorithmName());
  }

  kj::Array<kj::byte> exportRaw() const override final {
    JSG_FAIL_REQUIRE(
        DOMInvalidAccessError, "Cannot export \"", getAlgorithmName(), "\" in \"raw\" format.");
  }

  CryptoKey::AsymmetricKeyDetails getAsymmetricKeyDetail() const override {
    return KJ_ASSERT_NONNULL(Rsa::tryGetRsa(getEvpPkey())).getAsymmetricKeyDetail();
  }

  virtual kj::String jwkHashAlgorithmName() const = 0;
};

class RsassaPkcs1V15Key final: public RsaBase {
public:
  explicit RsassaPkcs1V15Key(
      AsymmetricKeyData keyData, CryptoKey::RsaKeyAlgorithm keyAlgorithm, bool extractable)
      : RsaBase(kj::mv(keyData), kj::mv(keyAlgorithm), extractable) {}

  CryptoKey::AlgorithmVariant getAlgorithm(jsg::Lock& js) const override {
    return keyAlgorithm.clone(js);
  }
  kj::StringPtr getAlgorithmName() const override {
    return "RSASSA-PKCS1-v1_5";
  }

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
  explicit RsaPssKey(
      AsymmetricKeyData keyData, CryptoKey::RsaKeyAlgorithm keyAlgorithm, bool extractable)
      : RsaBase(kj::mv(keyData), kj::mv(keyAlgorithm), extractable) {}

  CryptoKey::AlgorithmVariant getAlgorithm(jsg::Lock& js) const override {
    return keyAlgorithm.clone(js);
  }
  kj::StringPtr getAlgorithmName() const override {
    return keyAlgorithm.name;
  }

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

class RsaOaepKey final: public RsaBase {
  using InitFunction = decltype(EVP_PKEY_encrypt_init);
  using EncryptDecryptFunction = decltype(EVP_PKEY_encrypt);

public:
  explicit RsaOaepKey(
      AsymmetricKeyData keyData, CryptoKey::RsaKeyAlgorithm keyAlgorithm, bool extractable)
      : RsaBase(kj::mv(keyData), kj::mv(keyAlgorithm), extractable) {}

  CryptoKey::AlgorithmVariant getAlgorithm(jsg::Lock& js) const override {
    return keyAlgorithm.clone(js);
  }
  kj::StringPtr getAlgorithmName() const override {
    return keyAlgorithm.name;
  }

  kj::StringPtr chooseHash(
      const kj::Maybe<kj::OneOf<kj::String, SubtleCrypto::HashAlgorithm>>& callTimeHash)
      const override {
    // RSA-OAEP is for encryption/decryption, not signing, but this method is called by the
    // parent class when performing sign() or verify().
    JSG_FAIL_REQUIRE(DOMNotSupportedError,
        "The sign and verify operations are not implemented for \"", keyAlgorithm.name, "\".");
  }

  kj::Array<kj::byte> encrypt(SubtleCrypto::EncryptAlgorithm&& algorithm,
      kj::ArrayPtr<const kj::byte> plainText) const override {
    JSG_REQUIRE(getTypeEnum() == KeyType::PUBLIC, DOMInvalidAccessError,
        "Encryption/key wrapping only works with public keys, not \"", getType(), "\".");
    return commonEncryptDecrypt(
        kj::mv(algorithm), plainText, EVP_PKEY_encrypt_init, EVP_PKEY_encrypt);
  }

  kj::Array<kj::byte> decrypt(SubtleCrypto::EncryptAlgorithm&& algorithm,
      kj::ArrayPtr<const kj::byte> cipherText) const override {
    JSG_REQUIRE(getTypeEnum() == KeyType::PRIVATE, DOMInvalidAccessError,
        "Decryption/key unwrapping only works with private keys, not \"", getType(), "\".");
    return commonEncryptDecrypt(
        kj::mv(algorithm), cipherText, EVP_PKEY_decrypt_init, EVP_PKEY_decrypt);
  }

private:
  kj::Array<kj::byte> commonEncryptDecrypt(SubtleCrypto::EncryptAlgorithm&& algorithm,
      kj::ArrayPtr<const kj::byte> data,
      InitFunction init,
      EncryptDecryptFunction encryptDecrypt) const {
    auto pkey = getEvpPkey();
    auto digest = lookupDigestAlgorithm(KJ_REQUIRE_NONNULL(keyAlgorithm.hash).name).second;
    auto ctx = OSSL_NEW(EVP_PKEY_CTX, pkey, nullptr);
    JSG_REQUIRE(1 == init(ctx.get()), DOMOperationError, "RSA-OAEP failed to initialize",
        tryDescribeOpensslErrors());
    return KJ_ASSERT_NONNULL(Rsa::tryGetRsa(pkey))
        .cipher(ctx, kj::mv(algorithm), data, encryptDecrypt, digest);
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
  explicit RsaRawKey(
      AsymmetricKeyData keyData, CryptoKey::RsaKeyAlgorithm keyAlgorithm, bool extractable)
      : RsaBase(kj::mv(keyData), kj::mv(keyAlgorithm), extractable) {}

  kj::Array<kj::byte> sign(
      SubtleCrypto::SignAlgorithm&& algorithm, kj::ArrayPtr<const kj::byte> data) const override {
    auto rsa = JSG_REQUIRE_NONNULL(Rsa::tryGetRsa(getEvpPkey()), DOMDataError, "Missing RSA key");
    return rsa.sign(data);
  }

  bool verify(SubtleCrypto::SignAlgorithm&& algorithm,
      kj::ArrayPtr<const kj::byte> signature,
      kj::ArrayPtr<const kj::byte> data) const override {
    KJ_UNIMPLEMENTED("RawRsa Verification currently unsupported");
  }

  CryptoKey::AlgorithmVariant getAlgorithm(jsg::Lock& js) const override {
    return keyAlgorithm.clone(js);
  }

  kj::StringPtr getAlgorithmName() const override {
    return keyAlgorithm.name;
  }

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

CryptoKeyPair generateRsaPair(jsg::Lock& js,
    kj::StringPtr normalizedName,
    kj::Own<EVP_PKEY> privateEvpPKey,
    kj::Own<EVP_PKEY> publicEvpPKey,
    CryptoKey::RsaKeyAlgorithm&& keyAlgorithm,
    bool privateKeyExtractable,
    CryptoKeyUsageSet usages) {
  auto privateKeyAlgorithm = keyAlgorithm.clone(js);

  AsymmetricKeyData publicKeyData{
    .evpPkey = kj::mv(publicEvpPKey),
    .keyType = KeyType::PUBLIC,
    .usages = usages & CryptoKeyUsageSet::publicKeyMask(),
  };
  AsymmetricKeyData privateKeyData{
    .evpPkey = kj::mv(privateEvpPKey),
    .keyType = KeyType::PRIVATE,
    .usages = usages & CryptoKeyUsageSet::privateKeyMask(),
  };

  static constexpr auto createPair = [](kj::Own<CryptoKey::Impl> publicKey,
                                         kj::Own<CryptoKey::Impl> privateKey) {
    return CryptoKeyPair{.publicKey = jsg::alloc<CryptoKey>(kj::mv(publicKey)),
      .privateKey = jsg::alloc<CryptoKey>(kj::mv(privateKey))};
  };

  if (normalizedName == "RSASSA-PKCS1-v1_5") {
    return createPair(
        kj::heap<RsassaPkcs1V15Key>(kj::mv(publicKeyData), kj::mv(keyAlgorithm), true),
        kj::heap<RsassaPkcs1V15Key>(
            kj::mv(privateKeyData), kj::mv(privateKeyAlgorithm), privateKeyExtractable));
  } else if (normalizedName == "RSA-PSS") {
    return createPair(kj::heap<RsaPssKey>(kj::mv(publicKeyData), kj::mv(keyAlgorithm), true),
        kj::heap<RsaPssKey>(
            kj::mv(privateKeyData), kj::mv(privateKeyAlgorithm), privateKeyExtractable));
  } else if (normalizedName == "RSA-OAEP") {
    return createPair(kj::heap<RsaOaepKey>(kj::mv(publicKeyData), kj::mv(keyAlgorithm), true),
        kj::heap<RsaOaepKey>(
            kj::mv(privateKeyData), kj::mv(privateKeyAlgorithm), privateKeyExtractable));
  }
  JSG_FAIL_REQUIRE(DOMNotSupportedError, "Unimplemented RSA generation \"", normalizedName, "\".");
}

kj::Own<EVP_PKEY> rsaJwkReader(SubtleCrypto::JsonWebKey&& keyDataJwk) {
  auto rsaKey = OSSL_NEW(RSA);

  auto modulus = UNWRAP_JWK_BIGNUM(kj::mv(keyDataJwk.n), DOMDataError,
      "Invalid RSA key in JSON Web Key; missing or invalid Modulus "
      "parameter (\"n\").");
  auto publicExponent = UNWRAP_JWK_BIGNUM(kj::mv(keyDataJwk.e), DOMDataError,
      "Invalid RSA key in JSON Web Key; missing or invalid "
      "Exponent parameter (\"e\").");

  // RSA_set0_*() transfers BIGNUM ownership to the RSA key, so we don't need to worry about
  // calling BN_free().
  OSSLCALL(RSA_set0_key(
      rsaKey.get(), toBignumUnowned(modulus), toBignumUnowned(publicExponent), nullptr));

  if (keyDataJwk.d != kj::none) {
    // This is a private key.

    auto privateExponent = UNWRAP_JWK_BIGNUM(kj::mv(keyDataJwk.d), DOMDataError,
        "Invalid RSA key in JSON Web Key; missing or invalid "
        "Private Exponent parameter (\"d\").");

    OSSLCALL(RSA_set0_key(rsaKey.get(), nullptr, nullptr, toBignumUnowned(privateExponent)));

    auto presence = (keyDataJwk.p != kj::none) + (keyDataJwk.q != kj::none) +
        (keyDataJwk.dp != kj::none) + (keyDataJwk.dq != kj::none) + (keyDataJwk.qi != kj::none);

    if (presence == 5) {
      auto firstPrimeFactor = UNWRAP_JWK_BIGNUM(kj::mv(keyDataJwk.p), DOMDataError,
          "Invalid RSA key in JSON Web Key; invalid First Prime "
          "Factor parameter (\"p\").");
      auto secondPrimeFactor = UNWRAP_JWK_BIGNUM(kj::mv(keyDataJwk.q), DOMDataError,
          "Invalid RSA key in JSON Web Key; invalid Second Prime "
          "Factor parameter (\"q\").");
      auto firstFactorCrtExponent = UNWRAP_JWK_BIGNUM(kj::mv(keyDataJwk.dp), DOMDataError,
          "Invalid RSA key in JSON Web Key; invalid First Factor "
          "CRT Exponent parameter (\"dp\").");
      auto secondFactorCrtExponent = UNWRAP_JWK_BIGNUM(kj::mv(keyDataJwk.dq), DOMDataError,
          "Invalid RSA key in JSON Web Key; invalid Second Factor "
          "CRT Exponent parameter (\"dq\").");
      auto firstCrtCoefficient = UNWRAP_JWK_BIGNUM(kj::mv(keyDataJwk.qi), DOMDataError,
          "Invalid RSA key in JSON Web Key; invalid First CRT "
          "Coefficient parameter (\"qi\").");

      OSSLCALL(RSA_set0_factors(
          rsaKey.get(), toBignumUnowned(firstPrimeFactor), toBignumUnowned(secondPrimeFactor)));
      OSSLCALL(RSA_set0_crt_params(rsaKey.get(), toBignumUnowned(firstFactorCrtExponent),
          toBignumUnowned(secondFactorCrtExponent), toBignumUnowned(firstCrtCoefficient)));
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
}  // namespace

kj::OneOf<jsg::Ref<CryptoKey>, CryptoKeyPair> CryptoKey::Impl::generateRsa(jsg::Lock& js,
    kj::StringPtr normalizedName,
    SubtleCrypto::GenerateKeyAlgorithm&& algorithm,
    bool extractable,
    kj::ArrayPtr<const kj::String> keyUsages) {

  KJ_ASSERT(normalizedName == "RSASSA-PKCS1-v1_5" || normalizedName == "RSA-PSS" ||
          normalizedName == "RSA-OAEP",
      "generateRsa called on non-RSA cryptoKey", normalizedName);

  auto publicExponent = JSG_REQUIRE_NONNULL(kj::mv(algorithm.publicExponent), TypeError,
      "Missing field \"publicExponent\" in \"algorithm\".");
  kj::StringPtr hash = api::getAlgorithmName(
      JSG_REQUIRE_NONNULL(algorithm.hash, TypeError, "Missing field \"hash\" in \"algorithm\"."));
  int modulusLength = JSG_REQUIRE_NONNULL(
      algorithm.modulusLength, TypeError, "Missing field \"modulusLength\" in \"algorithm\".");
  JSG_REQUIRE(modulusLength > 0, DOMOperationError,
      "modulusLength must be greater than zero "
      "(requested ",
      modulusLength, ").");
  auto [normalizedHashName, hashEvpMd] = lookupDigestAlgorithm(hash);

  CryptoKeyUsageSet validUsages = (normalizedName == "RSA-OAEP")
      ? (CryptoKeyUsageSet::encrypt() | CryptoKeyUsageSet::decrypt() |
            CryptoKeyUsageSet::wrapKey() | CryptoKeyUsageSet::unwrapKey())
      : (CryptoKeyUsageSet::sign() | CryptoKeyUsageSet::verify());
  auto usages = CryptoKeyUsageSet::validate(
      normalizedName, CryptoKeyUsageSet::Context::generate, keyUsages, validUsages);

  Rsa::validateRsaParams(js, modulusLength, publicExponent.asPtr());
  // boringssl silently uses (modulusLength & ~127) for the key size, i.e. it rounds down to the
  // closest multiple of 128 bits. This can easily cause confusion when non-standard key sizes are
  // requested.
  // The `modulusLength` field of the resulting CryptoKey will be incorrect when the compat flag
  // is disabled and the key size is rounded down, but since it is not currently used this is
  // acceptable.
  JSG_REQUIRE(!(FeatureFlags::get(js).getStrictCrypto() && (modulusLength & 127)),
      DOMOperationError, "Can't generate key: RSA key size is required to be a multiple of 128");

  auto bnExponent = JSG_REQUIRE_NONNULL(
      toBignum(publicExponent), InternalDOMOperationError, "Error setting up RSA keygen.");

  auto rsaPrivateKey = OSSL_NEW(RSA);
  OSSLCALL(RSA_generate_key_ex(rsaPrivateKey, modulusLength, bnExponent.get(), 0));
  auto privateEvpPKey = OSSL_NEW(EVP_PKEY);
  OSSLCALL(EVP_PKEY_set1_RSA(privateEvpPKey.get(), rsaPrivateKey.get()));
  kj::Own<RSA> rsaPublicKey = OSSLCALL_OWN(RSA, RSAPublicKey_dup(rsaPrivateKey.get()),
      InternalDOMOperationError, "Error finalizing RSA keygen", internalDescribeOpensslErrors());
  auto publicEvpPKey = OSSL_NEW(EVP_PKEY);
  OSSLCALL(EVP_PKEY_set1_RSA(publicEvpPKey.get(), rsaPublicKey));

  auto keyAlgorithm = CryptoKey::RsaKeyAlgorithm{.name = normalizedName,
    .modulusLength = static_cast<uint16_t>(modulusLength),
    .publicExponent = kj::mv(publicExponent),
    .hash = KeyAlgorithm{normalizedHashName}};

  return generateRsaPair(js, normalizedName, kj::mv(privateEvpPKey), kj::mv(publicEvpPKey),
      kj::mv(keyAlgorithm), extractable, usages);
}

kj::Own<CryptoKey::Impl> CryptoKey::Impl::importRsa(jsg::Lock& js,
    kj::StringPtr normalizedName,
    kj::StringPtr format,
    SubtleCrypto::ImportKeyData keyData,
    SubtleCrypto::ImportKeyAlgorithm&& algorithm,
    bool extractable,
    kj::ArrayPtr<const kj::String> keyUsages) {
  kj::StringPtr hash = api::getAlgorithmName(
      JSG_REQUIRE_NONNULL(algorithm.hash, TypeError, "Missing field \"hash\" in \"algorithm\"."));

  CryptoKeyUsageSet allowedUsages = (normalizedName == "RSA-OAEP")
      ? (CryptoKeyUsageSet::encrypt() | CryptoKeyUsageSet::decrypt() |
            CryptoKeyUsageSet::wrapKey() | CryptoKeyUsageSet::unwrapKey())
      : (CryptoKeyUsageSet::sign() | CryptoKeyUsageSet::verify());

  auto [normalizedHashName, hashEvpMd] = lookupDigestAlgorithm(hash);

  auto importedKey = importAsymmetricForWebCrypto(js, kj::mv(format), kj::mv(keyData),
      normalizedName, extractable, keyUsages,
      // Verbose lambda capture needed because: https://bugs.llvm.org/show_bug.cgi?id=35984
      [hashEvpMd = hashEvpMd, &algorithm](
          SubtleCrypto::JsonWebKey keyDataJwk) -> kj::Own<EVP_PKEY> {
    JSG_REQUIRE(keyDataJwk.kty == "RSA", DOMDataError,
        "RSASSA-PKCS1-v1_5 \"jwk\" key import requires a JSON Web Key with Key Type parameter "
        "\"kty\" (\"",
        keyDataJwk.kty, "\") equal to \"RSA\".");

    KJ_IF_SOME(alg, keyDataJwk.alg) {
      // If this JWK specifies an algorithm, make sure it jives with the hash we were passed via
      // importKey().
      static const std::map<kj::StringPtr, const EVP_MD*> rsaShaAlgorithms{
        {"RS1", EVP_sha1()},
        {"RS256", EVP_sha256()},
        {"RS384", EVP_sha384()},
        {"RS512", EVP_sha512()},
      };
      static const std::map<kj::StringPtr, const EVP_MD*> rsaPssAlgorithms{
        {"PS1", EVP_sha1()},
        {"PS256", EVP_sha256()},
        {"PS384", EVP_sha384()},
        {"PS512", EVP_sha512()},
      };
      static const std::map<kj::StringPtr, const EVP_MD*> rsaOaepAlgorithms{
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
          JSG_FAIL_REQUIRE(
              DOMNotSupportedError, "Unrecognized RSA variant \"", algorithm.name, "\".");
        }
      }();
      auto jwkHash = validAlgorithms.find(alg);
      JSG_REQUIRE(jwkHash != rsaPssAlgorithms.end(), DOMNotSupportedError,
          "Unrecognized or unimplemented algorithm \"", alg,
          "\" listed in JSON Web Key Algorithm "
          "parameter.");

      JSG_REQUIRE(jwkHash->second == hashEvpMd, DOMDataError,
          "JSON Web Key Algorithm parameter \"alg\" (\"", alg,
          "\") does not match requested hash "
          "algorithm \"",
          jwkHash->first, "\".");
    }

    return rsaJwkReader(kj::mv(keyDataJwk));
  },
      allowedUsages);

  // get0 avoids adding a refcount...
  auto rsa = JSG_REQUIRE_NONNULL(Rsa::tryGetRsa(importedKey.evpPkey.get()), DOMDataError,
      "Input was not an RSA key", tryDescribeOpensslErrors());

  // TODO(conform): We're supposed to check if PKCS8/SPKI input specified a hash and, if so,
  //   compare it against the hash requested in `algorithm`. But, I can't find the OpenSSL
  //   interface to extract the hash from the ASN.1. Oh well...

  size_t modulusLength = rsa.getModulusBits();
  auto publicExponent = rsa.getPublicExponent();

  // Validate modulus and exponent, reject imported RSA keys that may be unsafe.
  Rsa::validateRsaParams(js, modulusLength, publicExponent, true);

  auto keyAlgorithm = CryptoKey::RsaKeyAlgorithm{.name = normalizedName,
    .modulusLength = static_cast<uint16_t>(modulusLength),
    .publicExponent = kj::mv(publicExponent),
    .hash = KeyAlgorithm{normalizedHashName}};
  if (normalizedName == "RSASSA-PKCS1-v1_5") {
    return kj::heap<RsassaPkcs1V15Key>(kj::mv(importedKey), kj::mv(keyAlgorithm), extractable);
  } else if (normalizedName == "RSA-PSS") {
    return kj::heap<RsaPssKey>(kj::mv(importedKey), kj::mv(keyAlgorithm), extractable);
  } else if (normalizedName == "RSA-OAEP") {
    return kj::heap<RsaOaepKey>(kj::mv(importedKey), kj::mv(keyAlgorithm), extractable);
  } else {
    JSG_FAIL_REQUIRE(DOMNotSupportedError, "Unrecognized RSA variant \"", normalizedName, "\".");
  }
}

kj::Own<CryptoKey::Impl> CryptoKey::Impl::importRsaRaw(jsg::Lock& js,
    kj::StringPtr normalizedName,
    kj::StringPtr format,
    SubtleCrypto::ImportKeyData keyData,
    SubtleCrypto::ImportKeyAlgorithm&& algorithm,
    bool extractable,
    kj::ArrayPtr<const kj::String> keyUsages) {
  // Note that in this context raw refers to the RSA-RAW algorithm, not to keys represented by raw
  // data. Importing raw keys is currently not supported for this algorithm.
  CryptoKeyUsageSet allowedUsages = CryptoKeyUsageSet::sign() | CryptoKeyUsageSet::verify();
  auto importedKey = importAsymmetricForWebCrypto(js, kj::mv(format), kj::mv(keyData),
      normalizedName, extractable, keyUsages,
      // Verbose lambda capture needed because: https://bugs.llvm.org/show_bug.cgi?id=35984
      [](SubtleCrypto::JsonWebKey keyDataJwk) -> kj::Own<EVP_PKEY> {
    JSG_REQUIRE(keyDataJwk.kty == "RSA", DOMDataError,
        "RSA-RAW \"jwk\" key import requires a JSON Web Key with Key Type parameter "
        "\"kty\" (\"",
        keyDataJwk.kty, "\") equal to \"RSA\".");

    KJ_IF_SOME(alg, keyDataJwk.alg) {
      // If this JWK specifies an algorithm, make sure it jives with the hash we were passed via
      // importKey().
      static const std::map<kj::StringPtr, const EVP_MD*> rsaAlgorithms{
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
      "RSA-RAW only supports private keys but requested \"", toStringPtr(importedKey.keyType),
      "\".");

  // get0 avoids adding a refcount...
  auto rsa = JSG_REQUIRE_NONNULL(Rsa::tryGetRsa(importedKey.evpPkey.get()), DOMDataError,
      "Input was not an RSA key", tryDescribeOpensslErrors());

  size_t modulusLength = rsa.getModulusBits();
  auto publicExponent = KJ_REQUIRE_NONNULL(bignumToArray(*rsa.getE()));

  // Validate modulus and exponent, reject imported RSA keys that may be unsafe.
  Rsa::validateRsaParams(js, modulusLength, publicExponent, true);

  auto keyAlgorithm = CryptoKey::RsaKeyAlgorithm{.name = "RSA-RAW"_kj,
    .modulusLength = static_cast<uint16_t>(modulusLength),
    .publicExponent = kj::mv(publicExponent)};

  return kj::heap<RsaRawKey>(kj::mv(importedKey), kj::mv(keyAlgorithm), extractable);
}

kj::Own<CryptoKey::Impl> fromRsaKey(kj::Own<EVP_PKEY> key) {
  return kj::heap<RsassaPkcs1V15Key>(AsymmetricKeyData{.evpPkey = kj::mv(key),
                                       .keyType = KeyType::PUBLIC,
                                       .usages = CryptoKeyUsageSet::decrypt() |
                                           CryptoKeyUsageSet::sign() | CryptoKeyUsageSet::verify()},
      CryptoKey::RsaKeyAlgorithm{.name = "RSA"_kj}, true);
}

}  // namespace workerd::api
