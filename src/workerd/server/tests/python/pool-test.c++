#include "workerd/api/pyodide/pyodide.h"
#include "workerd/io/observer.h"
#include "workerd/jsg/jsg.h"
#include "workerd/jsg/observer.h"
#include "workerd/jsg/url.h"

#include <workerd/api/basics.h>
#include <workerd/api/unsafe.h>
#include <workerd/jsg/modules-new.h>
#include <workerd/jsg/modules.capnp.h>
#include <workerd/jsg/setup.h>

#include <capnp/message.h>
#include <kj/async-io.h>
#include <kj/test.h>
#include <kj/thread.h>
#include <kj/vector.h>

using namespace workerd;
using jsg::CompilationObserver;
using jsg::ContextGlobal;
using jsg::IsolateObserver;
using jsg::Lock;
using jsg::Object;
using jsg::ResolveObserver;
using jsg::runInV8Stack;
using jsg::Url;
using jsg::V8System;
using jsg::Value;
using jsg::modules::Module;
using jsg::modules::ModuleBundle;
using jsg::modules::ModuleRegistry;
using jsg::modules::ResolveContext;

jsg::V8System v8System;

struct TestApi: public jsg::Object {
  TestApi() = default;
  TestApi(jsg::Lock&, const jsg::Url&) {}
  int test1(jsg::Lock& js) {
    return 1;
  }

  int test2(jsg::Lock& js) {
    return 2;
  }

  JSG_RESOURCE_TYPE(TestApi, CompatibilityFlags::Reader flags) {
    if (flags.getPythonWorkers()) {
      JSG_METHOD(test2);
    } else {
      JSG_METHOD(test1);
    }
  }
};

struct TestContext: public jsg::Object, public jsg::ContextGlobal {
  jsg::Ref<api::UnsafeEval> makeUnsafeEval() {
    return jsg::alloc<api::UnsafeEval>();
  }
  jsg::Ref<TestApi> makeTestApi() {
    return jsg::alloc<TestApi>();
  }

  JSG_RESOURCE_TYPE(TestContext, CompatibilityFlags::Reader flags) {
    if (flags.getPythonWorkers()) {
      JSG_METHOD(makeUnsafeEval);
    }
    JSG_METHOD(makeTestApi);
  }
};

struct CounterObject {
  jsg::Function<int()> getId;
  JSG_STRUCT(getId);
};

struct InstantiateEmscriptenMod {
  jsg::Function<jsg::Promise<void>(jsg::JsValue, jsg::JsValue, kj::ArrayPtr<byte>, kj::ArrayPtr<byte>)> instantiateEmscriptenModule;
  jsg::JsValue setGetRandomValues;
  jsg::JsValue setUnsafeEval;
  JSG_STRUCT(instantiateEmscriptenModule, setGetRandomValues, setUnsafeEval);
};

// struct SimpleTestContext: public jsg::Object, public jsg::ContextGlobal {
//   JSG_RESOURCE_TYPE(SimpleTestContext) {}
// };

JSG_DECLARE_ISOLATE_TYPE(
    TestIsolate, TestContext, TestApi, InstantiateEmscriptenMod, EW_UNSAFE_ISOLATE_TYPES);
// JSG_DECLARE_ISOLATE_TYPE(SimpleTestIsolate, SimpleTestContext, CounterObject);

class Configuration {
public:
  Configuration(CompatibilityFlags::Reader& flags): flags(flags) {}
  operator const CompatibilityFlags::Reader() const {
    return flags;
  }

private:
  CompatibilityFlags::Reader& flags;
};

