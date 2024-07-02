#pragma once

#include <openssl/dh.h>
#include <kj/common.h>
#include "impl.h"

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

  kj::Array<kj::byte> getPublicKey() KJ_WARN_UNUSED_RESULT;
  kj::Array<kj::byte> getPrivateKey() KJ_WARN_UNUSED_RESULT;
  kj::Array<kj::byte> getGenerator() KJ_WARN_UNUSED_RESULT;
  kj::Array<kj::byte> getPrime() KJ_WARN_UNUSED_RESULT;
  kj::Array<kj::byte> computeSecret(kj::ArrayPtr<kj::byte> key)
      KJ_WARN_UNUSED_RESULT;
  kj::Array<kj::byte> generateKeys() KJ_WARN_UNUSED_RESULT;

  kj::Maybe<int> check() KJ_WARN_UNUSED_RESULT;

private:
  kj::Own<DH> dh;
};

}  // namespace workerd::api
