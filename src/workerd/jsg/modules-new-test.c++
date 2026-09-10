// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "observer.h"
#include "type-wrapper.h"
#include "url.h"

#include <workerd/jsg/modules-new.h>
#include <workerd/jsg/modules.capnp.h>
#include <workerd/jsg/setup.h>

#include <capnp/message.h>
#include <kj/async-io.h>
#include <kj/test.h>
#include <kj/thread.h>
#include <kj/vector.h>

namespace workerd::jsg::test {
namespace {
using workerd::jsg::modules::Module;
using workerd::jsg::modules::ModuleBundle;
using workerd::jsg::modules::ModuleRegistry;
using workerd::jsg::modules::ResolveContext;

V8System v8System;
const jsg::Url BASE = "file:///"_url;
ResolveObserver noopResolveObserver;

kj::Array<kj::byte> makeTestWasm() {
  return kj::heapArray<kj::byte>({
    0x00,
    0x61,
    0x73,
    0x6d,
    0x01,
    0x00,
    0x00,
    0x00,
    0x01,
    0x07,
    0x01,
    0x60,
    0x02,
    0x7f,
    0x7f,
    0x01,
    0x7f,
    0x03,
    0x02,
    0x01,
    0x00,
    0x07,
    0x07,
    0x01,
    0x03,
    0x61,
    0x64,
    0x64,
    0x00,
    0x00,
    0x0a,
    0x09,
    0x01,
    0x07,
    0x00,
    0x20,
    0x00,
    0x20,
    0x01,
    0x6a,
    0x0b,
  });
}

struct ResolveObserverImpl: public ResolveObserver {
  struct Request {
    Url id;
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
      const Url& id, Context context, Source source) const override {
    modules.add(Request{
      .id = id.clone(),
      .context = context,
      .source = source,
    });
    return kj::heap<MyResolveStatus>(modules.back());
  }
};

struct CountingCompilationObserver final: public CompilationObserver {
  struct Counts {
    uint cacheFound = 0;
    uint cacheRejected = 0;
    uint cacheGenerated = 0;
    uint cacheGenerationFailed = 0;
    uint wasmCompiled = 0;
    uint wasmFromCache = 0;
  };

  Counts getCounts() const {
    auto lock = counts.lockShared();
    return *lock;
  }

  kj::Own<void> onWasmCompilationStart(v8::Isolate*, size_t) const override {
    auto lock = counts.lockExclusive();
    ++lock->wasmCompiled;
    return kj::Own<void>();
  }

  kj::Own<void> onWasmCompilationFromCacheStart(v8::Isolate*) const override {
    auto lock = counts.lockExclusive();
    ++lock->wasmFromCache;
    return kj::Own<void>();
  }

  void onCompileCacheFound(v8::Isolate*) const override {
    auto lock = counts.lockExclusive();
    ++lock->cacheFound;
  }

  void onCompileCacheRejected(v8::Isolate*) const override {
    auto lock = counts.lockExclusive();
    ++lock->cacheRejected;
  }

  void onCompileCacheGenerated(v8::Isolate*) const override {
    auto lock = counts.lockExclusive();
    ++lock->cacheGenerated;
  }

  void onCompileCacheGenerationFailed(v8::Isolate*) const override {
    auto lock = counts.lockExclusive();
    ++lock->cacheGenerationFailed;
  }

 private:
  mutable kj::MutexGuarded<Counts> counts;
};

struct TestType: public jsg::Object {
  bool barCalled = false;
  kj::Maybe<JsRef<JsObject>> exports;

  TestType(Lock&, const jsg::Url&) {}

  void bar() {
    barCalled = true;
  }

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
    return js.tryCatch([&] { return ModuleRegistry::resolve(js, specifier); },
        [&](Value exception) -> JsValue { js.throwException(kj::mv(exception)); });
  }

  jsg::JsValue getModuleExports(jsg::Lock& js) {
    return getExports(js);
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
  v8::Local<v8::Value> wrap(jsg::Lock& lock,
      v8::Local<v8::Context>,
      kj::Maybe<v8::Local<v8::Object>>,
      jsg::Ref<TestType>) {
    KJ_UNIMPLEMENTED("not implemented");
  }
};

struct TestContext: public Object, public ContextGlobal {
  JSG_RESOURCE_TYPE(TestContext) {}
};
JSG_DECLARE_ISOLATE_TYPE(TestIsolate, TestContext, TestType);

#define PREAMBLE(fn)                                                                               \
  TestIsolate isolate(v8System, v8::IsolateGroup::GetDefault(), 123, kj::heap<IsolateObserver>()); \
  isolate.runInLockScope([&](auto& lock) {                                                         \
    IsolateBase::from(lock.v8Isolate).setUsingNewModuleRegistry();                                 \
    JSG_WITHIN_CONTEXT_SCOPE(lock, lock.template newContext<TestContext>().getHandle(lock),        \
        [&](jsg::Lock& js) { fn(lock); });                                                         \
  });

// ======================================================================================

KJ_TEST("An empty registry") {
  // We should be able to create an empty registry that returns nothing.
  // Basic resolution of this kind does not require an isolate lock.

  ResolveObserverImpl observer;
  ModuleRegistry::Builder registryBuilder(BASE);
  auto registry = registryBuilder.finish();
  KJ_ASSERT(registry.get() != nullptr);

  ResolveContext context = {
    .type = ResolveContext::Type::BUNDLE,
    .source = ResolveContext::Source::INTERNAL,
    .normalizedSpecifier = "file:///foo"_url,
    .referrerNormalizedSpecifier = BASE,
  };

  KJ_ASSERT(registry->lookup(context, observer) == kj::none);

  KJ_ASSERT(observer.modules.size() == 1);
  KJ_ASSERT(observer.modules[0].found == false);
}

// ======================================================================================

KJ_TEST("A empty fallback bundle") {
  // We should be able to create an empty fallback bundle that returns nothing.
  // Basic resolution of this kind does not require an isolate lock.

  bool called = false;
  auto fallback = ModuleBundle::newFallbackBundle([&called](const ResolveContext& context) {
    called = true;
    return kj::none;
  });

  ResolveContext context = {
    .type = ResolveContext::Type::BUNDLE,
    .source = ResolveContext::Source::INTERNAL,
    .normalizedSpecifier = "file:///foo"_url,
    .referrerNormalizedSpecifier = BASE,
  };

  KJ_ASSERT(fallback->lookup(context) == kj::none);
  KJ_ASSERT(called);
}

// ======================================================================================

KJ_TEST("An empty user bundle") {
  // We should be able to create an empty user bundle that returns nothing.
  // Basic resolution of this kind does not require an isolate lock.

  ModuleBundle::BundleBuilder builder(BASE);
  auto bundle = builder.finish();

  ResolveContext context = {
    .type = ResolveContext::Type::BUNDLE,
    .source = ResolveContext::Source::INTERNAL,
    .normalizedSpecifier = "file:///foo"_url,
    .referrerNormalizedSpecifier = BASE,
  };

  KJ_ASSERT(bundle->lookup(context) == kj::none);
}

// ======================================================================================

KJ_TEST("An empty built-in bundle") {
  // We should be able to create an empty built-in bundle that returns nothing.
  // Basic resolution of this kind does not require an isolate lock.

  ModuleBundle::BuiltinBuilder builder;
  auto bundle = builder.finish();

  ResolveContext context = {
    .type = ResolveContext::Type::BUNDLE,
    .source = ResolveContext::Source::INTERNAL,
    .normalizedSpecifier = "file:///foo"_url,
    .referrerNormalizedSpecifier = BASE,
  };

  KJ_ASSERT(bundle->lookup(context) == kj::none);
}

// ======================================================================================

KJ_TEST("A registry with empty bundles") {
  // We should be able to create a registry with empty bundles that return nothing.
  // Basic resolution of this kind does not require an isolate lock.

  ResolveObserverImpl observer;
  ModuleRegistry::Builder registryBuilder(BASE, ModuleRegistry::Builder::Options::ALLOW_FALLBACK);

  registryBuilder.add(
      ModuleBundle::newFallbackBundle([](const ResolveContext& context) { return kj::none; }));

  ModuleBundle::BundleBuilder bundleBuilder(BASE);
  registryBuilder.add(bundleBuilder.finish());

  ModuleBundle::BuiltinBuilder builtinBuilder;
  registryBuilder.add(builtinBuilder.finish());

  auto registry = registryBuilder.finish();

  ResolveContext context = {
    .type = ResolveContext::Type::BUNDLE,
    .source = ResolveContext::Source::INTERNAL,
    .normalizedSpecifier = "file:///foo"_url,
    .referrerNormalizedSpecifier = BASE,
  };

  KJ_ASSERT(registry->lookup(context, observer) == kj::none);
  KJ_ASSERT(observer.modules.size() == 1);
  KJ_ASSERT(observer.modules[0].found == false);
}

// ======================================================================================

KJ_TEST("A user bundle with a single ESM module") {
  ModuleBundle::BundleBuilder builder(BASE);

  auto source = kj::str("export const foo = 123;");
  builder.addEsmModule("foo", source, Module::Flags::MAIN);

  auto bundle = builder.finish();

  const auto id = "file:///foo"_url;

  ResolveContext context = {
    .type = ResolveContext::Type::BUNDLE,
    .source = ResolveContext::Source::INTERNAL,
    .normalizedSpecifier = id,
    .referrerNormalizedSpecifier = BASE,
  };

  auto resolved = KJ_ASSERT_NONNULL(bundle->lookup(context));
  auto& module = KJ_ASSERT_NONNULL(resolved.module);

  KJ_ASSERT(module.id() == id);
  KJ_ASSERT(module.isEsm());
  KJ_ASSERT(module.isMain());
  KJ_ASSERT(module.type() == Module::Type::BUNDLE);
}

// ======================================================================================

KJ_TEST("A user bundle with an ESM module and a Synthetic module") {
  ModuleBundle::BundleBuilder builder(BASE);

  auto source = kj::str("export const foo = 123;");
  builder.addEsmModule("foo", source, Module::Flags::MAIN);
  builder.addSyntheticModule(
      "foo/bar", [](Lock&, const Url&, const Module::ModuleNamespace&, const CompilationObserver&) {
    return true;
  });

  const auto foo = "file:///foo"_url;
  const auto bar = "file:///foo/bar"_url;

  auto bundle = builder.finish();

  {
    ResolveContext context = {
      .type = ResolveContext::Type::BUNDLE,
      .source = ResolveContext::Source::INTERNAL,
      .normalizedSpecifier = foo,
      .referrerNormalizedSpecifier = BASE,
    };

    auto resolved = KJ_ASSERT_NONNULL(bundle->lookup(context));
    auto& module = KJ_ASSERT_NONNULL(resolved.module);

    KJ_ASSERT(module.id() == foo);
    KJ_ASSERT(module.isEsm());
    KJ_ASSERT(module.isMain());
    KJ_ASSERT(module.type() == Module::Type::BUNDLE);
  }

  {
    ResolveContext context = {
      .type = ResolveContext::Type::BUNDLE,
      .source = ResolveContext::Source::INTERNAL,
      .normalizedSpecifier = bar,
      .referrerNormalizedSpecifier = BASE,
    };

    auto resolved = KJ_ASSERT_NONNULL(bundle->lookup(context));
    auto& module = KJ_ASSERT_NONNULL(resolved.module);

    KJ_ASSERT(module.id() == bar);
    KJ_ASSERT(!module.isEsm());
    KJ_ASSERT(!module.isMain());
    KJ_ASSERT(module.type() == Module::Type::BUNDLE);
  }
}

// ======================================================================================

KJ_TEST("A built-in bundle with two modules") {
  ResolveObserverImpl observer;
  ModuleRegistry::Builder registryBuilder(BASE);

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
    v8::Local<v8::Value> wrap(jsg::Lock& lock,
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
      .source = ResolveContext::Source::INTERNAL,
      .normalizedSpecifier = foo,
      .referrerNormalizedSpecifier = foo,
    };

    auto& module = KJ_ASSERT_NONNULL(registry->lookup(context, observer));

    KJ_ASSERT(module.id() == foo);
    KJ_ASSERT(module.isEsm());
    KJ_ASSERT(!module.isMain());
    KJ_ASSERT(module.type() == Module::Type::BUILTIN);
  }

  {
    ResolveContext context = {
      .type = ResolveContext::Type::BUNDLE,
      .source = ResolveContext::Source::INTERNAL,
      .normalizedSpecifier = bar,
      .referrerNormalizedSpecifier = bar,
    };

    auto& module = KJ_ASSERT_NONNULL(registry->lookup(context, observer));

    KJ_ASSERT(module.id() == bar);
    KJ_ASSERT(!module.isEsm());
    KJ_ASSERT(!module.isMain());
    KJ_ASSERT(module.type() == Module::Type::BUILTIN);
  }

