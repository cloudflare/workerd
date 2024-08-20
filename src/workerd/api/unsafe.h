#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/jsg/modules-new.h>
#include <workerd/jsg/url.h>
#include <workerd/io/io-context.h>
#include <iostream>
#include <unistd.h>

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
  UnsafeEvalFunction newFunction(
      jsg::Lock& js,
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
  UnsafeEvalFunction newAsyncFunction(
      jsg::Lock& js,
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

#define REPRL_CRFD 100
#define REPRL_CWFD 101
#define REPRL_DRFD 102
#define REPRL_DWFD 103

#define CHECK(condition)                    \
    do {                                    \
        if (!(condition)) {                 \
            fprintf(stderr, "Error: %s:%d: condition failed: %s\n", __FILE__, __LINE__, #condition); \
            exit(EXIT_FAILURE);             \
        }                                   \
    } while (0)

// A special binding that allows access to stdin. Used for REPL.
class Stdin: public jsg::Object {
public:
  Stdin() = default;

  kj::String getline(jsg::Lock& js) {
    std::string res;
    std::getline(std::cin, res);
    return kj::heapString(res.c_str());
  }

  void reprl(jsg::Lock& js) {
    js.setAllowEval(true);
    char helo[] = "HELO";
    if (write(REPRL_CWFD, helo, 4) != 4 || read(REPRL_CRFD, helo, 4) != 4) {
      printf("Invalid HELO response from parent\n");
    }

    if (memcmp(helo, "HELO", 4) != 0) {
      printf("Invalid response from parent\n");
    }

    do {
      size_t script_size = 0;
      char action[4];
      CHECK(read(REPRL_CRFD, action, 4) == 4);
      if (strcmp(action, "cexe") == 0) {
        CHECK(read(REPRL_CRFD, &script_size, 8) == 8);
      } else {
        fprintf(stderr, "Unknown action: %s\n", action);
        _exit(-1);
      }

      char script[script_size+1];
      char *ptr = script;
      size_t remaining = script_size;
      while(remaining > 0) {
        ssize_t rv = read(REPRL_DRFD, ptr, remaining);
        if(rv <= 0) {
          fprintf(stderr, "Failed to load script\n");
          _exit(-1);
        }
        remaining -= rv;
        ptr += rv;
      }

      script[script_size] = 0;

      //eval the script
      int status = 0;
      auto compiled = jsg::NonModuleScript::compile(script, js, "reprl"_kj);
      auto val = jsg::JsValue(compiled.runAndReturn(js.v8Context()));

      fflush(stdout);
      fflush(stderr);

      CHECK(write(REPRL_CWFD, &status, 4) == 4);

    } while(true);
  }

  JSG_RESOURCE_TYPE(Stdin) {
    JSG_METHOD(getline);
    JSG_METHOD(reprl);
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
  registry.template addBuiltinModule<UnsafeModule>("workerd:unsafe",
    workerd::jsg::ModuleRegistry::Type::BUILTIN);
  registry.template addBuiltinModule<UnsafeEval>("workerd:unsafe-eval",
    workerd::jsg::ModuleRegistry::Type::BUILTIN);
}

#define EW_UNSAFE_ISOLATE_TYPES api::UnsafeEval, \
  api::UnsafeModule, \
  api::Stdin

template <class Registry> void registerUnsafeModules(Registry& registry, auto featureFlags) {
  registry.template addBuiltinModule<UnsafeEval>("internal:unsafe-eval",
                                                 workerd::jsg::ModuleRegistry::Type::INTERNAL);
  registry.template addBuiltinModule<Stdin>("workerd:stdin",
                                                 workerd::jsg::ModuleRegistry::Type::BUILTIN);
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
} // namespace workerd::api
