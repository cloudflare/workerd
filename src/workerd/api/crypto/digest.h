#pragma once

#include "impl.h"

#include <workerd/api/crypto/crypto.h>
#include <workerd/rust/api/lib.rs.h>

KJ_DECLARE_NON_POLYMORPHIC(HMAC_CTX)

namespace workerd::api {
class HmacContext final {
 public:
  using KeyData = kj::OneOf<kj::ArrayPtr<kj::byte>, CryptoKey::Impl*>;
  using State = kj::OneOf<kj::Own<HMAC_CTX>,
      ::rust::Box<workerd::rust::api::Sha3Hmac>,
      jsg::JsRef<jsg::JsUint8Array>>;

  HmacContext(jsg::Lock& js, kj::StringPtr algorithm, KeyData key);
  HmacContext(HmacContext&&) = default;
  HmacContext& operator=(HmacContext&&) = default;
  KJ_DISALLOW_COPY(HmacContext);

  void update(kj::ArrayPtr<kj::byte> data);
  jsg::JsUint8Array digest(jsg::Lock& js);

  size_t size() const;

 private:
  // Will be kj::Own<HMAC_CTX> or Rust SHA-3 HMAC state while the HMAC data is being updated,
  // and jsg::JsRef<jsg::JsUint8Array> after the digest() has been called.
  State state;
};

class HashContext final {
 public:
  using State = kj::OneOf<kj::Own<EVP_MD_CTX>,
      ::rust::Box<workerd::rust::api::Sha3Hash>,
      jsg::JsRef<jsg::JsUint8Array>>;

  HashContext(kj::StringPtr algorithm, kj::Maybe<uint32_t> maybeXof);
  HashContext(HashContext&&) = default;
  HashContext& operator=(HashContext&&) = default;
  KJ_DISALLOW_COPY(HashContext);

  void update(kj::ArrayPtr<kj::byte> data);
  jsg::JsUint8Array digest(jsg::Lock& js);
  HashContext clone(jsg::Lock& js, kj::Maybe<uint32_t> xofLen);

  size_t size() const;

 private:
  HashContext(State state, kj::Maybe<uint32_t> maybeXof);

  // Will be kj::Own<EVP_MD_CTX> or Rust SHA-3 state while the hash data is being updated,
  // and jsg::JsRef<jsg::JsUint8Array> after the digest() has been called.
  State state;
  kj::Maybe<uint32_t> maybeXof;
};
}  // namespace workerd::api