  KJ_ASSERT(observer.modules.size() == 2);
  KJ_ASSERT(observer.modules[0].id == foo);
  KJ_ASSERT(observer.modules[1].id == bar);
}

// ======================================================================================

KJ_TEST("Built-in and Built-in only bundles") {
  ResolveObserverImpl observer;
  ModuleRegistry::Builder registryBuilder(BASE);

  ModuleBundle::BuiltinBuilder builtinBuilder;
  ModuleBundle::BuiltinBuilder builtinOnlyBuilder(ModuleBundle::BuiltinBuilder::Type::BUILTIN_ONLY);

  const auto foo = "foo:bar"_url;
  const auto bar = "bar:baz"_url;
  auto source = "export const foo = 123;"_kjc;
  builtinBuilder.addEsm(foo, source.asArray());

  builtinOnlyBuilder.addObject<TestType, TestTypeWrapper>(bar);

  auto registry =
      registryBuilder.add(builtinBuilder.finish()).add(builtinOnlyBuilder.finish()).finish();

  {
    ResolveContext context = {
      .type = ResolveContext::Type::BUNDLE,
      .source = ResolveContext::Source::INTERNAL,
      .normalizedSpecifier = foo,
      .referrerNormalizedSpecifier = foo,
    };

    auto& module = KJ_ASSERT_NONNULL(registry->lookup(context, noopResolveObserver));

    KJ_ASSERT(module.id() == foo);
    KJ_ASSERT(module.isEsm());
    KJ_ASSERT(!module.isMain());
    KJ_ASSERT(module.type() == Module::Type::BUILTIN);
  }

  {
    ResolveContext context = {
      .type = ResolveContext::Type::BUNDLE,
      .source = ResolveContext::Source::INTERNAL,
      .normalizedSpecifier = bar,
      .referrerNormalizedSpecifier = bar,
    };

    // Built-in only modules cannot be resolved from a bundle context.
    KJ_ASSERT(registry->lookup(context, noopResolveObserver) == kj::none);
  }

  {
    ResolveContext context = {
      .type = ResolveContext::Type::BUILTIN,
      .source = ResolveContext::Source::INTERNAL,
      .normalizedSpecifier = bar,
      .referrerNormalizedSpecifier = bar,
    };

    auto& module = KJ_ASSERT_NONNULL(registry->lookup(context, noopResolveObserver));

    KJ_ASSERT(module.id() == bar);
    KJ_ASSERT(!module.isEsm());
    KJ_ASSERT(!module.isMain());
    KJ_ASSERT(module.type() == Module::Type::BUILTIN_ONLY);
  }

  {
    ResolveContext context = {
      .type = ResolveContext::Type::BUILTIN_ONLY,
      .source = ResolveContext::Source::INTERNAL,
      .normalizedSpecifier = bar,
      .referrerNormalizedSpecifier = bar,
    };

    auto& module = KJ_ASSERT_NONNULL(registry->lookup(context, noopResolveObserver));

    KJ_ASSERT(module.id() == bar);
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
  auto fallback = ModuleBundle::newFallbackBundle([](const ResolveContext& context) {
    kj::Own<Module> mod = Module::newSynthetic("file:///foo"_url, Module::Type::FALLBACK,
        [](Lock&, const Url&, const Module::ModuleNamespace&, const CompilationObserver&) -> bool {
      KJ_FAIL_ASSERT("Should not be called");
    });
    return kj::Maybe<kj::OneOf<kj::String, kj::Own<Module>>>(kj::mv(mod));
  });

  ResolveObserverImpl observer;
  ModuleRegistry::Builder registryBuilder(BASE, ModuleRegistry::Builder::Options::ALLOW_FALLBACK);
  auto registry = registryBuilder.add(kj::mv(fallback)).finish();

  const auto id = "file:///foo"_url;

  {
    ResolveContext context{
      .type = ResolveContext::Type::BUNDLE,
      .source = ResolveContext::Source::INTERNAL,
      .normalizedSpecifier = id,
      .referrerNormalizedSpecifier = BASE,
    };

    auto& module = KJ_ASSERT_NONNULL(registry->lookup(context, noopResolveObserver));
    KJ_ASSERT(module.id() == id);
    KJ_ASSERT(module.type() == Module::Type::FALLBACK);
    KJ_ASSERT(!module.isEsm());
  }

  // Built-in and built-in only contexts do not use the fallback
  {
    ResolveContext context{
      .type = ResolveContext::Type::BUILTIN,
      .source = ResolveContext::Source::INTERNAL,
      .normalizedSpecifier = id,
      .referrerNormalizedSpecifier = BASE,
    };

    KJ_ASSERT(registry->lookup(context, noopResolveObserver) == kj::none);
  }

  {
    ResolveContext context{
      .type = ResolveContext::Type::BUILTIN_ONLY,
      .source = ResolveContext::Source::INTERNAL,
      .normalizedSpecifier = id,
      .referrerNormalizedSpecifier = BASE,
    };

    KJ_ASSERT(registry->lookup(context, noopResolveObserver) == kj::none);
  }
}

// ======================================================================================

KJ_TEST("Duplicate module names in a single are caught and throw properly") {
  ModuleBundle::BundleBuilder builder(BASE);
  builder.addSyntheticModule(
      "foo", [](Lock&, const Url&, const Module::ModuleNamespace&, const CompilationObserver&) {
    return true;
  });
  try {
    builder.addSyntheticModule(
        "foo", [](Lock&, const Url&, const Module::ModuleNamespace&, const CompilationObserver&) {
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
  ModuleRegistry::Builder registryBuilder(BASE);
  try {
    registryBuilder.add(ModuleBundle::newFallbackBundle([](const ResolveContext& context) {
      kj::Own<Module> mod =
          Module::newSynthetic(context.normalizedSpecifier.clone(), Module::Type::FALLBACK,
              [](Lock&, const Url&, const Module::ModuleNamespace&,
                  const CompilationObserver&) -> bool { KJ_FAIL_ASSERT("Should not be called"); });
      return kj::Maybe<kj::OneOf<kj::String, kj::Own<Module>>>(kj::mv(mod));
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
  ModuleRegistry::Builder registryBuilder(BASE, ModuleRegistry::Builder::Options::ALLOW_FALLBACK);

  const auto foo = "foo:bar"_url;      // Fallback
  const auto bar = "bar:baz"_url;      // Built-in
  const auto baz = "abc:xyz"_url;      // Built-in only
  const auto qux = "file:///qux"_url;  // Bundle

  registryBuilder.add(ModuleBundle::newFallbackBundle(
      [&](const ResolveContext& context) -> kj::Maybe<kj::OneOf<kj::String, kj::Own<Module>>> {
    if (context.normalizedSpecifier != foo) return kj::none;
    kj::Own<Module> mod = Module::newSynthetic(foo.clone(), Module::Type::FALLBACK,
        [](Lock&, const Url&, const Module::ModuleNamespace&, const CompilationObserver&) -> bool {
      KJ_FAIL_ASSERT("should not have been called");
    });
    return kj::Maybe<kj::OneOf<kj::String, kj::Own<Module>>>(kj::mv(mod));
  }));

  ModuleBundle::BuiltinBuilder builtinBuilder;
  auto barSource = "export const foo = 123;"_kjc;
  builtinBuilder.addEsm(bar, barSource.asArray());
  registryBuilder.add(builtinBuilder.finish());

  ModuleBundle::BuiltinBuilder builtinOnlyBuilder(ModuleBundle::BuiltinBuilder::Type::BUILTIN_ONLY);
  builtinOnlyBuilder.addObject<TestType, TestTypeWrapper>(baz);
  registryBuilder.add(builtinOnlyBuilder.finish());

  ModuleBundle::BundleBuilder bundleBuilder(BASE);
  auto quxSource = kj::str("export const foo = 123;");
  bundleBuilder.addEsmModule("qux", quxSource, Module::Flags::MAIN);
  registryBuilder.add(bundleBuilder.finish());

  auto registry = registryBuilder.finish();

  auto resolve = [&observer](const auto& registry, ResolveContext::Type type, const Url& id) {
    ResolveContext context{
      .type = type,
      .source = ResolveContext::Source::INTERNAL,
      .normalizedSpecifier = id,
      .referrerNormalizedSpecifier = BASE,
    };
    return registry->lookup(context, observer);
  };

  {
    // The fallback module is resolved when using a bundle context
    auto& module = KJ_ASSERT_NONNULL(resolve(registry, ResolveContext::Type::BUNDLE, foo));
    KJ_ASSERT(module.id() == foo);
    KJ_ASSERT(module.type() == Module::Type::FALLBACK);
    KJ_ASSERT(!module.isEsm());
    KJ_ASSERT(!module.isMain());
  }

  {
    // A built-in module is resolved when using a bundle context
    auto& module = KJ_ASSERT_NONNULL(resolve(registry, ResolveContext::Type::BUNDLE, bar));
    KJ_ASSERT(module.id() == bar);
    KJ_ASSERT(module.type() == Module::Type::BUILTIN);
    KJ_ASSERT(module.isEsm());
    KJ_ASSERT(!module.isMain());
  }

  {
    // A bundle module is resolved when using a bundle context
    auto& module = KJ_ASSERT_NONNULL(resolve(registry, ResolveContext::Type::BUNDLE, qux));
    KJ_ASSERT(module.id() == qux);
    KJ_ASSERT(module.type() == Module::Type::BUNDLE);
    KJ_ASSERT(module.isEsm());
    KJ_ASSERT(module.isMain());
  }

  {
    // A built-in module is resolved when using a builtin context
    auto& module = KJ_ASSERT_NONNULL(resolve(registry, ResolveContext::Type::BUILTIN, bar));
    KJ_ASSERT(module.id() == bar);
    KJ_ASSERT(module.type() == Module::Type::BUILTIN);
    KJ_ASSERT(module.isEsm());
    KJ_ASSERT(!module.isMain());
  }

  {
    // A built-in only module is resolved when using a built-in context
    auto& module = KJ_ASSERT_NONNULL(resolve(registry, ResolveContext::Type::BUILTIN, baz));
    KJ_ASSERT(module.id() == baz);
    KJ_ASSERT(module.type() == Module::Type::BUILTIN_ONLY);
    KJ_ASSERT(!module.isEsm());
    KJ_ASSERT(!module.isMain());
  }

  {
    // A built-in only module is resolved when using a built-in only context
    auto& module = KJ_ASSERT_NONNULL(resolve(registry, ResolveContext::Type::BUILTIN_ONLY, baz));
    KJ_ASSERT(module.id() == baz);
    KJ_ASSERT(module.type() == Module::Type::BUILTIN_ONLY);
    KJ_ASSERT(!module.isEsm());
    KJ_ASSERT(!module.isMain());
  }

  // A built-in only module cannot be resolved from a bundle context
  KJ_ASSERT(resolve(registry, ResolveContext::Type::BUNDLE, baz) == kj::none);

  // Fallback modules cannot be resolved from a built-in context
  KJ_ASSERT(resolve(registry, ResolveContext::Type::BUILTIN, foo) == kj::none);
  KJ_ASSERT(resolve(registry, ResolveContext::Type::BUILTIN_ONLY, foo) == kj::none);

  // Bundle modules cannot be resolved from a built-in or built-in only context
  KJ_ASSERT(resolve(registry, ResolveContext::Type::BUILTIN, qux) == kj::none);
  KJ_ASSERT(resolve(registry, ResolveContext::Type::BUILTIN_ONLY, qux) == kj::none);

  // We should have seen eleven distinct resolution events.
  KJ_ASSERT(observer.modules.size() == 11);
}

// ======================================================================================

KJ_TEST("Bundle shadows built-in") {
  // A bundle module can shadow a built-in
  ResolveObserverImpl observer;
  ModuleRegistry::Builder registryBuilder(BASE);

  const auto foo = "foo:bar"_url;

  ModuleBundle::BuiltinBuilder builtinBuilder;
  auto source = "export const foo = 123;"_kjc;
  builtinBuilder.addEsm(foo, source.asArray());
  registryBuilder.add(builtinBuilder.finish());

  ModuleBundle::BundleBuilder bundleBuilder(BASE);
  auto bundleSource = kj::str("export const foo = 456;");
  bundleBuilder.addEsmModule("foo:bar", bundleSource, Module::Flags::MAIN);
  registryBuilder.add(bundleBuilder.finish());

  auto registry = registryBuilder.finish();

  ResolveContext context{
    .type = ResolveContext::Type::BUNDLE,
    .source = ResolveContext::Source::INTERNAL,
    .normalizedSpecifier = foo,
    .referrerNormalizedSpecifier = BASE,
  };

  auto& module = KJ_ASSERT_NONNULL(registry->lookup(context, noopResolveObserver));
  KJ_ASSERT(module.id() == foo);
  KJ_ASSERT(module.type() == Module::Type::BUNDLE);
  KJ_ASSERT(module.isEsm());
  KJ_ASSERT(module.isMain());
}

// ======================================================================================

KJ_TEST("A worker bundle module can shadow node:process") {
  // Regression test: unlike every other built-in (e.g. node:buffer, which *can* be
  // shadowed by a same-named bundle module -- see "Bundle shadows built-in" above),
  // "node:process" used to be unconditionally redirected to an internal
  // node-internal:public_process/legacy_process module *before* the worker bundle
  // ever had a chance to resolve it. This meant a worker bundle module registered
  // under "node:process" was silently unreachable. Verify that a worker bundle
  // module named "node:process" now takes priority over the internal redirect,
  // exactly as it would for any other built-in.
  PREAMBLE([&](Lock& js) {
    CompilationObserver compilationObserver;
    ModuleRegistry::Builder registryBuilder(BASE);

    // Fake stand-ins for the real node-internal:public_process / legacy_process
    // modules. If the redirect were incorrectly taken instead of the shadowing
    // bundle module below, we'd observe one of these values instead.
    ModuleBundle::BuiltinBuilder internalBuilder(ModuleBundle::BuiltinBuilder::Type::BUILTIN_ONLY);
    auto publicSource = "export default 'internal-public-process';"_kjc;
    auto legacySource = "export default 'internal-legacy-process';"_kjc;
    internalBuilder.addEsm("node-internal:public_process"_url, publicSource.asArray());
    internalBuilder.addEsm("node-internal:legacy_process"_url, legacySource.asArray());
    registryBuilder.add(internalBuilder.finish());

    ModuleBundle::BundleBuilder bundleBuilder(BASE);
    bundleBuilder.addEsmModule("node:process",
        kj::arc<OwnedAscii>(
            kj::heapArray<const char>("export default 'shadowed-process';"_kj.asArray())));
    registryBuilder.add(bundleBuilder.finish());

    auto registry = registryBuilder.finish();
    auto attached = registry->attachToIsolate(js, compilationObserver);

    js.tryCatch([&] {
      auto val = ModuleRegistry::resolve(js, "node:process");
      KJ_ASSERT(val.isString());
      auto value = kj::str(val);
      KJ_ASSERT(value == "shadowed-process"_kjc);
    }, [&](Value exception) { js.throwException(kj::mv(exception)); });
  });
}

// ======================================================================================

KJ_TEST("A worker bundle module can shadow node:process via dynamic import") {
  // Companion to "A worker bundle module can shadow node:process", which only
  // exercises the static resolve path (ModuleRegistry::resolve -> resolveCallback).
  // The dynamic import path (dynamicImportModuleCallback ->
  // IsolateModuleRegistry::dynamicResolve) previously derived its resolve context
  // type from the referrer and applied the node:process -> internal redirect
  // *before* consulting the worker bundle, so `await import('node:process')`
  // bypassed a same-named bundle module. Verify the bundle module also wins on the
  // dynamic-import path, so the shadow-priority fix in dynamicResolve() cannot
  // silently regress.
  PREAMBLE([&](Lock& js) {
    CompilationObserver compilationObserver;
    ModuleRegistry::Builder registryBuilder(BASE);

    // Fake stand-ins for the real node-internal:public_process / legacy_process
    // modules. If the redirect were incorrectly taken instead of the shadowing
    // bundle module below, we'd observe one of these values instead.
    ModuleBundle::BuiltinBuilder internalBuilder(ModuleBundle::BuiltinBuilder::Type::BUILTIN_ONLY);
    auto publicSource = "export default 'internal-public-process';"_kjc;
    auto legacySource = "export default 'internal-legacy-process';"_kjc;
    internalBuilder.addEsm("node-internal:public_process"_url, publicSource.asArray());
    internalBuilder.addEsm("node-internal:legacy_process"_url, legacySource.asArray());
    registryBuilder.add(internalBuilder.finish());

    ModuleBundle::BundleBuilder bundleBuilder(BASE);
    bundleBuilder.addEsmModule("node:process",
        kj::arc<OwnedAscii>(
            kj::heapArray<const char>("export default 'shadowed-process';"_kj.asArray())));
    // Entrypoint module that reaches node:process exclusively through dynamic
    // import(), forcing resolution through dynamicResolve() rather than the
    // static resolveCallback path.
    bundleBuilder.addEsmModule("main",
        kj::arc<OwnedAscii>(kj::heapArray<const char>(
            "export default (await import('node:process')).default;"_kj.asArray())));
    registryBuilder.add(bundleBuilder.finish());

    auto registry = registryBuilder.finish();
    auto attached = registry->attachToIsolate(js, compilationObserver);

    js.tryCatch([&] {
      auto val = ModuleRegistry::resolve(js, "file:///main", "default"_kjc);
      KJ_ASSERT(val.isString());
      auto value = kj::str(val);
      KJ_ASSERT(value == "shadowed-process"_kjc);
    }, [&](Value exception) { js.throwException(kj::mv(exception)); });
  });
}

// ======================================================================================

KJ_TEST("node:process falls back to the internal module when not shadowed") {
  // Companion to the test above: when no worker bundle module shadows
  // "node:process", resolution must still fall back to the internal
  // node-internal:public_process / legacy_process module selected by the
  // enable_nodejs_process_v2 compat flag.
  PREAMBLE([&](Lock& js) {
    CompilationObserver compilationObserver;
    ModuleRegistry::Builder registryBuilder(BASE);

    ModuleBundle::BuiltinBuilder internalBuilder(ModuleBundle::BuiltinBuilder::Type::BUILTIN_ONLY);
    auto publicSource = "export default 'internal-public-process';"_kjc;
    auto legacySource = "export default 'internal-legacy-process';"_kjc;
    internalBuilder.addEsm("node-internal:public_process"_url, publicSource.asArray());
    internalBuilder.addEsm("node-internal:legacy_process"_url, legacySource.asArray());
    registryBuilder.add(internalBuilder.finish());

    auto registry = registryBuilder.finish();
    auto attached = registry->attachToIsolate(js, compilationObserver);

    js.tryCatch([&] {
      auto val = ModuleRegistry::resolve(js, "node:process");
      KJ_ASSERT(val.isString());
      KJ_ASSERT(kj::str(val) == "internal-legacy-process"_kjc);
    }, [&](Value exception) { js.throwException(kj::mv(exception)); });

    js.setNodeJsProcessV2Enabled();

    js.tryCatch([&] {
      auto val = ModuleRegistry::resolve(js, "node:process");
      KJ_ASSERT(val.isString());
      KJ_ASSERT(kj::str(val) == "internal-public-process"_kjc);
    }, [&](Value exception) { js.throwException(kj::mv(exception)); });
  });
}

// ======================================================================================

KJ_TEST("Attaching a module registry works") {
  PREAMBLE(([&](Lock& js) {
    CompilationObserver compilationObserver;
    ModuleRegistry::Builder registryBuilder(BASE);

    ModuleBundle::BundleBuilder bundleBuilder(BASE);
    bundleBuilder.addEsmModule("main",
        kj::arc<OwnedAscii>(
            kj::heapArray<const char>("export default 123; export const m = 'abc';"_kj.asArray())));
    bundleBuilder.addEsmModule("worker1",
        kj::arc<OwnedAscii>(
            kj::heapArray<const char>("import foo from 'main'; export default foo;"_kj.asArray())),
        Module::Flags::MAIN);

    registryBuilder.add(bundleBuilder.finish());

    auto registry = registryBuilder.finish();

    const auto id = "file:///main"_url;

    ResolveContext resolveContext{
      .type = ResolveContext::Type::BUNDLE,
      .source = ResolveContext::Source::INTERNAL,
      .normalizedSpecifier = id,
      .referrerNormalizedSpecifier = BASE,
    };
    KJ_ASSERT(registry->lookup(resolveContext, noopResolveObserver) != kj::none);

    auto attached = registry->attachToIsolate(js, compilationObserver);

    js.tryCatch([&] {
      auto val = ModuleRegistry::resolve(js, "file:///worker1");
      KJ_ASSERT(val.isNumber());
    }, [&](Value exception) { js.throwException(kj::mv(exception)); });

    js.tryCatch([&] {
      auto val = ModuleRegistry::resolve(js, "worker1");
      KJ_ASSERT(val.isNumber());
    }, [&](Value exception) { js.throwException(kj::mv(exception)); });

    js.tryCatch([&] {
      auto val = ModuleRegistry::resolve(js, "./.././../worker1");
      KJ_ASSERT(val.isNumber());
    }, [&](Value exception) { js.throwException(kj::mv(exception)); });

    js.tryCatch([&] {
      auto val = ModuleRegistry::resolve(js, "file:///main", "m"_kjc);
      KJ_ASSERT(val.isString());
    }, [&](Value exception) { js.throwException(kj::mv(exception)); });
  }));
}

// ======================================================================================

KJ_TEST("Basic types of modules work (text, data, json, wasm)") {
  PREAMBLE(([&](Lock& js) {
    CompilationObserver compilationObserver;
    ModuleRegistry::Builder registryBuilder(BASE);

    auto abcSource = kj::str("hello");
    auto xyzData = kj::heapArray<kj::byte>({1, 2, 3});
    auto json = kj::str("{\"foo\":123}");
    auto wasm = makeTestWasm();
    ModuleBundle::BundleBuilder bundleBuilder(BASE);
    bundleBuilder.addSyntheticModule("abc", Module::newTextModuleHandler(abcSource));
    bundleBuilder.addSyntheticModule("xyz", Module::newDataModuleHandler(xyzData));
    bundleBuilder.addSyntheticModule("json", Module::newJsonModuleHandler(json.first(json.size())));
    bundleBuilder.addSyntheticModule("wasm", Module::newWasmModuleHandler(wasm));
    bundleBuilder.addEsmModule("worker",
        kj::arc<OwnedAscii>(
            kj::heapArray<const char>("export { default as abc } from 'abc';"
                                      "export { default as xyz } from 'xyz';"
                                      "export { default as json } from 'json';"
                                      "export { default as wasm } from 'wasm';"
                                      "export { default as wasm2 } from 'wasm?a';"_kj.asArray())),
        Module::Flags::MAIN);

    registryBuilder.add(bundleBuilder.finish());

    auto registry = registryBuilder.finish();

    const auto id = "file:///worker"_url;

    ResolveContext resolveContext{
      .type = ResolveContext::Type::BUNDLE,
      .source = ResolveContext::Source::INTERNAL,
      .normalizedSpecifier = id,
      .referrerNormalizedSpecifier = BASE,
    };
    auto& resolved KJ_UNUSED =
        KJ_ASSERT_NONNULL(registry->lookup(resolveContext, noopResolveObserver));

    auto attached = registry->attachToIsolate(js, compilationObserver);

    js.tryCatch([&] {
      auto val = ModuleRegistry::resolve(js, "file:///worker", "abc"_kjc);
      KJ_ASSERT(val.isString());
      KJ_ASSERT(kj::str(val) == "hello"_kjc);
    }, [&](Value exception) { js.throwException(kj::mv(exception)); });

    js.tryCatch([&] {
      auto val = ModuleRegistry::resolve(js, "file:///worker", "xyz"_kjc);
      KJ_ASSERT(val.isArrayBuffer());
    }, [&](Value exception) { js.throwException(kj::mv(exception)); });

    js.tryCatch([&] {
      auto val1 = ModuleRegistry::resolve(js, "file:///worker", "json"_kjc);
      auto val2 = ModuleRegistry::resolve(js, "file:///json", "default"_kjc);
      KJ_ASSERT(val1.isObject());
      KJ_ASSERT(val2.isObject());
      KJ_ASSERT(val1.strictEquals(val2));
      auto obj = KJ_ASSERT_NONNULL(val1.tryCast<JsObject>());
      KJ_ASSERT(obj.get(js, "foo").isNumber());
    }, [&](Value exception) { js.throwException(kj::mv(exception)); });

    js.tryCatch([&] {
      auto wasm1 = ModuleRegistry::resolve(js, "file:///worker", "wasm"_kjc);
      auto wasm2 = ModuleRegistry::resolve(js, "file:///wasm", "default"_kjc);
      auto wasm3 = ModuleRegistry::resolve(js, "file:///worker", "wasm2"_kjc);
      KJ_ASSERT(wasm1.isWasmModuleObject());
      KJ_ASSERT(wasm2.isWasmModuleObject());
      KJ_ASSERT(wasm3.isWasmModuleObject());
      KJ_ASSERT(wasm1.strictEquals(wasm2));
      KJ_ASSERT(!wasm1.strictEquals(wasm3));
    }, [&](Value exception) { js.throwException(kj::mv(exception)); });
  }));
}

// ======================================================================================

KJ_TEST("Source phase imports work for bundled and fallback Wasm modules") {
  auto wasm = makeTestWasm();

  ModuleBundle::BundleBuilder bundleBuilder(BASE);
  bundleBuilder.addWasmModule("wasm", wasm);
  bundleBuilder.addEsmModule("plain", "export default 123;"_kjc);
  bundleBuilder.addEsmModule("static", "import source wasm from 'wasm'; export default wasm;"_kjc);
  bundleBuilder.addEsmModule("dynamic", "export default await import.source('wasm');"_kjc);
  bundleBuilder.addEsmModule("failure",
      "export default await import.source('plain').then(() => 'resolved', "
      "    (error) => String(error));"_kjc);
  bundleBuilder.addEsmModule(
      "fallback", "import source wasm from 'fallback.wasm'; export default wasm;"_kjc);

  bool fallbackCalled = false;
  auto fallback = ModuleBundle::newFallbackBundle(
      [&](const ResolveContext& context) -> kj::Maybe<kj::OneOf<kj::String, kj::Own<Module>>> {
    if (context.normalizedSpecifier != "file:///fallback.wasm"_url) return kj::none;
    fallbackCalled = true;
    return kj::Maybe<kj::OneOf<kj::String, kj::Own<Module>>>(
        Module::newSynthetic(context.normalizedSpecifier.clone(), Module::Type::FALLBACK,
            Module::newWasmModuleHandler(wasm), nullptr, Module::Flags::WASM,
            Module::ContentType::WASM));
  });

  auto registry = ModuleRegistry::Builder(BASE, ModuleRegistry::Builder::Options::ALLOW_FALLBACK)
                      .add(bundleBuilder.finish())
                      .add(kj::mv(fallback))
                      .finish();

  PREAMBLE([&](Lock& js) {
    CompilationObserver compilationObserver;
    auto attached = registry->attachToIsolate(js, compilationObserver);

    KJ_ASSERT(ModuleRegistry::resolve(js, "static").isWasmModuleObject());
    KJ_ASSERT(ModuleRegistry::resolve(js, "dynamic").isWasmModuleObject());
    KJ_ASSERT(ModuleRegistry::resolve(js, "fallback").isWasmModuleObject());

    auto failure = ModuleRegistry::resolve(js, "failure");
    KJ_ASSERT(kj::str(failure) ==
        "SyntaxError: Source phase import not available for module: file:///plain");
  });

  KJ_ASSERT(fallbackCalled);
}

// ======================================================================================

KJ_TEST("Lazy Wasm compilation preserves the ambient allow-eval setting") {
  PREAMBLE([&](Lock& js) {
    CompilationObserver compilationObserver;

    auto wasm = makeTestWasm();
    ModuleBundle::BundleBuilder bundleBuilder(BASE);
    bundleBuilder.addWasmModule("wasm", wasm);

    auto registry = ModuleRegistry::Builder(BASE).add(bundleBuilder.finish()).finish();
    auto attached = registry->attachToIsolate(js, compilationObserver);

    // Wasm compiles lazily at evaluation time. When that happens inside a
    // window where eval is already permitted (e.g. worker startup with the
    // allow_eval_during_startup compat flag), the permission must survive the
    // compilation rather than being reset to false.
    js.setAllowEval(true);
    KJ_DEFER(js.setAllowEval(false));

    KJ_ASSERT(ModuleRegistry::resolve(js, "file:///wasm").isWasmModuleObject());
    KJ_ASSERT(js.isEvalAllowed());

    // The permission is effective in practice too: eval() still works.
    auto script = check(v8::Script::Compile(js.v8Context(), js.str("eval('6 * 7')"_kj)));
    auto result = JsValue(check(script->Run(js.v8Context())));
    KJ_ASSERT(kj::str(result) == "42");
  });
}

// ======================================================================================

KJ_TEST("compileEvalFunction in synthetic module works") {
  PREAMBLE([&](Lock& js) {
    CompilationObserver compilationObserver;

    ModuleBundle::BundleBuilder bundleBuilder(BASE);
    bundleBuilder.addSyntheticModule("abc",
        [](Lock& js, const Url& id, const Module::ModuleNamespace& ns,
            const CompilationObserver& observer) mutable -> bool {
      // The compileEvalFunction is used in CommonJs/Node.js compat modules to
      // evaluate the module as a function rather than as an ESM. This test just
      // verifies that compileEvalFunction works as expected.
      auto ext = js.alloc<TestType>(js, id);
      auto& wrapper = TestIsolate_TypeWrapper::from(js.v8Isolate);
      auto fn = Module::compileEvalFunction(js, "bar(123);"_kj, "foo"_kj,
          JsObject(wrapper.wrap(js, js.v8Context(), kj::none, ext.addRef())), observer);
      return js.tryCatch([&] {
        fn(js);
        KJ_ASSERT(ext->barCalled);
        return ns.setDefault(js, js.num(123));
      }, [&](Value exception) {
        js.v8Isolate->ThrowException(exception.getHandle(js));
        return false;
      });
    });

    bundleBuilder.addEsmModule("main",
        kj::arc<OwnedAscii>(kj::heapArray<const char>("import 'abc'"_kj.asArray())),
        Module::Flags::MAIN);

    auto registry = ModuleRegistry::Builder(BASE).add(bundleBuilder.finish()).finish();

    auto attached = registry->attachToIsolate(js, compilationObserver);

    js.tryCatch([&] {
      auto val = ModuleRegistry::resolve(js, "file:///main");
      KJ_ASSERT(val.isUndefined());
    }, [&](Value exception) { js.throwException(kj::mv(exception)); });
  });
}

// ======================================================================================

KJ_TEST("import.meta works as expected") {
  PREAMBLE([&](Lock& js) {
    CompilationObserver compilationObserver;

    ModuleBundle::BundleBuilder bundleBuilder(BASE);
    bundleBuilder.addEsmModule("foo", "export default import.meta"_kjc);
    bundleBuilder.addEsmModule(
        "foo/././././bar", "export default import.meta"_kjc, Module::Flags::MAIN);
    auto registry = ModuleRegistry::Builder(BASE).add(bundleBuilder.finish()).finish();

    auto attached = registry->attachToIsolate(js, compilationObserver);

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

      // import.meta.filename is the pathname of the file: URL.
      JsValue filename = obj.get(js, "filename");
      KJ_ASSERT(filename.isString());
      KJ_ASSERT(filename.toString(js) == "/foo"_kj);

      // import.meta.dirname is the parent directory of the pathname.
      JsValue dirname = obj.get(js, "dirname");
      KJ_ASSERT(dirname.isString());
      KJ_ASSERT(dirname.toString(js) == "/"_kj);

      auto mainVal = KJ_ASSERT_NONNULL(main.tryCast<JsBoolean>());
      KJ_ASSERT(!mainVal.value(js));

      auto& wrapper = TestIsolate_TypeWrapper::from(js.v8Isolate);
      KJ_IF_SOME(fn,
          wrapper.tryUnwrap(
              js, js.v8Context(), res, (Function<kj::String(kj::String)>*)nullptr, kj::none)) {
        KJ_ASSERT(fn(js, kj::str("foo/bar")) == "file:///foo/bar"_kj);
      } else {
      }
    }, [&](Value exception) { js.throwException(kj::mv(exception)); });

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

      // import.meta.filename for file:///foo/bar should be /foo/bar.
      JsValue filename = obj.get(js, "filename");
      KJ_ASSERT(filename.isString());
      KJ_ASSERT(filename.toString(js) == "/foo/bar"_kj);

      // import.meta.dirname for file:///foo/bar should be /foo.
      JsValue dirname = obj.get(js, "dirname");
      KJ_ASSERT(dirname.isString());
      KJ_ASSERT(dirname.toString(js) == "/foo"_kj);

      auto mainVal = KJ_ASSERT_NONNULL(main.tryCast<JsBoolean>());
      KJ_ASSERT(mainVal.value(js));
    }, [&](Value exception) { js.throwException(kj::mv(exception)); });
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
    CompilationObserver compilationObserver;

    ModuleBundle::BundleBuilder bundleBuilder(BASE);
    bundleBuilder.addEsmModule("foo", "export default import.meta"_kjc);

    auto registry = ModuleRegistry::Builder(BASE).add(bundleBuilder.finish()).finish();

    auto attached = registry->attachToIsolate(js, compilationObserver);

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

      // import.meta.filename and dirname should reflect the pathname without
      // query params or hash fragments.
      auto filename = obj.get(js, "filename");
      KJ_ASSERT(filename.isString());
      KJ_ASSERT(filename.toString(js) == "/foo"_kj);

      auto dirname = obj.get(js, "dirname");
      KJ_ASSERT(dirname.isString());
      KJ_ASSERT(dirname.toString(js) == "/"_kj);
    }, [&](Value exception) { js.throwException(kj::mv(exception)); });
  });
}

// ======================================================================================

KJ_TEST("Previously resolved modules not found with incompatible resolve context") {
  // If we have a built-in only module that is resolved with a built-in context, that
  // should not be found when later resolving with a bundle context.

  PREAMBLE([&](Lock& js) {
    ResolveObserverImpl observer;
    CompilationObserver compilationObserver;

    ModuleBundle::BuiltinBuilder builtinBuilder(ModuleBundle::BuiltinBuilder::Type::BUILTIN_ONLY);
    const auto foo = "foo:bar"_url;

    builtinBuilder.addEsm(foo, "export default 123;"_kjc);

    auto barData = kj::heapArray<kj::byte>({1, 2, 3});

    ModuleBundle::BundleBuilder bundleBuilder(BASE);
    bundleBuilder.addSyntheticModule("bar", Module::newDataModuleHandler(barData));

    auto registry = ModuleRegistry::Builder(BASE)
                        .add(builtinBuilder.finish())
                        .add(bundleBuilder.finish())
                        .finish();

    auto attached = registry->attachToIsolate(js, compilationObserver);

    js.tryCatch([&] {
      // The built-in only foo:bar module should be found when using a built-in context
      auto value1 =
          ModuleRegistry::resolve(js, "foo:bar", "default"_kjc, ResolveContext::Type::BUILTIN);

      KJ_ASSERT(value1.isNumber());

      // But since the module is an built-in only. it should not be found when
      // resolving with a bundle context.
      ModuleRegistry::resolve(js, "foo:bar", "default"_kjc, ResolveContext::Type::BUNDLE);
      JSG_FAIL_REQUIRE(Error, "Should have thrown");
    }, [&](Value exception) {
      auto str = kj::str(exception.getHandle(js));
      KJ_ASSERT(str == "Error: Module not found: foo:bar");
    });

    // Likewise, the bar module should be found when using a bundle context
    js.tryCatch([&] {
      auto value2 =
          ModuleRegistry::resolve(js, "file:///bar", "default"_kjc, ResolveContext::Type::BUNDLE);
      KJ_ASSERT(value2.isArrayBuffer());

      // But should not be found from a built-in context
      ModuleRegistry::resolve(js, "file:///bar", "default"_kjc, ResolveContext::Type::BUILTIN);
      JSG_FAIL_REQUIRE(Error, "Should have thrown");
    }, [&](Value exception) {
      auto str = kj::str(exception.getHandle(js));
      KJ_ASSERT(str == "Error: Module not found: file:///bar");
    });
  });
}

// ======================================================================================

KJ_TEST("Awaiting top-level dynamic import in synchronous require works as expected") {
  PREAMBLE([&](Lock& js) {
    ResolveObserverImpl observer;
    CompilationObserver compilationObserver;

    ModuleBundle::BundleBuilder bundleBuilder(BASE);
    bundleBuilder.addEsmModule("foo", "export default (await import('bar')).default;"_kjc);
    bundleBuilder.addEsmModule("bar", "export default 123;"_kjc);

    auto registry = ModuleRegistry::Builder(BASE).add(bundleBuilder.finish()).finish();

    auto attached = registry->attachToIsolate(js, compilationObserver);

    ModuleRegistry::resolve(js, "file:///foo", "default"_kjc);
  });
}

// ======================================================================================

KJ_TEST("Awaiting a never resolved promise in synchronous require fails as expected") {
  PREAMBLE([&](Lock& js) {
    ResolveObserverImpl observer;
    CompilationObserver compilationObserver;

    ModuleBundle::BundleBuilder bundleBuilder(BASE);
    bundleBuilder.addEsmModule("foo", "const p = new Promise(() => {}); await p;"_kjc);

    auto registry = ModuleRegistry::Builder(BASE).add(bundleBuilder.finish()).finish();

    auto attached = registry->attachToIsolate(js, compilationObserver);

    js.tryCatch([&] {
      ModuleRegistry::resolve(js, "file:///foo", "default"_kjc);
      KJ_FAIL_ASSERT("Should have failed");
    }, [&](Value exception) {
      auto str = kj::str(exception.getHandle(js));
      KJ_ASSERT(str ==
          "Error: Use of top-level await in a synchronously "
          "required module is restricted to promises that are resolved "
          "synchronously. This includes any top-level awaits in the "
          "entrypoint module for a worker. Specifier: \"file:///foo\".");
    });
  });
}

// ======================================================================================

KJ_TEST("Throwing an exception inside a ESM module works as expected") {
  PREAMBLE([&](Lock& js) {
    ResolveObserverImpl observer;
    CompilationObserver compilationObserver;

    ModuleBundle::BundleBuilder bundleBuilder(BASE);
    bundleBuilder.addEsmModule("foo", "throw new Error('foo');"_kjc);

    auto registry = ModuleRegistry::Builder(BASE).add(bundleBuilder.finish()).finish();

    auto attached = registry->attachToIsolate(js, compilationObserver);

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

    ModuleBundle::BundleBuilder bundleBuilder(BASE);
    bundleBuilder.addEsmModule("foo", "export default 123; syntax error"_kjc);

    auto registry = ModuleRegistry::Builder(BASE).add(bundleBuilder.finish()).finish();

    auto attached = registry->attachToIsolate(js, compilationObserver);

    js.tryCatch(
        [&] { ModuleRegistry::resolve(js, "file:///foo", "default"_kjc); }, [&](Value exception) {
      auto str = kj::str(exception.getHandle(js));
      KJ_ASSERT(str == "SyntaxError: Unexpected identifier 'error'");
    });
  });
}

// ======================================================================================

KJ_TEST("Syntax error in ESM module is reported consistently on repeated resolution") {
  PREAMBLE([&](Lock& js) {
    ResolveObserverImpl observer;
    CompilationObserver compilationObserver;

    ModuleBundle::BundleBuilder bundleBuilder(BASE);
    bundleBuilder.addEsmModule("foo", "export default 123; syntax error"_kjc);

    auto registry = ModuleRegistry::Builder(BASE).add(bundleBuilder.finish()).finish();

    auto attached = registry->attachToIsolate(js, compilationObserver);

    // A failed compilation must not leave stale resolution-cache state behind:
    // every attempt reports the original compile error rather than the second
    // and subsequent attempts failing with an internal error.
    for (int attempt: {1, 2, 3}) {
      bool threw = false;
      JSG_TRY(js) {
        ModuleRegistry::resolve(js, "file:///foo", "default"_kjc);
      }
      JSG_CATCH(exception) {
        threw = true;
        auto str = kj::str(exception.getHandle(js));
        KJ_ASSERT(str == "SyntaxError: Unexpected identifier 'error'", str, attempt);
      }
      KJ_ASSERT(threw, attempt);
    }
  });
}

// ======================================================================================

KJ_TEST("Throwing an exception inside a CJS-style eval module works as expected") {
  PREAMBLE([&](Lock& js) {
    ResolveObserverImpl observer;
    CompilationObserver compilationObserver;

    ModuleBundle::BundleBuilder bundleBuilder(BASE);

    kj::String source = kj::str("exports.foo = 123; throw new Error('bar');");

    bundleBuilder.addSyntheticModule(
        "foo", Module::newCjsStyleModuleHandler<TestType, TestIsolate_TypeWrapper>(source));

    auto registry = ModuleRegistry::Builder(BASE).add(bundleBuilder.finish()).finish();

    auto attached = registry->attachToIsolate(js, compilationObserver);

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

KJ_TEST("Module source is decoded as UTF-8 across all encoding tiers") {
  PREAMBLE([&](Lock& js) {
    CompilationObserver compilationObserver;

    ModuleBundle::BundleBuilder bundleBuilder(BASE);

    // Tier 1: pure-ASCII source (zero-copy external one-byte string).
    bundleBuilder.addEsmModule("ascii",
        kj::arc<OwnedAscii>(kj::heapArray<const char>("export default 'plain';"_kj.asArray())));

    // Tier 2: non-ASCII source whose code points all fit in Latin-1. The
    // identifier and the literal both contain é (U+00E9, UTF-8 c3 a9); under a
    // Latin-1 misread the identifier would be a SyntaxError (a UTF-8
    // continuation byte is not a valid identifier char) and the literal would
    // be mojibake.
    bundleBuilder.addEsmModule("latin1",
        kj::arc<OwnedAscii>(kj::heapArray<const char>(
            "const caf\xc3\xa9 = 'caf\xc3\xa9'; export default caf\xc3\xa9;"_kj.asArray())));

    // Tier 3: source requiring UTF-16 (CJK + non-BMP emoji).
    bundleBuilder.addEsmModule("utf16",
        kj::arc<OwnedAscii>(kj::heapArray<const char>(
            "export default '\xe9\x83\xa8\xe5\x93\x81 \xf0\x9f\x8e\x89';"_kj.asArray())));

    // Invalid UTF-8: a lone 0xE9 byte inside a literal. Malformed sequences are
    // replaced with U+FFFD (UTF-8 ef bf bd), matching v8::String::NewFromUtf8's
    // tolerance rather than rejecting the module.
    bundleBuilder.addEsmModule("invalid",
        kj::arc<OwnedAscii>(kj::heapArray<const char>("export default 'caf\xe9';"_kj.asArray())));

    auto registry = ModuleRegistry::Builder(BASE).add(bundleBuilder.finish()).finish();
    auto attached = registry->attachToIsolate(js, compilationObserver);

    JSG_TRY(js) {
      auto plain = kj::str(ModuleRegistry::resolve(js, "file:///ascii"));
      KJ_ASSERT(plain == "plain");

      auto cafe = kj::str(ModuleRegistry::resolve(js, "file:///latin1"));
      KJ_ASSERT(cafe == "caf\xc3\xa9", cafe);

      auto cjk = kj::str(ModuleRegistry::resolve(js, "file:///utf16"));
      KJ_ASSERT(cjk == "\xe9\x83\xa8\xe5\x93\x81 \xf0\x9f\x8e\x89", cjk);

      auto replaced = kj::str(ModuleRegistry::resolve(js, "file:///invalid"));
      KJ_ASSERT(replaced == "caf\xef\xbf\xbd", replaced);
    }
    JSG_CATCH(exception) {
      js.throwException(kj::mv(exception));
    }
  });
}

// ======================================================================================

KJ_TEST("Built-in source distinguishes UTF-8 from pre-encoded Latin-1") {
  PREAMBLE([&](Lock& js) {
    CompilationObserver compilationObserver;
    ModuleBundle::BuiltinBuilder builtinBuilder;
    static constexpr char utf8Source[] = "export default 'caf\xc3\xa9';";
    static constexpr char latin1Source[] = "export default 'caf\xe9';";
    builtinBuilder.addEsm("test:utf8"_url, kj::arrayPtr(utf8Source, sizeof(utf8Source) - 1));
    builtinBuilder.addEsm("test:latin1"_url,
        StaticExternalStringSource(kj::arrayPtr(latin1Source, sizeof(latin1Source) - 1)));

    auto registry = ModuleRegistry::Builder(BASE).add(builtinBuilder.finish()).finish();
    auto attached = registry->attachToIsolate(js, compilationObserver);

    JSG_TRY(js) {
      for (auto specifier: {"test:utf8"_kjc, "test:latin1"_kjc}) {
        auto value =
            ModuleRegistry::resolve(js, specifier, "default"_kjc, ResolveContext::Type::BUILTIN);
        KJ_ASSERT(kj::str(value) == "caf\xc3\xa9", kj::str(value));
      }
    }
    JSG_CATCH(exception) {
      js.throwException(kj::mv(exception));
    }
  });
}

// ======================================================================================

KJ_TEST("Owned ESM source outlives its release points across encoding tiers") {
  // The Arc<OwnedAscii> addEsmModule overload shares ownership of the UTF-8 source
  // buffer with the module. Non-ASCII sources are transcoded to an owned
  // V8-compatible representation on first compile, after which the UTF-8
  // original is released; pure-ASCII owned sources become the shared encoded
  // representation. V8 external strings retain shared ownership independently
  // of the module. This test asserts correctness across repeated resolution; a
  // buffer released too early (or read after release) is observed by ASAN builds.
  PREAMBLE([&](Lock& js) {
    CompilationObserver compilationObserver;

    ModuleBundle::BundleBuilder bundleBuilder(BASE);
    bundleBuilder.addEsmModule("latin1-owned",
        kj::arc<OwnedAscii>(
            kj::heapArray<const char>("export default 'caf\xc3\xa9';"_kj.asArray())));
    bundleBuilder.addEsmModule("ascii-owned",
        kj::arc<OwnedAscii>(kj::heapArray<const char>("export default 'plain';"_kj.asArray())));

    auto registry = ModuleRegistry::Builder(BASE).add(bundleBuilder.finish()).finish();
    auto attached = registry->attachToIsolate(js, compilationObserver);

    JSG_TRY(js) {
      // First compile transcodes and releases the owned UTF-8 for the
      // non-ASCII module.
      KJ_ASSERT(kj::str(ModuleRegistry::resolve(js, "file:///latin1-owned")) == "caf\xc3\xa9");
      // ASCII stays on the zero-copy path over the owned buffer.
      KJ_ASSERT(kj::str(ModuleRegistry::resolve(js, "file:///ascii-owned")) == "plain");
      // Re-resolution works from the caches; nothing re-reads the released
      // buffer.
      KJ_ASSERT(kj::str(ModuleRegistry::resolve(js, "file:///latin1-owned")) == "caf\xc3\xa9");
      KJ_ASSERT(kj::str(ModuleRegistry::resolve(js, "file:///ascii-owned")) == "plain");
    }
    JSG_CATCH(exception) {
      js.throwException(kj::mv(exception));
    }
  });
}

// ======================================================================================

KJ_TEST("Compiled ESM functions outlive owned source across encoding tiers") {
  PREAMBLE([&](Lock& js) {
    CompilationObserver compilationObserver;
    kj::Vector<JsRef<JsFunction>> functions;

    JSG_TRY(js) {
      {
        ModuleBundle::BundleBuilder bundleBuilder(BASE);
        bundleBuilder.addEsmModule("ascii-lifetime",
            kj::arc<OwnedAscii>(kj::heapArray<const char>(
                "export default function deferred() { return 'plain'; }"_kj.asArray())));
        bundleBuilder.addEsmModule("latin1-lifetime",
            kj::arc<OwnedAscii>(kj::heapArray<const char>(
                "export default function deferred() { return 'caf\xc3\xa9'; }"_kj.asArray())));
        bundleBuilder.addEsmModule("utf16-lifetime",
            kj::arc<OwnedAscii>(kj::heapArray<const char>(
                "export default function deferred() { return '\xe9\x83\xa8\xe5\x93\x81 \xf0\x9f\x8e\x89'; }"_kj
                    .asArray())));

        auto registry = ModuleRegistry::Builder(BASE).add(bundleBuilder.finish()).finish();
        auto attached = registry->attachToIsolate(js, compilationObserver);

        for (auto specifier: {"file:///ascii-lifetime"_kj, "file:///latin1-lifetime"_kj,
               "file:///utf16-lifetime"_kj}) {
          auto value = ModuleRegistry::resolve(js, specifier);
          auto function = KJ_ASSERT_NONNULL(value.tryCast<JsFunction>());
          functions.add(JsRef<JsFunction>(js, function));
        }
      }

      // The exported functions remain live in V8 after the registry, bundles,
      // and their owned source buffers have been destroyed.
      KJ_ASSERT(kj::str(functions[0].getHandle(js).call(js, js.null())) == "plain");
      KJ_ASSERT(kj::str(functions[1].getHandle(js).call(js, js.null())) == "caf\xc3\xa9");
      KJ_ASSERT(kj::str(functions[2].getHandle(js).call(js, js.null())) ==
          "\xe9\x83\xa8\xe5\x93\x81 \xf0\x9f\x8e\x89");
    }
    JSG_CATCH(exception) {
      js.throwException(kj::mv(exception));
    }
  });
}

// ======================================================================================

KJ_TEST("Dynamic import from within a CJS-style eval module works") {
  PREAMBLE([&](Lock& js) {
    ResolveObserverImpl observer;
    CompilationObserver compilationObserver;

    ModuleBundle::BundleBuilder bundleBuilder(BASE);

    // The CJS-style module performs a dynamic import. The compiled eval function's
    // script origin is the module's canonical URL, which is what lets the dynamic
    // import callback identify this module as the referrer and resolve the
    // specifier relative to it.
    kj::String source = kj::str("exports.p = import('dep');");
    bundleBuilder.addSyntheticModule(
        "cjs-dyn", Module::newCjsStyleModuleHandler<TestType, TestIsolate_TypeWrapper>(source));

    bundleBuilder.addEsmModule("dep", "export default 123;"_kjc);

    // The ESM entry point awaits the promise exported by the CJS module.
    bundleBuilder.addEsmModule(
        "entry", "import cjs from 'cjs-dyn'; export default (await cjs.p).default;"_kjc);

    auto registry = ModuleRegistry::Builder(BASE).add(bundleBuilder.finish()).finish();
    auto attached = registry->attachToIsolate(js, compilationObserver);

    JSG_TRY(js) {
      auto value = ModuleRegistry::resolve(js, "file:///entry", "default"_kjc);
      KJ_ASSERT(value.isNumber());
      KJ_ASSERT(kj::str(value) == "123");
    }
    JSG_CATCH(exception) {
      js.throwException(kj::mv(exception));
    }
  });
}

// ======================================================================================

KJ_TEST("Dynamic import from a script with a non-URL origin fails cleanly") {
  PREAMBLE([&](Lock& js) {
    ResolveObserverImpl observer;
    CompilationObserver compilationObserver;

    ModuleBundle::BundleBuilder bundleBuilder(BASE);
    bundleBuilder.addEsmModule("dep", "export default 123;"_kjc);

    auto registry = ModuleRegistry::Builder(BASE).add(bundleBuilder.finish()).finish();
    auto attached = registry->attachToIsolate(js, compilationObserver);

    // Scripts that are not modules (e.g. service-worker mains or eval'd code) can
    // carry arbitrary, non-URL ScriptOrigin names. A dynamic import from such a
    // script cannot identify a referring module; it must reject cleanly rather
    // than trip an internal assertion.
    JSG_TRY(js) {
      auto fn = Module::compileEvalFunction(js,
          "globalThis.result = import('dep'); globalThis.result.catch(() => {});"_kj,
          "plain-script-name"_kj, kj::none, compilationObserver);
      fn(js);
      js.runMicrotasks();
      auto result = JsObject(js.v8Context()->Global()).get(js, "result");
      auto promise = v8::Local<v8::Value>(result).As<v8::Promise>();
      KJ_ASSERT(promise->State() == v8::Promise::kRejected);
      auto err = kj::str(JsValue(promise->Result()));
      KJ_ASSERT(err == "TypeError: Referring module not found in the registry: file:///", err);
    }
    JSG_CATCH(exception) {
      js.throwException(kj::mv(exception));
    }
  });
}

// ======================================================================================

KJ_TEST("Invalid JSON syntax module throws exception as expected") {
  PREAMBLE([&](Lock& js) {
    ResolveObserverImpl observer;
    CompilationObserver compilationObserver;

    auto json = kj::str("not valid json");
    ModuleBundle::BundleBuilder bundleBuilder(BASE);
    bundleBuilder.addSyntheticModule("foo", Module::newJsonModuleHandler(json.first(json.size())));
    bundleBuilder.addEsmModule("bar", "import foo from 'foo'"_kjc, Module::Flags::MAIN);

    auto registry = ModuleRegistry::Builder(BASE).add(bundleBuilder.finish()).finish();

    auto attached = registry->attachToIsolate(js, compilationObserver);

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

    ModuleBundle::BundleBuilder bundleBuilder(BASE);

    // A recursive import with an ESM works just fine...
    bundleBuilder.addEsmModule("foo", "import foo from 'foo'; export default 123;"_kjc);

    auto source = kj::str("require('bar')");

    // A CommonJS-style module, however, does not allow recursive evaluation.
    bundleBuilder.addSyntheticModule(
        "bar", Module::newCjsStyleModuleHandler<TestType, TestIsolate_TypeWrapper>(source));

    auto registry = ModuleRegistry::Builder(BASE).add(bundleBuilder.finish()).finish();

    auto attached = registry->attachToIsolate(js, compilationObserver);

    auto val1 = ModuleRegistry::resolve(js, "file:///foo", "default"_kjc);
    KJ_ASSERT(val1.isNumber());

    js.tryCatch(
        [&] { ModuleRegistry::resolve(js, "file:///bar", "default"_kjc); }, [&](Value exception) {
      auto str = kj::str(exception.getHandle(js));
      KJ_ASSERT(str == "Error: Module cannot be recursively evaluated: file:///bar");
    });
  });
}

// ======================================================================================

KJ_TEST("Recursively require ESM from CJS required from ESM fails as expected (dynamic import)") {
  PREAMBLE([&](Lock& js) {
    ResolveObserverImpl observer;
    CompilationObserver compilationObserver;

    ModuleBundle::BundleBuilder bundleBuilder(BASE);

    // In this test, we have an ESM module (bar) that imports a CJS style
    // module (foo) that synchronously tries to require the ESM module (bar).

    // The circular dependency between foo and baz here, as CJS style modules,
    // should be ok in that it should not throw an error. However, the circular
    // dependency between foo and bar is more problematic since it is forbidden
    // to depend on an ESM that has not yet been fully resolved.

    auto source1 = kj::str("exports = require('foo');");
    auto source2 = kj::str("require('baz'); exports = require('bar');");

    bundleBuilder.addSyntheticModule(
        "baz", Module::newCjsStyleModuleHandler<TestType, TestIsolate_TypeWrapper>(source1));

    bundleBuilder.addSyntheticModule(
        "foo", Module::newCjsStyleModuleHandler<TestType, TestIsolate_TypeWrapper>(source2));

    bundleBuilder.addEsmModule("bar", "export default {}; await import('foo');"_kjc);

    auto registry = ModuleRegistry::Builder(BASE).add(bundleBuilder.finish()).finish();

    auto attached = registry->attachToIsolate(js, compilationObserver);

    js.tryCatch([&] {
      ModuleRegistry::resolve(js, "file:///bar", "default"_kjc);
      JSG_FAIL_REQUIRE(Error, "should have failed");
    }, [&](Value exception) {
      auto str = kj::str(exception.getHandle(js));
      KJ_ASSERT(str == "Error: Circular dependency when resolving module: file:///bar");
    });
  });
}

// ======================================================================================

KJ_TEST("Recursively require ESM from CJS required from ESM fails as expected (static import)") {
  PREAMBLE([&](Lock& js) {
    ResolveObserverImpl observer;
    CompilationObserver compilationObserver;

    ModuleBundle::BundleBuilder bundleBuilder(BASE);

    // In this test, we have an ESM module (bar) that imports a CJS style
    // module (foo) that synchronously tries to require the ESM module (bar).

    // The circular dependency between foo and baz here, as CJS style modules,
    // should be ok in that it should not throw an error. However, the circular
    // dependency between foo and bar is more problematic since it is forbidden
    // to depend on an ESM that has not yet been fully resolved.

    auto source1 = kj::str("exports = require('foo');");
    auto source2 = kj::str("require('baz'); exports = require('bar');");

    bundleBuilder.addSyntheticModule(
        "baz", Module::newCjsStyleModuleHandler<TestType, TestIsolate_TypeWrapper>(source1));

    bundleBuilder.addSyntheticModule(
        "foo", Module::newCjsStyleModuleHandler<TestType, TestIsolate_TypeWrapper>(source2));

    bundleBuilder.addEsmModule("bar", "export default {}; import bar from 'foo';"_kjc);

    auto registry = ModuleRegistry::Builder(BASE).add(bundleBuilder.finish()).finish();

    auto attached = registry->attachToIsolate(js, compilationObserver);

    js.tryCatch([&] {
      ModuleRegistry::resolve(js, "file:///bar", "default"_kjc);
      JSG_FAIL_REQUIRE(Error, "should have failed");
    }, [&](Value exception) {
      auto str = kj::str(exception.getHandle(js));
      KJ_ASSERT(str == "Error: Circular dependency when resolving module: file:///bar");
    });
  });
}

// ======================================================================================

KJ_TEST("ESM -> CJS -> require(ESM) -> static import CJS circular dependency fails gracefully") {
  // This tests a specific crash scenario: when a CJS module (b) is mid-evaluation
  // (kEvaluating), and it require()s an ESM (c) that statically imports the same
  // CJS module (b), V8's InnerModuleEvaluation would call Module::Evaluate on
  // the kEvaluating synthetic module, hitting a CHECK crash. The resolveModuleCallback
  // kEvaluating guard prevents this by rejecting the circular dependency at
  // instantiation time.
  PREAMBLE([&](Lock& js) {
    ResolveObserverImpl observer;
    CompilationObserver compilationObserver;

    ModuleBundle::BundleBuilder bundleBuilder(BASE);

    // a.js (ESM) -> imports b (CJS)
    bundleBuilder.addEsmModule(
        "a", "import b from 'b'; export default b;"_kjc, Module::Flags::MAIN);

    // b (CJS) -> require('c') which is an ESM that imports b back
    auto bSource = kj::str("exports = require('c');");
    bundleBuilder.addSyntheticModule(
        "b", Module::newCjsStyleModuleHandler<TestType, TestIsolate_TypeWrapper>(bSource));

    // c.js (ESM) -> imports b (CJS) — creates the circular dependency
    bundleBuilder.addEsmModule("c", "import b from 'b'; export default b;"_kjc);

    auto registry = ModuleRegistry::Builder(BASE).add(bundleBuilder.finish()).finish();

    auto attached = registry->attachToIsolate(js, compilationObserver);

    js.tryCatch([&] {
      ModuleRegistry::resolve(js, "file:///a", "default"_kjc);
      JSG_FAIL_REQUIRE(Error, "Should have thrown");
    }, [&](Value exception) {
      auto str = kj::str(exception.getHandle(js));
      KJ_ASSERT(str == "Error: Circular dependency when resolving module: b");
    });
  });
}

// ======================================================================================

KJ_TEST("Nested require() that pumps microtasks does not crash a sibling TLA module "
        "(kEvaluatingAsync)") {
  // Regression test for a V8 fatal CHECK: "status() >= kEvaluatingAsync".
  //
  // The entrypoint ESM (entry) statically imports a top-level-await module (leaf)
  // and then a CJS module (pump). During entry's synchronous graph evaluation V8
  // evaluates leaf first: leaf suspends at its await, becoming kEvaluatingAsync and
  // recording entry as an async parent, while entry itself is still kEvaluating.
  // V8 then evaluates pump, whose CJS body performs a nested require('trivial').
  // That require() pumps the microtask queue -- which, before the fix, would run
  // leaf's fulfillment callback while entry was still kEvaluating, driving
  // SourceTextModule::AsyncModuleExecutionFulfilled -> GatherAvailableAncestors ->
  // GetCycleRoot(entry) into a fatal CHECK.
  //
  // With the fix, the module loader inspects the module's evaluation promise before
  // draining anything: a synchronous module (like trivial) resolves its promise
  // during Evaluate() and is returned without any pump. A drain happens only for a
  // genuinely-pending top-level await, and even then only when not nested inside
  // another module's evaluation (js.isEvaluatingModule(), tracked by the RAII
  // Lock::ModuleEvaluationScope around module->Evaluate()). So the nested
  // require('trivial') never pumps, and entry's own top-level await is settled only
  // after its Evaluate() returns at depth 0, when the graph is coherent.
  PREAMBLE([&](Lock& js) {
    ResolveObserverImpl observer;
    CompilationObserver compilationObserver;

    ModuleBundle::BundleBuilder bundleBuilder(BASE);

    bundleBuilder.addEsmModule("entry",
        "import { v } from 'leaf';\n"
        "import 'pump';\n"
        "export default v;\n"_kjc,
        Module::Flags::MAIN);

    // Top-level await: leaf's evaluation promise fulfills on a later microtask.
    bundleBuilder.addEsmModule("leaf",
        "await Promise.resolve();\n"
        "export const v = 1;\n"_kjc);

    // pump (CJS): its evaluation performs a nested require(), which pumps the
    // microtask queue while entry is still kEvaluating.
    auto pumpSrc = kj::str("require('trivial');\n");
    bundleBuilder.addSyntheticModule(
        "pump", Module::newCjsStyleModuleHandler<TestType, TestIsolate_TypeWrapper>(pumpSrc));

    auto trivialSrc = kj::str("// nothing; require()-ing this pumps the microtask queue\n");
    bundleBuilder.addSyntheticModule(
        "trivial", Module::newCjsStyleModuleHandler<TestType, TestIsolate_TypeWrapper>(trivialSrc));

    auto registry = ModuleRegistry::Builder(BASE).add(bundleBuilder.finish()).finish();
    auto attached = registry->attachToIsolate(js, compilationObserver);

    js.tryCatch([&] {
      auto val = ModuleRegistry::resolve(js, "file:///entry", "default"_kjc);
      KJ_ASSERT(val.isNumber());
      auto num = KJ_ASSERT_NONNULL(val.tryCast<JsNumber>());
      KJ_ASSERT(KJ_ASSERT_NONNULL(num.value(js)) == 1.0);  // leaf's v
    }, [&](Value exception) { KJ_FAIL_ASSERT("resolve threw", kj::str(exception.getHandle(js))); });
  });
}

// ======================================================================================

KJ_TEST("A throwing module evaluation does not leak the evaluation depth") {
  // Companion to the kEvaluatingAsync regression test above. Each module->Evaluate()
  // is wrapped in a Lock::ModuleEvaluationScope (an evaluation-depth counter); the
  // loader only settles a pending top-level await when js.isEvaluatingModule() is
  // false (depth 0). This test guards the exception path: because the scope is RAII,
  // its destructor must run on both normal and exceptional exit -- otherwise the
  // depth would leak and the loader would treat itself as perpetually nested,
  // refusing to settle any subsequent top-level await.
  //
  // The boom module's top-level throws. Afterwards a *subsequent* top-level require
  // of a module whose top-level await settles synchronously must still succeed: if
  // the depth had leaked, isEvaluatingModule() would remain true and the loader
  // would throw the unsettled-top-level-await error instead of draining.
  PREAMBLE([&](Lock& js) {
    ResolveObserverImpl observer;
    CompilationObserver compilationObserver;

    ModuleBundle::BundleBuilder bundleBuilder(BASE);

    bundleBuilder.addEsmModule("boom", "throw new Error('boom');\n"_kjc, Module::Flags::MAIN);

    // Top-level await that settles within a single microtask drain. Requiring this at
    // the top level must pump and resolve -- which only works if the depth is back at 0.
    bundleBuilder.addEsmModule("after",
        "await Promise.resolve();\n"
        "export const ok = 1;\n"_kjc);

    auto registry = ModuleRegistry::Builder(BASE).add(bundleBuilder.finish()).finish();
    auto attached = registry->attachToIsolate(js, compilationObserver);

    bool threw = false;
    js.tryCatch([&] {
      ModuleRegistry::resolve(js, "file:///boom", "default"_kjc);
      JSG_FAIL_REQUIRE(Error, "Should have thrown");
    }, [&](Value exception) {
      threw = true;
      auto str = kj::str(exception.getHandle(js));
      KJ_ASSERT(str == "Error: boom", str);
    });
    KJ_ASSERT(threw);

    // A throwing evaluation must not leave the evaluation depth leaked: a subsequent
    // top-level require whose top-level await settles synchronously must resolve.
    js.tryCatch([&] {
      auto val = ModuleRegistry::resolve(js, "file:///after", "ok"_kjc);
      KJ_ASSERT(val.isNumber());
      auto num = KJ_ASSERT_NONNULL(val.tryCast<JsNumber>());
      KJ_ASSERT(KJ_ASSERT_NONNULL(num.value(js)) == 1.0);
    }, [&](Value exception) { KJ_FAIL_ASSERT("resolve threw", kj::str(exception.getHandle(js))); });
  });
}

// ======================================================================================

KJ_TEST("UNWRAP_DEFAULT returns namespace for bundle ESM, default for others") {
  PREAMBLE([&](Lock& js) {
    ResolveObserverImpl observer;
    CompilationObserver compilationObserver;

    // Synthetic module handlers borrow their source buffers.
    auto json = kj::str("{\"key\": \"value\"}");
    auto text = kj::str("hello world");
    ModuleBundle::BundleBuilder bundleBuilder(BASE);

    // Bundle ESM with named exports (no __cjsUnwrapDefault)
    bundleBuilder.addEsmModule(
        "esm-mod", "export default 42; export const name = 'esm';"_kjc, Module::Flags::MAIN);

    // Bundle ESM with __cjsUnwrapDefault convention
    bundleBuilder.addEsmModule(
        "esm-cjs", "export default 'unwrapped'; export const __cjsUnwrapDefault = true;"_kjc);

    // JSON synthetic module
    bundleBuilder.addSyntheticModule(
        "data.json", Module::newJsonModuleHandler(json.first(json.size())));

    // Text synthetic module
    bundleBuilder.addSyntheticModule(
        "data.txt", Module::newTextModuleHandler(text.first(text.size())));

    auto registry = ModuleRegistry::Builder(BASE).add(bundleBuilder.finish()).finish();
    auto attached = registry->attachToIsolate(js, compilationObserver);

    // Bundle ESM without __cjsUnwrapDefault: tryResolveModuleNamespace with UnwrapDefault::YES
    // returns the full namespace (has "default", "name" properties).
    js.tryCatch([&] {
      auto val = KJ_ASSERT_NONNULL(ModuleRegistry::tryResolveModuleNamespace(js, "file:///esm-mod",
          ResolveContext::Type::BUNDLE, ResolveContext::Source::REQUIRE, kj::none,
          modules::UnwrapDefault::YES));
      auto ns = KJ_ASSERT_NONNULL(val.tryCast<JsObject>());
      // The namespace should have both "default" and "name" properties.
      auto nameVal = ns.get(js, "name");
      KJ_ASSERT(!nameVal.isUndefined());
      KJ_ASSERT(kj::str(nameVal) == "esm");
      auto defaultVal = ns.get(js, "default");
      KJ_ASSERT(!defaultVal.isUndefined());
    }, [&](Value exception) { js.throwException(kj::mv(exception)); });

    // Bundle ESM with __cjsUnwrapDefault: returns the default export.
    js.tryCatch([&] {
      auto result = KJ_ASSERT_NONNULL(ModuleRegistry::tryResolveModuleNamespace(js,
          "file:///esm-cjs", ResolveContext::Type::BUNDLE, ResolveContext::Source::REQUIRE,
          kj::none, modules::UnwrapDefault::YES));
      KJ_ASSERT(kj::str(result) == "unwrapped");
    }, [&](Value exception) { js.throwException(kj::mv(exception)); });

    // JSON synthetic module: returns the parsed value (the default export).
    js.tryCatch([&] {
      auto result = KJ_ASSERT_NONNULL(ModuleRegistry::tryResolveModuleNamespace(js,
          "file:///data.json", ResolveContext::Type::BUNDLE, ResolveContext::Source::REQUIRE,
          kj::none, modules::UnwrapDefault::YES));
      // Should be the parsed JSON object, not the namespace.
      auto obj = KJ_ASSERT_NONNULL(result.tryCast<JsObject>());
      KJ_ASSERT(kj::str(obj.get(js, "key")) == "value");
    }, [&](Value exception) { js.throwException(kj::mv(exception)); });

    // Text synthetic module: returns the string value (the default export).
    js.tryCatch([&] {
      auto result = KJ_ASSERT_NONNULL(ModuleRegistry::tryResolveModuleNamespace(js,
          "file:///data.txt", ResolveContext::Type::BUNDLE, ResolveContext::Source::REQUIRE,
          kj::none, modules::UnwrapDefault::YES));
      // The default is a string — JsValue directly.
      KJ_ASSERT(kj::str(result) == "hello world");
    }, [&](Value exception) { js.throwException(kj::mv(exception)); });

    // Without UnwrapDefault, all modules return the namespace.
    js.tryCatch([&] {
      auto val = KJ_ASSERT_NONNULL(ModuleRegistry::tryResolveModuleNamespace(
          js, "file:///data.json", ResolveContext::Type::BUNDLE, ResolveContext::Source::REQUIRE));
      auto ns = KJ_ASSERT_NONNULL(val.tryCast<JsObject>());
      // Should have a "default" property (it's the namespace).
      KJ_ASSERT(!ns.get(js, "default").isUndefined());
    }, [&](Value exception) { js.throwException(kj::mv(exception)); });
  });
}

KJ_TEST("UNWRAP_DEFAULT honors module.exports, marker order, and builtin fallback") {
  PREAMBLE([&](Lock& js) {
    CompilationObserver compilationObserver;

    ModuleBundle::BundleBuilder bundleBuilder(BASE);

    // Node's official require(esm) mechanism: a string-named 'module.exports'
    // export controls the require() result.
    bundleBuilder.addEsmModule("mod-exports",
        "const impl = { hello: 1 };\n"
        "export { impl as 'module.exports' };\n"
        "export default 'not-this';\n"_kjc);

    // When both markers are present, __cjsUnwrapDefault wins (matching the
    // legacy registry's check order).
    bundleBuilder.addEsmModule("both-markers",
        "export const __cjsUnwrapDefault = true;\n"
        "const impl = 'module-exports-value';\n"
        "export { impl as 'module.exports' };\n"
        "export default 'default-value';\n"_kjc);

    // Builtin ESM with and without a default export.
    ModuleBundle::BuiltinBuilder builtinBuilder(ModuleBundle::BuiltinBuilder::Type::BUILTIN);
    static const auto kWithDefault = "test:with-default"_url;
    static const auto kNoDefault = "test:no-default"_url;
    builtinBuilder.addEsm(
        kWithDefault, "export default 'builtin-default'; export const extra = 1;"_kjc);
    builtinBuilder.addEsm(kNoDefault, "export const onlyNamed = 42;"_kjc);

    // Fallback-service ESM serves user code and behaves like bundle ESM.
    auto fallback = ModuleBundle::newFallbackBundle(
        [](const ResolveContext& context) -> kj::Maybe<kj::OneOf<kj::String, kj::Own<Module>>> {
      auto source = kj::heapArray<const char>(
          "export default 'fb-default'; export const named = 'fb';"_kj.asArray());
      return kj::Maybe<kj::OneOf<kj::String, kj::Own<Module>>>(
          Module::newEsm(context.normalizedSpecifier.clone(), Module::Type::FALLBACK,
              kj::arc<OwnedAscii>(kj::mv(source))));
    });

    auto registry = ModuleRegistry::Builder(BASE, ModuleRegistry::Builder::Options::ALLOW_FALLBACK)
                        .add(bundleBuilder.finish())
                        .add(builtinBuilder.finish())
                        .add(kj::mv(fallback))
                        .finish();
    auto attached = registry->attachToIsolate(js, compilationObserver);

    static constexpr auto req = [](Lock& js, kj::StringPtr spec) {
      return KJ_ASSERT_NONNULL(
          ModuleRegistry::tryResolveModuleNamespace(js, spec, ResolveContext::Type::BUNDLE,
              ResolveContext::Source::REQUIRE, kj::none, modules::UnwrapDefault::YES));
    };

    JSG_TRY(js) {
      // The 'module.exports' named export controls the result.
      JsValue me = req(js, "file:///mod-exports");
      auto meObj = KJ_ASSERT_NONNULL(me.tryCast<JsObject>());
      KJ_ASSERT(kj::str(meObj.get(js, "hello")) == "1");

      // __cjsUnwrapDefault wins over 'module.exports'.
      KJ_ASSERT(kj::str(req(js, "file:///both-markers")) == "default-value");

      // Builtin ESM with a default export unwraps it.
      KJ_ASSERT(kj::str(req(js, "test:with-default")) == "builtin-default");

      // Builtin ESM without a default export falls back to the namespace
      // rather than producing undefined.
      JsValue nd = req(js, "test:no-default");
      auto ndObj = KJ_ASSERT_NONNULL(nd.tryCast<JsObject>());
      KJ_ASSERT(kj::str(ndObj.get(js, "onlyNamed")) == "42");

      // Fallback-service ESM yields the namespace, like bundle ESM.
      JsValue fb = req(js, "file:///fb-esm");
      auto fbObj = KJ_ASSERT_NONNULL(fb.tryCast<JsObject>());
      KJ_ASSERT(kj::str(fbObj.get(js, "named")) == "fb");
      KJ_ASSERT(kj::str(fbObj.get(js, "default")) == "fb-default");
    }
    JSG_CATCH(exception) {
      js.throwException(kj::mv(exception));
    }
  });
}

// ======================================================================================

KJ_TEST("A URL can hold distinct modules per context type (bundle shadow vs builtin)") {
  PREAMBLE([&](Lock& js) {
    CompilationObserver compilationObserver;

    // A worker-bundle module that shadows a builtin name. It also performs a
    // dynamic import so the referrer probe is exercised for a URL that has
    // entries under multiple context types.
    ModuleBundle::BundleBuilder bundleBuilder(BASE);
    bundleBuilder.addEsmModule(
        "test:thing", "export default 'shadow'; export const p = import('file:///dep');"_kjc);
    bundleBuilder.addEsmModule("dep", "export default 'dep';"_kjc);

    // ...and the real builtin registered under the very same URL.
    ModuleBundle::BuiltinBuilder builtinBuilder(ModuleBundle::BuiltinBuilder::Type::BUILTIN);
    static const auto kThing = "test:thing"_url;
    builtinBuilder.addEsm(kThing, "export default 'builtin';"_kjc);

    // An unshadowed builtin, for the shared-instantiation direction.
    static const auto kShared = "test:shared"_url;
    builtinBuilder.addEsm(kShared, "export default 'shared';"_kjc);

    auto registry = ModuleRegistry::Builder(BASE)
                        .add(bundleBuilder.finish())
                        .add(builtinBuilder.finish())
                        .finish();
    auto attached = registry->attachToIsolate(js, compilationObserver);

    JSG_TRY(js) {
      // BUNDLE context resolves the shadow (bundle wins user-facing resolution).
      auto fromBundle =
          ModuleRegistry::resolve(js, "test:thing", "default"_kjc, ResolveContext::Type::BUNDLE);
      KJ_ASSERT(kj::str(fromBundle) == "shadow");

      // PUBLIC_BUILTIN context resolves the real builtin, even though the
      // shadow's entry for the same URL is already cached. (This is the
      // process.getBuiltinModule() path: it must never return a user module.)
      auto fromBuiltin = ModuleRegistry::resolve(
          js, "test:thing", "default"_kjc, ResolveContext::Type::PUBLIC_BUILTIN);
      KJ_ASSERT(kj::str(fromBuiltin) == "builtin");

      // Both entries coexist stably; repeated resolution stays correct in both
      // directions.
      KJ_ASSERT(kj::str(ModuleRegistry::resolve(
                    js, "test:thing", "default"_kjc, ResolveContext::Type::BUNDLE)) == "shadow");
      KJ_ASSERT(kj::str(ModuleRegistry::resolve(js, "test:thing", "default"_kjc,
                    ResolveContext::Type::PUBLIC_BUILTIN)) == "builtin");

      // The shadow's dynamic import resolved: the referrer probe identified the
      // bundle-typed entry for the shared URL.
      auto p = ModuleRegistry::resolve(js, "test:thing", "p"_kjc, ResolveContext::Type::BUNDLE);
      auto promise = v8::Local<v8::Value>(p).As<v8::Promise>();
      js.runMicrotasks();
      KJ_ASSERT(promise->State() == v8::Promise::kFulfilled);
      auto ns = JsObject(promise->Result().As<v8::Object>());
      KJ_ASSERT(kj::str(ns.get(js, "default")) == "dep");

      // Conversely, when the same specifier resolves to the same definition
      // through different context types, the instantiation is shared: BUNDLE
      // and PUBLIC_BUILTIN resolution of an unshadowed builtin yield the very
      // same namespace object. This is the process.getBuiltinModule() identity
      // guarantee, and it also keeps builtins per-isolate singletons (their
      // module-level state must not be duplicated).
      JsValue sharedViaBundle = KJ_ASSERT_NONNULL(ModuleRegistry::tryResolveModuleNamespace(
          js, "test:shared", ResolveContext::Type::BUNDLE));
      JsValue sharedViaBuiltin = KJ_ASSERT_NONNULL(ModuleRegistry::tryResolveModuleNamespace(
          js, "test:shared", ResolveContext::Type::PUBLIC_BUILTIN));
      KJ_ASSERT(sharedViaBundle == sharedViaBuiltin);
    }
    JSG_CATCH(exception) {
      js.throwException(kj::mv(exception));
    }
  });
}

// ======================================================================================

KJ_TEST("REQUIRE_ESM rejects non-ESM entry points before evaluation") {
  PREAMBLE([&](Lock& js) {
    CompilationObserver compilationObserver;

    auto json = kj::str("{\"key\": \"value\"}");
    ModuleBundle::BundleBuilder bundleBuilder(BASE);
    bundleBuilder.addEsmModule("main", "export default 42;"_kjc);

    bundleBuilder.addSyntheticModule(
        "data.json", Module::newJsonModuleHandler(json.first(json.size())));

    // A CJS-style synthetic module whose evaluation would throw. REQUIRE_ESM must
    // reject it before evaluation, so the throw must never run.
    bool cjsEvaluated = false;
    bundleBuilder.addSyntheticModule("cjs-boom",
        [&cjsEvaluated](Lock& js, const jsg::Url& id, const Module::ModuleNamespace& ns,
            const CompilationObserver&) -> bool {
      cjsEvaluated = true;
      js.v8Isolate->ThrowError(js.str("boom"_kj));
      return false;
    });

    auto registry = ModuleRegistry::Builder(BASE).add(bundleBuilder.finish()).finish();
    auto attached = registry->attachToIsolate(js, compilationObserver);

    // An ESM entry point resolves normally with RequireEsm::YES.
    JSG_TRY(js) {
      auto val = KJ_ASSERT_NONNULL(ModuleRegistry::tryResolveModuleNamespace(js, "file:///main",
          ResolveContext::Type::BUNDLE, ResolveContext::Source::INTERNAL, kj::none,
          modules::UnwrapDefault::NO, RequireEsm::YES));
      auto ns = KJ_ASSERT_NONNULL(val.tryCast<JsObject>());
      KJ_ASSERT(!ns.get(js, "default").isUndefined());
    }
    JSG_CATCH(exception) {
      js.throwException(kj::mv(exception));
    }

    // A synthetic (JSON) entry point is rejected with the same TypeError the legacy
    // registry produces ("Main module must be an ES module.").
    JSG_TRY(js) {
      ModuleRegistry::tryResolveModuleNamespace(js, "file:///data.json",
          ResolveContext::Type::BUNDLE, ResolveContext::Source::INTERNAL, kj::none,
          modules::UnwrapDefault::NO, RequireEsm::YES);
      KJ_FAIL_ASSERT("resolving a JSON module with RequireEsm::YES should have thrown");
    }
    JSG_CATCH(exception) {
      auto str = kj::str(JsValue(exception.getHandle(js)));
      KJ_ASSERT(str == "TypeError: Main module must be an ES module.", str);
    }

    // The rejection happens before evaluation: the CJS module's evaluation
    // callback must not have run.
    JSG_TRY(js) {
      ModuleRegistry::tryResolveModuleNamespace(js, "file:///cjs-boom",
          ResolveContext::Type::BUNDLE, ResolveContext::Source::INTERNAL, kj::none,
          modules::UnwrapDefault::NO, RequireEsm::YES);
      KJ_FAIL_ASSERT("resolving a CJS module with RequireEsm::YES should have thrown");
    }
    JSG_CATCH(exception) {
      auto str = kj::str(JsValue(exception.getHandle(js)));
      KJ_ASSERT(str == "TypeError: Main module must be an ES module.", str);
    }
    KJ_ASSERT(!cjsEvaluated);

    // Without RequireEsm, the same synthetic module resolves normally.
    JSG_TRY(js) {
      auto val = KJ_ASSERT_NONNULL(ModuleRegistry::tryResolveModuleNamespace(
          js, "file:///data.json", ResolveContext::Type::BUNDLE));
      auto ns = KJ_ASSERT_NONNULL(val.tryCast<JsObject>());
      KJ_ASSERT(!ns.get(js, "default").isUndefined());
    }
    JSG_CATCH(exception) {
      js.throwException(kj::mv(exception));
    }
  });
}

// ======================================================================================
KJ_TEST("Resolution occurs relative to the referrer") {

  CompilationObserver compilationObserver;
  ModuleRegistry::Builder registryBuilder(BASE);

  ModuleBundle::BundleBuilder builder(BASE);
  builder.addSyntheticModule("foo/bar", Module::newDataModuleHandler(nullptr));
  builder.addSyntheticModule("bar", Module::newDataModuleHandler(nullptr));

  // The base URL of the referrer is file:///foo/ ... so in each of the
  // following cases, the specifier should be resolved relative to that.
  // For instance, 'bar' should resolve as file:///foo/bar, while '../bar'
  // should resolve as file:///bar

  auto bar = kj::str("export * as abc from 'bar';"           // file:///foo/bar
                     "export * as def from './bar';"         // file:///foo/bar
                     "export * as ghi from '../bar';"        // file:///bar
                     "export * as jkl from '/bar';"          // file:///bar
                     "export * as lmn from '../foo/bar';");  // file:///foo/bar
  builder.addEsmModule("foo/", bar);

  auto registry = registryBuilder.add(builder.finish()).finish();

  PREAMBLE([&](Lock& js) {
    auto attached = registry->attachToIsolate(js, compilationObserver);

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
    }, [&](Value exception) { js.throwException(kj::mv(exception)); });
  });
}

// ======================================================================================

KJ_TEST("Building a bundle from a capnp description works") {

  capnp::MallocMessageBuilder builder;
  auto bundle = builder.initRoot<workerd::jsg::Bundle>();

  auto modules = bundle.initModules(3);

  auto str = kj::str("export default 1+1;");
  auto wasm = kj::heapArray<kj::byte>({
    0x00,
    0x61,
    0x73,
    0x6d,
    0x01,
    0x00,
    0x00,
    0x00,
    0x01,
    0x07,
    0x01,
    0x60,
    0x02,
    0x7f,
    0x7f,
    0x01,
    0x7f,
    0x03,
    0x02,
    0x01,
    0x00,
    0x07,
    0x07,
    0x01,
    0x03,
    0x61,
    0x64,
    0x64,
    0x00,
    0x00,
    0x0a,
    0x09,
    0x01,
    0x07,
    0x00,
    0x20,
    0x00,
    0x20,
    0x01,
    0x6a,
    0x0b,
  });
  auto data = kj::heapArray<kj::byte>({1, 2, 3});

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
  ModuleBundle::getBuiltInBundleFromCapnp(bundleBuilder, bundle.asReader());
  auto moduleBundle = bundleBuilder.finish();

  {
    const auto foo = "foo:bar"_url;
    ResolveContext context{
      .type = ResolveContext::Type::BUILTIN,
      .source = ResolveContext::Source::INTERNAL,
      .normalizedSpecifier = foo,
      .referrerNormalizedSpecifier = BASE,
    };
    auto resolved = KJ_ASSERT_NONNULL(moduleBundle->lookup(context));
    auto& module = KJ_ASSERT_NONNULL(resolved.module);

    KJ_ASSERT(module.id() == foo);
  }

  {
    const auto bar = "foo:baz"_url;
    ResolveContext context{
      .type = ResolveContext::Type::BUILTIN,
      .source = ResolveContext::Source::INTERNAL,
      .normalizedSpecifier = bar,
      .referrerNormalizedSpecifier = BASE,
    };
    auto resolved = KJ_ASSERT_NONNULL(moduleBundle->lookup(context));
    auto& module = KJ_ASSERT_NONNULL(resolved.module);
    KJ_ASSERT(module.id() == bar);
  }

  {
    const auto qux = "foo:qux"_url;
    ResolveContext context{
      .type = ResolveContext::Type::BUILTIN,
      .source = ResolveContext::Source::INTERNAL,
      .normalizedSpecifier = qux,
      .referrerNormalizedSpecifier = BASE,
    };
    auto resolved = KJ_ASSERT_NONNULL(moduleBundle->lookup(context));
    auto& module = KJ_ASSERT_NONNULL(resolved.module);
    KJ_ASSERT(module.id() == qux);
  }

  PREAMBLE([&](Lock& js) {
    CompilationObserver compilationObserver;
    auto registry = ModuleRegistry::Builder(BASE).add(kj::mv(moduleBundle)).finish();

    auto attached = registry->attachToIsolate(js, compilationObserver);

    // The foo:bar module is interpreted as an ESM
    js.tryCatch([&] {
      auto val = ModuleRegistry::resolve(js, "foo:bar");
      KJ_ASSERT(val.isNumber());
    }, [&](Value exception) { js.throwException(kj::mv(exception)); });
  });
}

// ======================================================================================

KJ_TEST("ESM compile cache is reused across isolates") {
  CountingCompilationObserver observer;
  ModuleBundle::BundleBuilder bundleBuilder(BASE);
  bundleBuilder.addEsmModule("foo", "export default 123;"_kjc);
  auto registry = ModuleRegistry::Builder(BASE).add(bundleBuilder.finish()).finish();

  {
    PREAMBLE([&](Lock& js) {
      auto attached = registry->attachToIsolate(js, observer);
      KJ_ASSERT(ModuleRegistry::resolve(js, "foo").isNumber());
    });
  }

  auto first = observer.getCounts();
  KJ_ASSERT(first.cacheGenerated == 1);
  KJ_ASSERT(first.cacheFound == 0);
  KJ_ASSERT(first.cacheRejected == 0);
  KJ_ASSERT(first.cacheGenerationFailed == 0);

  {
    PREAMBLE([&](Lock& js) {
      auto attached = registry->attachToIsolate(js, observer);
      KJ_ASSERT(ModuleRegistry::resolve(js, "foo").isNumber());
    });
  }

  auto second = observer.getCounts();
  KJ_ASSERT(second.cacheGenerated == 1);
  KJ_ASSERT(second.cacheFound == 1);
  KJ_ASSERT(second.cacheRejected == 0);
  KJ_ASSERT(second.cacheGenerationFailed == 0);
}

// ======================================================================================

KJ_TEST("Wasm compile cache is reused across isolates") {
  CountingCompilationObserver observer;
  auto wasm = makeTestWasm();
  ModuleBundle::BundleBuilder bundleBuilder(BASE);
  bundleBuilder.addWasmModule("wasm", wasm);
  auto registry = ModuleRegistry::Builder(BASE).add(bundleBuilder.finish()).finish();

  {
    PREAMBLE([&](Lock& js) {
      auto attached = registry->attachToIsolate(js, observer);
      KJ_ASSERT(ModuleRegistry::resolve(js, "wasm").isWasmModuleObject());
    });
  }

  auto first = observer.getCounts();
  KJ_ASSERT(first.wasmCompiled == 1);
  KJ_ASSERT(first.wasmFromCache == 0);

  {
    PREAMBLE([&](Lock& js) {
      auto attached = registry->attachToIsolate(js, observer);
      KJ_ASSERT(ModuleRegistry::resolve(js, "wasm").isWasmModuleObject());
    });
  }

  auto second = observer.getCounts();
  KJ_ASSERT(second.wasmCompiled == 1);
  KJ_ASSERT(second.wasmFromCache == 1);
}

// ======================================================================================

KJ_TEST("Using a registry from multiple threads works") {

  ModuleBundle::BundleBuilder bundleBuilder(BASE);
  // The non-ASCII literal forces the shared UTF-8 -> Latin-1 source transcode,
  // so this test also exercises its cross-thread once-init: all five isolates
  // race on the same kj::Lazy encoding and the same compile cache
  // (cacheGenerated == 1 below).
  static const auto foo =
      "export default 123; const s = 'caf\xc3\xa9'; for (let n = 0; n < 100000; n++) {}"_kjc;
  bundleBuilder.addEsmModule("foo", foo);

  auto registry = ModuleRegistry::Builder(BASE).add(bundleBuilder.finish()).finish();
  CountingCompilationObserver compilationObserver;

  kj::MutexGuarded<uint> successfulResolutions(0);

  static const auto makeRunnable = [](kj::Arc<workerd::jsg::modules::ModuleRegistry> registry,
                                       const CountingCompilationObserver& compilationObserver,
                                       kj::MutexGuarded<uint>& successfulResolutions) {
    return [registry = kj::mv(registry), &compilationObserver, &successfulResolutions]() mutable {
      PREAMBLE([&](Lock& js) {
        auto attached = registry->attachToIsolate(js, compilationObserver);
        js.tryCatch([&] {
          auto val = ModuleRegistry::resolve(js, "file:///foo");
          KJ_ASSERT(val.isNumber());
        }, [&](Value exception) { js.throwException(kj::mv(exception)); });
      });
      auto count = successfulResolutions.lockExclusive();
      ++*count;
    };
  };

  {
    kj::Thread thread1(makeRunnable(registry.addRef(), compilationObserver, successfulResolutions));
    kj::Thread thread2(makeRunnable(registry.addRef(), compilationObserver, successfulResolutions));
    kj::Thread thread3(makeRunnable(registry.addRef(), compilationObserver, successfulResolutions));
    kj::Thread thread4(makeRunnable(registry.addRef(), compilationObserver, successfulResolutions));
    kj::Thread thread5(makeRunnable(registry.addRef(), compilationObserver, successfulResolutions));
  }

  KJ_ASSERT(*successfulResolutions.lockShared() == 5);
  auto counts = compilationObserver.getCounts();
  KJ_ASSERT(counts.cacheGenerated == 1);
  KJ_ASSERT(counts.cacheFound <= 4);
  KJ_ASSERT(counts.cacheRejected == 0);
  KJ_ASSERT(counts.cacheGenerationFailed == 0);
}

// ======================================================================================

KJ_TEST("Fallback service can see original raw specifier if provided") {

  CompilationObserver compilationObserver;
  ModuleRegistry::Builder builder(BASE, ModuleRegistry::Builder::Options::ALLOW_FALLBACK);
  auto rawSpecifier = "nothing"_kjc;
  const auto id = "file:///nothing"_url;

  bool called = false;

  builder.add(ModuleBundle::newFallbackBundle([&](const ResolveContext& context) {
    KJ_ASSERT(context.rawSpecifier == rawSpecifier);
    KJ_ASSERT(context.normalizedSpecifier == id);
    KJ_ASSERT(context.referrerNormalizedSpecifier == BASE);
    called = true;
    return kj::none;
  }));

  auto registry = builder.finish();

  ResolveContext context = {
    .type = ResolveContext::Type::BUNDLE,
    .source = ResolveContext::Source::INTERNAL,
    .normalizedSpecifier = id,
    .referrerNormalizedSpecifier = BASE,
    .rawSpecifier = rawSpecifier,
  };

  KJ_ASSERT(registry->lookup(context, noopResolveObserver) == kj::none);
  KJ_ASSERT(called);
}

// ======================================================================================

KJ_TEST("Fallback service can return a module with a different specifier") {

  CompilationObserver compilationObserver;
  ModuleRegistry::Builder builder(BASE, ModuleRegistry::Builder::Options::ALLOW_FALLBACK);
  auto rawSpecifier = "nothing"_kjc;
  const auto id = "file:///nothing"_url;
  const auto url = "file:///different"_url;

  int called = 0;

  builder.add(ModuleBundle::newFallbackBundle([&](const ResolveContext& context) {
    called++;
    kj::Own<Module> mod = Module::newSynthetic(
        url.clone(), Module::Type::FALLBACK, Module::newDataModuleHandler(nullptr));
    return kj::Maybe<kj::OneOf<kj::String, kj::Own<Module>>>(kj::mv(mod));
  }));

  auto registry = builder.finish();

  ResolveContext context = {
    .type = ResolveContext::Type::BUNDLE,
    .source = ResolveContext::Source::INTERNAL,
    .normalizedSpecifier = id,
    .referrerNormalizedSpecifier = BASE,
    .rawSpecifier = rawSpecifier,
  };

  auto& module1 = KJ_ASSERT_NONNULL(registry->lookup(context, noopResolveObserver));

  ResolveContext context2 = {
    .type = ResolveContext::Type::BUNDLE,
    .source = ResolveContext::Source::INTERNAL,
    .normalizedSpecifier = url,
    .referrerNormalizedSpecifier = BASE,
    .rawSpecifier = rawSpecifier,
  };

  auto& module2 = KJ_ASSERT_NONNULL(registry->lookup(context2, noopResolveObserver));

  auto& module3 = KJ_ASSERT_NONNULL(registry->lookup(context, noopResolveObserver));

  // Both specifiers should resolve to the same module so the called count should be 1.
  KJ_ASSERT(called == 1);
  KJ_ASSERT(module1.id() == url);
  KJ_ASSERT(&module1 == &module2);
  KJ_ASSERT(&module2 == &module3);
}

// ======================================================================================

KJ_TEST("Fallback service CommonJS module source outlives the resolve callback") {
  // newCjsStyleModuleHandler retains the source as a non-owning kj::StringPtr, and does
  // not read it until the module is first evaluated -- which happens on the resolve
  // below, long after the resolve callback has returned. A callback that builds a module
  // from storage it owns locally must therefore attach that storage to the Module, as
  // WorkerdApi's fallback callback does for a CommonJS module returned by a fallback
  // service.
  //
  // The buffer below is freed when the callback returns unless it is attached, so
  // dropping the attachment makes this a heap-use-after-free. Only ASAN observes that;
  // the assertions themselves just confirm the module still evaluates.
  PREAMBLE([&](Lock& js) {
    CompilationObserver compilationObserver;

    ModuleRegistry::Builder builder(BASE, ModuleRegistry::Builder::Options::ALLOW_FALLBACK);

    builder.add(ModuleBundle::newFallbackBundle(
        [](const ResolveContext& context) -> kj::Maybe<kj::OneOf<kj::String, kj::Own<Module>>> {
      auto ownedSource = kj::str("exports.ok = 123;");
      auto sourcePtr = ownedSource.asPtr();

      kj::Own<Module> mod =
          Module::newSynthetic(context.normalizedSpecifier.clone(), Module::Type::FALLBACK,
              Module::newCjsStyleModuleHandler<TestType, TestIsolate_TypeWrapper>(sourcePtr));
      return kj::Maybe<kj::OneOf<kj::String, kj::Own<Module>>>(mod.attach(kj::mv(ownedSource)));
    }));

    auto registry = builder.finish();
    auto attached = registry->attachToIsolate(js, compilationObserver);

    auto value = ModuleRegistry::resolve(js, "file:///fallback-cjs", "default"_kjc);
    KJ_ASSERT(value.tryCast<JsObject>() != kj::none);
  });
}

// ======================================================================================

KJ_TEST("Fallback redirect restarts module resolution") {
  const auto source = "file:///source"_url;
  const auto target = "file:///target"_url;
  uint calls = 0;

  auto fallback = ModuleBundle::newFallbackBundle(
      [&](const ResolveContext& context) -> kj::Maybe<kj::OneOf<kj::String, kj::Own<Module>>> {
    ++calls;
    if (context.normalizedSpecifier == source) {
      return kj::Maybe<kj::OneOf<kj::String, kj::Own<Module>>>(
          kj::OneOf<kj::String, kj::Own<Module>>(kj::str(target.getHref())));
    }
    if (context.normalizedSpecifier == target) {
      return kj::Maybe<kj::OneOf<kj::String, kj::Own<Module>>>(Module::newSynthetic(
          target.clone(), Module::Type::FALLBACK, Module::newDataModuleHandler(nullptr)));
    }
    return kj::none;
  });

  auto registry = ModuleRegistry::Builder(BASE, ModuleRegistry::Builder::Options::ALLOW_FALLBACK)
                      .add(kj::mv(fallback))
                      .finish();
  ResolveContext context{
    .type = ResolveContext::Type::BUNDLE,
    .source = ResolveContext::Source::INTERNAL,
    .normalizedSpecifier = source,
    .referrerNormalizedSpecifier = BASE,
  };

  auto& module = KJ_ASSERT_NONNULL(registry->lookup(context, noopResolveObserver));
  KJ_ASSERT(module.id() == target);
  KJ_ASSERT(calls == 2);
}

// ======================================================================================

KJ_TEST("Fallback redirect cycles resolve to module-not-found") {
  const auto first = "file:///first"_url;
  const auto second = "file:///second"_url;
  uint calls = 0;

  auto fallback = ModuleBundle::newFallbackBundle(
      [&](const ResolveContext& context) -> kj::Maybe<kj::OneOf<kj::String, kj::Own<Module>>> {
    ++calls;
    if (context.normalizedSpecifier == first) {
      return kj::Maybe<kj::OneOf<kj::String, kj::Own<Module>>>(
          kj::OneOf<kj::String, kj::Own<Module>>(kj::str(second.getHref())));
    }
    if (context.normalizedSpecifier == second) {
      return kj::Maybe<kj::OneOf<kj::String, kj::Own<Module>>>(
          kj::OneOf<kj::String, kj::Own<Module>>(kj::str(first.getHref())));
    }
    return kj::none;
  });

  auto registry = ModuleRegistry::Builder(BASE, ModuleRegistry::Builder::Options::ALLOW_FALLBACK)
                      .add(kj::mv(fallback))
                      .finish();
  ResolveContext context{
    .type = ResolveContext::Type::BUNDLE,
    .source = ResolveContext::Source::INTERNAL,
    .normalizedSpecifier = first,
    .referrerNormalizedSpecifier = BASE,
  };

  // first -> second -> first is a redirect cycle: the resolution visits each
  // specifier once and then fails cleanly instead of recursing forever.
  KJ_ASSERT(registry->lookup(context, noopResolveObserver) == kj::none);
  KJ_ASSERT(calls == 2);
}

// ======================================================================================

KJ_TEST("Fallback redirect chains are followed to the final module") {
  const auto a = "file:///a"_url;
  const auto b = "file:///b"_url;
  const auto c = "file:///c"_url;
  uint calls = 0;

  auto fallback = ModuleBundle::newFallbackBundle(
      [&](const ResolveContext& context) -> kj::Maybe<kj::OneOf<kj::String, kj::Own<Module>>> {
    ++calls;
    if (context.normalizedSpecifier == a) {
      return kj::Maybe<kj::OneOf<kj::String, kj::Own<Module>>>(
          kj::OneOf<kj::String, kj::Own<Module>>(kj::str(b.getHref())));
    }
    if (context.normalizedSpecifier == b) {
      return kj::Maybe<kj::OneOf<kj::String, kj::Own<Module>>>(
          kj::OneOf<kj::String, kj::Own<Module>>(kj::str(c.getHref())));
    }
    if (context.normalizedSpecifier == c) {
      return kj::Maybe<kj::OneOf<kj::String, kj::Own<Module>>>(Module::newSynthetic(
          c.clone(), Module::Type::FALLBACK, Module::newDataModuleHandler(nullptr)));
    }
    return kj::none;
  });

  auto registry = ModuleRegistry::Builder(BASE, ModuleRegistry::Builder::Options::ALLOW_FALLBACK)
                      .add(kj::mv(fallback))
                      .finish();
  ResolveContext context{
    .type = ResolveContext::Type::BUNDLE,
    .source = ResolveContext::Source::INTERNAL,
    .normalizedSpecifier = a,
    .referrerNormalizedSpecifier = BASE,
  };

  // a -> b -> c -> module: the whole chain is followed.
  auto& module = KJ_ASSERT_NONNULL(registry->lookup(context, noopResolveObserver));
  KJ_ASSERT(module.id() == c);
  KJ_ASSERT(calls == 3);

  // The chain was cached as aliases, so resolving the entry specifier again
  // walks the cached aliases without consulting the fallback service again.
  auto& again = KJ_ASSERT_NONNULL(registry->lookup(context, noopResolveObserver));
  KJ_ASSERT(&again == &module);
  KJ_ASSERT(calls == 3);
}

// ======================================================================================

KJ_TEST("Fallback specifiers resolving to an already-stored module id are aliased") {
  const auto a = "file:///a"_url;
  const auto b = "file:///b"_url;
  const auto real = "file:///real"_url;
  uint calls = 0;

  // The fallback service returns, for two different specifiers, a module
  // whose canonical id is the same (e.g. two paths mapping to one file).
  auto fallback = ModuleBundle::newFallbackBundle(
      [&](const ResolveContext& context) -> kj::Maybe<kj::OneOf<kj::String, kj::Own<Module>>> {
    ++calls;
    if (context.normalizedSpecifier == a || context.normalizedSpecifier == b) {
      return kj::Maybe<kj::OneOf<kj::String, kj::Own<Module>>>(Module::newSynthetic(
          real.clone(), Module::Type::FALLBACK, Module::newDataModuleHandler(nullptr)));
    }
    return kj::none;
  });

  auto registry = ModuleRegistry::Builder(BASE, ModuleRegistry::Builder::Options::ALLOW_FALLBACK)
                      .add(kj::mv(fallback))
                      .finish();
  ResolveContext contextA{
    .type = ResolveContext::Type::BUNDLE,
    .source = ResolveContext::Source::INTERNAL,
    .normalizedSpecifier = a,
    .referrerNormalizedSpecifier = BASE,
  };
  ResolveContext contextB{
    .type = ResolveContext::Type::BUNDLE,
    .source = ResolveContext::Source::INTERNAL,
    .normalizedSpecifier = b,
    .referrerNormalizedSpecifier = BASE,
  };

  auto& moduleA = KJ_ASSERT_NONNULL(registry->lookup(contextA, noopResolveObserver));
  KJ_ASSERT(moduleA.id() == real);
  KJ_ASSERT(calls == 1);

  // The second specifier resolves to a module whose id is already stored:
  // the stored module is reused (not rejected, and not duplicated).
  auto& moduleB = KJ_ASSERT_NONNULL(registry->lookup(contextB, noopResolveObserver));
  KJ_ASSERT(&moduleB == &moduleA);
  KJ_ASSERT(calls == 2);

  // And the alias recorded for the second specifier makes later lookups
  // resolve without consulting the fallback service again.
  auto& moduleB2 = KJ_ASSERT_NONNULL(registry->lookup(contextB, noopResolveObserver));
  KJ_ASSERT(&moduleB2 == &moduleA);
  KJ_ASSERT(calls == 2);
}

// ======================================================================================

KJ_TEST("Percent-encoding in specifiers is normalized properly") {

  CompilationObserver compilationObserver;

  ModuleBundle::BundleBuilder builder(BASE);

  // A specifier might have percent-encoded characters. We want those to be normalized
  // so that they are matched correctly. For instance, %66oo%2fbar should be normalized
  // to foo%2Fbar, and %66oo/bar should be normalized to foo/bar. Specifically, characters
  // that generally do not need to be percent-encoded should be normalized to their
  // unencoded form, while characters that need percent encoded should be normalized
  // to their capitalized percent-encoded form (e.g. %2f becomes %2F). This ensures that
  // when these different forms are used to import they will resolve to the expected
  // module.

  builder.addSyntheticModule("foo%2fbar", Module::newDataModuleHandler(nullptr));
  builder.addSyntheticModule("foo/bar", Module::newDataModuleHandler(nullptr));

  auto foo = kj::str("export { default as abc } from 'foo%2fbar';"
                     "export { default as def } from 'foo/bar';"
                     "export { default as ghi } from '%66oo/bar';"
                     "export { default as jkl } from '%66oo%2fbar';");
  builder.addEsmModule("foo", foo);

  auto registry = ModuleRegistry::Builder(BASE).add(builder.finish()).finish();

  PREAMBLE([&](Lock& js) {
    auto attached = registry->attachToIsolate(js, compilationObserver);

    js.tryCatch([&] {
      auto abc = ModuleRegistry::resolve(js, "foo", "abc"_kjc);
      auto def = ModuleRegistry::resolve(js, "foo", "def"_kjc);
      auto ghi = ModuleRegistry::resolve(js, "foo", "ghi"_kjc);
      auto jkl = ModuleRegistry::resolve(js, "foo", "jkl"_kjc);

      KJ_ASSERT(abc.strictEquals(jkl));
      KJ_ASSERT(def.strictEquals(ghi));
      KJ_ASSERT(!abc.strictEquals(def));
    }, [&](Value exception) { js.throwException(kj::mv(exception)); });
  });
}

// ======================================================================================

KJ_TEST("Aliased modules (import maps) work") {

  CompilationObserver compilationObserver;
  ModuleBundle::BundleBuilder builder(BASE);

  builder.addSyntheticModule("http://example/foo", Module::newDataModuleHandler(nullptr));
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
  builder.addEsmModule("qux", src);

  auto registry = ModuleRegistry::Builder(BASE).add(builder.finish()).finish();

  ResolveContext contextBar{
    .type = ResolveContext::Type::BUNDLE,
    .source = ResolveContext::Source::INTERNAL,
    .normalizedSpecifier = "file:///bar"_url,
    .referrerNormalizedSpecifier = BASE,
  };

  ResolveContext contextFoo{
    .type = ResolveContext::Type::BUNDLE,
    .source = ResolveContext::Source::INTERNAL,
    .normalizedSpecifier = "http://example/foo"_url,
    .referrerNormalizedSpecifier = BASE,
  };

  auto& bar = KJ_ASSERT_NONNULL(registry->lookup(contextBar, noopResolveObserver));
  auto& foo = KJ_ASSERT_NONNULL(registry->lookup(contextFoo, noopResolveObserver));

  // The aliases resolve to the same underlying module...
  KJ_ASSERT(&bar == &foo);

  PREAMBLE([&](Lock& js) {
    auto attached = registry->attachToIsolate(js, compilationObserver);

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
    }, [&](Value exception) { js.throwException(kj::mv(exception)); });
  });
}

// ======================================================================================

KJ_TEST("Reserved protocols cannot be used in bundle module names") {
  ModuleBundle::BundleBuilder builder(BASE);

  KJ_EXPECT_THROW_MESSAGE("The data: protocol cannot be used in module bundles",
      (void)builder.addEsmModule("data:text/javascript,export default 1", ""_kjc));
  KJ_EXPECT_THROW_MESSAGE("The cloudflare: protocol is reserved",
      (void)builder.addEsmModule("cloudflare:test", ""_kjc));
  KJ_EXPECT_THROW_MESSAGE(
      "The workerd: protocol is reserved", (void)builder.addEsmModule("workerd:test", ""_kjc));
  KJ_EXPECT_THROW_MESSAGE("The data: protocol cannot be used in module bundles",
      (void)builder.alias("alias", "data:text/javascript,export default 1"));
  KJ_EXPECT_THROW_MESSAGE("The data: protocol cannot be used in module bundles",
      (void)builder.alias("data:text/javascript,export default 1", "target"));
}

// ======================================================================================

KJ_TEST("Import attributes are deliberately not part of the module cache key") {
  const auto id = "file:///data.json"_url;
  auto module =
      Module::newSynthetic(id.clone(), Module::Type::BUNDLE, Module::newDataModuleHandler(nullptr),
          nullptr, Module::Flags::NONE, Module::ContentType::JSON);

  ResolveContext withoutAttributes{
    .type = ResolveContext::Type::BUNDLE,
    .source = ResolveContext::Source::INTERNAL,
    .normalizedSpecifier = id,
    .referrerNormalizedSpecifier = BASE,
  };
  ResolveContext withAttributes{
    .type = ResolveContext::Type::BUNDLE,
    .source = ResolveContext::Source::INTERNAL,
    .normalizedSpecifier = id,
    .referrerNormalizedSpecifier = BASE,
    .importType = "json"_kj,
  };

  // The import type is deliberately not part of module identity: the same
  // specifier with and without a type attribute matches the same evaluation
  // context and resolves to the same definition. The type is instead validated
  // against the resolved module's content type on every import (see
  // validateImportType); instance identity is keyed by (URL, definition).
  KJ_ASSERT(module->evaluateContext(withoutAttributes));
  KJ_ASSERT(module->evaluateContext(withAttributes));

  auto json = kj::str("{}");
  ModuleBundle::BundleBuilder builder(BASE);
  builder.addSyntheticModule(
      "data.json", Module::newJsonModuleHandler(json), nullptr, Module::ContentType::JSON);
  auto registry = ModuleRegistry::Builder(BASE).add(builder.finish()).finish();

  auto& first = KJ_ASSERT_NONNULL(registry->lookup(withoutAttributes, noopResolveObserver));
  auto& second = KJ_ASSERT_NONNULL(registry->lookup(withAttributes, noopResolveObserver));
  KJ_ASSERT(&first == &second);
}

// ======================================================================================

KJ_TEST("Import attribute type:json succeeds for JSON modules") {

  CompilationObserver compilationObserver;
  ModuleBundle::BundleBuilder builder(BASE);

  // An ESM that imports a JSON module with the type attribute
  auto entry = kj::str("import data from 'data.json' with { type: 'json' }; export default data;");
  builder.addEsmModule("entry", entry, Module::Flags::MAIN);

  auto json = kj::str("{\"key\": \"value\"}");
  builder.addSyntheticModule("data.json", Module::newJsonModuleHandler(json.first(json.size())),
      nullptr, Module::ContentType::JSON);

  auto registry = ModuleRegistry::Builder(BASE).add(builder.finish()).finish();

  PREAMBLE([&](Lock& js) {
    auto attached = registry->attachToIsolate(js, compilationObserver);

    js.tryCatch([&] {
      auto val = ModuleRegistry::resolve(js, "file:///entry", "default"_kjc);
      // The JSON module should have been resolved and its value should be the parsed object.
      KJ_ASSERT(!val.isUndefined());
    }, [&](Value exception) { js.throwException(kj::mv(exception)); });
  });
}

// ======================================================================================

KJ_TEST("Dynamic import attribute type:json succeeds for JSON modules") {
  CompilationObserver compilationObserver;
  ModuleBundle::BundleBuilder builder(BASE);

  auto entry = kj::str(
      "export default (await import('data.json', { with: { type: 'json' } })).default.key;");
  builder.addEsmModule("entry", entry, Module::Flags::MAIN);

  auto json = kj::str("{\"key\": \"value\"}");
  builder.addSyntheticModule(
      "data.json", Module::newJsonModuleHandler(json), nullptr, Module::ContentType::JSON);

  auto registry = ModuleRegistry::Builder(BASE).add(builder.finish()).finish();
  PREAMBLE([&](Lock& js) {
    auto attached = registry->attachToIsolate(js, compilationObserver);
    auto value = ModuleRegistry::resolve(js, "entry");
    KJ_ASSERT(kj::str(value) == "value");
  });
}

// ======================================================================================

KJ_TEST("Import attribute type:json fails for non-JSON modules") {

  CompilationObserver compilationObserver;
  ModuleBundle::BundleBuilder builder(BASE);

  // An ESM that imports another ESM with type:json (should fail - ESM is not JSON)
  auto entry = kj::str("import foo from 'other' with { type: 'json' }; export default foo;");
  builder.addEsmModule("entry", entry, Module::Flags::MAIN);

  auto other = kj::str("export default 42;");
  builder.addEsmModule("other", other);

  auto registry = ModuleRegistry::Builder(BASE).add(builder.finish()).finish();

  PREAMBLE([&](Lock& js) {
    auto attached = registry->attachToIsolate(js, compilationObserver);

    js.tryCatch([&] {
      ModuleRegistry::resolve(js, "file:///entry", "default"_kjc);
      JSG_FAIL_REQUIRE(Error, "Should have thrown");
    }, [&](Value exception) {
      auto str = kj::str(exception.getHandle(js));
      KJ_ASSERT(str == "TypeError: Module \"other\" is not of type \"json\"");
    });
  });
}

// ======================================================================================

KJ_TEST("Dynamic import rejects unsupported attribute keys") {
  CompilationObserver compilationObserver;
  ModuleBundle::BundleBuilder builder(BASE);

  auto entry =
      kj::str("export default await import('data.json', { with: { unsupported: 'value' } })"
              "    .then(() => 'resolved', (error) => String(error));");
  builder.addEsmModule("entry", entry, Module::Flags::MAIN);

  auto json = kj::str("{}");
  builder.addSyntheticModule(
      "data.json", Module::newJsonModuleHandler(json), nullptr, Module::ContentType::JSON);

  auto registry = ModuleRegistry::Builder(BASE).add(builder.finish()).finish();
  PREAMBLE([&](Lock& js) {
    auto attached = registry->attachToIsolate(js, compilationObserver);
    auto value = ModuleRegistry::resolve(js, "entry");
    KJ_ASSERT(kj::str(value) == "TypeError: Unsupported import attribute: \"unsupported\"");
  });
}

// ======================================================================================

KJ_TEST("Import attribute types text and bytes are not yet supported") {
  CompilationObserver compilationObserver;
  ModuleBundle::BundleBuilder builder(BASE);

  auto text = kj::str("hello");
  builder.addSyntheticModule(
      "data.txt", Module::newTextModuleHandler(text), nullptr, Module::ContentType::TEXT);
  builder.addSyntheticModule(
      "data.bin", Module::newDataModuleHandler(nullptr), nullptr, Module::ContentType::DATA);
  builder.addEsmModule("static-text",
      "import value from 'data.txt' with { type: 'text' }; export default value;"_kjc);
  builder.addEsmModule("static-bytes",
      "import value from 'data.bin' with { type: 'bytes' }; export default value;"_kjc);
  builder.addEsmModule("dynamic-text",
      "export default await import('data.txt', { with: { type: 'text' } })"
      "    .then(() => 'resolved', (error) => String(error));"_kjc);
  builder.addEsmModule("dynamic-bytes",
      "export default await import('data.bin', { with: { type: 'bytes' } })"
      "    .then(() => 'resolved', (error) => String(error));"_kjc);

  auto registry = ModuleRegistry::Builder(BASE).add(builder.finish()).finish();
  PREAMBLE([&](Lock& js) {
    auto attached = registry->attachToIsolate(js, compilationObserver);

    js.tryCatch([&] {
      ModuleRegistry::resolve(js, "static-text");
      JSG_FAIL_REQUIRE(Error, "Should have thrown");
    }, [&](Value exception) {
      KJ_ASSERT(kj::str(exception.getHandle(js)) ==
          "TypeError: Import attribute type \"text\" is not yet supported");
    });
    js.tryCatch([&] {
      ModuleRegistry::resolve(js, "static-bytes");
      JSG_FAIL_REQUIRE(Error, "Should have thrown");
    }, [&](Value exception) {
      KJ_ASSERT(kj::str(exception.getHandle(js)) ==
          "TypeError: Import attribute type \"bytes\" is not yet supported");
    });

    auto dynamicText = ModuleRegistry::resolve(js, "dynamic-text");
    KJ_ASSERT(
        kj::str(dynamicText) == "TypeError: Import attribute type \"text\" is not yet supported");
    auto dynamicBytes = ModuleRegistry::resolve(js, "dynamic-bytes");
    KJ_ASSERT(
        kj::str(dynamicBytes) == "TypeError: Import attribute type \"bytes\" is not yet supported");
  });
}

// ======================================================================================

KJ_TEST("Unsupported import attributes are rejected") {

  CompilationObserver compilationObserver;
  ModuleBundle::BundleBuilder builder(BASE);

  auto foo = kj::str("import abc from 'foo' with { unsupported: 'value' };");
  builder.addEsmModule("foo", foo);

  auto registry = ModuleRegistry::Builder(BASE).add(builder.finish()).finish();

  PREAMBLE([&](Lock& js) {
    auto attached = registry->attachToIsolate(js, compilationObserver);

    js.tryCatch([&] {
      ModuleRegistry::resolve(js, "foo", "default"_kjc);
      JSG_FAIL_REQUIRE(Error, "Should have thrown");
    }, [&](Value exception) {
      auto str = kj::str(exception.getHandle(js));
      KJ_ASSERT(str == "TypeError: Unsupported import attribute: \"unsupported\"");
    });
  });
}

// ======================================================================================
KJ_TEST("Using a deferred eval callback works") {

  CompilationObserver compilationObserver;
  ModuleBundle::BundleBuilder builder(BASE);

  auto foo = kj::str("export default 1;");
  builder.addEsmModule("foo", foo);

  bool called = false;
  auto registry = ModuleRegistry::Builder(BASE)
                      .add(builder.finish())
                      .setEvalCallback([&called](Lock& js, const Module& module, auto v8Module,
                                           const auto& observer) {
    called = true;
    return js.resolvedJsPromise(js.num(123));
  }).finish();

  PREAMBLE([&](Lock& js) {
    auto attached = registry->attachToIsolate(js, compilationObserver);

    js.tryCatch([&] {
      ModuleRegistry::resolve(js, "foo", "default"_kjc);
      KJ_ASSERT(false);
    }, [&](auto exception) {});

    // We don't care about the specific exception above. We only want to know that
    // the eval callback was invoked.
    KJ_ASSERT(called);
  });
}

// ======================================================================================

KJ_TEST("Fallback receives rawSpecifier and source through V8 static import resolution") {
  // This test verifies that when a module is resolved through V8's static import
  // pipeline (which goes through IsolateModuleRegistry::resolveWithCaching), the
  // fallback callback receives the correct rawSpecifier and source fields.
  // Regression test for https://github.com/cloudflare/workerd/issues/6474 and
  // https://github.com/cloudflare/workerd/issues/6475.

  ResolveObserverImpl resolveObserver;
  CompilationObserver compilationObserver;

  // The entry module statically imports "./missing", which is not in the bundle.
  // This forces fallback resolution through resolveWithCaching.
  ModuleBundle::BundleBuilder bundleBuilder(BASE);
  auto main = kj::str("import value from './missing' with { type: 'json' }; export default value;");
  bundleBuilder.addEsmModule("main", main, Module::Flags::MAIN);

  bool fallbackCalled = false;
  auto json = kj::str("{}");
  auto fallback = ModuleBundle::newFallbackBundle(
      [&](const ResolveContext& context) -> kj::Maybe<kj::OneOf<kj::String, kj::Own<Module>>> {
    fallbackCalled = true;
    // The rawSpecifier should be the original "./missing" as written in the import.
    KJ_ASSERT(context.rawSpecifier == "./missing"_kjc);
    // A static import should have source == STATIC_IMPORT.
    KJ_ASSERT(context.source == ResolveContext::Source::STATIC_IMPORT);
    // The type import attribute must survive through to the fallback.
    KJ_ASSERT(KJ_ASSERT_NONNULL(context.importType) == "json"_kj);
    // Return a synthetic module to satisfy the import.
    return kj::Maybe<kj::OneOf<kj::String, kj::Own<Module>>>(
        Module::newSynthetic(context.normalizedSpecifier.clone(), Module::Type::FALLBACK,
            Module::newJsonModuleHandler(json), nullptr, Module::Flags::NONE,
            Module::ContentType::JSON));
  });

  auto registry = ModuleRegistry::Builder(BASE, ModuleRegistry::Builder::Options::ALLOW_FALLBACK)
                      .add(bundleBuilder.finish())
                      .add(kj::mv(fallback))
                      .finish();

  PREAMBLE([&](Lock& js) {
    auto attached = registry->attachToIsolate(js, compilationObserver);

    js.tryCatch([&] { ModuleRegistry::resolve(js, "file:///main"); },
        [&](Value exception) { js.throwException(kj::mv(exception)); });
  });

  KJ_ASSERT(fallbackCalled);
}

// ======================================================================================

KJ_TEST("Fallback receives REQUIRE source through require() resolution") {
  // This test verifies that when a module is resolved through require() (which also
  // goes through IsolateModuleRegistry::resolveWithCaching), the fallback callback
  // receives source == REQUIRE.
  // Regression test for https://github.com/cloudflare/workerd/issues/6475.

  ResolveObserverImpl resolveObserver;
  CompilationObserver compilationObserver;

  ModuleBundle::BundleBuilder bundleBuilder(BASE);

  bool fallbackCalled = false;
  auto fallback = ModuleBundle::newFallbackBundle(
      [&](const ResolveContext& context) -> kj::Maybe<kj::OneOf<kj::String, kj::Own<Module>>> {
    fallbackCalled = true;
    // A require() call should have source == REQUIRE.
    KJ_ASSERT(context.source == ResolveContext::Source::REQUIRE);
    KJ_ASSERT(context.rawSpecifier == "missing"_kjc);
    return kj::Maybe<kj::OneOf<kj::String, kj::Own<Module>>>(
        Module::newSynthetic(context.normalizedSpecifier.clone(), Module::Type::FALLBACK,
            Module::newDataModuleHandler(nullptr)));
  });

  auto registry = ModuleRegistry::Builder(BASE, ModuleRegistry::Builder::Options::ALLOW_FALLBACK)
                      .add(bundleBuilder.finish())
                      .add(kj::mv(fallback))
                      .finish();

  PREAMBLE([&](Lock& js) {
    auto attached = registry->attachToIsolate(js, compilationObserver);

    js.tryCatch([&] {
      // tryResolveModuleNamespace with Source::REQUIRE exercises the require() path
      // through IsolateModuleRegistry::require() -> resolveWithCaching().
      ModuleRegistry::tryResolveModuleNamespace(
          js, "missing", ResolveContext::Type::BUNDLE, ResolveContext::Source::REQUIRE);
    }, [&](Value exception) { js.throwException(kj::mv(exception)); });
  });

  KJ_ASSERT(fallbackCalled);
}

// ======================================================================================

KJ_TEST("Dynamic import from a redirected fallback module works") {
  // Reproduces the bug where a module loaded via a fallback redirect fails to
  // perform a dynamic import because V8's script origin (the module's canonical
  // URL) does not match the import specifier stored in the registry's
  // instantiation table.
  //
  // The fallback simulates a bare-specifier redirect:
  //   file:///pkg  -->  301 to file:///canonical/pkg/index.mjs
  //   file:///canonical/pkg/index.mjs  -->  ESM with `import("./dep.mjs")`
  //   file:///canonical/pkg/dep.mjs    -->  ESM exporting a value
  //
  // Without the fix, the dynamic import fails with "Referring module not found
  // in the registry: file:///canonical/pkg/index.mjs".

  const auto pkg = "file:///pkg"_url;
  const auto canonical = "file:///canonical/pkg/index.mjs"_url;
  const auto dep = "file:///canonical/pkg/dep.mjs"_url;

  // Source strings must outlive the Module objects that reference them
  // (the ArrayPtr<const char> overload of newEsm does not take ownership).
  auto pkgSource = kj::str("export async function load() { return await import('./dep.mjs'); }");
  auto depSource = kj::str("export const value = 'ok';");

  auto fallback = ModuleBundle::newFallbackBundle(
      [&](const ResolveContext& context) -> kj::Maybe<kj::OneOf<kj::String, kj::Own<Module>>> {
    if (context.normalizedSpecifier == pkg) {
      // Redirect bare specifier to canonical URL.
      return kj::Maybe<kj::OneOf<kj::String, kj::Own<Module>>>(kj::str(canonical.getHref()));
    }
    if (context.normalizedSpecifier == canonical) {
      // The package entry point: dynamically imports a sibling module.
      return kj::Maybe<kj::OneOf<kj::String, kj::Own<Module>>>(
          Module::newEsm(canonical.clone(), Module::Type::FALLBACK, pkgSource.asPtr()));
    }
    if (context.normalizedSpecifier == dep) {
      return kj::Maybe<kj::OneOf<kj::String, kj::Own<Module>>>(
          Module::newEsm(dep.clone(), Module::Type::FALLBACK, depSource.asPtr()));
    }
    return kj::none;
  });

  CompilationObserver compilationObserver;

  // An entry module that imports the bare specifier and calls load().
  ModuleBundle::BundleBuilder bundleBuilder(BASE);
  auto entry = kj::str("import { load } from 'pkg';\n"
                       "const m = await load();\n"
                       "export default m.value;\n");
  bundleBuilder.addEsmModule("entry", entry);

  auto registry = ModuleRegistry::Builder(BASE, ModuleRegistry::Builder::Options::ALLOW_FALLBACK)
                      .add(bundleBuilder.finish())
                      .add(kj::mv(fallback))
                      .finish();

  PREAMBLE([&](Lock& js) {
    auto attached = registry->attachToIsolate(js, compilationObserver);

    JSG_TRY(js) {
      auto value = ModuleRegistry::resolve(js, "file:///entry", "default"_kjc);
      KJ_ASSERT(kj::str(value) == "ok");
    }
    JSG_CATCH(exception) {
      js.throwException(kj::mv(exception));
    }
  });
}

}  // namespace
}  // namespace workerd::jsg::test
