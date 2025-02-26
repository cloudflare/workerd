// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/api/node/node.h>
#include <workerd/api/pyodide/pyodide.h>
#include <workerd/api/rtti.h>
#include <workerd/api/sockets.h>
#include <workerd/api/unsafe.h>
#include <workerd/api/worker-rpc.h>
#include <workerd/io/features.h>
#include <workerd/io/worker.h>
#include <workerd/jsg/modules-new.h>

#include <cloudflare/cloudflare.capnp.h>

namespace workerd::api {

class EnvModule final: public jsg::Object {
 public:
  EnvModule() = default;
  EnvModule(jsg::Lock&, const jsg::Url&) {}

  kj::Maybe<jsg::JsObject> getCurrent(jsg::Lock& js);

  // Arranges to propagate the given newEnv in the async context.
  jsg::JsRef<jsg::JsValue> withEnv(
      jsg::Lock& js, jsg::Value newEnv, jsg::Function<jsg::JsRef<jsg::JsValue>()> fn);

  JSG_RESOURCE_TYPE(EnvModule) {
    JSG_METHOD(getCurrent);
    JSG_METHOD(withEnv);
  }
};

template <class Registry>
void registerModules(Registry& registry, auto featureFlags) {
  node::registerNodeJsCompatModules(registry, featureFlags);
  registerUnsafeModules(registry, featureFlags);
  if (featureFlags.getRttiApi()) {
    registerRTTIModule(registry);
  }
  if (featureFlags.getUnsafeModule()) {
    registerUnsafeModule(registry);
  }
  registerSocketsModule(registry, featureFlags);
  registry.addBuiltinBundle(CLOUDFLARE_BUNDLE);
  registerRpcModules(registry, featureFlags);
  registry.template addBuiltinModule<EnvModule>(
      "cloudflare-internal:env", workerd::jsg::ModuleRegistry::Type::INTERNAL);
}

template <class TypeWrapper>
void registerBuiltinModules(jsg::modules::ModuleRegistry::Builder& builder, auto featureFlags) {
  builder.add(node::getInternalNodeJsCompatModuleBundle<TypeWrapper>(featureFlags));
  builder.add(node::getExternalNodeJsCompatModuleBundle(featureFlags));
  builder.add(getInternalSocketModuleBundle<TypeWrapper>(featureFlags));
  builder.add(getInternalRpcModuleBundle<TypeWrapper>(featureFlags));

  builder.add(getInternalUnsafeModuleBundle<TypeWrapper>(featureFlags));
  if (featureFlags.getUnsafeModule()) {
    builder.add(getExternalUnsafeModuleBundle<TypeWrapper>(featureFlags));
  }

  if (featureFlags.getRttiApi()) {
    builder.add(getExternalRttiModuleBundle<TypeWrapper>(featureFlags));
  }

  {
    jsg::modules::ModuleBundle::BuiltinBuilder builtinsBuilder(
        jsg::modules::ModuleBundle::BuiltinBuilder::Type::BUILTIN_ONLY);
    jsg::modules::ModuleBundle::getBuiltInBundleFromCapnp(builtinsBuilder, CLOUDFLARE_BUNDLE);
    builder.add(builtinsBuilder.finish());
  }

  {
    jsg::modules::ModuleBundle::BuiltinBuilder builtinsBuilder(
        jsg::modules::ModuleBundle::BuiltinBuilder::Type::BUILTIN);
    builtinsBuilder.addObject<EnvModule, TypeWrapper>("cloudflare-internal:env"_url);
    jsg::modules::ModuleBundle::getBuiltInBundleFromCapnp(builtinsBuilder, CLOUDFLARE_BUNDLE);
    builder.add(builtinsBuilder.finish());
  }
}

}  // namespace workerd::api
