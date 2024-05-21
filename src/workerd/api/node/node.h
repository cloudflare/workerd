#pragma once

#include "async-hooks.h"
#include "buffer.h"
#include "crypto.h"
#include "diagnostics-channel.h"
#include "util.h"
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/modules.h>
#include <capnp/dynamic.h>
#include <node/node.capnp.h>

namespace workerd::api::node {

// To be exposed only as an internal module for use by other built-ins.
// TODO(later): Consider moving out of node.h when needed for other
// built-ins
class CompatibilityFlags : public jsg::Object {
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

template <class Registry>
void registerNodeJsCompatModules(
    Registry& registry, auto featureFlags) {

#define NODEJS_MODULES(V)                                                       \
  V(CompatibilityFlags, "workerd:compatibility-flags")                          \
  V(AsyncHooksModule, "node-internal:async_hooks")                              \
  V(BufferUtil, "node-internal:buffer")                                         \
  V(CryptoImpl, "node-internal:crypto")                                         \
  V(UtilModule, "node-internal:util")                                           \
  V(DiagnosticsChannelModule, "node-internal:diagnostics_channel")

// Add to the NODEJS_MODULES_EXPERIMENTAL list any currently in-development
// node.js compat C++ modules that should be guarded by the experimental compat
// flag. Once they are ready to ship, move them up to the NODEJS_MODULES list.
#define NODEJS_MODULES_EXPERIMENTAL(V)

#define V(T, N)                                                                 \
  registry.template addBuiltinModule<T>(N, workerd::jsg::ModuleRegistry::Type::INTERNAL);

  NODEJS_MODULES(V)

  if (featureFlags.getWorkerdExperimental()) {
    NODEJS_MODULES_EXPERIMENTAL(V)
  }

#undef V
#undef NODEJS_MODULES

  bool nodeJsCompatEnabled = featureFlags.getNodeJsCompat() ||
                             featureFlags.getNodeJsCompatV2();

  // If the `nodejs_compat` flag isn't enabled, only register internal modules.
  // We need these for `console.log()`ing when running `workerd` locally.
  kj::Maybe<jsg::ModuleType> maybeFilter;
  if (!nodeJsCompatEnabled) maybeFilter = jsg::ModuleType::INTERNAL;

  registry.addBuiltinBundle(NODE_BUNDLE, maybeFilter);

  // If the `nodejs_compat` flag is off, but the `nodejs_als` flag is on, we
  // need to register the `node:async_hooks` module from the bundle.
  if (!nodeJsCompatEnabled && featureFlags.getNodeJsAls()) {
    jsg::Bundle::Reader reader = NODE_BUNDLE;
    for (auto module : reader.getModules()) {
      auto specifier = module.getName();
      if (specifier == "node:async_hooks") {
        KJ_DASSERT(module.getType() == jsg::ModuleType::BUILTIN);
        registry.addBuiltinModule(module);
      }
    }
  }
}

#define EW_NODE_ISOLATE_TYPES              \
  api::node::CompatibilityFlags,           \
  EW_NODE_BUFFER_ISOLATE_TYPES,            \
  EW_NODE_CRYPTO_ISOLATE_TYPES,            \
  EW_NODE_DIAGNOSTICCHANNEL_ISOLATE_TYPES, \
  EW_NODE_ASYNCHOOKS_ISOLATE_TYPES,        \
  EW_NODE_UTIL_ISOLATE_TYPES

}  // namespace workerd::api::node
