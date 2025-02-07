#include "jwk.h">

#include <ncrypto.h>
#include <openssl/curve25519.h>

namespace workerd::api {

using JsonWebKey = SubtleCrypto::JsonWebKey;
using ncrypto::EVPKeyPointer;

namespace {

kj::String getCurveName(int nid) {
  switch (nid) {
    case NID_X9_62_prime256v1:
      return kj::str("P-256");
    case NID_secp256k1:
      return kj::str("secp256k1");
    case NID_secp384r1:
      return kj::str("P-384");
    case NID_secp521r1:
      return kj::str("P-521");
    default:
      return kj::String();
  }
  KJ_UNREACHABLE;
}

JsonWebKey fromEdKey(const EVPKeyPointer& key, KeyType keyType) {
  KJ_REQUIRE(key, "Key must not be null");
  KJ_ASSERT(key.id() == EVP_PKEY_ED25519 || key.id() == EVP_PKEY_X25519,
      "Key must be an Ed25519 or X25519 key");

  auto pkey = key.rawPublicKey();
  JSG_REQUIRE(pkey, InternalDOMOperationError, "Failed to retrieve public key",
      internalDescribeOpensslErrors());
  KJ_ASSERT(pkey.size() == 32);
  kj::ArrayPtr<const kj::byte> rawPublicKey(static_cast<const kj::byte*>(pkey.get()), pkey.size());

  JsonWebKey jwk;
  jwk.kty = kj::str("OKP");
  jwk.crv = key.id() == EVP_PKEY_X25519 ? kj::str("X25519") : kj::str("Ed25519");
  jwk.x = fastEncodeBase64Url(rawPublicKey);
  if (key.id() == EVP_PKEY_ED25519) {
    jwk.alg = kj::str("EdDSA");
  }

  if (keyType == KeyType::PRIVATE) {
    // Deliberately use ED25519_PUBLIC_KEY_LEN here.
    // BoringSSL defines ED25519_PRIVATE_KEY_LEN as 64B since it stores the private key together
    // with public key data in some functions, but in the EVP interface only the 32B private key
    // itself is returned.
    uint8_t rawPrivateKey[ED25519_PUBLIC_KEY_LEN]{};
    size_t privateKeyLen = ED25519_PUBLIC_KEY_LEN;
    JSG_REQUIRE(1 == EVP_PKEY_get_raw_private_key(key.get(), rawPrivateKey, &privateKeyLen),
        InternalDOMOperationError, "Failed to retrieve private key",
        internalDescribeOpensslErrors());
    KJ_ASSERT(privateKeyLen == 32, privateKeyLen);
    jwk.d = fastEncodeBase64Url(kj::arrayPtr(rawPrivateKey, privateKeyLen));
  }

  return jwk;
}

JsonWebKey fromEcKey(const EVPKeyPointer& key, KeyType keyType) {
  KJ_REQUIRE(key, "Key must not be null");
  KJ_ASSERT(key.id() == EVP_PKEY_EC, "Key must be an EC key");

  ncrypto::Ec ec = key;

  JSG_REQUIRE(ec.getX() && ec.getY(), InternalDOMOperationError,
      "Error getting affine coordinates for export", internalDescribeOpensslErrors());

  JSG_REQUIRE(ec.getGroup() != nullptr, DOMOperationError, "No elliptic curve group in this key",
      tryDescribeOpensslErrors());
  JSG_REQUIRE(ec.getPublicKey(), DOMOperationError, "No public elliptic curve key data in this key",
      tryDescribeOpensslErrors());

  auto groupDegreeInBytes = integerCeilDivision(ec.getDegree(), 8U);
  // getDegree() returns number of bits. We need this because x, y, & d need
  // to match the group degree according to JWK.

  SubtleCrypto::JsonWebKey jwk;
  jwk.kty = kj::str("EC");
  jwk.crv = getCurveName(ec.getCurve());

  static constexpr auto handleBn = [](const BIGNUM& bn, size_t size) {
    return JSG_REQUIRE_NONNULL(bignumToArrayPadded(bn, size), InternalDOMOperationError,
        "Error converting EC affine co-ordinates to padded array", internalDescribeOpensslErrors());
  };

  // We check that getX and getY return good values above.
  auto xa = handleBn(*ec.getX().get(), groupDegreeInBytes);
  jwk.x = fastEncodeBase64Url(xa);

  auto ya = handleBn(*ec.getY().get(), groupDegreeInBytes);
  jwk.y = fastEncodeBase64Url(ya);

  if (keyType == KeyType::PRIVATE) {
    auto privateKey = ec.getPrivateKey();
    JSG_REQUIRE(privateKey, InternalDOMOperationError,
        "Error getting private key material for JSON Web Key export",
        internalDescribeOpensslErrors());
    auto pk = handleBn(*privateKey, groupDegreeInBytes);
    jwk.d = fastEncodeBase64Url(pk);
  }
  return jwk;
}

JsonWebKey fromRsaKey(const EVPKeyPointer& key, KeyType keyType) {
  SubtleCrypto::JsonWebKey jwk;
  jwk.kty = kj::str("RSA");

  ncrypto::Rsa rsa = key;
  auto publicKey = rsa.getPublicKey();

  if (publicKey.n != nullptr) {
    jwk.n = fastEncodeBase64Url(KJ_REQUIRE_NONNULL(bignumToArray(*publicKey.n)));
  }
  if (publicKey.e != nullptr) {
    jwk.e = fastEncodeBase64Url(KJ_REQUIRE_NONNULL(bignumToArray(*publicKey.e)));
  }

  if (keyType == KeyType::PRIVATE) {
    auto privateKey = rsa.getPrivateKey();
    if (publicKey.d != nullptr) {
      jwk.d = fastEncodeBase64Url(KJ_REQUIRE_NONNULL(bignumToArray(*publicKey.d)));
    }
    if (privateKey.p != nullptr) {
      jwk.p = fastEncodeBase64Url(KJ_REQUIRE_NONNULL(bignumToArray(*privateKey.p)));
    }
    if (privateKey.q != nullptr) {
      jwk.q = fastEncodeBase64Url(KJ_REQUIRE_NONNULL(bignumToArray(*privateKey.q)));
    }
    if (privateKey.dp != nullptr) {
      jwk.dp = fastEncodeBase64Url(KJ_REQUIRE_NONNULL(bignumToArray(*privateKey.dp)));
    }
    if (privateKey.dq != nullptr) {
      jwk.dq = fastEncodeBase64Url(KJ_REQUIRE_NONNULL(bignumToArray(*privateKey.dq)));
    }
    if (privateKey.qi != nullptr) {
      jwk.qi = fastEncodeBase64Url(KJ_REQUIRE_NONNULL(bignumToArray(*privateKey.qi)));
    }
  }

  return jwk;
}
}  // namespace

JsonWebKey toJwk(const EVPKeyPointer& key, KeyType keyType) {
  if (key) {
    switch (key.id()) {
      case EVP_PKEY_ED25519:
        return fromEdKey(key, keyType);
      case EVP_PKEY_X25519:
        return fromEdKey(key, keyType);
      case EVP_PKEY_EC:
        return fromEcKey(key, keyType);
      case EVP_PKEY_RSA:
        return fromRsaKey(key, keyType);
      case EVP_PKEY_RSA2:
        return fromRsaKey(key, keyType);
      case EVP_PKEY_RSA_PSS:
        return fromRsaKey(key, keyType);
      case EVP_PKEY_DSA: {
        // DSA keys are not supported for JWK export.
      }
    }
  }

  return JsonWebKey{
    .kty = kj::str("INVALID"),
  };
}

EVPKeyPointer fromJwk(const JsonWebKey& jwk) {
  return {};
}

}  // namespace workerd::api
