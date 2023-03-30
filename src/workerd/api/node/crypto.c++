#include "crypto.h"
#include "buffer.h"
#include <workerd/api/crypto.h>
#include <workerd/api/crypto-impl.h>
#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <map>

namespace workerd::api::node {

namespace {
class SecretKeyImpl final: public CryptoKey::Impl {
  // This implementation of CryptoKey::Impl is used only to support
  // the Node.js createSecretKey API. When a Node.js secret key is
  // created, the API requires specifying between aes or hmac, but
  // once the key is created it can be used to either purpose. So
  // this implementation will not use either the hmac or aes-specific
  // CryptoKey::Impl's. Instead, it just holds the bytes and we will
  // specialize when the key is actually used in a crypto operation.
public:
  static constexpr auto NAME = "secret"_kj;

  static jsg::Ref<CryptoKey> create(
      jsg::Lock& js,
      kj::OneOf<v8::Local<v8::String>, kj::Array<kj::byte>> keyData,
      jsg::Optional<kj::String> encoding) {
    KJ_SWITCH_ONEOF(keyData) {
      KJ_CASE_ONEOF(data, v8::Local<v8::String>) {
        auto enc = kj::mv(encoding).orDefault(kj::str("utf8"));
        return jsg::alloc<CryptoKey>(kj::heap<SecretKeyImpl>(
            decodeStringImpl(js, data, getEncoding(enc), false)));
      }
      KJ_CASE_ONEOF(data, kj::Array<kj::byte>) {
        return jsg::alloc<CryptoKey>(kj::heap<SecretKeyImpl>(kj::mv(data)));
      }
    }
    KJ_UNREACHABLE;
  }

  SecretKeyImpl(kj::Array<kj::byte> keyData) :
      CryptoKey::Impl(true, CryptoKeyUsageSet::privateKeyMask()),
      keyData(kj::mv(keyData)),
      keyAlgorithm(CryptoKey::ArbitraryKeyAlgorithm {
        .name = NAME,
        .length = this->keyData.size(),
      }) {}

  kj::StringPtr getAlgorithmName() const override {
    return NAME;
  }

  kj::StringPtr getType() const override {
    return "secret"_kj;
  }

  CryptoKey::AlgorithmVariant getAlgorithm() const override {
    return keyAlgorithm;
  }

  bool operator==(const Impl& other) const override {
    if (this == &other) return true;
    if (getType() != other.getType()) return false;
    auto exported = other.exportKey("raw"_kj).get<kj::Array<kj::byte>>();
    return exported == keyData;
  }

private:
  kj::Array<kj::byte> keyData;
  CryptoKey::ArbitraryKeyAlgorithm keyAlgorithm;
};

class AsymmetricKeyImpl final: public CryptoKey::Impl {
  // Unlike the WebCrypto importKey implementation, where the type of key
  // being imported is passed in explicitly as an argument, in this impl
  // we have to determine the algorithm type after the import.
public:
  enum class Type {
    PUBLIC,
    PRIVATE,
  };

  static jsg::Ref<CryptoKey> createPublic(
      jsg::Lock& js,
      CryptoImpl::AsymmetricKeyData key) {
    return create(js, Type::PUBLIC, kj::mv(key));
  }

  static jsg::Ref<CryptoKey> createPrivate(
      jsg::Lock& js,
      CryptoImpl::AsymmetricKeyData key) {
    return create(js, Type::PRIVATE, kj::mv(key));
  }

  AsymmetricKeyImpl(Type type,
                    kj::ArrayPtr<kj::byte> keyData,
                    jsg::Optional<kj::OneOf<kj::String, kj::Array<kj::byte>>> passphrase)
      : CryptoKey::Impl(true, CryptoKeyUsageSet::privateKeyMask()),
        type(type),
        passphrase(kj::mv(passphrase)),
        key(decodeKeyFromPem(keyData)) {}
  // Import using PEM format

  AsymmetricKeyImpl(Type type,
                    kj::ArrayPtr<kj::byte> keyData,
                    kj::StringPtr formatType,
                    jsg::Optional<kj::OneOf<kj::String, kj::Array<kj::byte>>> passphrase)
      : CryptoKey::Impl(true, CryptoKeyUsageSet::privateKeyMask()),
        type(type),
        passphrase(kj::mv(passphrase)),
        key(decodeKeyFromDer(keyData, formatType)) {}
  // Import using DER format

