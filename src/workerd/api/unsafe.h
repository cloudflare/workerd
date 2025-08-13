#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/jsg/modules-new.h>
#include <workerd/jsg/url.h>
#include <workerd/io/io-context.h>
#include <iostream>
#include <unistd.h>

#include <workerd/jsg/script.h>


namespace workerd::api {

struct shmem_data {
    uint32_t num_edges;
    unsigned char edges[];
};

#define SHM_SIZE 0x200000
#define MAX_EDGES ((SHM_SIZE - 4) * 8)
void __sanitizer_cov_reset_edgeguards();

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


// Define the REPRL file descriptors
#define REPRL_CRFD 100
#define REPRL_CWFD 101
#define REPRL_DRFD 102
#define REPRL_DWFD 103



#define CHECK(condition) \
do { \
    if (!(condition)) { \
        fprintf(stderr, "Error: %s:%d: condition failed: %s\n", __FILE__, __LINE__, #condition); \
        exit(EXIT_FAILURE); \
    } \
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
      if (nread != 4 || action != 0x63657865) { // 'exec'
          fprintf(stderr, "Unknown action: %x\n", action);
          _exit(-1);
      }

      CHECK(read(REPRL_CRFD, &script_size, 8) == 8);

      char* script_ = (char*) malloc(script_size + 1);
      CHECK(script_ != nullptr);


      char* source_buffer_tail = script_;
      ssize_t remaining = (ssize_t) script_size;

      //printf("Reading in script with size: %zu\n",script_size);
      //fflush(stdout);

      while (remaining > 0) {
        ssize_t rv = read(REPRL_DRFD, source_buffer_tail, (size_t) remaining);
        if (rv <= 0) {
          fprintf(stderr, "Failed to load script\n");
          _exit(-1);
        }
        remaining -= rv;
        source_buffer_tail += rv;
      }

      script_[script_size] = '\0';

      // Create a fresh context for this script execution to isolate variable scope
      // while preserving isolate-level global state (APIs, etc.)
      /*auto scriptContext = v8::Context::New(js.v8Isolate, nullptr, v8::ObjectTemplate::New(js.v8Isolate));
      // Set required embedder data slot to prevent fatal errors
      scriptContext->SetAlignedPointerInEmbedderData(3, nullptr);
	  {
        v8::Context::Scope originalScope(js.v8Context());
        auto originalGlobal = js.v8Context()->Global();
        v8::Context::Scope scriptScope(scriptContext);
        auto scriptGlobal = scriptContext->Global();
        
        // Get all property names from the original global object
        auto propNames = jsg::check(originalGlobal->GetOwnPropertyNames(js.v8Context()));
        uint32_t length = propNames->Length();
        
        // Copy each property to the new context's global object
        for (uint32_t i = 0; i < length; i++) {
          auto key = jsg::check(propNames->Get(js.v8Context(), i));
          auto value = jsg::check(originalGlobal->Get(js.v8Context(), key));
          jsg::check(scriptGlobal->Set(scriptContext, key, value));
        }
      }
      */
      //fprintf(stderr,"Script content: %s\n",script_);
      
      // Debug: Print global context before executing script
      /*printf("=== DEBUG: Global Context Properties ===\n");
      auto global = js.v8Context()->Global();
      auto propNames = jsg::check(global->GetOwnPropertyNames(js.v8Context()));
      uint32_t length = propNames->Length();
      for (uint32_t i = 0; i < length; i++) {
        auto key = jsg::check(propNames->Get(js.v8Context(), i));
         v8::String::Utf8Value keyStr(js.v8Isolate, key);
        printf("Global property: %s\n", *keyStr);
      }
      printf("=== END DEBUG ===\n");
      fflush(stdout);
      */

      int status = 0;
      int32_t res_val = 0;
      // Execute script in the fresh context using JSG_WITHIN_CONTEXT_SCOPE
      // This provides script isolation while maintaining access to global APIs
      /*JSG_WITHIN_CONTEXT_SCOPE(js, scriptContext, [&](jsg::Lock& contextJs) {
        const kj::String script = kj::str(script_);
        auto compiled = jsg::NonModuleScript::compile(contextJs, script, "reprl"_kj);
        try {
          auto result = compiled.runAndReturn(contextJs);
          res_val = jsg::check(v8::Local<v8::Value>(result)->Int32Value(contextJs.v8Context()));
          // if we reach that point execution was successful -> return 0
          res_val = 0;
        } catch(jsg::JsExceptionThrown&) {
          if(try_catch.HasCaught()) {
            res_val = 1;
            auto str = workerd::jsg::check(try_catch.Message()->Get()->ToDetailString(contextJs.v8Context()));
            v8::String::Utf8Value string(contextJs.v8Isolate, str);
            printf("%s\n",*string);
            fflush(stdout);
          }
        }
      });
      */
      const kj::String script = kj::str(script_);
      const kj::String wrapped = kj::str("{",script_,"}");
      auto compiled = jsg::NonModuleScript::compile(js, wrapped, "reprl"_kj);
      try {
        auto result = compiled.runAndReturn(js);
        res_val = jsg::check(v8::Local<v8::Value>(result)->Int32Value(js.v8Context()));
        //fprintf(stderr,"Res val: %d\n",res_val);
        // if we reach that point execution was successful -> return 0
        res_val = 0;
      } catch(jsg::JsExceptionThrown&) {
        if(try_catch.HasCaught()) {
          res_val = 1;
          auto str = workerd::jsg::check(try_catch.Message()->Get()->ToDetailString(js.v8Context()));
          v8::String::Utf8Value string(js.v8Isolate, str);
          if(string.length() > 0) {
            printf("%s\n",*string);
            fflush(stdout); 
          }
        }
      }


      fflush(stdout);
      fflush(stderr);
      status = (res_val & 0xFF) << 8;
      CHECK(write(REPRL_CWFD, &status, 4) == 4);
      __sanitizer_cov_reset_edgeguards();
      free(script_);
      //cleanup context
      

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
  registry.template addBuiltinModule<UnsafeModule>(
      "workerd:unsafe", workerd::jsg::ModuleRegistry::Type::BUILTIN);
  registry.template addBuiltinModule<UnsafeEval>(
      "workerd:unsafe-eval", workerd::jsg::ModuleRegistry::Type::BUILTIN);
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
}  // namespace workerd::api
