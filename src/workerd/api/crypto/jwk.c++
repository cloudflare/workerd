#include "jwk.h"

#include <ncrypto.h>
#include <openssl/curve25519.h>

namespace workerd::api {

using JsonWebKey = SubtleCrypto::JsonWebKey;
using ncrypto::BignumPointer;
using ncrypto::ECKeyPointer;
using ncrypto::EVPKeyPointer;
using ncrypto::RSAPointer;

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
}

int getCurveFromName(kj::StringPtr name) {
  int nid = EC_curve_nist2nid(name.begin());
  if (nid == NID_undef) nid = OBJ_sn2nid(name.begin());
  return nid;
}

int getOKPCurveFromName(kj::StringPtr name) {
  int nid;
  if (name == "Ed25519") {
    nid = EVP_PKEY_ED25519;
  } else if (name == "X25519") {
    nid = EVP_PKEY_X25519;
  } else {
    // 448 keys are not supported by boringssl
    nid = NID_undef;
  }
  return nid;
}

JsonWebKey jwkFromEdKey(const EVPKeyPointer& key, KeyType keyType) {
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

JsonWebKey jwkFromEcKey(const EVPKeyPointer& key, KeyType keyType) {
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

JsonWebKey jwkFromRsaKey(const EVPKeyPointer& key, KeyType keyType) {
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

EVPKeyPointer rsaKeyFromJwk(const JsonWebKey& jwk, KeyType keyType) {
  ncrypto::ClearErrorOnReturn clearErrorOnReturn;

  RSAPointer rsa(RSA_new());
  if (!rsa) return {};
  ncrypto::Rsa rsa_view(rsa.get());

  auto& n = JSG_REQUIRE_NONNULL(jwk.n, Error, "RSA JWK missing n parameter");
  auto& e = JSG_REQUIRE_NONNULL(jwk.e, Error, "RSA JWK missing e parameter");
  auto N = JSG_REQUIRE_NONNULL(simdutfBase64UrlDecode(n), Error, "RSA JWK invalid n parameter");
  auto E = JSG_REQUIRE_NONNULL(simdutfBase64UrlDecode(e), Error, "RSA JWK invalid e parameter");

  BignumPointer n_bn(N.begin(), N.size());
  BignumPointer e_bn(E.begin(), E.size());

  JSG_REQUIRE(
      rsa_view.setPublicKey(kj::mv(n_bn), kj::mv(e_bn)), Error, "RSA JWK invalid public key");

  if (keyType == KeyType::PRIVATE) {
    auto& d = JSG_REQUIRE_NONNULL(jwk.d, Error, "RSA JWK missing d parameter");
    auto& p = JSG_REQUIRE_NONNULL(jwk.p, Error, "RSA JWK missing p parameter");
    auto& q = JSG_REQUIRE_NONNULL(jwk.q, Error, "RSA JWK missing q parameter");
    auto& dp = JSG_REQUIRE_NONNULL(jwk.dp, Error, "RSA JWK missing dp parameter");
    auto& dq = JSG_REQUIRE_NONNULL(jwk.dq, Error, "RSA JWK missing dq parameter");
    auto& qi = JSG_REQUIRE_NONNULL(jwk.qi, Error, "RSA JWK missing qi parameter");

    auto D = JSG_REQUIRE_NONNULL(simdutfBase64UrlDecode(d), Error, "RSA JWK invalid d parameter");
    auto P = JSG_REQUIRE_NONNULL(simdutfBase64UrlDecode(p), Error, "RSA JWK invalid p parameter");
    auto Q = JSG_REQUIRE_NONNULL(simdutfBase64UrlDecode(q), Error, "RSA JWK invalid q parameter");
    auto DP =
        JSG_REQUIRE_NONNULL(simdutfBase64UrlDecode(dp), Error, "RSA JWK invalid dp parameter");
    auto DQ =
        JSG_REQUIRE_NONNULL(simdutfBase64UrlDecode(dq), Error, "RSA JWK invalid dq parameter");
    auto QI =
        JSG_REQUIRE_NONNULL(simdutfBase64UrlDecode(qi), Error, "RSA JWK invalid qi parameter");

    BignumPointer d_bn(D.begin(), D.size());
    BignumPointer p_bn(P.begin(), P.size());
    BignumPointer q_bn(Q.begin(), Q.size());
    BignumPointer dp_bn(DP.begin(), DP.size());
    BignumPointer dq_bn(DQ.begin(), DQ.size());
    BignumPointer qi_bn(QI.begin(), QI.size());

    JSG_REQUIRE(rsa_view.setPrivateKey(kj::mv(d_bn), kj::mv(q_bn), kj::mv(p_bn), kj::mv(dp_bn),
                    kj::mv(dq_bn), kj::mv(qi_bn)),
        Error, "RSA JWK invalid private key");
  }

  return EVPKeyPointer::NewRSA(std::move(rsa));
}

EVPKeyPointer ecKeyFromJwk(const JsonWebKey& jwk, KeyType keyType) {
  int nid = getCurveFromName(JSG_REQUIRE_NONNULL(jwk.crv, Error, "EC JWK missing crv parameter"));
  JSG_REQUIRE(nid != NID_undef, Error, "EC JWK unsupported crv parameter");

  auto ec = ECKeyPointer::NewByCurveName(nid);
  JSG_REQUIRE(ec, Error, "EC JWK unsupported curve");

  auto& x = JSG_REQUIRE_NONNULL(jwk.x, Error, "EC JWK missing x parameter");
  auto& y = JSG_REQUIRE_NONNULL(jwk.y, Error, "EC JWK missing y parameter");

  auto X = JSG_REQUIRE_NONNULL(simdutfBase64UrlDecode(x), Error, "EC JWK invalid x parameter");
  auto Y = JSG_REQUIRE_NONNULL(simdutfBase64UrlDecode(y), Error, "EC JWK invalid y parameter");

  BignumPointer x_bn(X.begin(), X.size());
  BignumPointer y_bn(Y.begin(), Y.size());

  JSG_REQUIRE(ec.setPublicKeyRaw(kj::mv(x_bn), kj::mv(y_bn)), Error, "EC JWK invalid public key");

  if (keyType == KeyType::PRIVATE) {
    auto& d = JSG_REQUIRE_NONNULL(jwk.d, Error, "EC JWK missing d parameter");
    auto D = JSG_REQUIRE_NONNULL(simdutfBase64UrlDecode(d), Error, "EC JWK invalid d parameter");

    BignumPointer d_bn(D.begin(), D.size());
    JSG_REQUIRE(ec.setPrivateKey(kj::mv(d_bn)), Error, "EW JWK invalid private key");
  }

  auto pkey = EVPKeyPointer::New();
  if (!pkey || !pkey.set(ec)) return {};
  return pkey;
}

EVPKeyPointer edKeyFromJwk(const JsonWebKey& jwk, KeyType keyType) {
  int nid =
      getOKPCurveFromName(JSG_REQUIRE_NONNULL(jwk.crv, Error, "OKP JWK missing crv parameter"));
  JSG_REQUIRE(nid != NID_undef, Error, "OKP JWK unsupported crv parameter");

  if (keyType == KeyType::PRIVATE) {
    auto& d = JSG_REQUIRE_NONNULL(jwk.d, Error, "OKP JWK missing d parameter");
    auto D = JSG_REQUIRE_NONNULL(simdutfBase64UrlDecode(d), Error, "OKP JWK invalid d parameter");
    return EVPKeyPointer::NewRawPrivate(nid, ToNcryptoBuffer(D.asPtr().asConst()));
  }

  auto& x = JSG_REQUIRE_NONNULL(jwk.x, Error, "OKP JWK missing x parameter");
  auto X = JSG_REQUIRE_NONNULL(simdutfBase64UrlDecode(x), Error, "OKP JWK invalid x parameter");
  return EVPKeyPointer::NewRawPrivate(nid, ToNcryptoBuffer(X.asPtr().asConst()));
}
}  // namespace

JsonWebKey toJwk(const EVPKeyPointer& key, KeyType keyType) {
  if (key) {
    switch (key.id()) {
      case EVP_PKEY_ED25519:
        return jwkFromEdKey(key, keyType);
      case EVP_PKEY_X25519:
        return jwkFromEdKey(key, keyType);
      case EVP_PKEY_EC:
        return jwkFromEcKey(key, keyType);
      case EVP_PKEY_RSA:
        return jwkFromRsaKey(key, keyType);
      case EVP_PKEY_RSA2:
        return jwkFromRsaKey(key, keyType);
      case EVP_PKEY_RSA_PSS:
        return jwkFromRsaKey(key, keyType);
      case EVP_PKEY_DSA: {
        // DSA keys are not supported for JWK export.
        break;
      }
    }
  }

  return JsonWebKey{
    .kty = kj::str("INVALID"),
  };
}

EVPKeyPointer fromJwk(const JsonWebKey& jwk, KeyType keyType) {
  if (jwk.kty == "OKP") return edKeyFromJwk(jwk, keyType);
  if (jwk.kty == "EC") return ecKeyFromJwk(jwk, keyType);
  if (jwk.kty == "RSA") return rsaKeyFromJwk(jwk, keyType);
  return {};
}

}  // namespace workerd::api
