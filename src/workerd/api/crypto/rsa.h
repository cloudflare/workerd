#pragma once

#include "crypto.h"
#include "keys.h"
#include <openssl/rsa.h>
#include <openssl/evp.h>
#include <kj/common.h>

namespace workerd::api {

class Rsa final {
public:
  static kj::Maybe<Rsa> tryGetRsa(const EVP_PKEY* key);
  Rsa(RSA* rsa);

  size_t getModulusBits() const;
  size_t getModulusSize() const;

  inline const BIGNUM* getN() const { return n; }
  inline const BIGNUM* getE() const { return e; }
  inline const BIGNUM* getD() const { return d; }

  kj::Array<kj::byte> getPublicExponent() KJ_WARN_UNUSED_RESULT;

  CryptoKey::AsymmetricKeyDetails getAsymmetricKeyDetail() const;

  kj::Array<kj::byte> sign(const kj::ArrayPtr<const kj::byte> data) const;

  SubtleCrypto::JsonWebKey toJwk(KeyType keytype,
                                 kj::Maybe<kj::String> maybeHashAlgorithm) const;

  using EncryptDecryptFunction = decltype(EVP_PKEY_encrypt);
  kj::Array<kj::byte> cipher(EVP_PKEY_CTX* ctx,
                             SubtleCrypto::EncryptAlgorithm&& algorithm,
                             kj::ArrayPtr<const kj::byte> data,
                             EncryptDecryptFunction encryptDecrypt,
                             const EVP_MD* cipher) const;

  // The W3C standard itself doesn't describe any parameter validation but the conformance tests
  // do test "bad" exponents, likely because everyone uses OpenSSL that suffers from poor behavior
  // with these bad exponents (e.g. if an exponent < 3 or 65535 generates an infinite loop, a
  // library might be expected to handle such cases on its own, no?).
  static void validateRsaParams(jsg::Lock& js,
                                size_t modulusLength,
                                kj::ArrayPtr<kj::byte> publicExponent,
                                bool isImport = false);

private:
  RSA* rsa;
  const BIGNUM* n = nullptr;
  const BIGNUM* e = nullptr;
  const BIGNUM* d = nullptr;
};

}  // namespace workerd::api