void expectEval(
    jsg::Lock& js, kj::StringPtr code, kj::StringPtr expectedType, kj::StringPtr expectedValue) {
  // Create a string containing the JavaScript source code.
  v8::Local<v8::String> source = jsg::v8Str(js.v8Isolate, code);

  // Compile the source code.
  v8::Local<v8::Script> script;
  if (!v8::Script::Compile(js.v8Context(), source).ToLocal(&script)) {
    KJ_FAIL_EXPECT("code didn't parse", code);
    return;
  }

  v8::TryCatch catcher(js.v8Isolate);

  // Run the script to get the result.
  v8::Local<v8::Value> result;
  if (script->Run(js.v8Context()).ToLocal(&result)) {
    v8::String::Utf8Value type(js.v8Isolate, result->TypeOf(js.v8Isolate));
    v8::String::Utf8Value value(js.v8Isolate, result);

    KJ_EXPECT(*type == expectedType, *type, expectedType);
    KJ_EXPECT(*value == expectedValue, *value, expectedValue);
  } else if (catcher.HasCaught()) {
    v8::String::Utf8Value message(js.v8Isolate, catcher.Exception());

    KJ_EXPECT(expectedType == "throws", expectedType, catcher.Exception());
    KJ_EXPECT(*message == expectedValue, *message, expectedValue);
  } else {
    KJ_FAIL_EXPECT("returned empty handle but didn't throw exception?");
  }
}

v8::MaybeLocal<v8::Module> resolveCallback(v8::Local<v8::Context> context,
    v8::Local<v8::String> specifier,
    v8::Local<v8::FixedArray> import_assertions,
    v8::Local<v8::Module> referrer) {
  KJ_DBG("resolveCallback");
  KJ_FAIL_REQUIRE("oops");
}

// This test passes and surprisingly enough shows that the counter object can be reused in a new
// context even after it's context has gone out of scope. This point isn't very critical as we could
// create counter in a context and save counter and that context in scope then do the rest of the
// worker's initialization in a subcontext as shown in https://v8.dev/docs/embed#contexts and use
// a reference to the counter class in that subcontext.
KJ_TEST("Reuse an object created from another context 2") {
  auto modules = PYODIDE_BUNDLE->getModules();
  kj::ArrayPtr<const char> code;
  for (auto mod: modules) {
    if (mod.getName() == "pyodide-internal:generated/emscriptenSetup") {
      code = mod.getSrc().asChars();
    }
  }

  capnp::MallocMessageBuilder flagsArena;
  auto flags = flagsArena.initRoot<::workerd::CompatibilityFlags>();
  auto flagsReader = flags.asReader();
  Configuration config(flagsReader);
  auto observer = kj::atomicRefcounted<workerd::IsolateObserver>();
  TestIsolate isolate(v8System, config, kj::atomicAddRef(*observer));
  isolate.runInLockScope([&](TestIsolate::Lock& lock) {
    lock.withinHandleScope([&]() -> auto {
      v8::Local<v8::Context> ctx = v8::Context::New(lock.v8Isolate);
      KJ_ASSERT(!ctx.IsEmpty(), "unable to enter invalid v8::Context");
      v8::Context::Scope scope(ctx);

      v8::ScriptOrigin origin(lock.str("blah"_kj), 0, 0, false, -1, {}, false, false, true);
      v8::ScriptCompiler::Source source(lock.str(code), origin);
      v8::Local<v8::Module> mod;
      if (!v8::ScriptCompiler::CompileModule(lock.v8Isolate, &source).ToLocal(&mod)) {
        KJ_FAIL_EXPECT("code didn't parse", code);
        return;
      }
      KJ_LOG(DBG, "code did parse");
      bool result;
      if (!mod->InstantiateModule(lock.v8Context(), resolveCallback).To(&result)) {
        KJ_DBG("Failed");
      }
      if (!result) {
        KJ_DBG("Result is false?");
        // lock.v8Isolate->ThrowError(
        //     lock.str(kj::str("Failed to instantiate module: ")));
        return;
      }

      v8::Local<v8::Value> res;
      auto io = kj::setupAsyncIo();
      if (mod->Evaluate(lock.v8Context()).ToLocal(&res)) {
        kj::String desc = kj::str(jsg::check(res->ToString(lock.v8Context())));
        auto& wrapper = TestIsolate_TypeWrapper::from(lock.v8Isolate);
        kj::Maybe<jsg::Promise<InstantiateEmscriptenMod>> p = wrapper.tryUnwrap(
            lock.v8Context(), res, (jsg::Promise<InstantiateEmscriptenMod>*)nullptr, kj::none);
        KJ_DBG("Okay??");
      }
    });
  });
}
