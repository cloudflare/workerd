// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/api/base64.h>
#include <workerd/api/filesystem.h>
#include <workerd/api/messagechannel.h>
#include <workerd/api/node/node.h>
#include <workerd/api/pyodide/pyodide.h>
#include <workerd/api/rtti.h>
#include <workerd/api/sockets.h>
#include <workerd/api/tracing.h>
#include <workerd/api/unsafe-api/ffi.rs.h>
#include <workerd/api/unsafe.h>
#include <workerd/api/workers-module.h>
#include <workerd/api/wrapped-binding.h>
#include <workerd/jsg/modules-new.h>
#include <workerd/rust/jsg/jsg.h>
#include <workerd/util/autogate.h>

#include <cloudflare/cloudflare.capnp.h>

namespace workerd::api {

class EnvModule final: public jsg::Object {
 public:
  EnvModule() = default;
  EnvModule(jsg::Lock&, const jsg::Url&) {}

  // Stateless module object, safe to recreate for a worker started from a snapshot.
  static constexpr bool jsgSnapshotStateless = true;

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

  JSG_RESOURCE_TYPE(EnvModule) {
    JSG_METHOD(getCurrentEnv);
    JSG_METHOD(getCurrentExports);
    JSG_METHOD(withEnv);
    JSG_METHOD(withExports);
    JSG_METHOD(withEnvAndExports);
  }
};

// The unsafe modules come from Rust (src/workerd/api/unsafe-api) when the RUST_UNSAFE_MODULES
// autogate is on, and from unsafe.h otherwise. workerd:fuzzilli always comes from unsafe.h.
inline bool useRustUnsafeModules() {
  return util::Autogate::isEnabled(util::AutogateKey::RUST_UNSAFE_MODULES);
}

// registerUnsafeModules() and registerUnsafeModule(), from Rust or C++.
template <class Registry>
void registerUnsafeModulesOf(Registry& registry, auto featureFlags) {
  if (!useRustUnsafeModules()) {
    registerUnsafeModules(registry, featureFlags);
    if (featureFlags.getUnsafeModule()) {
      registerUnsafeModule(registry);
    }
    return;
  }

  ::workerd::rust::jsg::RustModuleRegistry<Registry, SuppressIoContextScope> rustRegistry(registry);
  unsafe_api::register_unsafe_modules(rustRegistry);
#ifdef WORKERD_FUZZILLI
  unsafe_api::register_stdin_module(rustRegistry);
  if (featureFlags.getWorkerdExperimental()) {
    registry.template addBuiltinModule<Fuzzilli>(
        "workerd:fuzzilli", workerd::jsg::ModuleRegistry::Type::BUILTIN);
  }
#endif
  if (featureFlags.getUnsafeModule()) {
    unsafe_api::register_unsafe_module(rustRegistry);
  }
}

// getInternalUnsafeModuleBundle(), from Rust or C++.
template <typename TypeWrapper>
kj::Own<jsg::modules::ModuleBundle> getInternalUnsafeModuleBundleOf(auto featureFlags) {
  if (!useRustUnsafeModules()) {
    return getInternalUnsafeModuleBundle<TypeWrapper>(featureFlags);
  }
  jsg::modules::ModuleBundle::BuiltinBuilder builder(
      jsg::modules::ModuleBundle::BuiltinBuilder::Type::BUILTIN_ONLY);
  ::workerd::rust::jsg::RustBuiltinModuleAdapter<SuppressIoContextScope> adapter(builder);
  unsafe_api::register_unsafe_modules(adapter);
  return builder.finish();
}

// getExternalUnsafeModuleBundle(), from Rust or C++.
template <typename TypeWrapper>
kj::Own<jsg::modules::ModuleBundle> getExternalUnsafeModuleBundleOf(auto featureFlags) {
  if (!useRustUnsafeModules()) {
    return getExternalUnsafeModuleBundle<TypeWrapper>(featureFlags);
  }
  jsg::modules::ModuleBundle::BuiltinBuilder builder(
      jsg::modules::ModuleBundle::BuiltinBuilder::Type::BUILTIN);
  ::workerd::rust::jsg::RustBuiltinModuleAdapter<SuppressIoContextScope> adapter(builder);
  unsafe_api::register_unsafe_module(adapter);
  return builder.finish();
}

#ifdef WORKERD_FUZZILLI
// getExternalFuzzilliModuleBundle(), with workerd:stdin from Rust or C++.
template <typename TypeWrapper>
kj::Own<jsg::modules::ModuleBundle> getExternalFuzzilliModuleBundleOf(auto featureFlags) {
  if (!useRustUnsafeModules()) {
    return getExternalFuzzilliModuleBundle<TypeWrapper>(featureFlags);
  }
  jsg::modules::ModuleBundle::BuiltinBuilder builder(
      jsg::modules::ModuleBundle::BuiltinBuilder::Type::BUILTIN);
  {
    ::workerd::rust::jsg::RustBuiltinModuleAdapter<SuppressIoContextScope> adapter(builder);
    unsafe_api::register_stdin_module(adapter);
  }
  if (featureFlags.getWorkerdExperimental()) {
    static const auto kFuzzilliSpecifier = "workerd:fuzzilli"_url;
    builder.addSynthetic(kFuzzilliSpecifier,
        jsg::modules::Module::newJsgObjectModuleHandler<Fuzzilli, TypeWrapper>(
            [](jsg::Lock& js) { return js.alloc<Fuzzilli>(); }));
  }
  return builder.finish();
}
#endif

// `moduleSource`, if given, provides the storage V8 reads the compiled-in module sources from;
// see jsg/module-source.h.
template <class Registry>
void registerModules(
    Registry& registry, auto featureFlags, const jsg::ModuleSource* moduleSource = nullptr) {
  node::registerNodeJsCompatModules(registry, featureFlags, moduleSource);
  registerUnsafeModulesOf(registry, featureFlags);
  if (featureFlags.getRttiApi()) {
    registerRTTIModule(registry);
  }
  registerSocketsModule(registry, featureFlags);
  registerBase64Module(registry, featureFlags);
  registerMessageChannelModule(registry, featureFlags);
  registry.addBuiltinBundle(CLOUDFLARE_BUNDLE, kj::none, moduleSource);
  registerWorkersModule(registry, featureFlags);
  registerTracingModule(registry, featureFlags);
  registerWrappedBindingModule(registry, featureFlags);
  registry.template addBuiltinModule<EnvModule>(
      "cloudflare-internal:env", workerd::jsg::ModuleRegistry::Type::INTERNAL);
  registry.template addBuiltinModule<FileSystemModule>(
      "cloudflare-internal:filesystem", workerd::jsg::ModuleRegistry::Type::INTERNAL);
}

template <class TypeWrapper>
void registerBuiltinModules(jsg::modules::ModuleRegistry::Builder& builder,
    auto featureFlags,
    const jsg::ModuleSource* moduleSource = nullptr) {
  builder.add(node::getInternalNodeJsCompatModuleBundle<TypeWrapper>(featureFlags, moduleSource));
  builder.add(node::getExternalNodeJsCompatModuleBundle(featureFlags, moduleSource));
  builder.add(getInternalSocketModuleBundle<TypeWrapper>(featureFlags));
  builder.add(getInternalBase64ModuleBundle<TypeWrapper>(featureFlags));
  builder.add(getInternalMessageChannelModuleBundle<TypeWrapper>(featureFlags));
  builder.add(getInternalRpcModuleBundle<TypeWrapper>(featureFlags));

  builder.add(getInternalUnsafeModuleBundleOf<TypeWrapper>(featureFlags));
  builder.add(getInternalTracingModuleBundle<TypeWrapper>(featureFlags));
  builder.add(getInternalWrappedBindingModuleBundle<TypeWrapper>(featureFlags));
  if (featureFlags.getUnsafeModule()) {
    builder.add(getExternalUnsafeModuleBundleOf<TypeWrapper>(featureFlags));
  }
#ifdef WORKERD_FUZZILLI
  builder.add(getExternalFuzzilliModuleBundleOf<TypeWrapper>(featureFlags));
#endif

  if (featureFlags.getRttiApi()) {
    builder.add(getExternalRttiModuleBundle<TypeWrapper>(featureFlags));
  }

  {
    jsg::modules::ModuleBundle::BuiltinBuilder builtinsBuilder(
        jsg::modules::ModuleBundle::BuiltinBuilder::Type::BUILTIN);
    jsg::modules::ModuleBundle::getBuiltInBundleFromCapnp(
        builtinsBuilder, CLOUDFLARE_BUNDLE, moduleSource);
    builder.add(builtinsBuilder.finish());
  }

  {
    jsg::modules::ModuleBundle::BuiltinBuilder builtinsBuilder(
        jsg::modules::ModuleBundle::BuiltinBuilder::Type::BUILTIN_ONLY);
    builtinsBuilder.addObject<EnvModule, TypeWrapper>("cloudflare-internal:env"_url);
    builtinsBuilder.addObject<FileSystemModule, TypeWrapper>("cloudflare-internal:filesystem"_url);
    jsg::modules::ModuleBundle::getBuiltInBundleFromCapnp(
        builtinsBuilder, CLOUDFLARE_BUNDLE, moduleSource);
    builder.add(builtinsBuilder.finish());
  }
}

}  // namespace workerd::api
