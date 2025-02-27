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
#include <workerd/jsg/setup.h>

#include <cloudflare/cloudflare.capnp.h>

namespace workerd::api {

class EnvModule final: public jsg::Object {
 public:
  kj::Maybe<jsg::JsObject> getCurrent(jsg::Lock& js) {
    auto& key = jsg::IsolateBase::from(js.v8Isolate).getEnvAsyncContextKey();
    KJ_IF_SOME(frame, jsg::AsyncContextFrame::current(js)) {
      KJ_IF_SOME(value, frame.get(key)) {
        auto handle = value.getHandle(js);
        if (handle->IsObject()) {
          return jsg::JsObject(handle.As<v8::Object>());
        }
      }
    }
    return kj::none;
  }

  jsg::JsValue withEnv(jsg::Lock& js, jsg::Value newEnv, jsg::Function<jsg::JsValue()> fn) {
    auto& key = jsg::IsolateBase::from(js.v8Isolate).getEnvAsyncContextKey();
    jsg::AsyncContextFrame::StorageScope storage(js, key, kj::mv(newEnv));
    return fn(js);
  }

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
    jsg::modules::ModuleBundle::getBuiltInBundleFromCapnp(builtinsBuilder, CLOUDFLARE_BUNDLE);
    builder.add(builtinsBuilder.finish());
  }

  // TODO(later): Add the internal env module also.
}

}  // namespace workerd::api
