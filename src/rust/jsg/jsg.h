#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/jsg/modules.h>
#include <workerd/rust/jsg/ffi.h>

#include <kj-rs/kj-rs.h>
#include <rust/cxx.h>

#include <kj/function.h>

namespace workerd::rust::jsg {

template <typename Registry>
struct RustModuleRegistry: public ::workerd::rust::jsg::ModuleRegistry {
  virtual ~RustModuleRegistry() = default;
  RustModuleRegistry(Registry& registry): registry(registry) {}

  // todo; add a parameter for jsg::moduletype
  void addBuiltinModule(::rust::Str specifier, ModuleCallback moduleCallback) override {
    auto kj_specifier = kj::str(specifier);
    registry.addBuiltinModule(kj_specifier,
        [kj_specifier = kj::mv(kj_specifier), callback = kj::mv(moduleCallback)](
            ::workerd::jsg::Lock& js, ::workerd::jsg::ModuleRegistry::ResolveMethod,
            kj::Maybe<const kj::Path&>&) mutable
        -> kj::Maybe<::workerd::jsg::ModuleRegistry::ModuleInfo> {
      auto localValue = callback(js.v8Isolate);

      // Convert uint64_t LocalValue back to v8::Local<v8::Value>
      auto value = local_from_ffi<v8::Value>(localValue);

      KJ_DASSERT(value->IsObject());

      // Convert to v8::Object
      auto wrap = value.As<v8::Object>();

      using ModuleInfo = ::workerd::jsg::ModuleRegistry::ModuleInfo;
      using ObjectModuleInfo = ::workerd::jsg::ModuleRegistry::ObjectModuleInfo;
      return kj::Maybe(ModuleInfo(js, kj_specifier, kj::none, ObjectModuleInfo(js, wrap)));
    },
        ::workerd::jsg::ModuleType::INTERNAL);
  }

  Registry& registry;
};
}  // namespace workerd::rust::jsg
