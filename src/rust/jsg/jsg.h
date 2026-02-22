#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/jsg/modules.h>
#include <workerd/rust/jsg/ffi-inl.h>
#include <workerd/rust/jsg/ffi.h>
#include <workerd/rust/jsg/v8.rs.h>

#include <kj-rs/kj-rs.h>
#include <rust/cxx.h>

#include <kj/function.h>

namespace workerd::rust::jsg {

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
}  // namespace workerd::rust::jsg
