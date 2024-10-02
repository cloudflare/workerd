#include <workerd/jsg/setup.h>
#include <workerd/jsg/modules-new.h>
#include <workerd/jsg/modules.capnp.h>
#include "workerd/jsg/observer.h"
#include "workerd/io/observer.h"
#include <workerd/api/unsafe.h>
#include <workerd/api/basics.h>
#include "workerd/jsg/url.h"
#include "workerd/jsg/jsg.h"
#include "workerd/api/pyodide/pyodide.h"
#include <kj/async-io.h>
#include <kj/thread.h>
#include <kj/vector.h>
#include <kj/test.h>
#include <capnp/message.h>

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

/// Test plan: create 2 isolate types one with EW_UNSAFE_ISOLATE_TYPES which implements UnsafeEval which has newWasmModule
/// run newWasmModule and see it fails, run
///    const ac = new AbortController();
///    ac.signal.throwIfAborted();
/// Something will break.
/// Then transfer that isolate to an isolate type with EW_BASICS_ISOLATE_TYPES and watch it succeed
/// ????
/// Profit

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

void expectEval(TestIsolate& isolate,
    kj::StringPtr code,
    kj::StringPtr expectedType,
    kj::StringPtr expectedValue) {
  isolate.runInLockScope([&](typename TestIsolate::Lock& lock) {
    JSG_WITHIN_CONTEXT_SCOPE(lock,
        lock.template newContext<TestContext>().getHandle(lock.v8Isolate),
        [&](jsg::Lock& js) { expectEval(js, code, expectedType, expectedValue); });
  });
}
// KJ_TEST("Attaching APIs at runtime") {
//   capnp::MallocMessageBuilder flagsArena;
//   auto flags = flagsArena.initRoot<::workerd::CompatibilityFlags>();
//   auto flagsReader = flags.asReader();
//   Configuration config(flagsReader);
//   TestIsolate isolate(v8System, config, kj::heap<jsg::IsolateObserver>());
//   isolate.runInLockScope([&](typename TestIsolate::Lock& lock) {
//     JSG_WITHIN_CONTEXT_SCOPE(lock,
//         lock.template newContext<TestContext>().getHandle(lock.v8Isolate), [&](jsg::Lock& js) {
//       expectEval(js, "makeTestApi().test1()", "number", "1");
//       expectEval(js, "makeTestApi().test2()", "throws",
//           "TypeError: makeTestApi(...).test2 is not a function");
//     });
//   });
//   TestIsolate isolate2(v8System, config, kj::heap<jsg::IsolateObserver>());
//   isolate2.runInLockScope([&](typename TestIsolate::Lock& lock) {
//     JSG_WITHIN_CONTEXT_SCOPE(lock,
//         lock.template newContext<TestContext>().getHandle(lock.v8Isolate), [&](jsg::Lock& js) {
//       // Preinitialization
//       expectEval(js, "makeUnsafeEval().eval('1 + 1')", "number", "2");
//       // `flags` is read lazily, so long as the APIs aren't used during preinitialization then
//       // the relevant modules will only be initialized after flags is set correctly.
//       flags.setPythonWorkers(true);
//       expectEval(js, "makeTestApi().test2()", "number", "2");
//       expectEval(js, "makeTestApi().test1()", "throws",
//           "TypeError: makeTestApi(...).test1 is not a function");
//     });
//   });
// }

// Create a global counter in a javascript module in a barebones v8 isolate.
// Adopt the isolate into a jsg isolate.
// Create a context and use that counter.

