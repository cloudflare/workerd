// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <initializer_list>
#include <kj/array.h>
#include <kj/string.h>
#include <kj/vector.h>
#include <workerd/io/compatibility-date.h>
#include <workerd/jsg/modules.h>
#include <workerd/jsg/rtti.h>

namespace workerd::api {

static constexpr auto RTTI_MODULE_SPECIFIER = "workerd:rtti"_kjc;

class RttiRegistry final {
public:
  RttiRegistry() = default;
  KJ_DISALLOW_COPY_AND_MOVE(RttiRegistry);

  struct CppModuleContents final {
    kj::String structureName;
  };
  struct TypeScriptModuleContents final {
    kj::StringPtr tsDeclarations;
  };

  struct Module final {
    kj::String specifier;
    kj::OneOf<CppModuleContents, TypeScriptModuleContents> contents;
  };

  void add(jsg::Bundle::Reader bundle);

  template <typename T>
  void add(kj::StringPtr specifier) {
    modules.add(Module {
      .specifier = kj::str(specifier),
      .contents = CppModuleContents {
        .structureName = jsg::fullyQualifiedTypeName(typeid(T))
      }
    });
  }

  kj::Array<Module> finish();

private:
  kj::Vector<Module> modules;
};

class RTTIModule final: public jsg::Object {
public:
  kj::Array<byte> exportTypes(kj::String compatDate, kj::Array<kj::String> compatFlags);

  JSG_RESOURCE_TYPE(RTTIModule) {
    JSG_METHOD(exportTypes);
  }
};

template <typename TypeWrapper>
kj::Own<jsg::modules::ModuleBundle> registerRTTIModule(
    jsg::CompilationObserver& observer) {
  jsg::modules::BuiltinModuleBundleBuilder builder(jsg::modules::Module::Type::BUILTIN, observer);
  builder.add<RTTIModule, TypeWrapper>(RTTI_MODULE_SPECIFIER);
  return builder.finish();
}

void rttiRegisterRtti(RttiRegistry& registry, auto featureFlags) {
  registry.add<RTTIModule>(RTTI_MODULE_SPECIFIER);
}

#define EW_RTTI_ISOLATE_TYPES api::RTTIModule

} // namespace workerd::api
