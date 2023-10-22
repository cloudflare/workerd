#pragma once

#include "async-hooks.h"
#include "buffer.h"
#include "crypto.h"
#include "diagnostics-channel.h"
#include "util.h"
#include <workerd/api/rtti.h>
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

template <typename TypeWrapper>
kj::Array<kj::Own<jsg::modules::ModuleBundle>> registerNodeJsCompatModules(
    auto featureFlags, jsg::CompilationObserver& observer) {
  jsg::modules::BuiltinModuleBundleBuilder internal(jsg::modules::Module::Type::INTERNAL,
                                                    observer);
  jsg::modules::BuiltinModuleBundleBuilder builtin(jsg::modules::Module::Type::BUILTIN,
                                                   observer);

  internal.add<CompatibilityFlags, TypeWrapper>("workerd:compatibility-flags"_kj);
  internal.add<AsyncHooksModule, TypeWrapper>("node-internal:async_hooks"_kj);
  internal.add<BufferUtil, TypeWrapper>("node-internal:buffer"_kj);
  internal.add<CryptoImpl, TypeWrapper>("node-internal:crypto"_kj);
  internal.add<UtilModule, TypeWrapper>("node-internal:util"_kj);
  internal.add<DiagnosticsChannelModule, TypeWrapper>("node-internal:diagnostics_channel");

  internal.add(NODE_BUNDLE);
  if (featureFlags.getNodeJsCompat()) {
    builtin.add(NODE_BUNDLE);
  }

  return kj::arr(internal.finish(), builtin.finish());
}

void nodeRegisterRtti(RttiRegistry& registry, auto featureFlags) {
  registry.add<CompatibilityFlags>("workerd:compatibility-flags"_kj);
  registry.add<CompatibilityFlags>("workerd:compatibility-flags"_kj);
  registry.add<AsyncHooksModule>("node-internal:async_hooks"_kj);
  registry.add<BufferUtil>("node-internal:buffer"_kj);
  registry.add<CryptoImpl>("node-internal:crypto"_kj);
  registry.add<UtilModule>("node-internal:util"_kj);
  registry.add<DiagnosticsChannelModule>("node-internal:diagnostics_channel");
  if (featureFlags.getNodeJsCompat()) {
    registry.add(NODE_BUNDLE);
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
