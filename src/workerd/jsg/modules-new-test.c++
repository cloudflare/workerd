// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "jsg-test.h"
#include <workerd/jsg/modules-new.h>
#include <workerd/jsg/modules.capnp.h>
#include "observer.h"
#include "url.h"
#include <kj/async-io.h>
#include <kj/thread.h>
#include <kj/vector.h>
#include <capnp/message.h>

namespace workerd::jsg::test {
namespace {
using workerd::jsg::modules::Module;
using workerd::jsg::modules::ModuleBundle;
using workerd::jsg::modules::ModuleRegistry;
using workerd::jsg::modules::ResolveContext;

V8System v8System;

struct ResolveObserverImpl: public ResolveObserver {
  struct Request {
    Url specifier;
    ResolveObserver::Context context;
    ResolveObserver::Source source;
    bool found = false;
  };
  mutable kj::Vector<Request> modules;

  struct MyResolveStatus: public ResolveObserver::ResolveStatus {
    Request& request;
    MyResolveStatus(Request& request): request(request) {}
    void found() override {
      request.found = true;
    }
    void notFound() override {
      request.found = false;
    }
  };

  kj::Own<ResolveObserver::ResolveStatus> onResolveModule(
      const Url& specifier,
      Context context,
      Source source) const override {
    modules.add(Request {
      .specifier = specifier.clone(),
      .context = context,
      .source = source,
    });
    return kj::heap<MyResolveStatus>(modules.back());
  }
};

struct TestType: public jsg::Object {
  bool barCalled = false;
  kj::Maybe<JsRef<JsObject>> exports;

  TestType(Lock&, const jsg::Url&) {}

  void bar() { barCalled = true; }

  JsObject getExports(Lock& js) {
    KJ_IF_SOME(exp, exports) {
      return exp.getHandle(js);
    }
    return exports.emplace(JsRef<JsObject>(js, js.obj())).getHandle(js);
  }

  void setExports(Lock& js, JsObject obj) {
    exports = JsRef(js, obj);
  }

  JsValue require(Lock& js, kj::String specifier) {
    return js.tryCatch([&] {
      return ModuleRegistry::resolve(js, specifier);
    }, [&](Value exception) -> JsValue {
      js.throwException(kj::mv(exception));
    });
  }