KJ_TEST("Attaching APIs at runtime") {
  capnp::MallocMessageBuilder flagsArena;
  auto flags = flagsArena.initRoot<::workerd::CompatibilityFlags>();
  auto flagsReader = flags.asReader();
  Configuration config(flagsReader);
  // jsg::IsolateBase isolate(v8System, v8::Isolate::CreateParams{}, kj::heap<jsg::IsolateObserver>());
  // TestIsolate isolate(v8System, config, kj::heap<jsg::IsolateObserver>());
  // isolate.runInLockScope([&](typename TestIsolate::Lock& lock) {
  //   JSG_WITHIN_CONTEXT_SCOPE(lock,
  //       lock.template newContext<TestContext>().getHandle(lock.v8Isolate), [&](jsg::Lock& js) {
  //     expectEval(js, "makeTestApi().test1()", "number", "1");
  //     expectEval(js, "makeTestApi().test2()", "throws",
  //         "TypeError: makeTestApi(...).test2 is not a function");
  //   });
  // });
  // TestIsolate isolate2(v8System, config, kj::heap<jsg::IsolateObserver>());
  // isolate2.runInLockScope([&](typename TestIsolate::Lock& lock) {
  //   JSG_WITHIN_CONTEXT_SCOPE(lock,
  //       lock.template newContext<TestContext>().getHandle(lock.v8Isolate), [&](jsg::Lock& js) {
  //     // Preinitialization
  //     expectEval(js, "makeUnsafeEval().eval('1 + 1')", "number", "2");
  //     // `flags` is read lazily, so long as the APIs aren't used during preinitialization then
  //     // the relevant modules will only be initialized after flags is set correctly.
  //     flags.setPythonWorkers(true);
  //     expectEval(js, "makeTestApi().test2()", "number", "2");
  //     expectEval(js, "makeTestApi().test1()", "throws",
  //         "TypeError: makeTestApi(...).test1 is not a function");
  //   });
  // });
}

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
void doTest(SimpleTestIsolate::Lock& lock) {
  auto value1 =
      ModuleRegistry::resolve(lock, "foo:bar", "counter"_kjc, ResolveContext::Type::BUILTIN_ONLY);
  auto& wrapper = SimpleTestIsolate_TypeWrapper::from(lock.v8Isolate);
  auto fn = KJ_ASSERT_NONNULL(
      wrapper.tryUnwrap(lock.v8Context(), value1, (CounterObject*)nullptr, kj::none));
  KJ_ASSERT(fn.getId(lock) == 1);
  KJ_ASSERT(fn.getId(lock) == 2);
  KJ_ASSERT(fn.getId(lock) == 3);
  // auto& info = JSG_REQUIRE_NONNULL(maybe, Error, kj::str("No such module: ", specifier));
  // auto value1 = resolveFromRegistry(js, "foo:bar");
  // KJ_ASSERT(value1.isNumber());
  // KJ_ASSERT(value1.strictEquals(lock.num(123)));
  // auto value2 = ModuleRegistry::resolve(
  //     js, "pyodide-internal:emscriptenSetup", "f"_kjc, ResolveContext::Type::BUILTIN);
  // KJ_ASSERT(value1.isObject());
  // jsg::Identified<EventTarget::Handler> identified = {.identity = {js.v8Isolate, fn},
  //   .unwrapped = JSG_REQUIRE_NONNULL(handler.tryUnwrap(js, fn.As<v8::Value>()), TypeError,
  //       "Unable to create AbortSignal.any handler")};
  // auto& wrapper = TestIsolate_TypeWrapper::from(lock.v8Isolate);
  // auto fn = KJ_ASSERT_NONNULL(wrapper.tryUnwrap(
  //     lock.v8Context(), value1, (jsg::Function<int()>*)nullptr, kj::none));
}
KJ_TEST("??? 2") {
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
  TestIsolate newIsolate(kj::mv(isolate), config);
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

// lock.tryCatch([&] {
//   doTest(lock);
// }, [&](Value exception) {
//   auto str = kj::str(exception.getHandle(lock));
//   KJ_FAIL_ASSERT(str);
//  });

// void setupIsolate(TestIsolate& isolate, auto flags, auto source, auto func) {
//   isolate.runInLockScope([&](TestIsolate::Lock& lock) {
//     v8::Local<v8::Context> context = lock.newContext<TestContext>().getHandle(lock);
//     v8::Context::Scope contextScope(context);
//     // context->SetAlignedPointerInEmbedderData(2, nullptr);
//     auto observer = isolate.getObserver();

//     auto registry = jsg::ModuleRegistryImpl<TestIsolate_TypeWrapper>::from(lock);
//     auto path1 = kj::Path::parse("main");
//     registry->add(path1,
//         jsg::ModuleRegistry::ModuleInfo(
//             lock, "main", source, jsg::ModuleInfoCompileOption::BUNDLE, observer));
//     auto path2 = kj::Path::parse("pyodide");
//     registry->add(path2,
//         jsg::ModuleRegistry::ModuleInfo(lock, "pyodide", R"script(
//   import { default as UnsafeEval } from 'internal:unsafe-eval';

//   function test1() {
//     return UnsafeEval.eval('1 + 1');
//   }
//   export const value = test1();
//         )script"_kjc,
//             jsg::ModuleInfoCompileOption::BUILTIN, observer));
//     api::registerUnsafeModules(*registry, flags);
//     func(lock);
//     // jsg::ResolveObserver observer;
//     // jsg::CompilationObserver compilationObserver;
//     // auto registry =
//     //     jsg::modules::ModuleRegistry::Builder(observer)
//     //         .add(getExternalTestApiModuleBundle<TestIsolate_TypeWrapper>(flags))
//     //         .add(workerd::api::getExternalUnsafeModuleBundle<TestIsolate_TypeWrapper>(flags))
//     //         .finish();

//     // auto attached = registry->attachToIsolate(lock, compilationObserver);
//   });
// }
// KJ_TEST("Attaching APIs at runtime2") {
//   capnp::MallocMessageBuilder flagsArena;
//   auto flags = flagsArena.initRoot<::workerd::CompatibilityFlags>();
//   auto flagsReader = flags.asReader();
//   Configuration config(flagsReader);
//   TestIsolate isolate(v8System, config, kj::heap<jsg::IsolateObserver>());
//   const auto foo = "foo:bar"_url;
//   auto source = kj::str(R"script(
//   import { value } from 'pyodide:pyodide';

//   export function test1() {
//     return value;
//   })script"_kjc);
//   setupIsolate(isolate, flags, source.asArray(), [&](TestIsolate::Lock& lock) {
//     // expectEval((jsg::Lock&)lock, "UnsafeEval.eval('1 + 1')", "number", "2");
//     auto moduleRegistry = jsg::ModuleRegistry::from(lock);
//     auto path1 = kj::Path::parse("main");
//     auto path2 = kj::Path::parse("test1");
//     auto test1 = moduleRegistry->resolve(
//         lock, path1, path2, jsg::ModuleRegistry::ResolveOption::DEFAULT);
//     KJ_ASSERT(test1.isFunction());
//     // auto& wrapper = TestIsolate_TypeWrapper::from(lock.v8Isolate);
//     // auto fn = KJ_ASSERT_NONNULL(
//     //     wrapper.tryUnwrap(lock.v8Context(), test1, (jsg::Function<int()>*)nullptr, kj::none));
//     // KJ_ASSERT(fn(lock) == 2);
//   });
// }
// expectEval(isolate, "function f() { return true; }; f()", "boolean", "true");
// expectEval(isolate,
//     "import { default as UnsafeEval } from 'workerd:unsafe-eval'; UnsafeEval.eval('1 + 1')",
//     "number", "2");

// expectEval(isolate, "test()", "boolean", "true");
// expectEval<api::UnsafeEval>(isolate, "const fn = newFunction('return m', 'foo', 'm'); fn(1)", "number", "1");
//   const fn = env.unsafe.newFunction('return m', 'foo', 'm');
// console.log(fn(1));  // prints 1
// jsg::runInV8Stack([&](auto& stackScope) {
//   TestIsolate::Lock js(isolate, stackScope);
//   js.withinHandleScope([&] {
//     v8::Local<v8::Context> context = js.newContext<TestContext>().getHandle(js);
//     v8::Context::Scope contextScope(context);
//     context->SetAlignedPointerInEmbedderData(2, nullptr);
//     jsg::modules::ModuleBundle::BundleBuilder builder;

//     kj::String source = kj::str("export const bar = 123;");
//     builder.addEsmModule("foo", source.releaseArray(), jsg::modules::Module::Flags::MAIN);
//     jsg::ResolveObserver resolveObserver;
//     auto registry =
//         jsg::modules::ModuleRegistry::Builder(resolveObserver).add(builder.finish()).finish();
//     jsg::CompilationObserver compilationObserver;
//     auto attached = registry->attachToIsolate(js, compilationObserver);

//     const auto specifier = "file:///foo"_url;
//     js.tryCatch([&] {
//       // The built-in only foo:bar module should be found when using a built-in context
//       auto value1 = jsg::modules::ModuleRegistry::resolve(
//           js, "foo", "bar"_kjc, jsg::modules::ResolveContext::Type::BUNDLE);

//       KJ_ASSERT(value1.isNumber());
//       KJ_ASSERT(value1.strictEquals(js.num(123)));
//     }, [&](jsg::Value exception) {
//       auto str = kj::str(exception.getHandle(js));
//       KJ_ASSERT(str == "!Error: Module not found: foo:bar");
//     });
//     // jsg::modules::ResolveContext context = {
//     //   .type = jsg::modules::ResolveContext::Type::BUNDLE,
//     //   .source = jsg::modules::ResolveContext::Source::OTHER,
//     //   .specifier = specifier,
//     //   .referrer = jsg::modules::ModuleBundle::BundleBuilder::BASE,
//     // };
//   });
// });

// void expectEval(TestIsolate& isolate,
//     kj::StringPtr code,
//     kj::StringPtr expectedType,
//     kj::StringPtr expectedValue) {
//   isolate.runInLockScope([&](typename TestIsolate::Lock& lock) {
//     JSG_WITHIN_CONTEXT_SCOPE(lock,
//         lock.template newContext<TestContext>().getHandle(lock.v8Isolate), [&](jsg::Lock& js) {
//       // Create a string containing the JavaScript source code.
//       v8::Local<v8::String> source = jsg::v8Str(js.v8Isolate, code);

//       // Compile the source code.
//       v8::Local<v8::Script> script;
//       if (!v8::Script::Compile(js.v8Context(), source).ToLocal(&script)) {
//         KJ_FAIL_EXPECT("code didn't parse", code);
//         return;
//       }

//       v8::TryCatch catcher(js.v8Isolate);

//       // Run the script to get the result.
//       v8::Local<v8::Value> result;
//       if (script->Run(js.v8Context()).ToLocal(&result)) {
//         v8::String::Utf8Value type(js.v8Isolate, result->TypeOf(js.v8Isolate));
//         v8::String::Utf8Value value(js.v8Isolate, result);

//         KJ_EXPECT(*type == expectedType, *type, expectedType);
//         KJ_EXPECT(*value == expectedValue, *value, expectedValue);
//       } else if (catcher.HasCaught()) {
//         v8::String::Utf8Value message(js.v8Isolate, catcher.Exception());

//         KJ_EXPECT(expectedType == "throws", expectedType, catcher.Exception());
//         KJ_EXPECT(*message == expectedValue, *message, expectedValue);
//       } else {
//         KJ_FAIL_EXPECT("returned empty handle but didn't throw exception?");
//       }
//     });
//   });
// }
// #define PREAMBLE(fn)                                                                               \
//   TestIsolate isolate(v8System, 123, kj::heap<jsg::IsolateObserver>());                            \
//   jsg::runInV8Stack([&](auto& stackScope) {                                                        \
//     TestIsolate::Lock lock(isolate, stackScope);                                                   \
//     lock.withinHandleScope([&] {                                                                   \
//       v8::Local<v8::Context> context = lock.newContext<TestContext>().getHandle(lock);             \
//       v8::Context::Scope contextScope(context);                                                    \
//       context->SetAlignedPointerInEmbedderData(2, nullptr);                                        \
//       fn(lock);                                                                                    \
//     });                                                                                            \
//   });

// KJ_TEST("Using a deferred eval callback works") {
//   PREAMBLE(([&](Lock& js) {
//     ModuleBundle::BundleBuilder builder;

//     kj::String source = kj::str("export const bar = 123;");
//     builder.addEsmModule("foo", source.releaseArray(), Module::Flags::MAIN);
//     ResolveObserver resolveObserver;
//     auto registry = ModuleRegistry::Builder(resolveObserver).add(builder.finish()).finish();
//     CompilationObserver compilationObserver;
//     auto attached = registry->attachToIsolate(js, compilationObserver);

//     const auto specifier = "file:///foo"_url;

//     ResolveContext context = {
//       .type = ResolveContext::Type::BUNDLE,
//       .source = ResolveContext::Source::OTHER,
//       .specifier = specifier,
//       .referrer = ModuleBundle::BundleBuilder::BASE,
//     };

//     js.tryCatch([&] {
//       // auto value1 = ModuleRegistry::resolve(js, "file:///foo", "bar"_kjc,
//       //                                         ResolveContext::Type::BUNDLE);
//       // KJ_LOG(WARNING, value1.isNumber());
//     }, [&](Value exception) { js.throwException(kj::mv(exception)); });
//   }));
// }

// KJ_TEST("???") {
//   PREAMBLE([&](Lock& js) {
//     ResolveObserver observer;
//     CompilationObserver compilationObserver;

//     ModuleBundle::BundleBuilder builder;
//     const auto foo = "foo:bar"_url;

//     auto source = "export default 123;"_kjc;
//     builder.addEsmModule("foo", source.slice(0, source.size()).attach(kj::mv(source)));

//     auto registry = ModuleRegistry::Builder(observer).add(builder.finish()).finish();

//     auto attached = registry->attachToIsolate(js, compilationObserver);

//     js.tryCatch([&] {
//       // The built-in only foo:bar module should be found when using a built-in context
//       auto value1 =
//           ModuleRegistry::resolve(js, "file:///foo", "default"_kjc, ResolveContext::Type::BUNDLE);

//       KJ_ASSERT(value1.isNumber());
//       KJ_ASSERT(value1.strictEquals(js.num(123)));
//     }, [&](Value exception) {
//       auto str = kj::str(exception.getHandle(js));
//       KJ_ASSERT(str == "!Error: Module not found: foo:bar");
//     });
//   });
// }

// typedef void SetupEmscriptenModule();

// void setRes(jsg::JsRef<jsg::JsValue>& res, jsg::JsRef<jsg::JsValue> v) {
//   res = kj::mv(v);
// }
// void doTest(jsg::Lock& js) {
//     // The built-in only foo:bar module should be found when using a built-in context
//     auto value1 =
//         jsg::modules::ModuleRegistry::resolve(js, "file:///foo", "default"_kjc, jsg::modules::ResolveContext::Type::BUNDLE);

//     KJ_ASSERT(value1.isNumber());
//     KJ_ASSERT(value1.strictEquals(js.num(123)));

//     auto setupEmscriptenModule = jsg::modules::ModuleRegistry::resolve(
//         js, "pyodide-internal:emscriptenSetup", "f"_kjc, jsg::modules::ResolveContext::Type::BUILTIN);
//     KJ_ASSERT(setupEmscriptenModule.isFunction());
//     auto& wrapper = TestIsolate_TypeWrapper::from(js.v8Isolate);
//     auto fn = KJ_ASSERT_NONNULL(wrapper.tryUnwrap(
//         js.v8Context(), setupEmscriptenModule, (jsg::Function<jsg::Promise<jsg::JsRef<jsg::JsValue>>(kj::String)>*)nullptr, kj::none));
//     jsg::JsRef<jsg::JsValue> res;
//     auto fnrun = fn(js, kj::str("test")).then(js, [&](jsg::Lock&, jsg::JsRef<jsg::JsValue> v) { setRes(res, kj::mv(v)); });
//     auto io = kj::setupAsyncIo();
//     auto promise = IoContext::current().awaitJs(js, kj::mv(fnrun));
//     promise.wait(io.waitScope);
//     int a = 5;
//     a = 6;
//     int b = a + 1;
// }

// KJ_TEST("??? 2") {
//   PREAMBLE([&](jsg::Lock& js) {
//     jsg::ResolveObserver observer;
//     jsg::CompilationObserver compilationObserver;

//     jsg::modules::ModuleBundle::BundleBuilder builder;
//     const auto foo = "foo:bar"_url;

//     auto source = "export default 123;"_kjc;
//     builder.addEsmModule("foo", source.slice(0, source.size()).attach(kj::mv(source)));
//     jsg::modules::ModuleBundle::BuiltinBuilder builtinBuilder(jsg::modules::ModuleBundle::BuiltinBuilder::Type::BUILTIN_ONLY);
//     jsg::modules::ModuleBundle::getBuiltInBundleFromCapnp(builtinBuilder, PYODIDE_BUNDLE);
//     capnp::MallocMessageBuilder flagsArena;
//     auto flags = flagsArena.initRoot<::workerd::CompatibilityFlags>();
//     auto registry =
//         jsg::modules::ModuleRegistry::Builder(observer)
//             .add(builder.finish())
//             .add(builtinBuilder.finish())
//             .add(workerd::api::getInternalUnsafeModuleBundle<TestIsolate_TypeWrapper>(flags))
//             .finish();

//     auto attached = registry->attachToIsolate(js, compilationObserver);

//     js.tryCatch([&] {
//       doTest(js);
//       // res.map(Func &&f)
//     }, [&](jsg::Value exception) {
//       auto str = kj::str(exception.getHandle(js));
//       KJ_ASSERT(str == "!Error: Module not found: foo:bar");
//     });
//   });
// }

// ModuleBundle::BundleBuilder builder;
// ResolveObserverImpl observer;

// auto foo = kj::str("export default 1;");
// builder.addEsmModule("foo", foo.slice(0, foo.size()).attach(kj::mv(foo)));

// bool called = false;
// auto registry = ModuleRegistry::Builder(observer).add(builder.finish()).finish();

// PREAMBLE([&](Lock& js) {
//   auto attached = registry->attachToIsolate(js);

//   js.tryCatch([&] {
//     ModuleRegistry::resolve(js, "foo", "default"_kjc);
//     KJ_ASSERT(false);
//   }, [&](auto exception) {});

// We don't care about the specific exception above. We only want to know that
// the eval callback was invoked.
// KJ_ASSERT(called);
// });

// KJ_TEST("??? 2") {
//   PREAMBLE([&](Lock& js) {
//     ResolveObserver observer;
//     CompilationObserver compilationObserver;

//     ModuleBundle::BundleBuilder builder;
//     const auto foo = "foo:bar"_url;

//     auto source = "export default 123;"_kjc;
//     builder.addEsmModule("foo", source.slice(0, source.size()).attach(kj::mv(source)));
//     ModuleBundle::BuiltinBuilder builtinBuilder(ModuleBundle::BuiltinBuilder::Type::BUILTIN_ONLY);
//     ModuleBundle::getBuiltInBundleFromCapnp(builtinBuilder, PYODIDE_BUNDLE);
//     capnp::MallocMessageBuilder flagsArena;
//     auto flags = flagsArena.initRoot<::workerd::CompatibilityFlags>();
//     auto registry =
//         ModuleRegistry::Builder(observer)
//             .add(builder.finish())
//             .add(builtinBuilder.finish())
//             .add(workerd::api::getInternalUnsafeModuleBundle<TestIsolate_TypeWrapper>(flags))
//             .finish();

//     auto attached = registry->attachToIsolate(js, compilationObserver);

//     js.tryCatch([&] {
//       // The built-in only foo:bar module should be found when using a built-in context
//       auto value1 =
//           ModuleRegistry::resolve(js, "file:///foo", "default"_kjc, ResolveContext::Type::BUNDLE);

//       KJ_ASSERT(value1.isNumber());
//       KJ_ASSERT(value1.strictEquals(js.num(123)));

//       auto value2 = ModuleRegistry::resolve(
//           js, "pyodide-internal:emscriptenSetup", "f"_kjc, ResolveContext::Type::BUILTIN);
//       KJ_ASSERT(value2.isFunction());
//       auto& wrapper = TestIsolate_TypeWrapper::from(js.v8Isolate);
//       auto fn = KJ_ASSERT_NONNULL(wrapper.tryUnwrap(
//           js.v8Context(), value2, (jsg::Function<kj::String(kj::String)>*)nullptr, kj::none));
//       KJ_ASSERT(fn(js, kj::str("test")) == "test"_kj);
//     }, [&](Value exception) {
//       auto str = kj::str(exception.getHandle(js));
//       KJ_ASSERT(str == "!Error: Module not found: foo:bar");
//     });
//   });
// }

// ModuleBundle::BundleBuilder builder;
// ResolveObserverImpl observer;

// auto foo = kj::str("export default 1;");
// builder.addEsmModule("foo", foo.slice(0, foo.size()).attach(kj::mv(foo)));

// bool called = false;
// auto registry = ModuleRegistry::Builder(observer).add(builder.finish()).finish();

// PREAMBLE([&](Lock& js) {
//   auto attached = registry->attachToIsolate(js);

//   js.tryCatch([&] {
//     ModuleRegistry::resolve(js, "foo", "default"_kjc);
//     KJ_ASSERT(false);
//   }, [&](auto exception) {});

// We don't care about the specific exception above. We only want to know that
// the eval callback was invoked.
// KJ_ASSERT(called);
// });
