#pragma once

#include <workerd/io/io-context.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/modules-new.h>
#include <workerd/jsg/script.h>
#include <workerd/jsg/url.h>

#include <csignal>
#include <iostream>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include "fuzzilli.h"

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

// A special binding that allows access to stdin. Used for REPL.
class Stdin: public jsg::Object {
 public:
  Stdin() = default;

  void reprl(jsg::Lock& js);

  kj::String getline(jsg::Lock& js) {
    std::string res;
    std::getline(std::cin, res);
    return kj::heapString(res.c_str());
  }

  JSG_RESOURCE_TYPE(Stdin) {
    JSG_METHOD(getline);
#ifdef WORKERD_FUZZILLI
    JSG_METHOD(reprl);
#endif
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

#ifdef WORKERD_FUZZILLI
// Fuzzilli fuzzing support for triggering crashes and printing debug output
class Fuzzilli: public jsg::Object {
 public:
  Fuzzilli() = default;
  Fuzzilli(jsg::Lock&, const jsg::Url&) {}

  // Fuzzilli function for triggering crashes or printing debug output
  // fuzzilli('FUZZILLI_CRASH', type: number): Triggers a crash based on type
  // fuzzilli('FUZZILLI_PRINT', message: string): Prints message to fuzzer output
  void fuzzilli(jsg::Lock& js, jsg::Arguments<jsg::Value> args);

  JSG_RESOURCE_TYPE(Fuzzilli) {
    JSG_METHOD(fuzzilli);
  }
};
#endif

template <class Registry>
void registerUnsafeModule(Registry& registry) {
  registry.template addBuiltinModule<UnsafeModule>(
      "workerd:unsafe", workerd::jsg::ModuleRegistry::Type::BUILTIN);
  registry.template addBuiltinModule<UnsafeEval>(
      "workerd:unsafe-eval", workerd::jsg::ModuleRegistry::Type::BUILTIN);
}

#ifdef WORKERD_FUZZILLI
#define EW_UNSAFE_ISOLATE_TYPES api::UnsafeEval, api::UnsafeModule, api::Stdin, api::Fuzzilli
#else
#define EW_UNSAFE_ISOLATE_TYPES api::UnsafeEval, api::UnsafeModule, api::Stdin
#endif

template <class Registry>
void registerUnsafeModules(Registry& registry, auto featureFlags) {
  registry.template addBuiltinModule<UnsafeEval>(
      "internal:unsafe-eval", workerd::jsg::ModuleRegistry::Type::INTERNAL);
#ifdef WORKERD_FUZZILLI
  registry.template addBuiltinModule<Stdin>(
      "workerd:stdin", workerd::jsg::ModuleRegistry::Type::BUILTIN);

  if (featureFlags.getWorkerdExperimental()) {
    registry.template addBuiltinModule<Fuzzilli>(
        "workerd:fuzzilli", workerd::jsg::ModuleRegistry::Type::BUILTIN);
  }
#endif
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
