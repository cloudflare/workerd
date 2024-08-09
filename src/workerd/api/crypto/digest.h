#pragma once

#include "impl.h"
#include <workerd/api/crypto/crypto.h>

KJ_DECLARE_NON_POLYMORPHIC(HMAC_CTX)

namespace workerd::api {
class HmacContext final {
public:
  using KeyData = kj::OneOf<kj::ArrayPtr<kj::byte>, CryptoKey::Impl*>;

  HmacContext(kj::StringPtr algorithm, KeyData key);
  HmacContext(HmacContext&&) = default;
  HmacContext& operator=(HmacContext&&) = default;
  KJ_DISALLOW_COPY(HmacContext);

  void update(kj::ArrayPtr<kj::byte> data);
  kj::ArrayPtr<kj::byte> digest();

  size_t size() const;

private:
  // Will be kj::Own<HMAC_CTX> while the hmac data is being updated,
  // and kj::Array<kj::byte> after the digest() has been called.
  kj::OneOf<kj::Own<HMAC_CTX>, kj::Array<kj::byte>> state;
};

class HashContext final {
public:
  HashContext(kj::StringPtr algorithm, kj::Maybe<uint32_t> maybeXof);
  HashContext(HashContext&&) = default;
  HashContext& operator=(HashContext&&) = default;
  KJ_DISALLOW_COPY(HashContext);

  void update(kj::ArrayPtr<kj::byte> data);
  kj::ArrayPtr<kj::byte> digest();
  HashContext clone(kj::Maybe<uint32_t> xofLen);

  size_t size() const;

private:
  HashContext(kj::OneOf<kj::Own<EVP_MD_CTX>, kj::Array<kj::byte>>, kj::Maybe<uint32_t> maybeXof);

  // Will be kj::Own<EVP_MD_CTX> while the hash data is being updated,
  // and kj::Array<kj::byte> after the digest() has been called.
  kj::OneOf<kj::Own<EVP_MD_CTX>, kj::Array<kj::byte>> state;
  kj::Maybe<uint32_t> maybeXof;
};
}  // namespace workerd::api
