#pragma once

#include <workerd/jsg/jsg.h>

namespace workerd::api::node {

class CryptoImpl final: public jsg::Object {
public:

  JSG_RESOURCE_TYPE(CryptoImpl) {
  }
};

#define EW_NODE_CRYPTO_ISOLATE_TYPES       \
    api::node::CryptoImpl
}  // namespace workerd::api::node
