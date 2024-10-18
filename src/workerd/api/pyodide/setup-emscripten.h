#pragma once

#include <workerd/io/io-context.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/modules-new.h>
#include <workerd/jsg/url.h>

namespace workerd::api::pyodide {

class EmscriptenModule: public jsg::Object {
public:
  explicit EmscriptenModule(jsg::Lock& js) {};
  JSG_STRUCT(EmscriptenModule);

private:
};

using instantiateEmscriptenModuleFunction = jsg::Function<jsg::Promise<jsg::JsRef<jsg::JsValue>>(
    jsg::JsBoolean, jsg::JsString, jsg::JsString)>;

class SetupEmscripten: public jsg::Object {
public:
  SetupEmscripten() {};
  SetupEmscripten(jsg::Lock& js, const jsg::Url&) {}

  jsg::JsValue getModule(jsg::Lock& js);

  JSG_RESOURCE_TYPE(SetupEmscripten) {
    JSG_METHOD(getModule);
  }

private:
  kj::Maybe<jsg::JsRef<jsg::JsValue>> module_;
};

#define EW_SETUP_EMSCRIPTEN_ISOLATE_TYPES api::pyodide::SetupEmscripten

template <class Registry>
void registerSetupEmscriptenModule(Registry& registry, auto featureFlags) {
  registry.template addBuiltinModule<SetupEmscripten>(
      "internal:setup-emscripten", workerd::jsg::ModuleRegistry::Type::INTERNAL);
}

template <typename TypeWrapper>
kj::Own<jsg::modules::ModuleBundle> getInternalSetupEmscriptenModuleBundle(auto featureFlags) {
  jsg::modules::ModuleBundle::BuiltinBuilder builder(
      jsg::modules::ModuleBundle::BuiltinBuilder::Type::BUILTIN_ONLY);
  static const auto kSpecifier = "internal:setup-emscripten"_url;
  builder.addObject<SetupEmscripten, TypeWrapper>(kSpecifier);
  return builder.finish();
}

jsg::JsValue initializeEmscriptenRuntime(jsg::Lock&);

}  // namespace workerd::api::pyodide
