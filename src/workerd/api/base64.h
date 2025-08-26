#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/jsg/modules-new.h>
#include <workerd/jsg/url.h>

namespace workerd::api {

class Base64Module final: public jsg::Object {
 public:
  Base64Module() = default;
  Base64Module(jsg::Lock&, const jsg::Url&) {}

  jsg::BufferSource decodeArray(jsg::Lock& js, jsg::BufferSource source);
  jsg::BufferSource encodeArray(jsg::Lock& js, jsg::BufferSource source);
  jsg::JsString encodeArrayToString(jsg::Lock&, jsg::BufferSource source);

  JSG_RESOURCE_TYPE(Base64Module) {
    JSG_METHOD(encodeArray);
    JSG_METHOD(decodeArray);
    JSG_METHOD(encodeArrayToString);
    JSG_TS_OVERRIDE({
      decodeArray(source: ArrayBuffer | ArrayBufferView): ArrayBuffer;
      encodeArray(source: ArrayBuffer | ArrayBufferView): ArrayBuffer;
      encodeArrayToString(source: ArrayBuffer | ArrayBufferView): string;
    });
  }
};

template <class Registry>
void registerBase64Module(Registry& registry, auto featureFlags) {
  registry.template addBuiltinModule<Base64Module>(
      "cloudflare-internal:base64", workerd::jsg::ModuleRegistry::Type::INTERNAL);
}

template <typename TypeWrapper>
kj::Own<jsg::modules::ModuleBundle> getInternalBase64ModuleBundle(auto featureFlags) {
  jsg::modules::ModuleBundle::BuiltinBuilder builder(
      jsg::modules::ModuleBundle::BuiltinBuilder::Type::BUILTIN_ONLY);
  static const auto kSpecifier = "cloudflare-internal:base64"_url;
  builder.addObject<Base64Module, TypeWrapper>(kSpecifier);
  return builder.finish();
}

#define EW_BASE64_ISOLATE_TYPES api::Base64Module

}  // namespace workerd::api