  AsymmetricKeyImpl(Type type, jsg::Lock& js, SubtleCrypto::JsonWebKey jwk)
      : CryptoKey::Impl(true, CryptoKeyUsageSet::privateKeyMask()),
        type(type),
        key(decodeKeyFromJwk(js, kj::mv(jwk))) {}

  kj::StringPtr getType() const override {
    return type == Type::PUBLIC ? "public"_kj : "private"_kj;
  }

  kj::StringPtr getAlgorithmName() const override {
    switch (key->type) {
      case EVP_PKEY_RSA: return "RSASSA-PKCS1-v1_5"_kj;
      case EVP_PKEY_RSA_PSS: return "RSS-PSS"_kj;
      case EVP_PKEY_EC: return "ECDSA"_kj;
      case EVP_PKEY_X25519: return "X25519"_kj;
      case EVP_PKEY_ED25519: return "ED25519"_kj;
      case EVP_PKEY_DH: return "DH"_kj;
      case EVP_PKEY_DSA: return "DSA"_kj;
    }
    // We don't actually need to provide anything here. These are only used for
    // Node.js KeyObject, which does not expose this information.
    return ""_kj;
  }

  CryptoKey::AlgorithmVariant getAlgorithm() const override {
    // We don't actually need to provide anything here. These are only used for
    // Node.js KeyObject, which does not expose this information.
    return CryptoKey::ArbitraryKeyAlgorithm {};
  }

  inline operator EVP_PKEY*() { return key.get(); }

  bool operator==(const Impl& other) const override {
    if (this == &other) return true;
    if (getType() != other.getType()) return false;
    // TODO(now): Implement
    KJ_UNIMPLEMENTED("not yet implemented");
  }

private:
  Type type;
  jsg::Optional<kj::OneOf<kj::String, kj::Array<kj::byte>>> passphrase;
  kj::Own<EVP_PKEY> key;

  kj::Maybe<kj::Own<EVP_PKEY>> tryParsePublicKey(
      BIO* bio,
      const char* name,
      auto parse) {
    unsigned char* der_data;
    long der_len;  // NOLINT(runtime/int)

    // This skips surrounding data and decodes PEM to DER.
    {
      MarkPopErrorOnReturn mark_pop_error_on_return;
      if (PEM_bytes_read_bio(&der_data, &der_len, nullptr, name, bio, nullptr, nullptr) != 1)
        return nullptr;
    }

    // OpenSSL might modify the pointer, so we need to make a copy before parsing.
    const unsigned char* p = der_data;
    return parse(&p, der_len);
  }

  kj::Own<EVP_PKEY> decodeKeyFromPem(kj::ArrayPtr<kj::byte> keyData) {
    auto bio = OSSL_BIO(keyData);
    switch (type) {
      case Type::PRIVATE: {
        return OSSLCALL_OWN(EVP_PKEY,
            PEM_read_bio_PrivateKey(bio.get(), nullptr, PasswordCallback, this),
            TypeError,
            "Failed to construct private key",
            internalDescribeOpensslErrors());
      }
      case Type::PUBLIC: {
        // We accept multiple things here, and will try one after the other.
        // First, try parsing as a SubjectPublicKeyInfo
        // Then, as PKCS#1
        // Then, as X.509
        // Failing that, we'll try parsing as a private key to derive a public key from.

        KJ_IF_MAYBE(key, tryParsePublicKey(bio.get(), "PUBLIC KEY",
            [](const unsigned char** p, long l) -> kj::Maybe<kj::Own<EVP_PKEY>> {
              auto ptr = d2i_PUBKEY(nullptr, p, l);
              if (!ptr) return nullptr;
              return OSSL_WRAP(EVP_PKEY, ptr);
            })) {
          return kj::mv(*key);
        }

        KJ_IF_MAYBE(key, tryParsePublicKey(bio.get(), "RSA PUBLIC KEY",
            [](const unsigned char** p, long l) -> kj::Maybe<kj::Own<EVP_PKEY>> {
              auto ptr = d2i_PublicKey(EVP_PKEY_RSA, nullptr, p, l);
              if (!ptr) return nullptr;
              return OSSL_WRAP(EVP_PKEY, ptr);
            })) {
          return kj::mv(*key);
        }

        KJ_IF_MAYBE(key, tryParsePublicKey(bio.get(), "CERTIFICATE",
            [](const unsigned char** p, long l) -> kj::Maybe<kj::Own<EVP_PKEY>> {
              auto x509 = d2i_X509(nullptr, p, l);
              if (!x509) return nullptr;
              auto wrapped = OSSL_WRAP(X509, x509);
              return OSSL_WRAP(EVP_PKEY, X509_get_pubkey(x509));
            })) {
          return kj::mv(*key);
        }

        return OSSLCALL_OWN(EVP_PKEY,
            PEM_read_bio_PrivateKey(bio.get(), nullptr, PasswordCallback, this),
            TypeError,
            "Failed to construct public key",
            internalDescribeOpensslErrors());
      }
    }
    KJ_UNREACHABLE;
  }

