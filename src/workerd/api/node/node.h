#pragma once

#include "async-hooks.h"
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/modules.h>
#include <capnp/dynamic.h>
#include <node/bundle.capnp.h>

namespace workerd::api::node {

class CompatibilityFlags : public jsg::Object {
  // To be exposed only as an internal module for use by other built-ins.
  // TODO(later): Consider moving out of node.h when needed for other
  // built-ins
public:
  JSG_RESOURCE_TYPE(CompatibilityFlags, workerd::CompatibilityFlags::Reader flags) {
    // Not your typical JSG_RESOURCE_TYPE definition.. here we are iterating
    // through all of the compatibility flags and registering each as read-only
    // literal values on the instance...
    auto dynamic = capnp::toDynamic(flags);
    auto schema = dynamic.getSchema();
    for (auto field : schema.getFields()) {
      registry.template registerReadonlyInstanceProperty<bool>(
          field.getProto().getName(),
          dynamic.get(field).as<bool>());
    }
  }
};

template <typename TypeWrapper>
void registerNodeJsCompatModules(
    workerd::jsg::ModuleRegistryImpl<TypeWrapper>& registry, auto featureFlags) {
  registry.template addBuiltinModule<CompatibilityFlags>("workerd:compatibility-flags",
      workerd::jsg::ModuleRegistry::Type::INTERNAL);
  registry.template addBuiltinModule<AsyncHooksModule>("node-internal:async_hooks",
      workerd::jsg::ModuleRegistry::Type::INTERNAL);
  registry.addBuiltinBundle(NODE_BUNDLE);
}

#define EW_NODE_ISOLATE_TYPES      \
  api::node::CompatibilityFlags,   \
  EW_NODE_ASYNCHOOKS_ISOLATE_TYPES

}  // namespace workerd::api::node
