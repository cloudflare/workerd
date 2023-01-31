#pragma once

#include "async-hooks.h"
#include <workerd/jsg/modules.h>
#include <node/bundle.capnp.h>

namespace workerd::api::node {

template <typename TypeWrapper>
void registerNodeJsCompatModules(
    workerd::jsg::ModuleRegistryImpl<TypeWrapper>& registry, auto featureFlags) {
  registry.template addBuiltinModule<AsyncHooksModule>("node-internal:async_hooks",
      workerd::jsg::ModuleRegistry::Type::INTERNAL);

  registry.addBuiltinBundle(NODE_BUNDLE);
}

#define EW_NODE_ISOLATE_TYPES \
  EW_NODE_ASYNCHOOKS_ISOLATE_TYPES

}  // namespace workerd::api::node
