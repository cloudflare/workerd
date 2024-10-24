#pragma once

#include <workerd/io/io-context.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/modules-new.h>
#include <workerd/jsg/url.h>

namespace workerd::api::pyodide {

using instantiateEmscriptenModuleFunction = jsg::Function<jsg::Promise<jsg::JsRef<jsg::JsValue>>(
    jsg::JsBoolean, jsg::JsString, jsg::JsString)>;

struct EmscriptenRuntime {
  jsg::JsRef<jsg::JsValue> contextToken;
  jsg::JsRef<jsg::JsValue> emscriptenRuntime;
};

class SetupEmscripten: public jsg::Object {
public:
  SetupEmscripten() {};
  SetupEmscripten(jsg::Lock& js, const jsg::Url&) {}

  jsg::JsValue getModule(jsg::Lock& js);

  JSG_RESOURCE_TYPE(SetupEmscripten) {
    JSG_METHOD(getModule);
  }

private:
  // Reference to the api value of the emscripten module.
  // Used for visitForGc when no js is currently running.
  kj::Maybe<const jsg::JsRef<jsg::JsValue>&> emscriptenModule;
  void visitForGc(jsg::GcVisitor& visitor);
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

EmscriptenRuntime initializeEmscriptenRuntime(jsg::Lock& js, bool isWorkerd);

}  // namespace workerd::api::pyodide