  kj::Own<EVP_PKEY> decodeKeyFromDer(kj::ArrayPtr<kj::byte> keyData,
                                     kj::StringPtr formatType) {
    if (formatType == "pkcs1"_kj) {
      // May be a public or private key.
      auto data = reinterpret_cast<const unsigned char*>(keyData.begin());
      if (!isRSAPrivateKey(data, keyData.size())) {
        return OSSLCALL_OWN(EVP_PKEY,
            d2i_PublicKey(EVP_PKEY_RSA, nullptr, &data, keyData.size()),
            TypeError,
            "Failed to construct public key", internalDescribeOpensslErrors());
      } else {
        return OSSLCALL_OWN(EVP_PKEY,
            d2i_PrivateKey(EVP_PKEY_RSA, nullptr, &data, keyData.size()),
            TypeError,
            "Failed to construct private key", internalDescribeOpensslErrors());
      }
    } else if (formatType == "pkcs8"_kj) {
      JSG_REQUIRE(type == Type::PRIVATE, TypeError, "pkcs8 is an invalid type for public keys");
      auto bio = OSSL_BIO(keyData);
      if (isEncrypted(keyData)) {
        return OSSLCALL_OWN(EVP_PKEY,
            d2i_PKCS8PrivateKey_bio(bio.get(), nullptr, PasswordCallback, this),
            TypeError,
            "Failed to construct private key", internalDescribeOpensslErrors());
      } else {
        auto info = OSSLCALL_OWN(PKCS8_PRIV_KEY_INFO,
            d2i_PKCS8_PRIV_KEY_INFO_bio(bio.get(), nullptr),
            TypeError,
            "Failed to construct private key", internalDescribeOpensslErrors());
        return OSSLCALL_OWN(EVP_PKEY, EVP_PKCS82PKEY(info.get()), Error,
            "Failed to construct private key");
      }
    } else if (formatType == "sec1"_kj) {
      JSG_REQUIRE(type == Type::PRIVATE, TypeError, "sec1 is an invalid type for public keys");
      auto data = reinterpret_cast<const unsigned char*>(keyData.begin());
      return OSSLCALL_OWN(EVP_PKEY,
          d2i_PrivateKey(EVP_PKEY_EC, nullptr, &data, keyData.size()),
          TypeError,
          "Failed to construct private key", internalDescribeOpensslErrors());
    } else if (formatType == "spki"_kj) {
      JSG_REQUIRE(type == Type::PUBLIC, TypeError, "spki is an invalid type for private keys");
      // Always a public key.
      auto data = reinterpret_cast<const unsigned char*>(keyData.begin());
      return OSSLCALL_OWN(EVP_PKEY,
          d2i_PUBKEY(nullptr, &data, keyData.size()),
          TypeError,
          "Failed to construct public key", internalDescribeOpensslErrors());
    }
    JSG_FAIL_REQUIRE(TypeError, "Invalid DER-encoded key type: ", formatType);
  }

  kj::Own<EVP_PKEY> decodeKeyFromJwk(jsg::Lock& js, SubtleCrypto::JsonWebKey key) {
    if (key.kty == "OKP"_kj) {
      KJ_IF_MAYBE(crv, key.crv) {
        // BoringSSL doesn't support ED448/X448.
        if (*crv == "Ed25519"_kj) {
          return ellipticJwkReader(NID_ED25519, kj::mv(key));
        } else if (*crv == "X25519"_kj) {
          return ellipticJwkReader(NID_X25519, kj::mv(key));
        }
      }
      JSG_FAIL_REQUIRE(TypeError, "key.crv must be one of either Ed25519 or X25519");
    } else if (key.kty == "EC"_kj) {
      KJ_IF_MAYBE(crv, key.crv) {
        if (*crv == "P-256"_kj) {
          return ellipticJwkReader(NID_X9_62_prime256v1, kj::mv(key));
        } else if (*crv == "secp256k1") {
          return ellipticJwkReader(NID_secp256k1, kj::mv(key));
        } else if (*crv == "P-384") {
          return ellipticJwkReader(NID_secp384r1, kj::mv(key));
        } else if (*crv == "P-521") {
          return ellipticJwkReader(NID_secp521r1, kj::mv(key));
        }
      }
      JSG_FAIL_REQUIRE(TypeError,
          "key.crv must be one of either P-256, secp256k1, P-384, or P-521");
    } else if (key.kty == "RSA"_kj) {
      return importRsaFromJwk(kj::mv(key));
    }

    JSG_FAIL_REQUIRE(TypeError, "key.kty must be one of either RSA, EC, or OKP");
  }

