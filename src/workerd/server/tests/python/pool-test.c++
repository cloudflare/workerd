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

  JSG_RESOURCE_TYPE(TestContext) {
    JSG_METHOD(makeUnsafeEval);
    JSG_METHOD(makeTestApi);
  }
};

struct CounterObject {
  jsg::Function<int()> getId;
  JSG_STRUCT(getId);
};
struct SimpleTestContext: public jsg::Object, public jsg::ContextGlobal {
  JSG_RESOURCE_TYPE(SimpleTestContext) {}
};

JSG_DECLARE_ISOLATE_TYPE(TestIsolate, TestContext, TestApi, CounterObject, EW_UNSAFE_ISOLATE_TYPES);
JSG_DECLARE_ISOLATE_TYPE(SimpleTestIsolate, SimpleTestContext, CounterObject);

class Configuration {
public:
  Configuration(CompatibilityFlags::Reader& flags): flags(flags) {}
  operator const CompatibilityFlags::Reader() const {
    return flags;
  }

private:
  CompatibilityFlags::Reader& flags;
};

// This just creates a bundle with code that has our Counter class, essentially this should signify
// that we can create a class and hold a reference to it.
kj::Own<ModuleRegistry> initializeBundleModuleRegistry(const jsg::ResolveObserver& observer) {
  ModuleRegistry::Builder builder(observer, ModuleRegistry::Builder::Options::ALLOW_FALLBACK);
  ModuleBundle::BuiltinBuilder builtinBuilder(ModuleBundle::BuiltinBuilder::Type::BUILTIN_ONLY);
  auto source = R"script(
class Counter {
  constructor() {
    this._counter = 0;
    console.log(this._counter);
  }

  getId() {
    let val = ++this._counter;
    console.log(val);
    return val;
  }
}
export let counter = new Counter();
  )script"_kjc;
  const auto specifier = "foo:bar"_url;
  builtinBuilder.addEsm(specifier, source.slice(0, source.size()).attach(kj::mv(source)));
  builder.add(builtinBuilder.finish());

  return builder.finish();
}

// This test passes and surprisingly enough shows that the counter object can be reused in a new
// context even after it's context has gone out of scope. This point isn't very critical as we could
// create counter in a context and save counter and that context in scope then do the rest of the
// worker's initialization in a subcontext as shown in https://v8.dev/docs/embed#contexts and use
// a reference to the counter class in that subcontext.
KJ_TEST("Reuse an object created from another context 2") {
  auto observer = kj::atomicRefcounted<workerd::IsolateObserver>();
  auto registry = initializeBundleModuleRegistry(*observer);
  jsg::NewContextOptions options{.newModuleRegistry = *registry};

  SimpleTestIsolate isolate(v8System, kj::atomicAddRef(*observer));
  kj::Maybe<CounterObject> counter;
  isolate.runInLockScope([&](SimpleTestIsolate::Lock& lock) {
    lock.withinHandleScope([&]() -> auto {
      jsg::JsContext<SimpleTestContext> context = lock.newContext<SimpleTestContext>(options);
      v8::Local<v8::Context> ctx = context.getHandle(lock);
      KJ_ASSERT(!ctx.IsEmpty(), "unable to enter invalid v8::Context");
      v8::Context::Scope scope(ctx);
      auto value1 = ModuleRegistry::resolve(
          lock, "foo:bar", "counter"_kjc, ResolveContext::Type::BUILTIN_ONLY);
      auto& wrapper = SimpleTestIsolate_TypeWrapper::from(lock.v8Isolate);
      counter = wrapper.tryUnwrap(lock.v8Context(), value1, (CounterObject*)nullptr, kj::none);

      auto& localcounter = KJ_ASSERT_NONNULL(counter);
      KJ_ASSERT(localcounter.getId(lock) == 1);
      KJ_ASSERT(localcounter.getId(lock) == 2);
      KJ_ASSERT(localcounter.getId(lock) == 3);
    });
  });
  isolate.runInLockScope([&](SimpleTestIsolate::Lock& lock) {
    lock.withinHandleScope([&]() -> auto {
      for (int i = 4; i < 30; ++i) {
        jsg::JsContext<SimpleTestContext> context = lock.newContext<SimpleTestContext>(options);
        v8::Local<v8::Context> ctx = context.getHandle(lock);
        KJ_ASSERT(!ctx.IsEmpty(), "unable to enter invalid v8::Context");
        v8::Context::Scope scope(ctx);

        auto& localcounter = KJ_ASSERT_NONNULL(counter);
        KJ_ASSERT(localcounter.getId(lock) == i);
      }
    });
  });
}

// This test shows some idea for an implementation where we first create a Simple Isolate Type
// with only the api types that we require for emscripten initialization, do the setup then later
// "move" that isolate into a more elaborate isolate type and use that for the rest of the code flow.
KJ_TEST("??? 3") {
  auto observer = kj::atomicRefcounted<workerd::IsolateObserver>();
  auto registry = initializeBundleModuleRegistry(*observer);
  jsg::NewContextOptions options{.newModuleRegistry = *registry};

  SimpleTestIsolate isolate(v8System, kj::atomicAddRef(*observer));
  kj::Maybe<CounterObject> counter;
  isolate.runInLockScope([&](SimpleTestIsolate::Lock& lock) {
    lock.withinHandleScope([&]() -> auto {
      jsg::JsContext<SimpleTestContext> context = lock.newContext<SimpleTestContext>(options);
      v8::Local<v8::Context> ctx = context.getHandle(lock);
      KJ_ASSERT(!ctx.IsEmpty(), "unable to enter invalid v8::Context");
      v8::Context::Scope scope(ctx);
      auto value1 = ModuleRegistry::resolve(
          lock, "foo:bar", "counter"_kjc, ResolveContext::Type::BUILTIN_ONLY);
      auto& wrapper = SimpleTestIsolate_TypeWrapper::from(lock.v8Isolate);
      counter = wrapper.tryUnwrap(lock.v8Context(), value1, (CounterObject*)nullptr, kj::none);

      auto& localcounter = KJ_ASSERT_NONNULL(counter);
      KJ_ASSERT(localcounter.getId(lock) == 1);
      KJ_ASSERT(localcounter.getId(lock) == 2);
      KJ_ASSERT(localcounter.getId(lock) == 3);
    });
  });
  capnp::MallocMessageBuilder flagsArena;
  auto flags = flagsArena.initRoot<::workerd::CompatibilityFlags>();
  auto flagsReader = flags.asReader();
  Configuration config(flagsReader);
  // TestIsolate newIsolate(kj::mv(isolate), config); // This syntax isn't implemented yet obviously.
  // isolate.runInLockScope([&](SimpleTestIsolate::Lock& lock) {
  //   lock.withinHandleScope([&]() -> auto {
  //     for (int i = 4; i < 30; ++i) {
  //       jsg::JsContext<SimpleTestContext> context = lock.newContext<SimpleTestContext>(options);
  //       v8::Local<v8::Context> ctx = context.getHandle(lock);
  //       KJ_ASSERT(!ctx.IsEmpty(), "unable to enter invalid v8::Context");
  //       v8::Context::Scope scope(ctx);

  //       auto& localcounter = KJ_ASSERT_NONNULL(counter);
  //       KJ_ASSERT(localcounter.getId(lock) == i);
  //     }
  //   });
  // });
}
