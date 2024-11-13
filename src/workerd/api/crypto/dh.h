#pragma once

#include "impl.h"

#include <workerd/jsg/jsg.h>

#include <openssl/dh.h>

#include <kj/common.h>

namespace workerd::api {

class DiffieHellman final {
public:
  DiffieHellman(kj::StringPtr group);
  DiffieHellman(kj::OneOf<kj::Array<kj::byte>, int>& sizeOrKey,
      kj::OneOf<kj::Array<kj::byte>, int>& generator);
  DiffieHellman(DiffieHellman&&) = default;
  DiffieHellman& operator=(DiffieHellman&&) = default;
  KJ_DISALLOW_COPY(DiffieHellman);

  void setPrivateKey(kj::ArrayPtr<kj::byte> key);
  void setPublicKey(kj::ArrayPtr<kj::byte> key);

  jsg::BufferSource getPublicKey(jsg::Lock& js) KJ_WARN_UNUSED_RESULT;
  jsg::BufferSource getPrivateKey(jsg::Lock& js) KJ_WARN_UNUSED_RESULT;
  jsg::BufferSource getGenerator(jsg::Lock& js) KJ_WARN_UNUSED_RESULT;
  jsg::BufferSource getPrime(jsg::Lock& js) KJ_WARN_UNUSED_RESULT;
  jsg::BufferSource computeSecret(jsg::Lock& js, kj::ArrayPtr<kj::byte> key) KJ_WARN_UNUSED_RESULT;
  jsg::BufferSource generateKeys(jsg::Lock& js) KJ_WARN_UNUSED_RESULT;

  kj::Maybe<int> check() KJ_WARN_UNUSED_RESULT;

private:
  kj::Own<DH> dh;
};

}  // namespace workerd::api
