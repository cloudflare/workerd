// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/jsg/modules-new.h>
#include <workerd/jsg/modules.h>
#include <workerd/rust/jsg/ffi-inl.h>
#include <workerd/rust/jsg/ffi.h>
#include <workerd/rust/jsg/v8.rs.h>

#include <rust/cxx.h>

#include <kj/function.h>

namespace workerd::rust::jsg {

// Adapter for registering Rust-implemented modules into the OLD module registry
// (jsg::ModuleRegistryImpl). Translates Rust ModuleCallback into ModuleInfo.
template <typename Registry>
struct RustModuleRegistry: public ::workerd::rust::jsg::ModuleRegistry {
  virtual ~RustModuleRegistry() = default;
  RustModuleRegistry(Registry& registry): registry(registry) {}

  void addBuiltinModule(
      ::rust::Str specifier, ModuleCallback moduleCallback, ModuleType moduleType) override {
    ::workerd::jsg::ModuleType jsgModuleType;
    switch (moduleType) {
      case ModuleType::Bundle:
        jsgModuleType = ::workerd::jsg::ModuleType::BUNDLE;
        break;
      case ModuleType::Builtin:
        jsgModuleType = ::workerd::jsg::ModuleType::BUILTIN;
        break;
      case ModuleType::Internal:
        jsgModuleType = ::workerd::jsg::ModuleType::INTERNAL;
        break;
      default:
        KJ_UNREACHABLE;
    }

    registry.addBuiltinModule(kj::str(specifier),
        [kj_specifier = kj::str(specifier), callback = kj::mv(moduleCallback)](
            ::workerd::jsg::Lock& js, ::workerd::jsg::ModuleRegistry::ResolveMethod,
            kj::Maybe<const kj::Path&>&) mutable
        -> kj::Maybe<::workerd::jsg::ModuleRegistry::ModuleInfo> {
      auto localValue = callback(js.v8Isolate);
      auto value = local_from_ffi<v8::Value>(kj::mv(localValue));
      KJ_DASSERT(value->IsObject());

      using ModuleInfo = ::workerd::jsg::ModuleRegistry::ModuleInfo;
      using ObjectModuleInfo = ::workerd::jsg::ModuleRegistry::ObjectModuleInfo;
      return kj::Maybe(
          ModuleInfo(js, kj_specifier, kj::none, ObjectModuleInfo(js, value.As<v8::Object>())));
    },
        jsgModuleType);
  }

  Registry& registry;
};

// Adapter for registering Rust-implemented modules into the NEW module registry
// (jsg::modules::ModuleBundle::BuiltinBuilder). Translates Rust ModuleCallback
// into BuiltinBuilder::addSynthetic calls. Each Rust module callback receives a
// v8::Isolate* and returns a v8::Local<v8::Value> (the module's default export).
//
// Usage:
//   RustBuiltinModuleAdapter adapter(builder);
//   ::workerd::rust::api::register_nodejs_modules(adapter);
struct RustBuiltinModuleAdapter: public ::workerd::rust::jsg::ModuleRegistry {
  ::workerd::jsg::modules::ModuleBundle::BuiltinBuilder& builder;

  explicit RustBuiltinModuleAdapter(::workerd::jsg::modules::ModuleBundle::BuiltinBuilder& builder)
      : builder(builder) {}

  void addBuiltinModule(
      ::rust::Str specifier, ModuleCallback moduleCallback, ModuleType moduleType) override {
    auto specifierPtr = kj::ArrayPtr<const char>(specifier.data(), specifier.size());

    // A BuiltinBuilder can only represent builtin modules.
    KJ_REQUIRE(moduleType != ModuleType::Bundle,
        "Bundle-typed Rust modules cannot be registered through a BuiltinBuilder", specifierPtr);

    // Register the module only when its declared visibility matches the
    // builder this adapter wraps. The registration functions are invoked once
    // per bundle -- user-importable (BUILTIN) and internal-only (BUILTIN_ONLY)
    // -- and the adapter wrapping the matching builder performs the
    // registration. Ignoring the declared type here would silently give the
    // module a different visibility under the new module registry than the
    // RustModuleRegistry adapter above gives it under the original registry.
    auto accepted = builder.type() == ::workerd::jsg::modules::ModuleBundle::Type::BUILTIN
        ? ModuleType::Builtin
        : ModuleType::Internal;
    if (moduleType != accepted) {
      return;
    }

    KJ_IF_SOME(url, ::workerd::jsg::Url::tryParse(specifierPtr)) {
      builder.addSynthetic(url,
          [callback = kj::mv(moduleCallback)](::workerd::jsg::Lock& js, const ::workerd::jsg::Url&,
              const ::workerd::jsg::modules::Module::ModuleNamespace& ns,
              const ::workerd::jsg::CompilationObserver&) mutable {
        auto localValue = callback(js.v8Isolate);
        auto value = local_from_ffi<v8::Value>(kj::mv(localValue));
        KJ_DASSERT(value->IsObject());
        ns.setDefault(js, ::workerd::jsg::JsValue(value));
        return true;
      });
    } else {
      KJ_LOG(WARNING, "Ignoring Rust module with invalid specifier", specifierPtr);
    }
  }
};

}  // namespace workerd::rust::jsg
