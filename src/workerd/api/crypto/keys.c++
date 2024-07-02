#include "keys.h"
#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/pkcs12.h>

KJ_DECLARE_NON_POLYMORPHIC(BIO);
KJ_DECLARE_NON_POLYMORPHIC(PKCS8_PRIV_KEY_INFO)
KJ_DECLARE_NON_POLYMORPHIC(X509);

namespace workerd::api {

SecretKey::SecretKey(kj::Array<kj::byte> keyData)
    : Impl(true, CryptoKeyUsageSet::privateKeyMask() |
                  CryptoKeyUsageSet::publicKeyMask()),
      keyData(kj::mv(keyData)) {}

kj::StringPtr SecretKey::getAlgorithmName() const {
  return "secret"_kj;
}

CryptoKey::AlgorithmVariant SecretKey::getAlgorithm(jsg::Lock& js) const {
  return CryptoKey::KeyAlgorithm { .name = "secret"_kj };
}

bool SecretKey::equals(const CryptoKey::Impl& other) const {
  return this == &other || (other.getType() == "secret"_kj && other.equals(keyData));
}

bool SecretKey::equals(const kj::Array<kj::byte>& other) const {
  return keyData.size() == other.size() &&
          CRYPTO_memcmp(keyData.begin(), other.begin(), keyData.size()) == 0;
}

SubtleCrypto::ExportKeyData SecretKey::exportKey(kj::StringPtr format) const {
  JSG_REQUIRE(format == "raw" || format == "jwk", DOMNotSupportedError,
      getAlgorithmName(), " key only supports exporting \"raw\" & \"jwk\", not \"", format,
      "\".");

  if (format == "jwk") {
    SubtleCrypto::JsonWebKey jwk;
    jwk.kty = kj::str("oct");
    jwk.k = kj::encodeBase64Url(keyData);
    jwk.ext = true;
    return jwk;
  }

  return kj::heapArray(keyData.asPtr());
}

void SecretKey::jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const {
  tracker.trackFieldWithSize("keyData", keyData.size());
}

// ======================================================================================

namespace {
constexpr int32_t kMaxInt = kj::maxValue;
using ParseCallback = EVP_PKEY*(const kj::byte**, long);

int passwordCallback(char* buf, int size, int rwflag, void* u) {
  ParseKeyOptions& opts = *static_cast<ParseKeyOptions*>(u);
  KJ_IF_SOME(passphrase, opts.maybePassphrase) {
    size_t buflen = static_cast<size_t>(size);
    if (passphrase.size() == 0) return 0;
    if (buflen < passphrase.size()) return -1;
    auto dest = kj::arrayPtr(buf, passphrase.size());
    dest.copyFrom(passphrase.asChars());
    return static_cast<int>(passphrase.size());
  }
  return -1;
}

kj::Maybe<kj::Own<BIO>> tryBioWrap(kj::ArrayPtr<const kj::byte> buf) {
  BIO* bio = BIO_new_mem_buf(buf.begin(), buf.size());
  if (bio == nullptr) return kj::none;
  return kj::disposeWith<BIO_free_all>(bio);
}

bool isASN1Sequence(kj::ArrayPtr<const kj::byte> seq,
                    size_t* data_offset, size_t* data_size) {
  if (seq.size() < 2 || seq[0] != 0x30)
    return false;

  if (seq[1] & 0x80) {
    // Long form.
    size_t n_bytes = seq[1] & ~0x80;
    if (n_bytes + 2 > seq.size() || n_bytes > sizeof(size_t))
      return false;
    size_t length = 0;
    for (size_t i = 0; i < n_bytes; i++)
      length = (length << 8) | seq[i + 2];
    *data_offset = 2 + n_bytes;
    *data_size = std::min(seq.size() - 2 - n_bytes, length);
  } else {
    // Short form.
    *data_offset = 2;
    *data_size = std::min<size_t>(seq.size() - 2, seq[1]);
  }

  return true;
}

bool isEncryptedPrivateKeyInfo(kj::ArrayPtr<const kj::byte> key) {
  // Both PrivateKeyInfo and EncryptedPrivateKeyInfo start with a SEQUENCE.
  size_t offset, len;
  if (!isASN1Sequence(key, &offset, &len))
    return false;

  // A PrivateKeyInfo sequence always starts with an integer whereas an
  // EncryptedPrivateKeyInfo starts with an AlgorithmIdentifier.
  return len >= 1 && key[offset] != 2;
}

kj::Maybe<ParsedKey> tryParsePublicKey(BIO* bio, const char* name,
                                               kj::Function<ParseCallback> parse) {
  unsigned char* data;
  long len;

  // This skips surrounding data and decodes PEM to DER.
  {
    MarkPopErrorOnReturn mark_pop_error_on_return;
    if (PEM_bytes_read_bio(&data, &len, nullptr, name, bio, nullptr, nullptr) != 1) {
      return kj::none;
    }
  }

  KJ_ASSERT(data != nullptr);
  KJ_ASSERT(len >= 0);

  KJ_DEFER(OPENSSL_clear_free(data, len));

  // OpenSSL might modify the pointer, so we need to make a copy before parsing.
  const unsigned char* p = data;
  auto pkey = parse(&p, len);
  if (pkey == nullptr) return kj::none;
  return ParsedKey {
    .type = KeyType::Public,
    .key = kj::disposeWith<EVP_PKEY_free>(pkey),
  };
}

kj::Maybe<ParsedKey> tryParsePublicKeyPEM(kj::ArrayPtr<const kj::byte> key) {
  KJ_IF_SOME(bp, tryBioWrap(key)) {
    // Try parsing as a SubjectPublicKeyInfo first.
    KJ_IF_SOME(key, tryParsePublicKey(bp.get(), "PUBLIC KEY",
        [](const kj::byte** p, long l) { return d2i_PUBKEY(nullptr, p, l); })) {
      return kj::mv(key);
    }

    // Maybe it is PKCS#1.
    KJ_ASSERT(BIO_reset(bp.get()));
    KJ_IF_SOME(key, tryParsePublicKey(bp.get(), "RSA PUBLIC KEY",
        [](const kj::byte** p, long l) { return d2i_PublicKey(EVP_PKEY_RSA, nullptr, p, l); })) {
      return kj::mv(key);
    }

    // X.509 fallback.
    KJ_ASSERT(BIO_reset(bp.get()));
    return tryParsePublicKey(bp.get(), "CERTIFICATE",
        [](const kj::byte** p, long l) {  // NOLINT(runtime/int)
          X509* x509 = d2i_X509(nullptr, p, l);
          if (x509 == nullptr) return (EVP_PKEY*)nullptr;
          auto ptr = kj::disposeWith<X509_free>(x509);
          return X509_get_pubkey(ptr.get());
        });
  }
  return kj::none;
}

kj::Maybe<ParsedKey> tryParsePublicKey(kj::ArrayPtr<const kj::byte> key,
                                               const ParseKeyOptions& opts) {
  if (opts.format == PkFormat::PEM) {
    return tryParsePublicKeyPEM(key);
  } else {
    KJ_ASSERT(opts.format == PkFormat::DER);
    const kj::byte* p = key.begin();
    if (opts.encoding == PkEncoding::PKCS1) {
      auto pkey = d2i_PublicKey(EVP_PKEY_RSA, nullptr, &p, key.size());
      if (pkey != nullptr) {
        return ParsedKey {
          .type = KeyType::Public,
          .key = kj::disposeWith<EVP_PKEY_free>(pkey)
        };
      }
    } else {
      KJ_ASSERT(opts.encoding == PkEncoding::SPKI);
      auto pkey = d2i_PUBKEY(nullptr, &p, key.size());
      if (pkey != nullptr) {
        return ParsedKey {
          .type = KeyType::Public,
          .key = kj::disposeWith<EVP_PKEY_free>(pkey),
        };
      }
    }
  }
  return kj::none;
}

kj::Maybe<ParsedKey> tryParsePrivateKey(kj::ArrayPtr<const kj::byte> key,
                                                const ParseKeyOptions& opts) {
  KJ_IF_SOME(passphrase, opts.maybePassphrase) {
    JSG_REQUIRE(passphrase.size() <= kMaxInt, RangeError, "Passphrase too large.");
  }

  KJ_IF_SOME(bp, tryBioWrap(key)) {
    ClearErrorOnReturn clearErrorOnReturn;
    EVP_PKEY* pkey = nullptr;

    switch (opts.format) {
      case PkFormat::PEM: {
        pkey = PEM_read_bio_PrivateKey(bp.get(), nullptr, passwordCallback, (void*)&opts);
        break;
      }
      case PkFormat::DER: {
        if (opts.encoding == PkEncoding::PKCS1) {
          const kj::byte* p = key.begin();
          pkey = d2i_PrivateKey(EVP_PKEY_RSA, nullptr, &p, key.size());
        } else if (opts.encoding == PkEncoding::PKCS8) {
          if (isEncryptedPrivateKeyInfo(key)) {
            pkey = d2i_PKCS8PrivateKey_bio(bp.get(), nullptr, passwordCallback, (void*)&opts);
          } else {
            auto pkcs8 = d2i_PKCS8_PRIV_KEY_INFO_bio(bp.get(), nullptr);
            if (pkcs8 == nullptr) return kj::none;
            auto ptr = kj::disposeWith<PKCS8_PRIV_KEY_INFO_free>(pkcs8);
            pkey = EVP_PKCS82PKEY(pkcs8);
          }
        } else {
          KJ_ASSERT(opts.encoding == PkEncoding::SEC1);
          const kj::byte* p = key.begin();
          pkey = d2i_PrivateKey(EVP_PKEY_EC, nullptr, &p, key.size());
        }
        break;
      }
      case PkFormat::JWK: {
        KJ_UNREACHABLE;
      }
    }

    if (pkey == nullptr) return kj::none;
    auto ret = kj::disposeWith<EVP_PKEY_free>(pkey);

    // OpenSSL can fail to parse the key but still return a non-null pointer.
    // Not sure if BoringSSL does the same. Let's be careful and see.
    int err = clearErrorOnReturn.peekError();
    if (err == 0) {
      return ParsedKey {
        .type = KeyType::Private,
        .key = kj::mv(ret),
      };
    }

    if (ERR_GET_LIB(err) == ERR_LIB_PEM && ERR_GET_REASON(err) == PEM_R_BAD_PASSWORD_READ) {
      JSG_FAIL_REQUIRE(Error, "Passphrase needed to parse private key.");
    }
  }
  return kj::none;
}

bool isRSAPrivateKey(kj::ArrayPtr<const kj::byte> buf) {
  // Both RSAPrivateKey and RSAPublicKey structures start with a SEQUENCE.
  size_t offset, len;
  if (!isASN1Sequence(buf, &offset, &len))
    return false;

  // An RSAPrivateKey sequence always starts with a single-byte integer whose
  // value is either 0 or 1, whereas an RSAPublicKey starts with the modulus
  // (which is the product of two primes and therefore at least 4), so we can
  // decide the type of the structure based on the first three bytes of the
  // sequence.
  return len >= 3 &&
         buf[offset] == 2 &&
         buf[offset + 1] == 1 &&
         !(buf[offset + 2] & 0xfe);
}
}  // namespace

kj::Maybe<ParsedKey> tryParseKey(kj::ArrayPtr<const kj::byte> keyData,
                                 kj::Maybe<ParseKeyOptions> options) {
  JSG_REQUIRE(keyData.size() <= kMaxInt, RangeError, "Key data too large.");

  ParseKeyOptions opts = kj::mv(options).orDefault({});

  const EVP_CIPHER* cipher = nullptr;
  KJ_IF_SOME(cipherName, opts.maybeCipherName) {
    cipher = EVP_get_cipherbyname(cipherName.begin());
    JSG_REQUIRE(cipher != nullptr, Error, kj::str("Unknown cipher: ", cipherName));
  }

  switch (opts.format) {
    case PkFormat::PEM: {
      // First try to parse as a public key.
      KJ_IF_SOME(pkey, tryParsePublicKeyPEM(keyData)) {
        return kj::mv(pkey);
      }
      return tryParsePrivateKey(keyData, opts);
    }
    case PkFormat::DER: {
      bool isPublic = false;
      switch (opts.encoding) {
        case PkEncoding::PKCS1: {
          isPublic = !isRSAPrivateKey(keyData);
          break;
        }
        case PkEncoding::PKCS8: {
          // isPublic = false
          break;
        }
        case PkEncoding::SEC1: {
          // isPublic = false
          break;
        }
        case PkEncoding::SPKI: {
          isPublic = true;
          break;
        }
        default: {
          KJ_UNREACHABLE;
        }
      }
      if (isPublic) {
        return tryParsePublicKey(keyData, opts);
      } else {
        return tryParsePrivateKey(keyData, opts);
      }
    }
    case PkFormat::JWK: {}
  }

  KJ_UNREACHABLE;
}

kj::Maybe<ParsedKey> tryParseKeyPrivate(kj::ArrayPtr<const kj::byte> keyData,
                                        kj::Maybe<ParseKeyOptions> options) {
  ParseKeyOptions opts = kj::mv(options).orDefault({});
  return tryParsePrivateKey(keyData, opts);
}

kj::Maybe<jsg::Ref<CryptoKey>> newCryptoKeyImpl(ParsedKey&& parsedKey) {
  auto impl = ([&]() mutable -> kj::Maybe<kj::Own<CryptoKey::Impl>> {
    switch (EVP_PKEY_id(parsedKey.key.get())) {
      case EVP_PKEY_RSA: return newRsaCryptoKeyImpl(parsedKey.type, kj::mv(parsedKey.key));
      case EVP_PKEY_RSA_PSS: return newRsaPssCryptoKeyImpl(parsedKey.type, kj::mv(parsedKey.key));
      case EVP_PKEY_EC: return newEcCryptoKeyImpl(parsedKey.type, kj::mv(parsedKey.key));
      case EVP_PKEY_ED25519: return newEd25519CryptoKeyImpl(parsedKey.type, kj::mv(parsedKey.key));
      case EVP_PKEY_DSA: return newDsaCryptoKeyImpl(parsedKey.type, kj::mv(parsedKey.key));
      default: return kj::none;
    }
    KJ_UNREACHABLE;
  })();

  KJ_IF_SOME(i, impl) {
    return jsg::alloc<CryptoKey>(kj::mv(i));
  }
  return kj::none;
}

}  // namespace workerd::api
