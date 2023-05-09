#pragma once

#include <workerd/jsg/jsg.h>

namespace workerd::api::node {

class CryptoImpl final: public jsg::Object {
public:
  kj::Array<kj::byte> randomPrime(uint32_t size, bool safe,
      jsg::Optional<kj::Array<kj::byte>> add, jsg::Optional<kj::Array<kj::byte>> rem);

  bool checkPrimeSync(kj::Array<kj::byte> bufferView, uint32_t num_checks);
  // Pbkdf2
  kj::Array<kj::byte> getPbkdf(kj::Array<kj::byte> password, kj::Array<kj::byte> salt,
                               uint32_t num_iterations, uint32_t keylen, kj::String name);
  JSG_RESOURCE_TYPE(CryptoImpl) {
    JSG_METHOD(randomPrime);
    JSG_METHOD(checkPrimeSync);
    JSG_METHOD(getPbkdf);
  }
};

#define EW_NODE_CRYPTO_ISOLATE_TYPES       \
    api::node::CryptoImpl
}  // namespace workerd::api::node