  JSG_RESOURCE_TYPE(TestType) {
    JSG_METHOD(bar);
    JSG_METHOD(require);
    JSG_PROTOTYPE_PROPERTY(exports, getExports, setExports);
  }
};

struct TestTypeWrapper {
  static TestTypeWrapper& from(v8::Isolate*) {
    KJ_UNIMPLEMENTED("not implemented");
  }
  v8::Local<v8::Value> wrap(
      v8::Local<v8::Context>,
      kj::Maybe<v8::Local<v8::Object>>,
      jsg::Ref<TestType>) {
    KJ_UNIMPLEMENTED("not implemented");
  }
};

struct TestContext: public Object, public ContextGlobal {
  JSG_RESOURCE_TYPE(TestContext) {
  }
};
JSG_DECLARE_ISOLATE_TYPE(TestIsolate, TestContext, TestType);

#define PREAMBLE(fn)                                                                     \
  TestIsolate isolate(v8System, 123, kj::heap<IsolateObserver>());                       \
  runInV8Stack([&](auto& stackScope) {                                                   \
    TestIsolate::Lock lock(isolate, stackScope);                                         \
    lock.withinHandleScope([&] {                                                         \
      v8::Local<v8::Context> context = lock.newContext<TestContext>().getHandle(lock);   \
      v8::Context::Scope contextScope(context);                                          \
      context->SetAlignedPointerInEmbedderData(2, nullptr);                              \
      fn(lock);                                                                          \
    });                                                                                  \
  });

// ======================================================================================

KJ_TEST("An empty registry") {
  // We should be able to create an empty registry that returns nothing.
  // Basic resolution of this kind does not require an isolate lock.

  ResolveObserverImpl observer;
  ModuleRegistry::Builder registryBuilder(observer);
  auto registry = registryBuilder.finish();
  KJ_ASSERT(registry.get() != nullptr);

  // Resolve should return nothing.
  ResolveContext context = {
    .type = ResolveContext::Type::BUNDLE,
    .source = ResolveContext::Source::OTHER,
    .specifier = "file:///foo"_url,
    .referrer = ModuleBundle::BundleBuilder::BASE,
  };

  KJ_ASSERT(registry->resolve(context) == kj::none);

  KJ_ASSERT(observer.modules.size() == 1);
  KJ_ASSERT(observer.modules[0].found == false);
}

// ======================================================================================

KJ_TEST("A empty fallback bundle") {
  // We should be able to create an empty fallback bundle that returns nothing.
  // Basic resolution of this kind does not require an isolate lock.

  bool called = false;
  auto fallback = ModuleBundle::newFallbackBundle(
      [&called](const ResolveContext& context) {
    called = true;
    return kj::none;
  });

  // Resolve should return nothing.
  ResolveContext context = {
    .type = ResolveContext::Type::BUNDLE,
    .source = ResolveContext::Source::OTHER,
    .specifier = "file:///foo"_url,
    .referrer = ModuleBundle::BundleBuilder::BASE,
  };

  KJ_ASSERT(fallback->resolve(context) == kj::none);
  KJ_ASSERT(called);
}

// ======================================================================================

KJ_TEST("An empty user bundle") {
  // We should be able to create an empty user bundle that returns nothing.
  // Basic resolution of this kind does not require an isolate lock.

  ModuleBundle::BundleBuilder builder;
  auto bundle = builder.finish();

  // Resolve should return nothing.
  ResolveContext context = {
    .type = ResolveContext::Type::BUNDLE,
    .source = ResolveContext::Source::OTHER,
    .specifier = "file:///foo"_url,
    .referrer = ModuleBundle::BundleBuilder::BASE,
  };

  KJ_ASSERT(bundle->resolve(context) == kj::none);
}

// ======================================================================================

KJ_TEST("An empty built-in bundle") {
  // We should be able to create an empty built-in bundle that returns nothing.
  // Basic resolution of this kind does not require an isolate lock.

  ModuleBundle::BuiltinBuilder builder;
  auto bundle = builder.finish();

  // Resolve should return nothing.
  ResolveContext context = {
    .type = ResolveContext::Type::BUNDLE,
    .source = ResolveContext::Source::OTHER,
    .specifier = "file:///foo"_url,
    .referrer = ModuleBundle::BundleBuilder::BASE,
  };

  KJ_ASSERT(bundle->resolve(context) == kj::none);
}

// ======================================================================================

KJ_TEST("A registry with empty bundles") {
  // We should be able to create a registry with empty bundles that return nothing.
  // Basic resolution of this kind does not require an isolate lock.

  ResolveObserverImpl observer;
  ModuleRegistry::Builder registryBuilder(
      observer,
      ModuleRegistry::Builder::Options::ALLOW_FALLBACK);

  registryBuilder.add(ModuleBundle::newFallbackBundle(
      [](const ResolveContext& context) {
    return kj::none;
  }));

  ModuleBundle::BundleBuilder bundleBuilder;
  registryBuilder.add(bundleBuilder.finish());

  ModuleBundle::BuiltinBuilder builtinBuilder;
  registryBuilder.add(builtinBuilder.finish());

  auto registry = registryBuilder.finish();

  // Resolve should return nothing.
  ResolveContext context = {
    .type = ResolveContext::Type::BUNDLE,
    .source = ResolveContext::Source::OTHER,
    .specifier = "file:///foo"_url,
    .referrer = ModuleBundle::BundleBuilder::BASE,
  };

  KJ_ASSERT(registry->resolve(context) == kj::none);
  KJ_ASSERT(observer.modules.size() == 1);
  KJ_ASSERT(observer.modules[0].found == false);
}

// ======================================================================================

KJ_TEST("A user bundle with a single ESM module") {
  ModuleBundle::BundleBuilder builder;

  kj::String source = kj::str("export const foo = 123;");
  builder.addEsmModule("foo", source.releaseArray(), Module::Flags::MAIN);

  auto bundle = builder.finish();

  const auto specifier = "file:///foo"_url;

  // Resolve should return nothing.
  ResolveContext context = {
    .type = ResolveContext::Type::BUNDLE,
    .source = ResolveContext::Source::OTHER,
    .specifier = specifier,
    .referrer = ModuleBundle::BundleBuilder::BASE,
  };

  auto& module = KJ_ASSERT_NONNULL(bundle->resolve(context));

  KJ_ASSERT(module.specifier() == specifier);
  KJ_ASSERT(module.isEsm());
  KJ_ASSERT(module.isMain());
  KJ_ASSERT(module.type() == Module::Type::BUNDLE);
}

// ======================================================================================

KJ_TEST("A registry with a parent") {
  ModuleBundle::BundleBuilder builder;

  kj::String source = kj::str("export const foo = 123;");
  builder.addEsmModule("foo", source.releaseArray(), Module::Flags::MAIN);

  const auto specifier = "file:///foo"_url;

  ResolveObserver observer;
  auto parent = ModuleRegistry::Builder(observer).add(builder.finish()).finish();
  auto registry = ModuleRegistry::Builder(observer).setParent(*parent).finish();

  // Resolve should return nothing.
  ResolveContext context = {
    .type = ResolveContext::Type::BUNDLE,
    .source = ResolveContext::Source::OTHER,
    .specifier = specifier,
    .referrer = ModuleBundle::BundleBuilder::BASE,
  };

  auto& module = KJ_ASSERT_NONNULL(registry->resolve(context));

  KJ_ASSERT(module.specifier() == specifier);
  KJ_ASSERT(module.isEsm());
  KJ_ASSERT(module.isMain());
  KJ_ASSERT(module.type() == Module::Type::BUNDLE);
}

// ======================================================================================

KJ_TEST("A user bundle with an ESM module and a Synthetic module") {
  ModuleBundle::BundleBuilder builder;

  kj::String source = kj::str("export const foo = 123;");
  builder.addEsmModule("foo", source.releaseArray(), Module::Flags::MAIN);
  builder.addSyntheticModule("foo/bar",
      [](Lock&, const Url&, const Module::ModuleNamespace&, const CompilationObserver&) {
    return true;
  });

  const auto foo = "file:///foo"_url;
  const auto bar = "file:///foo/bar"_url;

  auto bundle = builder.finish();

  {
    // Resolve should return nothing.
    ResolveContext context = {
      .type = ResolveContext::Type::BUNDLE,
      .source = ResolveContext::Source::OTHER,
      .specifier = foo,
      .referrer = ModuleBundle::BundleBuilder::BASE,
    };

    auto& module = KJ_ASSERT_NONNULL(bundle->resolve(context));

    KJ_ASSERT(module.specifier() == foo);
    KJ_ASSERT(module.isEsm());
    KJ_ASSERT(module.isMain());
    KJ_ASSERT(module.type() == Module::Type::BUNDLE);
  }

  {
    // Resolve should return nothing.
    ResolveContext context = {
      .type = ResolveContext::Type::BUNDLE,
      .source = ResolveContext::Source::OTHER,
      .specifier = bar,
      .referrer = ModuleBundle::BundleBuilder::BASE,
    };

    auto& module = KJ_ASSERT_NONNULL(bundle->resolve(context));

    KJ_ASSERT(module.specifier() == bar);
    KJ_ASSERT(!module.isEsm());
    KJ_ASSERT(!module.isMain());
    KJ_ASSERT(module.type() == Module::Type::BUNDLE);
  }
}

// ======================================================================================

KJ_TEST("A built-in bundle with two modules") {
  ResolveObserverImpl observer;
  ModuleRegistry::Builder registryBuilder(observer);

  ModuleBundle::BuiltinBuilder builder;

  const auto foo = "foo:bar"_url;
  const auto bar = "bar:baz"_url;
  auto source = "export const foo = 123;"_kjc;
  builder.addEsm(foo, source.asArray());

  struct W {
    static W& from(v8::Isolate*) {
      static W w;
      return w;
    }
    v8::Local<v8::Value> wrap(
        v8::Local<v8::Context>,
        kj::Maybe<v8::Local<v8::Object>>,
        jsg::Ref<TestType>) {
      return v8::Local<v8::Value>();
    }
  };
  builder.addObject<TestType, W>(bar);

  auto registry = registryBuilder.add(builder.finish()).finish();

  {
    ResolveContext context = {
      .type = ResolveContext::Type::BUNDLE,
      .source = ResolveContext::Source::OTHER,
      .specifier = foo,
      .referrer = foo,
    };

    auto& module = KJ_ASSERT_NONNULL(registry->resolve(context));

    KJ_ASSERT(module.specifier() == foo);
    KJ_ASSERT(module.isEsm());
    KJ_ASSERT(!module.isMain());
    KJ_ASSERT(module.type() == Module::Type::BUILTIN);
  }

  {
    ResolveContext context = {
      .type = ResolveContext::Type::BUNDLE,
      .source = ResolveContext::Source::OTHER,
      .specifier = bar,
      .referrer = bar,
    };

    auto& module = KJ_ASSERT_NONNULL(registry->resolve(context));

    KJ_ASSERT(module.specifier() == bar);
    KJ_ASSERT(!module.isEsm());
    KJ_ASSERT(!module.isMain());
    KJ_ASSERT(module.type() == Module::Type::BUILTIN);
  }

  KJ_ASSERT(observer.modules.size() == 2);
  KJ_ASSERT(observer.modules[0].specifier == foo);
  KJ_ASSERT(observer.modules[1].specifier == bar);
}

// ======================================================================================

KJ_TEST("Built-in and Built-in only bundles") {
  ResolveObserverImpl observer;
  ModuleRegistry::Builder registryBuilder(observer);

  ModuleBundle::BuiltinBuilder builtinBuilder;
  ModuleBundle::BuiltinBuilder builtinOnlyBuilder(
      ModuleBundle::BuiltinBuilder::Type::BUILTIN_ONLY);

  const auto foo = "foo:bar"_url;
  const auto bar = "bar:baz"_url;
  auto source = "export const foo = 123;"_kjc;
  builtinBuilder.addEsm(foo, source.asArray());

  builtinOnlyBuilder.addObject<TestType, TestTypeWrapper>(bar);

  auto registry = registryBuilder
      .add(builtinBuilder.finish())
      .add(builtinOnlyBuilder.finish())
      .finish();

  {
    // Resolve should return nothing.
    ResolveContext context = {
      .type = ResolveContext::Type::BUNDLE,
      .source = ResolveContext::Source::OTHER,
      .specifier = foo,
      .referrer = foo,
    };

    auto& module = KJ_ASSERT_NONNULL(registry->resolve(context));

    KJ_ASSERT(module.specifier() == foo);
    KJ_ASSERT(module.isEsm());
    KJ_ASSERT(!module.isMain());
    KJ_ASSERT(module.type() == Module::Type::BUILTIN);
  }

  {
    // Resolve should return nothing.
    ResolveContext context = {
      .type = ResolveContext::Type::BUNDLE,
      .source = ResolveContext::Source::OTHER,
      .specifier = bar,
      .referrer = bar,
    };

    // Built-in only modules cannot be resolved from a bundle context.
    KJ_ASSERT(registry->resolve(context) == kj::none);
  }

  {
    // Resolve should return nothing.
    ResolveContext context = {
      .type = ResolveContext::Type::BUILTIN,
      .source = ResolveContext::Source::OTHER,
      .specifier = bar,
      .referrer = bar,
    };

    auto& module = KJ_ASSERT_NONNULL(registry->resolve(context));

    KJ_ASSERT(module.specifier() == bar);
    KJ_ASSERT(!module.isEsm());
    KJ_ASSERT(!module.isMain());
    KJ_ASSERT(module.type() == Module::Type::BUILTIN_ONLY);
  }

  {
    // Resolve should return nothing.
    ResolveContext context = {
      .type = ResolveContext::Type::BUILTIN_ONLY,
      .source = ResolveContext::Source::OTHER,
      .specifier = bar,
      .referrer = bar,
    };

    auto& module = KJ_ASSERT_NONNULL(registry->resolve(context));

    KJ_ASSERT(module.specifier() == bar);
    KJ_ASSERT(!module.isEsm());
    KJ_ASSERT(!module.isMain());
    KJ_ASSERT(module.type() == Module::Type::BUILTIN_ONLY);
  }
}

// ======================================================================================

KJ_TEST("Built-in modules cannot use file:") {
  ModuleBundle::BuiltinBuilder builder;
  const auto foo = "file:///foo"_url;
  auto source = "export const foo = 123;"_kjc;

  try {
    builder.addEsm(foo, source.asArray());
    KJ_FAIL_ASSERT("Expected an exception");
  } catch (kj::Exception& exception) {
    KJ_ASSERT(exception.getDescription().endsWith(
        "The file: protocol is reserved for bundle type modules"_kjc));
  }
}

// ======================================================================================

KJ_TEST("Fallback bundle that returns something") {
  auto fallback = ModuleBundle::newFallbackBundle(
      [](const ResolveContext& context) {
    return Module::newSynthetic("file:///foo"_url, Module::Type::FALLBACK,
        [](Lock&, const Url&, const Module::ModuleNamespace&, const CompilationObserver&) -> bool {
      KJ_FAIL_ASSERT("Should not be called");
    });
  });

  ResolveObserverImpl observer;
  ModuleRegistry::Builder registryBuilder(
      observer,
      ModuleRegistry::Builder::Options::ALLOW_FALLBACK);
  auto registry = registryBuilder.add(kj::mv(fallback)).finish();

  const auto specifier = "file:///foo"_url;

  {
    ResolveContext context {
      .type = ResolveContext::Type::BUNDLE,
      .source = ResolveContext::Source::OTHER,
      .specifier = specifier,
      .referrer = ModuleBundle::BundleBuilder::BASE,
    };

    auto& module = KJ_ASSERT_NONNULL(registry->resolve(context));
    KJ_ASSERT(module.specifier() == specifier);
    KJ_ASSERT(module.type() == Module::Type::FALLBACK);
    KJ_ASSERT(!module.isEsm());
  }

  // Built-in and built-in only contexts do not use the fallback
  {
    ResolveContext context {
      .type = ResolveContext::Type::BUILTIN,
      .source = ResolveContext::Source::OTHER,
      .specifier = specifier,
      .referrer = ModuleBundle::BundleBuilder::BASE,
    };

    KJ_ASSERT(registry->resolve(context) == kj::none);
  }

  {
    ResolveContext context {
      .type = ResolveContext::Type::BUILTIN_ONLY,
      .source = ResolveContext::Source::OTHER,
      .specifier = specifier,
      .referrer = ModuleBundle::BundleBuilder::BASE,
    };

    KJ_ASSERT(registry->resolve(context) == kj::none);
  }
}

// ======================================================================================

KJ_TEST("Duplicate module names in a single are caught and throw properly") {
  ModuleBundle::BundleBuilder builder;
  builder.addSyntheticModule("foo",
      [](Lock&, const Url&, const Module::ModuleNamespace&, const CompilationObserver&) {
    return true;
  });
  try {
    builder.addSyntheticModule("foo",
        [](Lock&, const Url&, const Module::ModuleNamespace&, const CompilationObserver&) {
      return true;
    });
    KJ_FAIL_ASSERT("Expected an exception");
  } catch (kj::Exception& exception) {
    KJ_ASSERT(exception.getDescription() == "Module \"file:///foo\" already added to bundle"_kjc);
  }
}

// ======================================================================================

KJ_TEST("Fallback bundles are not permitted in production") {
  ResolveObserverImpl observer;
  ModuleRegistry::Builder registryBuilder(observer);
  try {
    registryBuilder.add(ModuleBundle::newFallbackBundle(
        [](const ResolveContext& context) {
      return Module::newSynthetic(
          context.specifier.clone(), Module::Type::FALLBACK,
          [](Lock&, const Url&, const Module::ModuleNamespace&,
             const CompilationObserver&) -> bool {
        KJ_FAIL_ASSERT("Should not be called");
      });
    }));
    KJ_FAIL_ASSERT("Expected an exception");
  } catch (kj::Exception& exception) {
    KJ_ASSERT(exception.getDescription().endsWith(
        "Fallback bundle types are not allowed for this registry"_kjc));
  }
}

// ======================================================================================

KJ_TEST("Compound Registry") {
  ResolveObserverImpl observer;
  ModuleRegistry::Builder registryBuilder(
      observer,
      ModuleRegistry::Builder::Options::ALLOW_FALLBACK);

  const auto foo = "foo:bar"_url;  // Fallback
  const auto bar = "bar:baz"_url;  // Built-in
  const auto baz = "abc:xyz"_url;  // Built-in only
  const auto qux = "file:///qux"_url;  // Bundle

  registryBuilder.add(ModuleBundle::newFallbackBundle(
      [&](const ResolveContext& context) -> kj::Maybe<kj::Own<Module>> {
    if (context.specifier != foo) return kj::none;
    return Module::newSynthetic(
        foo.clone(), Module::Type::FALLBACK,
        [](Lock&, const Url&, const Module::ModuleNamespace&, const CompilationObserver&) -> bool {
      KJ_FAIL_ASSERT("should not have been called");
    });
  }));

  ModuleBundle::BuiltinBuilder builtinBuilder;
  auto barSource = "export const foo = 123;"_kjc;
  builtinBuilder.addEsm(bar, barSource.asArray());
  registryBuilder.add(builtinBuilder.finish());

  ModuleBundle::BuiltinBuilder builtinOnlyBuilder(
      ModuleBundle::BuiltinBuilder::Type::BUILTIN_ONLY);
  builtinOnlyBuilder.addObject<TestType, TestTypeWrapper>(baz);
  registryBuilder.add(builtinOnlyBuilder.finish());

  ModuleBundle::BundleBuilder bundleBuilder;
  kj::String quxSource = kj::str("export const foo = 123;");
  bundleBuilder.addEsmModule("qux", quxSource.releaseArray(), Module::Flags::MAIN);
  registryBuilder.add(bundleBuilder.finish());

  auto registry = registryBuilder.finish();

  constexpr auto resolve = [](ModuleRegistry& registry,
                              ResolveContext::Type type,
                              const Url& specifier) {
    ResolveContext context {
      .type = type,
      .source = ResolveContext::Source::OTHER,
      .specifier = specifier,
      .referrer = ModuleBundle::BundleBuilder::BASE,
    };
    return registry.resolve(context);
  };

  {
    // The fallback module is resolved when using a bundle context
    auto& module = KJ_ASSERT_NONNULL(resolve(*registry, ResolveContext::Type::BUNDLE, foo));
    KJ_ASSERT(module.specifier() == foo);
    KJ_ASSERT(module.type() == Module::Type::FALLBACK);
    KJ_ASSERT(!module.isEsm());
    KJ_ASSERT(!module.isMain());
  }

  {
    // A built-in module is resolved when using a bundle context
    auto& module = KJ_ASSERT_NONNULL(resolve(*registry, ResolveContext::Type::BUNDLE, bar));
    KJ_ASSERT(module.specifier() == bar);
    KJ_ASSERT(module.type() == Module::Type::BUILTIN);
    KJ_ASSERT(module.isEsm());
    KJ_ASSERT(!module.isMain());
  }

  {
    // A bundle module is resolved when using a bundle context
    auto& module = KJ_ASSERT_NONNULL(resolve(*registry, ResolveContext::Type::BUNDLE, qux));
    KJ_ASSERT(module.specifier() == qux);
    KJ_ASSERT(module.type() == Module::Type::BUNDLE);
    KJ_ASSERT(module.isEsm());
    KJ_ASSERT(module.isMain());
  }

  {
    // A built-in module is resolved when using a builtin contxt
    auto& module = KJ_ASSERT_NONNULL(resolve(*registry, ResolveContext::Type::BUILTIN, bar));
    KJ_ASSERT(module.specifier() == bar);
    KJ_ASSERT(module.type() == Module::Type::BUILTIN);
    KJ_ASSERT(module.isEsm());
    KJ_ASSERT(!module.isMain());
  }

  {
    // A built-in only module is resolved when using a built-in context
    auto& module = KJ_ASSERT_NONNULL(resolve(*registry, ResolveContext::Type::BUILTIN, baz));
    KJ_ASSERT(module.specifier() == baz);
    KJ_ASSERT(module.type() == Module::Type::BUILTIN_ONLY);
    KJ_ASSERT(!module.isEsm());
    KJ_ASSERT(!module.isMain());
  }

  {
    // A built-in only module is resolved when using a built-in only context
    auto& module = KJ_ASSERT_NONNULL(resolve(*registry, ResolveContext::Type::BUILTIN_ONLY, baz));
    KJ_ASSERT(module.specifier() == baz);
    KJ_ASSERT(module.type() == Module::Type::BUILTIN_ONLY);
    KJ_ASSERT(!module.isEsm());
    KJ_ASSERT(!module.isMain());
  }

  // A built-in only module cannot be resolved from a bundle context
  KJ_ASSERT(resolve(*registry, ResolveContext::Type::BUNDLE, baz) == kj::none);

  // Fallback modules cannot be resolved from a built-in context
  KJ_ASSERT(resolve(*registry, ResolveContext::Type::BUILTIN, foo) == kj::none);
  KJ_ASSERT(resolve(*registry, ResolveContext::Type::BUILTIN_ONLY, foo) == kj::none);

  // Bundle modules cannot be resolved from a built-in or built-in only context
  KJ_ASSERT(resolve(*registry, ResolveContext::Type::BUILTIN, qux) == kj::none);
  KJ_ASSERT(resolve(*registry, ResolveContext::Type::BUILTIN_ONLY, qux) == kj::none);

  // We should have seen eleven distinct resolution events.
  KJ_ASSERT(observer.modules.size() == 11);
}

// ======================================================================================

KJ_TEST("Bundle shadows built-in") {
  // A bundle module can shadow a built-in
  ResolveObserverImpl observer;
  ModuleRegistry::Builder registryBuilder(observer);

  const auto foo = "foo:bar"_url;

  ModuleBundle::BuiltinBuilder builtinBuilder;
  auto source = "export const foo = 123;"_kjc;
  builtinBuilder.addEsm(foo, source.asArray());
  registryBuilder.add(builtinBuilder.finish());

  ModuleBundle::BundleBuilder bundleBuilder;
  kj::String bundleSource = kj::str("export const foo = 456;");
  bundleBuilder.addEsmModule("foo:bar", bundleSource.releaseArray(), Module::Flags::MAIN);
  registryBuilder.add(bundleBuilder.finish());

  auto registry = registryBuilder.finish();

  ResolveContext context {
    .type = ResolveContext::Type::BUNDLE,
    .source = ResolveContext::Source::OTHER,
    .specifier = foo,
    .referrer = ModuleBundle::BundleBuilder::BASE,
  };

  auto& module = KJ_ASSERT_NONNULL(registry->resolve(context));
  KJ_ASSERT(module.specifier() == foo);
  KJ_ASSERT(module.type() == Module::Type::BUNDLE);
  KJ_ASSERT(module.isEsm());
  KJ_ASSERT(module.isMain());
}

// ======================================================================================

KJ_TEST("Attaching a module registry works") {
  PREAMBLE(([&](Lock& js) {
    ResolveObserver resolveObserver;
    CompilationObserver compilationObserver;
    ModuleRegistry::Builder registryBuilder(resolveObserver);

    ModuleBundle::BundleBuilder bundleBuilder;
    kj::String source = kj::str("export default 123; export const m = 'abc';");
    // Done this way to avoid including the nullptr at the end...
    bundleBuilder.addEsmModule("main",
        source.slice(0, source.size()).attach(kj::mv(source)));

    kj::String mainSource = kj::str("import foo from 'main'; export default foo;");
    bundleBuilder.addEsmModule("worker1",
        mainSource.slice(0, mainSource.size()).attach(kj::mv(mainSource)),
        Module::Flags::MAIN);

    kj::String mainSource2 =
        kj::str("const foo = (await import('main')).default; export default foo;");
    bundleBuilder.addEsmModule("worker2",
        mainSource2.slice(0, mainSource2.size()).attach(kj::mv(mainSource2)),
        Module::Flags::MAIN);

    registryBuilder.add(bundleBuilder.finish());

    auto registry = registryBuilder.finish();

    const auto specifier = "file:///main"_url;

    ResolveContext resolveContext {
      .type = ResolveContext::Type::BUNDLE,
      .source = ResolveContext::Source::OTHER,
      .specifier = specifier,
      .referrer = ModuleBundle::BundleBuilder::BASE,
    };
    KJ_ASSERT(registry->resolve(resolveContext) != kj::none);

    registry->attachToIsolate(js, compilationObserver);

    js.tryCatch([&] {
      auto val = ModuleRegistry::resolve(js, "file:///worker1");
      KJ_ASSERT(val.isNumber());
    }, [&](Value exception) {
      js.throwException(kj::mv(exception));
    });

    js.tryCatch([&] {
      auto val = ModuleRegistry::resolve(js, "worker1");
      KJ_ASSERT(val.isNumber());
    }, [&](Value exception) {
      js.throwException(kj::mv(exception));
    });

    js.tryCatch([&] {
      auto val = ModuleRegistry::resolve(js, "./.././../worker1");
      KJ_ASSERT(val.isNumber());
    }, [&](Value exception) {
      js.throwException(kj::mv(exception));
    });

    js.tryCatch([&] {
      auto val = ModuleRegistry::resolve(js, "file:///worker2");
      KJ_ASSERT(val.isNumber());
    }, [&](Value exception) {
      js.throwException(kj::mv(exception));
    });

    js.tryCatch([&] {
      auto val = ModuleRegistry::resolve(js, "file:///main", "m"_kjc);
      KJ_ASSERT(val.isString());
    }, [&](Value exception) {
      js.throwException(kj::mv(exception));
    });
  }));
}

// ======================================================================================

KJ_TEST("Basic types of modules work (text, data, json, wasm)") {
  PREAMBLE(([&](Lock& js) {
    ResolveObserver resolveObserver;
    CompilationObserver compilationObserver;
    ModuleRegistry::Builder registryBuilder(resolveObserver);

    ModuleBundle::BundleBuilder bundleBuilder;
    bundleBuilder.addSyntheticModule("abc",
        Module::newTextModuleHandler(kj::str("hello").releaseArray()));
    bundleBuilder.addSyntheticModule("xyz",
        Module::newDataModuleHandler(kj::heapArray<kj::byte>({1,2,3})));

    auto json = kj::str("{\"foo\":123}");
    bundleBuilder.addSyntheticModule("json",
        Module::newJsonModuleHandler(json.slice(0, json.size()).attach(kj::mv(json))));

    auto wasm = kj::heapArray<kj::byte>({
      0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
      0x01, 0x07, 0x01, 0x60, 0x02, 0x7f, 0x7f, 0x01,
      0x7f, 0x03, 0x02, 0x01, 0x00, 0x07, 0x07, 0x01,
      0x03, 0x61, 0x64, 0x64, 0x00, 0x00, 0x0a, 0x09,
      0x01, 0x07, 0x00, 0x20, 0x00, 0x20, 0x01, 0x6a,
      0x0b,
    });
    bundleBuilder.addSyntheticModule("wasm", Module::newWasmModuleHandler(kj::mv(wasm)));

    kj::String mainSource2 =
        kj::str("export { default as abc } from 'abc';"
                "export { default as xyz } from 'xyz';"
                "export { default as json } from 'json';"
                "export { default as wasm } from 'wasm';"
                "export { default as wasm2 } from 'wasm?a';");

    bundleBuilder.addEsmModule("worker",
        mainSource2.slice(0, mainSource2.size()).attach(kj::mv(mainSource2)),
        Module::Flags::MAIN);

    registryBuilder.add(bundleBuilder.finish());

    auto registry = registryBuilder.finish();

    const auto specifier = "file:///worker"_url;

    ResolveContext resolveContext {
      .type = ResolveContext::Type::BUNDLE,
      .source = ResolveContext::Source::OTHER,
      .specifier = specifier,
      .referrer = ModuleBundle::BundleBuilder::BASE,
    };
    KJ_ASSERT_NONNULL(registry->resolve(resolveContext));

    registry->attachToIsolate(js, compilationObserver);

    js.tryCatch([&] {
      auto val = ModuleRegistry::resolve(js, "file:///worker", "abc"_kjc);
      KJ_ASSERT(val.isString());
      KJ_ASSERT(kj::str(val) == "hello"_kjc);
    }, [&](Value exception) {
      js.throwException(kj::mv(exception));
    });

    js.tryCatch([&] {
      auto val = ModuleRegistry::resolve(js, "file:///worker", "xyz"_kjc);
      KJ_ASSERT(val.isArrayBuffer());
    }, [&](Value exception) {
      js.throwException(kj::mv(exception));
    });

    js.tryCatch([&] {
      auto val1 = ModuleRegistry::resolve(js, "file:///worker", "json"_kjc);
      auto val2 = ModuleRegistry::resolve(js, "file:///json", "default"_kjc);
      KJ_ASSERT(val1.isObject());
      KJ_ASSERT(val2.isObject());
      KJ_ASSERT(val1.strictEquals(val2));
      auto obj = KJ_ASSERT_NONNULL(val1.tryCast<JsObject>());
      KJ_ASSERT(obj.get(js, "foo").isNumber());
    }, [&](Value exception) {
      js.throwException(kj::mv(exception));
    });

    js.tryCatch([&] {
      auto wasm1 = ModuleRegistry::resolve(js, "file:///worker", "wasm"_kjc);
      auto wasm2 = ModuleRegistry::resolve(js, "file:///wasm", "default"_kjc);
      auto wasm3 = ModuleRegistry::resolve(js, "file:///worker", "wasm2"_kjc);
      KJ_ASSERT(wasm1.isWasmModuleObject());
      KJ_ASSERT(wasm2.isWasmModuleObject());
      KJ_ASSERT(wasm3.isWasmModuleObject());
      KJ_ASSERT(wasm1.strictEquals(wasm2));
      KJ_ASSERT(!wasm1.strictEquals(wasm3));
    }, [&](Value exception) {
      js.throwException(kj::mv(exception));
    });
  }));
}

// ======================================================================================

KJ_TEST("compileEvalFunction in synthetic module works") {
  PREAMBLE([&](Lock& js) {
    CompilationObserver compilationObserver;
    ResolveObserver resolveObserver;
    ModuleBundle::BundleBuilder bundleBuilder;
    bundleBuilder.addSyntheticModule("abc",
        [](Lock& js,
          const Url& specifier,
          const Module::ModuleNamespace& ns,
          const CompilationObserver& observer)
            mutable -> bool {
      auto ext = alloc<TestType>(js, specifier);
      auto& wrapper = TestIsolate_TypeWrapper::from(js.v8Isolate);
      auto fn = Module::compileEvalFunction(js, "bar(123);"_kj, "foo"_kj,
          JsObject(wrapper.wrap(js.v8Context(), kj::none, ext.addRef())),
          observer);
      return js.tryCatch([&] {
        fn(js);
        KJ_ASSERT(ext->barCalled);
        return ns.setDefault(js, js.num(123));
      }, [&](Value exception) {
        js.v8Isolate->ThrowException(exception.getHandle(js));
        return false;
      });
    });

    auto source = kj::str("import 'abc'");
    bundleBuilder.addEsmModule("main",
        source.slice(0, source.size()).attach(kj::mv(source)),
        Module::Flags::MAIN);

    auto registry = ModuleRegistry::Builder(resolveObserver).add(bundleBuilder.finish()).finish();

    registry->attachToIsolate(js, compilationObserver);

    js.tryCatch([&] {
      auto val = ModuleRegistry::resolve(js, "file:///main");
      KJ_ASSERT(val.isUndefined());
    }, [&](Value exception) {
      js.throwException(kj::mv(exception));
    });
  });
}

// ======================================================================================

KJ_TEST("import.meta works as expected") {
  PREAMBLE([&](Lock& js) {
    ResolveObserver ResolveObserver;
    CompilationObserver compilationObserver;

    ModuleBundle::BundleBuilder bundleBuilder;
    auto foo = kj::str("export default import.meta");
    bundleBuilder.addEsmModule("foo", foo.slice(0, foo.size()).attach(kj::mv(foo)));
    auto bar = kj::str("export default import.meta");
    bundleBuilder.addEsmModule("foo/././././bar", bar.slice(0, bar.size()).attach(kj::mv(bar)),
        Module::Flags::MAIN);
    auto registry = ModuleRegistry::Builder(ResolveObserver).add(bundleBuilder.finish()).finish();

    registry->attachToIsolate(js, compilationObserver);

    js.tryCatch([&] {
      auto val = ModuleRegistry::resolve(js, "file:///foo");
      KJ_ASSERT(val.isObject());
      auto obj = KJ_ASSERT_NONNULL(val.tryCast<JsObject>());
      JsValue url = obj.get(js, "url");
      JsValue main = obj.get(js, "main");
      JsValue res = obj.get(js, "resolve");

      KJ_ASSERT(url.isString());
      KJ_ASSERT(main.isBoolean());
      KJ_ASSERT(res.isFunction());

      KJ_ASSERT(url.toString(js) == "file:///foo"_kj);

      auto mainVal = KJ_ASSERT_NONNULL(main.tryCast<JsBoolean>());
      KJ_ASSERT(!mainVal.value(js));

      auto& wrapper = TestIsolate_TypeWrapper::from(js.v8Isolate);
      KJ_IF_SOME(fn, wrapper.tryUnwrap(js.v8Context(),
                                      res, (Function<kj::String(kj::String)>*)nullptr,
                                      kj::none)) {
        KJ_ASSERT(fn(js, kj::str("foo/bar")) == "file:///foo/bar"_kj);
      } else {}

    }, [&](Value exception) {
      js.throwException(kj::mv(exception));
    });

    js.tryCatch([&] {
      auto val = ModuleRegistry::resolve(js, "file:///foo/bar");
      KJ_ASSERT(val.isObject());
      auto obj = KJ_ASSERT_NONNULL(val.tryCast<JsObject>());
      JsValue url = obj.get(js, "url");
      JsValue main = obj.get(js, "main");
      JsValue res = obj.get(js, "resolve");

      KJ_ASSERT(url.isString());
      KJ_ASSERT(main.isBoolean());
      KJ_ASSERT(res.isFunction());

      KJ_ASSERT(url.toString(js) == "file:///foo/bar"_kj);

      auto mainVal = KJ_ASSERT_NONNULL(main.tryCast<JsBoolean>());
      KJ_ASSERT(mainVal.value(js));
    }, [&](Value exception) {
      js.throwException(kj::mv(exception));
    });
  });
}

// ======================================================================================

KJ_TEST("import specifiers with query params and hash fragments work") {
  // If we have two imports with the same base specifier URL
  // but different query params or hash fragments, they should
  // resolve to the same underlying Module but get evaluated
  // separately. This means the EvaluationCallback can be called
  // multiple times.

  PREAMBLE([&](jsg::Lock& js) {
    ResolveObserver ResolveObserver;
    CompilationObserver compilationObserver;

    ModuleBundle::BundleBuilder bundleBuilder;
    auto foo = kj::str("export default import.meta");
    bundleBuilder.addEsmModule("foo", foo.slice(0, foo.size()).attach(kj::mv(foo)));

    auto registry = ModuleRegistry::Builder(ResolveObserver).add(bundleBuilder.finish()).finish();

    registry->attachToIsolate(js, compilationObserver);

    js.tryCatch([&] {
      auto val1 = ModuleRegistry::resolve(js, "file:///foo?1");
      auto val2 = ModuleRegistry::resolve(js, "file:///foo?2");
      auto val3 = ModuleRegistry::resolve(js, "file:///foo#1");
      auto val4 = ModuleRegistry::resolve(js, "file:///foo#2");

      KJ_ASSERT(val1.isObject());
      KJ_ASSERT(val2.isObject());
      KJ_ASSERT(val3.isObject());
      KJ_ASSERT(val4.isObject());
      KJ_ASSERT(!val1.strictEquals(val2));
      KJ_ASSERT(!val2.strictEquals(val3));
      KJ_ASSERT(!val3.strictEquals(val4));
      KJ_ASSERT(!val4.strictEquals(val1));

      auto obj = KJ_ASSERT_NONNULL(val1.tryCast<JsObject>());
      auto url = obj.get(js, "url");
      KJ_ASSERT(url.isString());
      // The import.meta.url should include the query param and hash fragment
      KJ_ASSERT(url.toString(js) == "file:///foo?1"_kj);

    }, [&](Value exception) {
      js.throwException(kj::mv(exception));
    });
  });
}

// ======================================================================================

KJ_TEST("Previously resolved modules not found with incompatible resolve context") {
  // If we have a built-in only module that is resolved with a built-in context, that
  // should not be found when later resolving with a bundle context.

  PREAMBLE([&](Lock& js) {
    ResolveObserverImpl observer;
    CompilationObserver compilationObserver;

    ModuleBundle::BuiltinBuilder builtinBuilder(
        ModuleBundle::BuiltinBuilder::Type::BUILTIN_ONLY);
    const auto foo = "foo:bar"_url;

    auto source = "export default 123;"_kjc;
    builtinBuilder.addEsm(foo, source.slice(0, source.size()).attach(kj::mv(source)));

    ModuleBundle::BundleBuilder bundleBuilder;
    bundleBuilder.addSyntheticModule("bar",
        Module::newDataModuleHandler(kj::heapArray<kj::byte>({1,2,3})));

    auto registry = ModuleRegistry::Builder(observer)
        .add(builtinBuilder.finish())
        .add(bundleBuilder.finish())
        .finish();

    registry->attachToIsolate(js, compilationObserver);

    js.tryCatch([&] {
      // The built-in only foo:bar module should be found when using a built-in context
      auto value1 = ModuleRegistry::resolve(js, "foo:bar", "default"_kjc,
                                            ResolveContext::Type::BUILTIN);

      KJ_ASSERT(value1.isNumber());

      // But since the module is an built-in only. it should not be found when
      // resolving with a bundle context.
      ModuleRegistry::resolve(js, "foo:bar", "default"_kjc,
                              ResolveContext::Type::BUNDLE);
      JSG_FAIL_REQUIRE(Error, "Should have thrown");
    }, [&](Value exception) {
      auto str = kj::str(exception.getHandle(js));
      KJ_ASSERT(str == "Error: Module not found: foo:bar");
    });

    // Likewise, the bar module should be found when using a bundle context
    js.tryCatch([&] {
      auto value2 = ModuleRegistry::resolve(js, "file:///bar", "default"_kjc,
                                            ResolveContext::Type::BUNDLE);
      KJ_ASSERT(value2.isArrayBuffer());

      // But should not be found from a built-in context
      ModuleRegistry::resolve(js, "file:///bar", "default"_kjc,
                              ResolveContext::Type::BUILTIN);
      JSG_FAIL_REQUIRE(Error, "Should have thrown");
    }, [&](Value exception) {
      auto str = kj::str(exception.getHandle(js));
      KJ_ASSERT(str == "Error: Module not found: file:///bar");
    });
  });
}

// ======================================================================================

KJ_TEST("Awaiting top-level dynamic import in synchronous require works") {
  PREAMBLE([&](Lock& js) {
    ResolveObserverImpl observer;
    CompilationObserver compilationObserver;

    ModuleBundle::BundleBuilder bundleBuilder;
    auto foo = kj::str("export default (await import('bar')).default;");
    bundleBuilder.addEsmModule("foo", foo.slice(0, foo.size()).attach(kj::mv(foo)));

    auto bar = kj::str("export default 123;");
    bundleBuilder.addEsmModule("bar", bar.slice(0, bar.size()).attach(kj::mv(bar)));

    auto registry = ModuleRegistry::Builder(observer).add(bundleBuilder.finish()).finish();

    registry->attachToIsolate(js, compilationObserver);

    js.tryCatch([&] {
      // This is a synchronous resolve that should successfully wait on the async
      // import for the bar module.
      auto val = ModuleRegistry::resolve(js, "file:///foo", "default"_kjc);
      KJ_ASSERT(val.isNumber());
    }, [&](Value exception) {
      js.throwException(kj::mv(exception));
    });
  });
}

// ======================================================================================

KJ_TEST("Awaiting a never resolved promise in synchronous require fails as expected") {
  PREAMBLE([&](Lock& js) {
    ResolveObserverImpl observer;
    CompilationObserver compilationObserver;

    ModuleBundle::BundleBuilder bundleBuilder;
    auto foo = kj::str("const p = new Promise(() => {}); await p;");
    bundleBuilder.addEsmModule("foo", foo.slice(0, foo.size()).attach(kj::mv(foo)));

    auto bar = kj::str("export default 123;");
    bundleBuilder.addEsmModule("bar", bar.slice(0, bar.size()).attach(kj::mv(bar)));

    auto registry = ModuleRegistry::Builder(observer).add(bundleBuilder.finish()).finish();

    registry->attachToIsolate(js, compilationObserver);

    js.tryCatch([&] {
      // This is a synchronous resolve that should successfully wait on the async
      // import for the bar module.
      auto val = ModuleRegistry::resolve(js, "file:///foo", "default"_kjc);
      KJ_ASSERT(val.isNumber());
    }, [&](Value exception) {
      auto str = kj::str(exception.getHandle(js));
      KJ_ASSERT(str == "Error: Module evaluation failed to resolve: file:///foo");
    });
  });
}

// ======================================================================================

KJ_TEST("Throwing an exception inside a ESM module works as expected") {
  PREAMBLE([&](Lock& js) {
    ResolveObserverImpl observer;
    CompilationObserver compilationObserver;

    ModuleBundle::BundleBuilder bundleBuilder;
    auto foo = kj::str("throw new Error('foo');");
    bundleBuilder.addEsmModule("foo", foo.slice(0, foo.size()).attach(kj::mv(foo)));

    auto registry = ModuleRegistry::Builder(observer).add(bundleBuilder.finish()).finish();

    registry->attachToIsolate(js, compilationObserver);

    js.tryCatch([&] {
      ModuleRegistry::resolve(js, "file:///foo", "default"_kjc);
      JSG_FAIL_REQUIRE(Error, "Should have thrown");
    }, [&](Value exception) {
      auto str = kj::str(exception.getHandle(js));
      KJ_ASSERT(str == "Error: foo");
    });

    js.tryCatch([&] {
      ModuleRegistry::resolve(js, "file:///foo", "default"_kjc);
      JSG_FAIL_REQUIRE(Error, "Should have thrown");
    }, [&](Value exception) {
      auto str = kj::str(exception.getHandle(js));
      KJ_ASSERT(str == "Error: foo");
    });
  });
}

// ======================================================================================

KJ_TEST("Syntax error in ESM module is properly reported") {
  PREAMBLE([&](Lock& js) {
    ResolveObserverImpl observer;
    CompilationObserver compilationObserver;

    ModuleBundle::BundleBuilder bundleBuilder;

    auto foo = kj::str("export default 123; syntax error");
    bundleBuilder.addEsmModule("foo", foo.slice(0, foo.size()).attach(kj::mv(foo)));

    auto registry = ModuleRegistry::Builder(observer).add(bundleBuilder.finish()).finish();

    registry->attachToIsolate(js, compilationObserver);

    js.tryCatch([&] {
      ModuleRegistry::resolve(js, "file:///foo", "default"_kjc);
    }, [&](Value exception) {
      auto str = kj::str(exception.getHandle(js));
      KJ_ASSERT(str == "SyntaxError: Unexpected identifier 'error'");
    });
  });
}

// ======================================================================================

KJ_TEST("Throwing an exception inside a CJS-style eval module works as expected") {
  PREAMBLE([&](Lock& js) {
    ResolveObserverImpl observer;
    CompilationObserver compilationObserver;

    ModuleBundle::BundleBuilder bundleBuilder;
    bundleBuilder.addSyntheticModule("foo",
        Module::newCjsStyleModuleHandler<TestType, TestIsolate_TypeWrapper>(
            kj::str("exports.foo = 123; throw new Error('bar');"),
            kj::str("foo")));

    auto registry = ModuleRegistry::Builder(observer).add(bundleBuilder.finish()).finish();

    registry->attachToIsolate(js, compilationObserver);

    js.tryCatch([&] {
      ModuleRegistry::resolve(js, "file:///foo", "foo"_kjc);
      JSG_FAIL_REQUIRE(Error, "Should have thrown");
    }, [&](Value exception) {
      auto str = kj::str(exception.getHandle(js));
      KJ_ASSERT(str == "Error: bar");
    });
  });
}

// ======================================================================================

KJ_TEST("Invalid JSON syntax module throws exception as expected") {
  PREAMBLE([&](Lock& js) {
    ResolveObserverImpl observer;
    CompilationObserver compilationObserver;

    ModuleBundle::BundleBuilder bundleBuilder;
    auto json = kj::str("not valid json");
    bundleBuilder.addSyntheticModule("foo",
        Module::newJsonModuleHandler(json.slice(0, json.size()).attach(kj::mv(json))));

    auto esm = kj::str("import foo from 'foo'");
    bundleBuilder.addEsmModule("bar", esm.slice(0, esm.size()).attach(kj::mv(esm)),
        Module::Flags::MAIN);

    auto registry = ModuleRegistry::Builder(observer).add(bundleBuilder.finish()).finish();

    registry->attachToIsolate(js, compilationObserver);

    js.tryCatch([&] {
      ModuleRegistry::resolve(js, "file:///foo", "default"_kjc);
      JSG_FAIL_REQUIRE(Error, "Should have thrown");
    }, [&](Value exception) {
      auto str = kj::str(exception.getHandle(js));
      KJ_ASSERT(str == "SyntaxError: Unexpected token 'o', \"not valid json\" is not valid JSON");
    });

    // We can try multiple times and it doesn't matter.
    js.tryCatch([&] {
      ModuleRegistry::resolve(js, "file:///foo", "default"_kjc);
      JSG_FAIL_REQUIRE(Error, "Should have thrown");
    }, [&](Value exception) {
      auto str = kj::str(exception.getHandle(js));
      KJ_ASSERT(str == "SyntaxError: Unexpected token 'o', \"not valid json\" is not valid JSON");
    });

    // We get the same error even if statically imported after the previous imports
    js.tryCatch([&] {
      ModuleRegistry::resolve(js, "file:///bar", "default"_kjc);
      JSG_FAIL_REQUIRE(Error, "Should have thrown");
    }, [&](Value exception) {
      auto str = kj::str(exception.getHandle(js));
      KJ_ASSERT(str == "SyntaxError: Unexpected token 'o', \"not valid json\" is not valid JSON");
    });
  });
}

// ======================================================================================

KJ_TEST("Recursive import works or fails as expected") {
  PREAMBLE([&](Lock& js) {
    ResolveObserverImpl observer;
    CompilationObserver compilationObserver;

    ModuleBundle::BundleBuilder bundleBuilder;

    // A recursive import with an ESM works just fine...
    auto foo = kj::str("import foo from 'foo'; export default 123;");
    bundleBuilder.addEsmModule("foo", foo.slice(0, foo.size()).attach(kj::mv(foo)));

    // A CommonJS-style module, however, does not allow recursive evaluation.
    bundleBuilder.addSyntheticModule("bar",
        Module::newCjsStyleModuleHandler<TestType, TestIsolate_TypeWrapper>(
            kj::str("require('bar')"),
            kj::str("bar")));

    auto registry = ModuleRegistry::Builder(observer).add(bundleBuilder.finish()).finish();

    registry->attachToIsolate(js, compilationObserver);

    auto val1 = ModuleRegistry::resolve(js, "file:///foo", "default"_kjc);
    KJ_ASSERT(val1.isNumber());

    js.tryCatch([&] {
      ModuleRegistry::resolve(js, "file:///bar", "default"_kjc);
    }, [&](Value exception) {
      auto str = kj::str(exception.getHandle(js));
      KJ_ASSERT(str == "Error: Module cannot be recursively evaluated: file:///bar");
    });
  });
}

// ======================================================================================

KJ_TEST("Recursively require ESM from CJS required from ESM fails as expected") {
  PREAMBLE([&](Lock& js) {
    ResolveObserverImpl observer;
    CompilationObserver compilationObserver;

    ModuleBundle::BundleBuilder bundleBuilder;

    // In this test, we have an ESM module (bar) that imports a CJS style
    // module (foo) that synchronously tries to require the ESM module (bar).
    // This is not allowed because the CJS module cannot successfully require
    // a module that is still in the process of being evaluated.

    bundleBuilder.addSyntheticModule("foo",
        Module::newCjsStyleModuleHandler<TestType, TestIsolate_TypeWrapper>(
            kj::str("exports = require('bar');"),
            kj::str("foo")));

    auto bar = kj::str("export default 123; await import('foo');");
    bundleBuilder.addEsmModule("bar", bar.slice(0, bar.size()).attach(kj::mv(bar)));

    auto registry = ModuleRegistry::Builder(observer).add(bundleBuilder.finish()).finish();

    registry->attachToIsolate(js, compilationObserver);

    js.tryCatch([&] {
      ModuleRegistry::resolve(js, "file:///bar", "default"_kjc);
      JSG_FAIL_REQUIRE(Error, "Should have failed");
    }, [&](Value exception) {
      auto str = kj::str(exception.getHandle(js));
      KJ_ASSERT(str == "TypeError: Circular module dependency with synchronous require: "
                       "file:///bar");
    });
  });
}

// ======================================================================================

KJ_TEST("Recursively require ESM from CJS required from ESM fails as expected (2)") {
  PREAMBLE([&](Lock& js) {
    ResolveObserverImpl observer;
    CompilationObserver compilationObserver;

    ModuleBundle::BundleBuilder bundleBuilder;

    // In this test, we have an ESM module (bar) that imports a CJS style
    // module (foo) that synchronously tries to require the ESM module (bar).
    // This is not allowed because the CJS module cannot successfully require
    // a module that is still in the process of being evaluated.

    bundleBuilder.addSyntheticModule("foo",
        Module::newCjsStyleModuleHandler<TestType, TestIsolate_TypeWrapper>(
            kj::str("exports = require('bar');"),
            kj::str("foo")));

    auto bar = kj::str("export default 123; import bar from 'foo';");
    bundleBuilder.addEsmModule("bar", bar.slice(0, bar.size()).attach(kj::mv(bar)));

    auto registry = ModuleRegistry::Builder(observer).add(bundleBuilder.finish()).finish();

    registry->attachToIsolate(js, compilationObserver);

    js.tryCatch([&] {
      ModuleRegistry::resolve(js, "file:///bar", "default"_kjc);
      JSG_FAIL_REQUIRE(Error, "Should have failed");
    }, [&](Value exception) {
      auto str = kj::str(exception.getHandle(js));
      KJ_ASSERT(str == "TypeError: Circular module dependency with synchronous require: "
                       "file:///bar");
    });
  });
}

// ======================================================================================
KJ_TEST("Resolution occurs relative to the referrer") {
  ResolveObserver observer;
  CompilationObserver compilationObserver;
  ModuleRegistry::Builder registryBuilder(observer);

  ModuleBundle::BundleBuilder builder;
  builder.addSyntheticModule("foo/bar",
      Module::newDataModuleHandler(kj::heapArray<kj::byte>(0)));
  builder.addSyntheticModule("bar",
      Module::newDataModuleHandler(kj::heapArray<kj::byte>(0)));

  // The base URL of the referrer is file:///foo/ ... so in each of the
  // following cases, the specifier should be resolved relative to that.
  // For instance, 'bar' should resolve as file:///foo/bar, while '../bar'
  // should resolve as file:///bar

  auto bar = kj::str("export * as abc from 'bar';"           // file:///foo/bar
                     "export * as def from './bar';"         // file:///foo/bar
                     "export * as ghi from '../bar';"        // file:///bar
                     "export * as jkl from '/bar';"          // file:///bar
                     "export * as lmn from '../foo/bar';");  // file:///foo/bar
  builder.addEsmModule("foo/", bar.slice(0, bar.size()).attach(kj::mv(bar)));

  auto registry = registryBuilder.add(builder.finish()).finish();

  PREAMBLE([&](Lock& js) {
    registry->attachToIsolate(js, compilationObserver);

    js.tryCatch([&] {
      auto abc = ModuleRegistry::resolve(js, "file:///foo/", "abc"_kjc);
      auto def = ModuleRegistry::resolve(js, "file:///foo/", "def"_kjc);
      auto ghi = ModuleRegistry::resolve(js, "file:///foo/", "ghi"_kjc);
      auto jkl = ModuleRegistry::resolve(js, "file:///foo/", "jkl"_kjc);
      auto lmn = ModuleRegistry::resolve(js, "file:///foo/", "lmn"_kjc);

      KJ_ASSERT(abc.strictEquals(def));
      KJ_ASSERT(abc.strictEquals(lmn));
      KJ_ASSERT(!abc.strictEquals(ghi));
      KJ_ASSERT(ghi.strictEquals(jkl));
    }, [&](Value exception) {
      js.throwException(kj::mv(exception));
    });
  });
}

// ======================================================================================

KJ_TEST("Fail if CJS evaluation returns a promise") {
  PREAMBLE([&](Lock& js) {
    ResolveObserverImpl observer;
    CompilationObserver compilationObserver;

    ModuleBundle::BundleBuilder bundleBuilder;

    bundleBuilder.addSyntheticModule("foo",
        Module::newCjsStyleModuleHandler<TestType, TestIsolate_TypeWrapper>(
            kj::str("return Promise.resolve(123);"),
            kj::str("foo")));

    auto registry = ModuleRegistry::Builder(observer).add(bundleBuilder.finish()).finish();

    registry->attachToIsolate(js, compilationObserver);

    js.tryCatch([&] {
      ModuleRegistry::resolve(js, "file:///foo", "default"_kjc);
      JSG_FAIL_REQUIRE(Error, "Should have failed");
    }, [&](Value exception) {
      auto str = kj::str(exception.getHandle(js));
      KJ_ASSERT(str == "TypeError: Module evaluation function must not return a promise");
    });
  });
}

// ======================================================================================

KJ_TEST("Building a bundle from a capnp description works") {

  capnp::MallocMessageBuilder builder;
  auto bundle = builder.initRoot<workerd::jsg::Bundle>();

  auto modules = bundle.initModules(3);

  auto str = kj::str("export default 1+1;");
  auto wasm = kj::heapArray<kj::byte>({
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x07, 0x01, 0x60, 0x02, 0x7f, 0x7f, 0x01,
    0x7f, 0x03, 0x02, 0x01, 0x00, 0x07, 0x07, 0x01,
    0x03, 0x61, 0x64, 0x64, 0x00, 0x00, 0x0a, 0x09,
    0x01, 0x07, 0x00, 0x20, 0x00, 0x20, 0x01, 0x6a,
    0x0b,
  });
  auto data = kj::heapArray<kj::byte>({1,2,3});

  modules[0].setName("foo:bar");
  modules[0].setSrc(str.asBytes());
  modules[0].setType(workerd::jsg::ModuleType::BUILTIN);

  modules[1].setName("foo:baz");
  modules[1].setWasm(wasm);
  modules[1].setType(workerd::jsg::ModuleType::BUILTIN);

  modules[2].setName("foo:qux");
  modules[2].setSrc(data.asBytes());
  modules[2].setType(workerd::jsg::ModuleType::BUILTIN);

  ModuleBundle::BuiltinBuilder bundleBuilder;
  ModuleBundle::getBuiltInBundleFromCapnp(
      bundleBuilder,
      bundle.asReader(),
      ModuleBundle::BuiltInBundleOptions::ALLOW_DATA_MODULES);
  auto moduleBundle = bundleBuilder.finish();

  {
    const auto foo = "foo:bar"_url;
    ResolveContext context {
      .type = ResolveContext::Type::BUILTIN,
      .source = ResolveContext::Source::OTHER,
      .specifier = foo,
      .referrer = ModuleBundle::BundleBuilder::BASE,
    };
    auto& module = KJ_ASSERT_NONNULL(moduleBundle->resolve(context));
    KJ_ASSERT(module.specifier() == foo);
  }

  {
    const auto bar = "foo:baz"_url;
    ResolveContext context {
      .type = ResolveContext::Type::BUILTIN,
      .source = ResolveContext::Source::OTHER,
      .specifier = bar,
      .referrer = ModuleBundle::BundleBuilder::BASE,
    };
    auto& module = KJ_ASSERT_NONNULL(moduleBundle->resolve(context));
    KJ_ASSERT(module.specifier() == bar);
  }

  {
    const auto qux = "foo:qux"_url;
    ResolveContext context {
      .type = ResolveContext::Type::BUILTIN,
      .source = ResolveContext::Source::OTHER,
      .specifier = qux,
      .referrer = ModuleBundle::BundleBuilder::BASE,
    };
    auto& module = KJ_ASSERT_NONNULL(moduleBundle->resolve(context));
    KJ_ASSERT(module.specifier() == qux);
  }

  PREAMBLE([&](Lock& js) {
    ResolveObserver resolveObserver;
    CompilationObserver compilationObserver;
    auto registry = ModuleRegistry::Builder(resolveObserver)
        .add(kj::mv(moduleBundle)).finish();

    registry->attachToIsolate(js, compilationObserver);

    // The foo:bar module is interpreted as an ESM
    js.tryCatch([&] {
      auto val = ModuleRegistry::resolve(js, "foo:bar");
      KJ_ASSERT(val.isNumber());
    }, [&](Value exception) {
      js.throwException(kj::mv(exception));
    });
  });
}

// ======================================================================================

KJ_TEST("Using a registry from multiple threads works") {

  kj::AsyncIoContext io = kj::setupAsyncIo();

  ModuleBundle::BundleBuilder bundleBuilder;
  auto foo = kj::str("export default 123; for (let n = 0; n < 1000000; n++) {}");
  bundleBuilder.addEsmModule("foo", foo.slice(0, foo.size()).attach(kj::mv(foo)));
  ResolveObserver resolveObserver;
  auto registry = ModuleRegistry::Builder(resolveObserver)
    .add(bundleBuilder.finish()).finish();

  static constexpr auto makeThread = [](ModuleRegistry& registry) {
    auto paf = kj::newPromiseAndCrossThreadFulfiller<void>();
    kj::Thread thread([&registry,fulfiller=kj::mv(paf.fulfiller)] {
      PREAMBLE([&](Lock& js) {
        CompilationObserver compilationObserver;
        registry.attachToIsolate(js, compilationObserver);
        js.tryCatch([&] {
          auto val = ModuleRegistry::resolve(js, "file:///foo");
          KJ_ASSERT(val.isNumber());
        }, [&](Value exception) {
          js.throwException(kj::mv(exception));
        });
        fulfiller->fulfill();
      });
    });
    thread.detach();
    return kj::mv(paf.promise);
  };

  kj::joinPromises(kj::arr(
    makeThread(*registry),
    makeThread(*registry),
    makeThread(*registry),
    makeThread(*registry),
    makeThread(*registry)))
      .wait(io.waitScope);
}

// ======================================================================================

KJ_TEST("Fallback service can see original raw specifier if provided") {

  ResolveObserver resolveObserver;
  CompilationObserver compilationObserver;
  ModuleRegistry::Builder builder(resolveObserver,
      ModuleRegistry::Builder::Options::ALLOW_FALLBACK);
  auto rawSpecifier = "nothing"_kjc;
  const auto specifier = "file:///nothing"_url;

  bool called = false;

  builder.add(ModuleBundle::newFallbackBundle(
      [&](const ResolveContext& context) {
    KJ_ASSERT(context.rawSpecifier == rawSpecifier);
    KJ_ASSERT(context.specifier == specifier);
    KJ_ASSERT(context.referrer == ModuleBundle::BundleBuilder::BASE);
    called = true;
    return kj::none;
  }));

  auto registry = builder.finish();

  ResolveContext context = {
    .type = ResolveContext::Type::BUNDLE,
    .source = ResolveContext::Source::OTHER,
    .specifier = specifier,
    .referrer = ModuleBundle::BundleBuilder::BASE,
    .rawSpecifier = rawSpecifier,
  };

  KJ_ASSERT(registry->resolve(context) == kj::none);
  KJ_ASSERT(called);
}

// ======================================================================================

KJ_TEST("Fallback service can return a module with a different specifier") {

  ResolveObserver resolveObserver;
  CompilationObserver compilationObserver;
  ModuleRegistry::Builder builder(resolveObserver,
      ModuleRegistry::Builder::Options::ALLOW_FALLBACK);
  auto rawSpecifier = "nothing"_kjc;
  const auto specifier = "file:///nothing"_url;
  const auto url = "file:///different"_url;

  int called = 0;

  builder.add(ModuleBundle::newFallbackBundle(
      [&](const ResolveContext& context) {
    called++;
    return Module::newSynthetic(url.clone(),
        Module::Type::FALLBACK,
        Module::newDataModuleHandler(kj::heapArray<kj::byte>(0)));
  }));

  auto registry = builder.finish();

  ResolveContext context = {
    .type = ResolveContext::Type::BUNDLE,
    .source = ResolveContext::Source::OTHER,
    .specifier = specifier,
    .referrer = ModuleBundle::BundleBuilder::BASE,
    .rawSpecifier = rawSpecifier,
  };

  auto& module1 = KJ_ASSERT_NONNULL(registry->resolve(context));

  ResolveContext context2 = {
    .type = ResolveContext::Type::BUNDLE,
    .source = ResolveContext::Source::OTHER,
    .specifier = url,
    .referrer = ModuleBundle::BundleBuilder::BASE,
    .rawSpecifier = rawSpecifier,
  };

  auto& module2 = KJ_ASSERT_NONNULL(registry->resolve(context2));

  auto& module3 = KJ_ASSERT_NONNULL(registry->resolve(context));

  // Both specifiers should resolve to the same module so the called count should be 1.
  KJ_ASSERT(called == 1);
  KJ_ASSERT(module1.specifier() == url);
  KJ_ASSERT(&module1 == &module2);
  KJ_ASSERT(&module2 == &module3);
}

// ======================================================================================

KJ_TEST("Percent-encoding in specifiers is normalized properly") {
  ResolveObserver resolveObserver;
  CompilationObserver compilationObserver;

  ModuleBundle::BundleBuilder builder;

  // A specifier might have percent-encoded characters. We want those to be normalized
  // so that they are matched correctly. For instance, %66oo%2fbar should be normalized
  // to foo%2Fbar, and %66oo/bar should be normalized to foo/bar. Specifically, characters
  // that generally do not need to be percent-encoded should be normalized to their
  // unencoded form, while characters that need percent encoded should be normalized
  // to their capitalized percent-encoded form (e.g. %2f becomes %2F). This ensures that
  // when these different forms are used to import they will resolve to the expected
  // module.

  builder.addSyntheticModule("foo%2fbar",
      Module::newDataModuleHandler(kj::heapArray<kj::byte>(0)));
  builder.addSyntheticModule("foo/bar",
      Module::newDataModuleHandler(kj::heapArray<kj::byte>(0)));

  auto foo = kj::str("export { default as abc } from 'foo%2fbar';"
                     "export { default as def } from 'foo/bar';"
                     "export { default as ghi } from '%66oo/bar';"
                     "export { default as jkl } from '%66oo%2fbar';"
                     "export const mno = (await import('%66oo%2fbar')).default;"
                     "export const pqr = (await import('%66oo/bar')).default;");
  builder.addEsmModule("foo", foo.slice(0, foo.size()).attach(kj::mv(foo)));

  auto registry = ModuleRegistry::Builder(resolveObserver).add(builder.finish()).finish();

  PREAMBLE([&](Lock& js) {
    registry->attachToIsolate(js, compilationObserver);

    js.tryCatch([&] {
      auto abc = ModuleRegistry::resolve(js, "foo", "abc"_kjc);
      auto def = ModuleRegistry::resolve(js, "foo", "def"_kjc);
      auto ghi = ModuleRegistry::resolve(js, "foo", "ghi"_kjc);
      auto jkl = ModuleRegistry::resolve(js, "foo", "jkl"_kjc);
      auto mno = ModuleRegistry::resolve(js, "foo", "mno"_kjc);
      auto pqr = ModuleRegistry::resolve(js, "foo", "pqr"_kjc);

      KJ_ASSERT(abc.strictEquals(jkl));
      KJ_ASSERT(def.strictEquals(ghi));
      KJ_ASSERT(!abc.strictEquals(def));
      KJ_ASSERT(abc.strictEquals(mno));
      KJ_ASSERT(def.strictEquals(pqr));
    }, [&](Value exception) {
      js.throwException(kj::mv(exception));
    });
  });
}

// ======================================================================================

KJ_TEST("Aliased modules (import maps) work") {
  ResolveObserver resolveObserver;
  CompilationObserver compilationObserver;
  ModuleBundle::BundleBuilder builder;

  builder.addSyntheticModule("http://example/foo",
      Module::newDataModuleHandler(kj::heapArray<kj::byte>(0)));
  builder.alias("bar", "http://example/foo");

  try {
    builder.alias("bar", "baz");
    KJ_FAIL_ASSERT("should have thrown");
  } catch (kj::Exception& ex) {
    KJ_ASSERT(ex.getDescription() == "Module \"file:///bar\" already added to bundle");
  }

  try {
    builder.alias("http://example/%66oo", "baz");
    KJ_FAIL_ASSERT("should have thrown");
  } catch (kj::Exception& ex) {
    KJ_ASSERT(ex.getDescription() == "Module \"http://example/foo\" already added to bundle");
  }

  auto src = kj::str("export { default as abc } from 'bar';"
                     "export { default as def } from 'http://example/%66oo';");
  builder.addEsmModule("qux", src.slice(0, src.size()).attach(kj::mv(src)));

  auto registry = ModuleRegistry::Builder(resolveObserver).add(builder.finish()).finish();

  ResolveContext contextBar {
    .type = ResolveContext::Type::BUNDLE,
    .source = ResolveContext::Source::OTHER,
    .specifier = "file:///bar"_url,
    .referrer = ModuleBundle::BundleBuilder::BASE,
  };

  ResolveContext contextFoo {
    .type = ResolveContext::Type::BUNDLE,
    .source = ResolveContext::Source::OTHER,
    .specifier = "http://example/foo"_url,
    .referrer = ModuleBundle::BundleBuilder::BASE,
  };

  auto& bar = KJ_ASSERT_NONNULL(registry->resolve(contextBar));
  auto& foo = KJ_ASSERT_NONNULL(registry->resolve(contextFoo));

  // The aliases resolve to the same underlying module...
  KJ_ASSERT(&bar == &foo);

  PREAMBLE([&](Lock& js) {
    registry->attachToIsolate(js, compilationObserver);

    js.tryCatch([&] {
      // While the aliased identifiers resolve to the same underlying module, the
      // evaluate into two separate module instances. This is similar in behavior
      // to how query string and fragments work. The fact that they use the same
      // underlying definition is not really that important.
      auto abc = ModuleRegistry::resolve(js, "qux", "abc"_kjc);
      auto def = ModuleRegistry::resolve(js, "qux", "def"_kjc);
      KJ_ASSERT(abc.isArrayBuffer());
      KJ_ASSERT(def.isArrayBuffer());
      KJ_ASSERT(!abc.strictEquals(def));
    }, [&](Value exception) {
      js.throwException(kj::mv(exception));
    });
  });
}

// ======================================================================================

// If/when we decide to support import attributes, this test will need to be updated.
KJ_TEST("Import attributes are currently unsupported") {
  ResolveObserver resolveObserver;
  CompilationObserver compilationObserver;
  ModuleBundle::BundleBuilder builder;

  // TODO(soon): The import attribute spec has been updated to replace the "assert"
  // with the "with" keyword. V8 has not yet picked up this change. Once it does,
  // this test will likely need to be changed.
  auto foo = kj::str("import abc from 'foo' assert { type: 'json' };");
  builder.addEsmModule("foo", foo.slice(0, foo.size()).attach(kj::mv(foo)));

  auto bar = kj::str("export const abc = await import('foo', { assert: { type: 'json' }});");
  builder.addEsmModule("bar", bar.slice(0, bar.size()).attach(kj::mv(bar)));

  auto registry = ModuleRegistry::Builder(resolveObserver).add(builder.finish()).finish();

  PREAMBLE([&](Lock& js) {
    registry->attachToIsolate(js, compilationObserver);

    js.tryCatch([&] {
      ModuleRegistry::resolve(js, "foo", "default"_kjc);
      JSG_FAIL_REQUIRE(Error, "Should have thrown");
    }, [&](Value exception) {
      auto str = kj::str(exception.getHandle(js));
      KJ_ASSERT(str == "TypeError: Import attributes are not supported");
    });

    js.tryCatch([&] {
      ModuleRegistry::resolve(js, "bar", "default"_kjc);
      JSG_FAIL_REQUIRE(Error, "Should have thrown");
    }, [&](Value exception) {
      auto str = kj::str(exception.getHandle(js));
      KJ_ASSERT(str == "TypeError: Import attributes are not supported");
    });
  });
}

}  // namespace
}  // namespace workerd::jsg::test
