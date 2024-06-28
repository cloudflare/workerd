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
}  // namespace workerd::api

