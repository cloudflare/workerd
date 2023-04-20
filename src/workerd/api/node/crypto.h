#pragma once

#include <workerd/jsg/jsg.h>

namespace workerd::api::node {

class CryptoImpl final: public jsg::Object {
public:
  bool checkPrimeSync(kj::Array<kj::byte> bufferView, uint32_t num_checks);
  JSG_RESOURCE_TYPE(CryptoImpl) {
    JSG_METHOD(checkPrimeSync);
  }
};

#define EW_NODE_CRYPTO_ISOLATE_TYPES       \
    api::node::CryptoImpl
}  // namespace workerd::api::node
