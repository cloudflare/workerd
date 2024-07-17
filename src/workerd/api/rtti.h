// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/array.h>
#include <kj/string.h>
#include <workerd/io/compatibility-date.h>
#include <workerd/jsg/modules.h>
#include <workerd/jsg/modules-new.h>
#include <workerd/jsg/url.h>
#include <workerd/jsg/rtti.h>

namespace workerd::api {

class RTTIModule final: public jsg::Object {
public:
  RTTIModule() = default;
  RTTIModule(jsg::Lock&, const jsg::Url&) {}

  kj::Array<byte> exportTypes(kj::String compatDate, kj::Array<kj::String> compatFlags);
  kj::Array<byte> exportExperimentalTypes();

  JSG_RESOURCE_TYPE(RTTIModule) {
    JSG_METHOD(exportTypes);
    JSG_METHOD(exportExperimentalTypes);
  }
};

template <class Registry>
void registerRTTIModule(Registry& registry) {
  registry.template addBuiltinModule<RTTIModule>("workerd:rtti",
    workerd::jsg::ModuleRegistry::Type::BUILTIN);
}

template <typename TypeWrapper>
kj::Own<jsg::modules::ModuleBundle> getExternalRttiModuleBundle(auto featureFlags) {
  jsg::modules::ModuleBundle::BuiltinBuilder builder(
      jsg::modules::ModuleBundle::BuiltinBuilder::Type::BUILTIN);
  static const auto kSpecifier = "internal:rtti"_url;
  builder.addObject<RTTIModule, TypeWrapper>(kSpecifier);
  return builder.finish();
}

#define EW_RTTI_ISOLATE_TYPES api::RTTIModule

} // namespace workerd::api
