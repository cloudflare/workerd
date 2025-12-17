// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/api/base64.h>
#include <workerd/api/filesystem.h>
#include <workerd/api/node/node.h>
#include <workerd/api/pyodide/pyodide.h>
#include <workerd/api/rtti.h>
#include <workerd/api/sockets.h>
#include <workerd/api/tracing-module.h>
#include <workerd/api/unsafe.h>
#include <workerd/api/workers-module.h>
#include <workerd/jsg/modules-new.h>

#include <cloudflare/cloudflare.capnp.h>

namespace workerd::api {

// An object with a [Symbol.dispose]() method to remove patch to environment. Not exposed
// publically, just used to implement Python's `patch_env()` context manager.
// See src/pyodide/internal/envHelpers.ts
class PythonPatchedEnv: public jsg::Object {
 public:
  PythonPatchedEnv(jsg::Lock& js, jsg::AsyncContextFrame::StorageKey& key, jsg::Value store) {
    scope.emplace(js, key, kj::mv(store));
  }

  void dispose() {
    scope = kj::none;
  }

  JSG_RESOURCE_TYPE(PythonPatchedEnv) {
    JSG_DISPOSE(dispose);
  }

 private:
  kj::Maybe<jsg::AsyncContextFrame::StorageScope> scope;
};

class EnvModule final: public jsg::Object {
 public:
  EnvModule() = default;
  EnvModule(jsg::Lock&, const jsg::Url&) {}

  kj::Maybe<jsg::JsObject> getCurrentEnv(jsg::Lock& js);
  kj::Maybe<jsg::JsObject> getCurrentExports(jsg::Lock& js);

  // Arranges to propagate the given newEnv in the async context.
  jsg::JsRef<jsg::JsValue> withEnv(
      jsg::Lock& js, jsg::Value newEnv, jsg::Function<jsg::JsRef<jsg::JsValue>()> fn);

  jsg::JsRef<jsg::JsValue> withExports(
      jsg::Lock& js, jsg::Value newExports, jsg::Function<jsg::JsRef<jsg::JsValue>()> fn);

  jsg::JsRef<jsg::JsValue> withEnvAndExports(jsg::Lock& js,
      jsg::Value newEnv,
      jsg::Value newExports,
      jsg::Function<jsg::JsRef<jsg::JsValue>()> fn);

  // Patch environment and return an object with a [Symbol.dispose]() method to restore it.
  // Not exposed publically, just used to implement Python's `patch_env()` context manager.
  // See src/pyodide/internal/envHelpers.ts
  jsg::Ref<PythonPatchedEnv> pythonPatchEnv(jsg::Lock& js, jsg::Value newEnv);

  JSG_RESOURCE_TYPE(EnvModule) {
    JSG_METHOD(getCurrentEnv);
    JSG_METHOD(getCurrentExports);
    JSG_METHOD(withEnv);
    JSG_METHOD(withExports);
    JSG_METHOD(withEnvAndExports);
    JSG_METHOD(pythonPatchEnv);
  }
};

template <class Registry>
void registerModules(Registry& registry, auto featureFlags) {
  node::registerNodeJsCompatModules(registry, featureFlags);
  registerUnsafeModules(registry, featureFlags);
  if (featureFlags.getPythonWorkers()) {
    pyodide::registerPyodideModules(registry, featureFlags);
  }
  if (featureFlags.getRttiApi()) {
    registerRTTIModule(registry);
  }
  if (featureFlags.getUnsafeModule()) {
    registerUnsafeModule(registry);
  }
  registerSocketsModule(registry, featureFlags);
  registerBase64Module(registry, featureFlags);
  registry.addBuiltinBundle(CLOUDFLARE_BUNDLE);
  registerWorkersModule(registry, featureFlags);
  registerTracingModule(registry, featureFlags);
  registry.template addBuiltinModule<EnvModule>(
      "cloudflare-internal:env", workerd::jsg::ModuleRegistry::Type::INTERNAL);
  registry.template addBuiltinModule<FileSystemModule>(
      "cloudflare-internal:filesystem", workerd::jsg::ModuleRegistry::Type::INTERNAL);
}

template <class TypeWrapper>
void registerBuiltinModules(jsg::modules::ModuleRegistry::Builder& builder, auto featureFlags) {
  builder.add(node::getInternalNodeJsCompatModuleBundle<TypeWrapper>(featureFlags));
  builder.add(node::getExternalNodeJsCompatModuleBundle(featureFlags));
  builder.add(getInternalSocketModuleBundle<TypeWrapper>(featureFlags));
  builder.add(getInternalBase64ModuleBundle<TypeWrapper>(featureFlags));
  builder.add(getInternalRpcModuleBundle<TypeWrapper>(featureFlags));

  builder.add(getInternalUnsafeModuleBundle<TypeWrapper>(featureFlags));
  builder.add(getInternalTracingModuleBundle<TypeWrapper>(featureFlags));
  if (featureFlags.getUnsafeModule()) {
    builder.add(getExternalUnsafeModuleBundle<TypeWrapper>(featureFlags));
  }

  if (featureFlags.getRttiApi()) {
    builder.add(getExternalRttiModuleBundle<TypeWrapper>(featureFlags));
  }

  {
    jsg::modules::ModuleBundle::BuiltinBuilder builtinsBuilder(
        jsg::modules::ModuleBundle::BuiltinBuilder::Type::BUILTIN);
    jsg::modules::ModuleBundle::getBuiltInBundleFromCapnp(builtinsBuilder, CLOUDFLARE_BUNDLE);
    builder.add(builtinsBuilder.finish());
  }

  {
    jsg::modules::ModuleBundle::BuiltinBuilder builtinsBuilder(
        jsg::modules::ModuleBundle::BuiltinBuilder::Type::BUILTIN_ONLY);
    builtinsBuilder.addObject<EnvModule, TypeWrapper>("cloudflare-internal:env"_url);
    builtinsBuilder.addObject<FileSystemModule, TypeWrapper>("cloudflare-internal:filesystem"_url);
    jsg::modules::ModuleBundle::getBuiltInBundleFromCapnp(builtinsBuilder, CLOUDFLARE_BUNDLE);
    builder.add(builtinsBuilder.finish());
  }
}

}  // namespace workerd::api
