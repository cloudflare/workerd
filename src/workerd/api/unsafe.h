#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/jsg/modules-new.h>
#include <workerd/jsg/url.h>
#include <workerd/io/io-context.h>

namespace workerd::api {

// A special binding object that allows for dynamic evaluation.
class UnsafeEval: public jsg::Object {
public:
  UnsafeEval() = default;
  UnsafeEval(jsg::Lock&, const jsg::Url&) {}

  // A non-capturing eval. Compile and evaluates the given script, returning whatever
  // value is returned by the script. This version of eval intentionally does not
  // capture any part of the outer scope other than globalThis and globally scoped
  // variables. The optional `name` will appear in stack traces for any errors thrown.
  //
  // console.log(env.unsafe.eval('1 + 1'));  // prints 2
  //
  jsg::JsValue eval(jsg::Lock& js, kj::String script, jsg::Optional<kj::String> name);

  using UnsafeEvalFunction = jsg::Function<jsg::Value(jsg::Arguments<jsg::Value>)>;

  // Compiles and returns a new Function using the given script. The function does not
  // capture any part of the outer scope other than globalThis and globally scoped
  // variables. The optional `name` will be set as the name of the function and will
  // appear in stack traces for any errors thrown. An optional list of argument names
  // can be passed in.
  //
  // const fn = env.unsafe.newFunction('return m', 'foo', 'm');
  // console.log(fn(1));  // prints 1
  //
  UnsafeEvalFunction newFunction(jsg::Lock& js,
      jsg::JsString script,
      jsg::Optional<kj::String> name,
      jsg::Arguments<jsg::JsRef<jsg::JsString>> args,
      const jsg::TypeHandler<UnsafeEvalFunction>& handler);

  // Compiles and returns a new Async Function using the given script. The function
  // does not capture any part of the outer scope other than globalThis and globally
  // scoped variables. The optional `name` will be set as the name of the function
  // and will appear in stack traces for any errors thrown. An optional list of
  // arguments names can be passed in. If your function needs to use the await
  // key, use this instead of newFunction.
  UnsafeEvalFunction newAsyncFunction(jsg::Lock& js,
      jsg::JsString script,
      jsg::Optional<kj::String> name,
      jsg::Arguments<jsg::JsRef<jsg::JsString>> args,
      const jsg::TypeHandler<UnsafeEvalFunction>& handler);

  jsg::JsValue newWasmModule(jsg::Lock& js, kj::Array<kj::byte> src);

  JSG_RESOURCE_TYPE(UnsafeEval) {
    JSG_METHOD(eval);
    JSG_METHOD(newFunction);
    JSG_METHOD(newAsyncFunction);
    JSG_METHOD(newWasmModule);
  }
};

class UnsafeModule: public jsg::Object {
public:
  UnsafeModule() = default;
  UnsafeModule(jsg::Lock&, const jsg::Url&) {}
  jsg::Promise<void> abortAllDurableObjects(jsg::Lock& js);

  JSG_RESOURCE_TYPE(UnsafeModule) {
    JSG_METHOD(abortAllDurableObjects);
  }
};

template <class Registry>
void registerUnsafeModule(Registry& registry) {
  registry.template addBuiltinModule<UnsafeModule>(
      "workerd:unsafe", workerd::jsg::ModuleRegistry::Type::BUILTIN);
  registry.template addBuiltinModule<UnsafeEval>(
      "workerd:unsafe-eval", workerd::jsg::ModuleRegistry::Type::BUILTIN);
}

#define EW_UNSAFE_ISOLATE_TYPES api::UnsafeEval, api::UnsafeModule

template <class Registry>
void registerUnsafeModules(Registry& registry, auto featureFlags) {
  registry.template addBuiltinModule<UnsafeEval>(
      "internal:unsafe-eval", workerd::jsg::ModuleRegistry::Type::INTERNAL);
}

template <typename TypeWrapper>
kj::Own<jsg::modules::ModuleBundle> getInternalUnsafeModuleBundle(auto featureFlags) {
  jsg::modules::ModuleBundle::BuiltinBuilder builder(
      jsg::modules::ModuleBundle::BuiltinBuilder::Type::BUILTIN_ONLY);
  static const auto kSpecifier = "internal:unsafe-eval"_url;
  builder.addObject<UnsafeEval, TypeWrapper>(kSpecifier);
  return builder.finish();
}

template <typename TypeWrapper>
kj::Own<jsg::modules::ModuleBundle> getExternalUnsafeModuleBundle(auto featureFlags) {
  jsg::modules::ModuleBundle::BuiltinBuilder builder(
      jsg::modules::ModuleBundle::BuiltinBuilder::Type::BUILTIN);
  static const auto kSpecifier = "workerd:unsafe-eval"_url;
  builder.addObject<UnsafeEval, TypeWrapper>(kSpecifier);

  static const auto kUnsafeSpecifier = "workerd:unsafe"_url;
  builder.addObject<UnsafeModule, TypeWrapper>(kUnsafeSpecifier);
  return builder.finish();
}
}  // namespace workerd::api