  static int PasswordCallback(char* buf, int size, int rwflag, void* u) {
    auto self = static_cast<AsymmetricKeyImpl*>(u);
    KJ_IF_MAYBE(pass, self->passphrase) {
      size_t buflen = static_cast<size_t>(size);
      KJ_SWITCH_ONEOF(*pass) {
        KJ_CASE_ONEOF(str, kj::String) {
          size_t len = str.size();
          if (buflen < len) return -1;
          memcpy(buf, str.begin(), len);
          return len;
        }
        KJ_CASE_ONEOF(data, kj::Array<kj::byte>) {
          size_t len = data.size();
          if (buflen < len) return -1;
          memcpy(buf, data.begin(), len);
          return len;
        }
      }
    }
    return -1;
  }

  static bool isASN1Sequence(const unsigned char* data, size_t size,
                      size_t* data_offset, size_t* data_size) {
    if (size < 2 || data[0] != 0x30)
      return false;

    if (data[1] & 0x80) {
      // Long form.
      size_t n_bytes = data[1] & ~0x80;
      if (n_bytes + 2 > size || n_bytes > sizeof(size_t))
        return false;
      size_t length = 0;
      for (size_t i = 0; i < n_bytes; i++)
        length = (length << 8) | data[i + 2];
      *data_offset = 2 + n_bytes;
      *data_size = std::min(size - 2 - n_bytes, length);
    } else {
      // Short form.
      *data_offset = 2;
      *data_size = std::min<size_t>(size - 2, data[1]);
    }

    return true;
  }

  static bool isRSAPrivateKey(const unsigned char* data, size_t size) {
    // Both RSAPrivateKey and RSAPublicKey structures start with a SEQUENCE.
    size_t offset, len;
    if (isASN1Sequence(data, size, &offset, &len))
      return false;

    // An RSAPrivateKey sequence always starts with a single-byte integer whose
    // value is either 0 or 1, whereas an RSAPublicKey starts with the modulus
    // (which is the product of two primes and therefore at least 4), so we can
    // decide the type of the structure based on the first three bytes of the
    // sequence.
    return len >= 3 &&
          data[offset] == 2 &&
          data[offset + 1] == 1 &&
          !(data[offset + 2] & 0xfe);
  }

  static bool isEncrypted(kj::ArrayPtr<kj::byte> data) {
    auto d = reinterpret_cast<const unsigned char*>(data.begin());
    // Both PrivateKeyInfo and EncryptedPrivateKeyInfo start with a SEQUENCE.
    size_t offset, len;
    if (!isASN1Sequence(d, data.size(), &offset, &len))
      return false;

    // A PrivateKeyInfo sequence always starts with an integer whereas an
    // EncryptedPrivateKeyInfo starts with an AlgorithmIdentifier.
    return len >= 1 && data[offset] != 2;
  }

