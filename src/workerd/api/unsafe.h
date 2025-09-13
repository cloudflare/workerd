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

  kj::String getline(jsg::Lock& js) {
    std::string res;
    std::getline(std::cin, res);
    return kj::heapString(res.c_str());
  }

  void reprl(jsg::Lock& js) {
    js.setAllowEval(true);

    /*
    cov_init_builtins_edges(static_cast<uint32_t>(
        v8::internal::BasicBlockProfiler::Get()
            ->GetCoverageBitmap(reinterpret_cast<v8::Isolate*>(js.v8Isolate))
            .size()));
    */

    char helo[] = "HELO";
    if (write(REPRL_CWFD, helo, 4) != 4 || read(REPRL_CRFD, helo, 4) != 4) {
      printf("Invalid HELO response from parent\n");
    }

    if (memcmp(helo, "HELO", 4) != 0) {
      printf("Invalid response from parent\n");
    }

    do {
      v8::HandleScope handle_scope(js.v8Isolate);
      v8::TryCatch try_catch(js.v8Isolate);
      try_catch.SetVerbose(true);

      size_t script_size = 0;
      unsigned action = 0;
      ssize_t nread = read(REPRL_CRFD, &action, 4);
      fflush(0);
      fflush(stderr);
      if (nread != 4 || action != 0x63657865) {  // 'exec'
        fprintf(stderr, "Unknown action: %x\n", action);
        exit(-1);
      }

      CHECK(read(REPRL_CRFD, &script_size, 8) == 8);

      char* script_ = (char*)malloc(script_size + 1);
      CHECK(script_ != nullptr);

      char* source_buffer_tail = script_;
      ssize_t remaining = (ssize_t)script_size;

      while (remaining > 0) {
        ssize_t rv = read(REPRL_DRFD, source_buffer_tail, (size_t)remaining);
        if (rv <= 0) {
          fprintf(stderr, "Failed to load script\n");
          exit(-1);
        }
        remaining -= rv;
        source_buffer_tail += rv;
      }

      script_[script_size] = '\0';

      int status = 0;
      unsigned res_val = 0;
      const kj::String script = kj::str(script_);
      const kj::String wrapped = kj::str("{", script_, "}");
      auto compiled = jsg::NonModuleScript::compile(js, wrapped, "reprl"_kj);
      try {
        auto result = compiled.runAndReturn(js);
        res_val = jsg::check(v8::Local<v8::Value>(result)->Int32Value(js.v8Context()));
        // if we reach that point execution was successful -> return 0
        res_val = 0;
      } catch (jsg::JsExceptionThrown&) {
        res_val = 11;
        if (try_catch.HasCaught()) {
          auto str =
              workerd::jsg::check(try_catch.Message()->Get()->ToDetailString(js.v8Context()));
          v8::String::Utf8Value utf8String(js.v8Isolate, str);
          fflush(stdout);
        }
      }

      fflush(stdout);
      fflush(stderr);
      status = (res_val & 0xFF) << 8;
      CHECK(write(REPRL_CWFD, &status, 4) == 4);
      __sanitizer_cov_reset_edgeguards();
      free(script_);
      //cleanup context

    } while (true);
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

template <class Registry>
void registerUnsafeModule(Registry& registry) {
  registry.template addBuiltinModule<UnsafeModule>(
      "workerd:unsafe", workerd::jsg::ModuleRegistry::Type::BUILTIN);
  registry.template addBuiltinModule<UnsafeEval>(
      "workerd:unsafe-eval", workerd::jsg::ModuleRegistry::Type::BUILTIN);
}

#define EW_UNSAFE_ISOLATE_TYPES api::UnsafeEval, api::UnsafeModule, api::Stdin

template <class Registry>
void registerUnsafeModules(Registry& registry, auto featureFlags) {
  registry.template addBuiltinModule<UnsafeEval>(
      "internal:unsafe-eval", workerd::jsg::ModuleRegistry::Type::INTERNAL);
  registry.template addBuiltinModule<Stdin>(
      "workerd:stdin", workerd::jsg::ModuleRegistry::Type::BUILTIN);
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
