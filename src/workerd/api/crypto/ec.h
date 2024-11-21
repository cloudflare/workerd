#pragma once

#include "impl.h"
#include "keys.h"

#include <openssl/base.h>

#include <kj/common.h>

namespace workerd::api {

class Ec final {
 public:
  static kj::Maybe<Ec> tryGetEc(const EVP_PKEY* key);
  Ec(EC_KEY* key);

  inline const EC_KEY* getKey() {
    return key;
  }
  inline const EC_GROUP* getGroup() const {
    return group;
  }
  int getCurveName() const;

  const EC_POINT* getPublicKey() const;
  const BIGNUM* getPrivateKey() const;
  uint32_t getDegree() const;

  inline const BIGNUM& getX() const {
    return *x;
  }
  inline const BIGNUM& getY() const {
    return *y;
  }

  SubtleCrypto::JsonWebKey toJwk(
      KeyType keyType, kj::StringPtr curveName) const KJ_WARN_UNUSED_RESULT;

  jsg::BufferSource getRawPublicKey(jsg::Lock& js) const KJ_WARN_UNUSED_RESULT;

  CryptoKey::AsymmetricKeyDetails getAsymmetricKeyDetail() const;

 private:
  EC_KEY* key;
  const EC_GROUP* group = nullptr;
  kj::Own<BIGNUM> x;
  kj::Own<BIGNUM> y;
};

}  // namespace workerd::api