  static jsg::Ref<CryptoKey> create(
      jsg::Lock& js,
      AsymmetricKeyImpl::Type type,
      CryptoImpl::AsymmetricKeyData key) {
    MarkPopErrorOnReturn mark_pop_error_on_return;
    KJ_SWITCH_ONEOF(key) {
      KJ_CASE_ONEOF(options, CryptoImpl::CreateAsymmetricKeyOptions) {
        auto format = kj::mv(options.format).orDefault([] { return kj::str("pem"); });
        if (format == "pem"_kj) {
          KJ_SWITCH_ONEOF(options.key) {
            KJ_CASE_ONEOF(data, jsg::V8Ref<v8::String>) {
              auto enc = kj::mv(options.encoding).orDefault(kj::str("utf8"));
              auto keyData = decodeStringImpl(js, data.getHandle(js), getEncoding(enc), false);
              return jsg::alloc<CryptoKey>(
                  kj::heap<AsymmetricKeyImpl>(type,
                                              kj::mv(keyData),
                                              kj::mv(options.passphrase)));
            }
            KJ_CASE_ONEOF(keyData, kj::Array<kj::byte>) {
              return jsg::alloc<CryptoKey>(
                  kj::heap<AsymmetricKeyImpl>(type,
                                              kj::mv(keyData),
                                              kj::mv(options.passphrase)));
            }
            KJ_CASE_ONEOF(obj, SubtleCrypto::JsonWebKey) {
              JSG_FAIL_REQUIRE(TypeError, "When format is 'pem', the key cannot be an object");
            }
          }
          KJ_UNREACHABLE;
        } else if (format == "der"_kj) {
          auto formatType = JSG_REQUIRE_NONNULL(kj::mv(options.type), TypeError,
            "When format is 'der', the type is required.");
          KJ_SWITCH_ONEOF(options.key) {
            KJ_CASE_ONEOF(data, jsg::V8Ref<v8::String>) {
              auto enc = kj::mv(options.encoding).orDefault(kj::str("utf8"));
              auto keyData = decodeStringImpl(js, data.getHandle(js), getEncoding(enc), false);
              return jsg::alloc<CryptoKey>(
                  kj::heap<AsymmetricKeyImpl>(type,
                                              kj::mv(keyData),
                                              kj::mv(formatType),
                                              kj::mv(options.passphrase)));
            }
            KJ_CASE_ONEOF(keyData, kj::Array<kj::byte>) {
              return jsg::alloc<CryptoKey>(
                  kj::heap<AsymmetricKeyImpl>(type,
                                              kj::mv(keyData),
                                              kj::mv(formatType),
                                              kj::mv(options.passphrase)));
            }
            KJ_CASE_ONEOF(obj, SubtleCrypto::JsonWebKey) {
              JSG_FAIL_REQUIRE(TypeError, "When format is 'der', the key cannot be an object");
            }
          }
          KJ_UNREACHABLE;
        } else if (format == "jwk"_kj) {
          KJ_SWITCH_ONEOF(options.key) {
            KJ_CASE_ONEOF(data, jsg::V8Ref<v8::String>) {
              JSG_FAIL_REQUIRE(TypeError, "When format is 'jwk', the key must be an object.");
            }
            KJ_CASE_ONEOF(data, kj::Array<kj::byte>) {
              JSG_FAIL_REQUIRE(TypeError, "When format is 'jwk', the key must be an object.");
            }
            KJ_CASE_ONEOF(obj, SubtleCrypto::JsonWebKey) {
              return jsg::alloc<CryptoKey>(kj::heap<AsymmetricKeyImpl>(type, js, kj::mv(obj)));
            }
          }
        }
        JSG_FAIL_REQUIRE(TypeError, "Invalid key format");
      }
      KJ_CASE_ONEOF(data, jsg::V8Ref<v8::String>) {
        // The data is expected to be PEM format...here we are just going to defer to the
        // above implementation...
        return create(js, type, CryptoImpl::CreateAsymmetricKeyOptions {
          .key = kj::mv(data),
        });
      }
      KJ_CASE_ONEOF(data, kj::Array<kj::byte>) {
        // The data is expected to be PEM format...here we are just going to defer to the
        // above implementation...
        return create(js, type, CryptoImpl::CreateAsymmetricKeyOptions {
          .key = kj::mv(data)
        });
      }
    }

    KJ_UNREACHABLE;
  }
};

}  // namespace

v8::Local<v8::Value> CryptoImpl::exportKey(
    jsg::Lock& js,
    jsg::Ref<CryptoKey> key,
    jsg::Optional<CryptoImpl::ExportOptions> options) {
  KJ_UNIMPLEMENTED("not implemented yet");
}

bool CryptoImpl::equals(
    jsg::Lock& js,
    jsg::Ref<CryptoKey> key,
    jsg::Ref<CryptoKey> otherKey) {
  return key.get() == otherKey.get() || *key.get() == *otherKey.get();
}

CryptoImpl::AsymmetricKeyDetails CryptoImpl::getAsymmetricKeyDetail(
    jsg::Lock& js,
    jsg::Ref<CryptoKey> key) {
  auto type = key->getType();
  JSG_REQUIRE(type == "public"_kj || type == "private"_kj,
      TypeError, "Invalid asymmetric key type");
  // TODO(now): Implement
  return {};
}

kj::StringPtr CryptoImpl::getAsymmetricKeyType(
    jsg::Lock& js,
    jsg::Ref<CryptoKey> key) {
  static const std::map<kj::StringPtr, kj::StringPtr> names {
    {"X25519", "x25519"},
    {"ED25519", "ed25519"},
    {"RSASSA-PKCS1-v1_5", "rsa"},
    {"RSA-PSS", "rsa-pss"},
    {"ECDSA", "ec"},
    {"DSA", "dsa"},
    {"DH", "dh"}
  };
  auto type = key->getType();
  JSG_REQUIRE(type == "public"_kj || type == "private"_kj,
      TypeError, "Invalid asymmetric key type");
  auto iter = names.find(key->getAlgorithmName());
  JSG_REQUIRE(iter != names.end(), Error, "Unsupported or unimplemented asymmetric key type");
  return iter->second;
}

CryptoKeyPair CryptoImpl::generateKeyPair(
    jsg::Lock& js,
    kj::String type,
    GenerateKeyPairOptions options) {

  const auto generateRsaPair = [&](kj::StringPtr name) {
    SubtleCrypto::GenerateKeyAlgorithm algorithm;
    algorithm.name = kj::str(name);
    // TODO(now): Convert the given options.hash to normalized form.
    algorithm.hash = kj::str("SHA-256");
    algorithm.modulusLength = options.modulusLength.orDefault(1024);
    // TODO(now): Convert public exponent from uint64 in options
    algorithm.publicExponent = kj::arrOf<kj::byte>(3);
    return CryptoKey::Impl::generateRsa(
        name, kj::mv(algorithm), true, kj::arr(kj::str("sign"), kj::str("verify")));
  };

  const auto requireKeyPair = [](auto res) {
    auto& pair = KJ_REQUIRE_NONNULL(kj::mv(res).template tryGet<CryptoKeyPair>());
    return kj::mv(pair);
  };

  if (type == "rsa"_kj) {
    return requireKeyPair(generateRsaPair("RSASSA-PKCS1-v1_5"_kj));
  } else if (type == "rsa-pss"_kj) {
    return requireKeyPair(generateRsaPair("RSA-PSS"_kj));
  } else if (type == "dsa"_kj) {
    KJ_UNIMPLEMENTED("dsa keygen not yet implemented");
  } else if (type == "ec"_kj) {
    SubtleCrypto::GenerateKeyAlgorithm algorithm;
    algorithm.name = kj::str("ECDSA");
    algorithm.namedCurve = JSG_REQUIRE_NONNULL(kj::mv(options.namedCurve), TypeError,
        "The options.namedCurve is missing");
    // TODO(now): Map the additional options
    return requireKeyPair(CryptoKey::Impl::generateEcdsa("ECDSA"_kj,
        kj::mv(algorithm), true, kj::arr(kj::str("sign"), kj::str("verify"))));
  } else if (type == "x25519"_kj) {
    SubtleCrypto::GenerateKeyAlgorithm algorithm;
    algorithm.name = kj::str("X255519");
    return requireKeyPair(CryptoKey::Impl::generateEddsa(
        "X25519"_kj, kj::mv(algorithm), true, kj::arr(kj::str("deriveKey"))));
  } else if (type == "ed25519"_kj) {
    SubtleCrypto::GenerateKeyAlgorithm algorithm;
    algorithm.name = kj::str("ED255519");
    return requireKeyPair(CryptoKey::Impl::generateEddsa(
        "ED25519"_kj, kj::mv(algorithm), true, kj::arr(kj::str("sign"), kj::str("verify"))));
  } else if (type == "dh"_kj) {
    KJ_UNIMPLEMENTED("dh key gen not yet implemented");
  }
  JSG_FAIL_REQUIRE(TypeError, "Invalid key pair type: ", type);
}

jsg::Ref<CryptoKey> CryptoImpl::createSecretKey(
    jsg::Lock& js,
    kj::OneOf<v8::Local<v8::String>, kj::Array<kj::byte>> keyData,
    jsg::Optional<kj::String> encoding) {
  return SecretKeyImpl::create(js, kj::mv(keyData), kj::mv(encoding));
}

jsg::Ref<CryptoKey> CryptoImpl::createPrivateKey(jsg::Lock& js, AsymmetricKeyData key) {
  return AsymmetricKeyImpl::createPrivate(js, kj::mv(key));
}

jsg::Ref<CryptoKey> CryptoImpl::createPublicKey(jsg::Lock& js, AsymmetricKeyData key) {
  return AsymmetricKeyImpl::createPublic(js, kj::mv(key));
}

}  // namespace workerd::api::node
