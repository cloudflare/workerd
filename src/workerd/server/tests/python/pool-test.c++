#include <workerd/jsg/setup.h>
#include <workerd/jsg/modules-new.h>
#include <workerd/jsg/modules.capnp.h>
#include "workerd/jsg/observer.h"
#include <workerd/api/unsafe.h>
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

V8System v8System;

struct TestContext: public Object, public ContextGlobal {
  JSG_RESOURCE_TYPE(TestContext) {}
};
JSG_DECLARE_ISOLATE_TYPE(TestIsolate, TestContext, EW_UNSAFE_ISOLATE_TYPES);

#define PREAMBLE(fn)                                                                               \
  TestIsolate isolate(v8System, 123, kj::heap<jsg::IsolateObserver>());                            \
  runInV8Stack([&](auto& stackScope) {                                                             \
    TestIsolate::Lock lock(isolate, stackScope);                                                   \
    lock.withinHandleScope([&] {                                                                   \
      v8::Local<v8::Context> context = lock.newContext<TestContext>().getHandle(lock);             \
      v8::Context::Scope contextScope(context);                                                    \
      context->SetAlignedPointerInEmbedderData(2, nullptr);                                        \
      fn(lock);                                                                                    \
    });                                                                                            \
  });

KJ_TEST("Using a deferred eval callback works") {
  PREAMBLE(([&](Lock& js) {
    ModuleBundle::BundleBuilder builder;

    kj::String source = kj::str("export const bar = 123;");
    builder.addEsmModule("foo", source.releaseArray(), Module::Flags::MAIN);
    ResolveObserver resolveObserver;
    auto registry = ModuleRegistry::Builder(resolveObserver).add(builder.finish()).finish();
    CompilationObserver compilationObserver;
    auto attached = registry->attachToIsolate(js, compilationObserver);

    const auto specifier = "file:///foo"_url;

    ResolveContext context = {
      .type = ResolveContext::Type::BUNDLE,
      .source = ResolveContext::Source::OTHER,
      .specifier = specifier,
      .referrer = ModuleBundle::BundleBuilder::BASE,
    };

    js.tryCatch([&] {
      // auto value1 = ModuleRegistry::resolve(js, "file:///foo", "bar"_kjc,
      //                                         ResolveContext::Type::BUNDLE);
      // KJ_LOG(WARNING, value1.isNumber());
    }, [&](Value exception) { js.throwException(kj::mv(exception)); });
  }));
}

KJ_TEST("???") {
  PREAMBLE([&](Lock& js) {
    ResolveObserver observer;
    CompilationObserver compilationObserver;

    ModuleBundle::BundleBuilder builder;
    const auto foo = "foo:bar"_url;

    auto source = "export default 123;"_kjc;
    builder.addEsmModule("foo", source.slice(0, source.size()).attach(kj::mv(source)));

    auto registry = ModuleRegistry::Builder(observer).add(builder.finish()).finish();

    auto attached = registry->attachToIsolate(js, compilationObserver);

    js.tryCatch([&] {
      // The built-in only foo:bar module should be found when using a built-in context
      auto value1 =
          ModuleRegistry::resolve(js, "file:///foo", "default"_kjc, ResolveContext::Type::BUNDLE);

      KJ_ASSERT(value1.isNumber());
      KJ_ASSERT(value1.strictEquals(js.num(123)));
    }, [&](Value exception) {
      auto str = kj::str(exception.getHandle(js));
      KJ_ASSERT(str == "!Error: Module not found: foo:bar");
    });
  });
}

typedef void SetupEmscriptenModule();

KJ_TEST("??? 2") {
  PREAMBLE([&](Lock& js) {
    ResolveObserver observer;
    CompilationObserver compilationObserver;

    ModuleBundle::BundleBuilder builder;
    const auto foo = "foo:bar"_url;

    auto source = "export default 123;"_kjc;
    builder.addEsmModule("foo", source.slice(0, source.size()).attach(kj::mv(source)));
    ModuleBundle::BuiltinBuilder builtinBuilder(ModuleBundle::BuiltinBuilder::Type::BUILTIN_ONLY);
    ModuleBundle::getBuiltInBundleFromCapnp(builtinBuilder, PYODIDE_BUNDLE);
    capnp::MallocMessageBuilder flagsArena;
    auto flags = flagsArena.initRoot<::workerd::CompatibilityFlags>();
    auto registry =
        ModuleRegistry::Builder(observer)
            .add(builder.finish())
            .add(builtinBuilder.finish())
            .add(workerd::api::getInternalUnsafeModuleBundle<TestIsolate_TypeWrapper>(flags))
            .finish();

    auto attached = registry->attachToIsolate(js, compilationObserver);

    js.tryCatch([&] {
      // The built-in only foo:bar module should be found when using a built-in context
      auto value1 =
          ModuleRegistry::resolve(js, "file:///foo", "default"_kjc, ResolveContext::Type::BUNDLE);

      KJ_ASSERT(value1.isNumber());
      KJ_ASSERT(value1.strictEquals(js.num(123)));

      auto value2 = ModuleRegistry::resolve(
          js, "pyodide-internal:emscriptenSetup", "f"_kjc, ResolveContext::Type::BUILTIN);
      KJ_ASSERT(value2.typeOf(js) == "function"_kj);
      KJ_ASSERT(value2.tryCast<v8::Function>() != kj::none);
      // res.map(Func &&f)
    }, [&](Value exception) {
      auto str = kj::str(exception.getHandle(js));
      KJ_ASSERT(str == "!Error: Module not found: foo:bar");
    });
  });
}

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
