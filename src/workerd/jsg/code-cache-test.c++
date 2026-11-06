// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "code-cache.h"
#include "compilation.h"
#include "jsg-test.h"
#include "modules-new.h"
#include "script.h"

#include <capnp/message.h>
#include <kj/map.h>
#include <kj/mutex.h>

#include <memory>

namespace workerd::jsg {

// Fault injection for a definition-local slot without changing the module's source identity.
class SharedModuleCompilationCacheTestAccess {
 public:
  static void corrupt(SharedModuleCompilationCache& cache, size_t offset) {
    auto lock = cache.cachedData.lockExclusive();
    auto& data = *KJ_ASSERT_NONNULL(lock->data);
    KJ_ASSERT(offset < data.length);
    const_cast<kj::byte*>(data.data)[offset] ^= 1;
  }

  static auto compatibilityCheck(SharedModuleCompilationCache& cache, v8::Isolate* isolate) {
    auto lock = cache.cachedData.lockShared();
    auto& cached = KJ_ASSERT_NONNULL(lock->data);
    v8::ScriptCompiler::CachedData data(cached->data, cached->length);
    return data.CompatibilityCheck(isolate);
  }
};

}  // namespace workerd::jsg

namespace workerd::jsg::test {

WD_STRONG_BOOL(Defer);
WD_STRONG_BOOL(CollectCompileHints);
WD_STRONG_BOOL(UseNewModuleRegistry);

namespace {

using CompatibilityCheckResult = v8::ScriptCompiler::CachedData::CompatibilityCheckResult;

constexpr size_t SOURCE_HASH_OFFSET = 2 * sizeof(uint32_t);
constexpr size_t FLAGS_HASH_OFFSET = SOURCE_HASH_OFFSET + 32;

V8System v8System;

// An in-memory CodeCacheStore that records how it is used.
class FakeStore final: public CodeCacheStore {
 public:
  struct State {
    kj::HashMap<CodeCacheKey, kj::Arc<Bytes>> entries;
    kj::HashSet<CodeCacheKey> claimed;
    uint lookups = 0;
    uint hits = 0;
    uint inserts = 0;
    uint generationFailures = 0;
    kj::Vector<kj::Maybe<CompatibilityCheckResult>> rejects;
    kj::Vector<size_t> insertedSizes;

    // Applied to the bytes of every insert, to simulate a cache that V8 refuses.
    kj::Maybe<kj::Function<Bytes(kj::ArrayPtr<const kj::byte>)>> mangle;
    // Stores every entry under the same key, to hand V8 a cache made for other source.
    bool singleSlot = false;
  };

  kj::MutexGuarded<State> state;

  LookupResult lookup(const CodeCacheKey& key, MayElect mayElect) const override {
    auto lock = state.lockExclusive();
    ++lock->lookups;
    auto slot = slotFor(*lock, key);
    KJ_IF_SOME(bytes, lock->entries.find(slot)) {
      ++lock->hits;
      return {.hit = bytes.addRef()};
    }
    if (!mayElect) return {};
    return {.producer = tryClaim(*lock, slot)};
  }

  kj::Maybe<kj::Own<Producer>> reject(const CodeCacheKey& key,
      const Bytes& rejected,
      kj::Maybe<CompatibilityCheckResult> reason,
      MayElect mayElect) const override {
    auto lock = state.lockExclusive();
    lock->rejects.add(reason);
    auto slot = slotFor(*lock, key);
    KJ_IF_SOME(bytes, lock->entries.find(slot)) {
      if (bytes.get() == &rejected) lock->entries.erase(slot);
    }
    if (!mayElect) return kj::none;
    return tryClaim(*lock, slot);
  }

  uint claimCount() const {
    return state.lockShared()->claimed.size();
  }

 private:
  class FakeProducer final: public Producer {
   public:
    FakeProducer(const FakeStore& store, CodeCacheKey key): store(store), key(key) {}
    ~FakeProducer() noexcept(false) {
      store.state.lockExclusive()->claimed.eraseMatch(key);
    }

    void insert(Bytes bytes) override {
      auto lock = store.state.lockExclusive();
      ++lock->inserts;
      lock->insertedSizes.add(bytes.size());
      KJ_IF_SOME(m, lock->mangle) {
        bytes = m(bytes);
      }
      lock->entries.upsert(key, kj::arc<Bytes>(kj::mv(bytes)));
    }

    void generationFailed() override {
      ++store.state.lockExclusive()->generationFailures;
    }

   private:
    const FakeStore& store;
    CodeCacheKey key;
  };

  static CodeCacheKey slotFor(const State& state, const CodeCacheKey& key) {
    if (state.singleSlot) return CodeCacheKey{.unit = key.unit};
    return key;
  }

  kj::Maybe<kj::Own<Producer>> tryClaim(State& state, const CodeCacheKey& key) const {
    if (state.claimed.contains(key)) return kj::none;
    state.claimed.insert(key);
    return kj::Own<Producer>(kj::heap<FakeProducer>(*this, key));
  }
};

struct CountingObserver final: public CompilationObserver {
  mutable uint found = 0;
  mutable uint rejected = 0;
  mutable uint generated = 0;
  mutable uint generationFailed = 0;
  mutable uint hintsConsumed = 0;
  mutable kj::Maybe<kj::Function<void()>> onRejected;

  void onCompileCacheFound(v8::Isolate*) const override {
    ++found;
  }
  void onCompileCacheRejected(v8::Isolate*) const override {
    ++rejected;
    KJ_IF_SOME(callback, onRejected) {
      callback();
    }
  }
  void onCompileCacheGenerated(v8::Isolate*) const override {
    ++generated;
  }
  void onCompileCacheGenerationFailed(v8::Isolate*) const override {
    ++generationFailed;
  }
  void onCompileHintsConsumed(v8::Isolate*, size_t, size_t) const override {
    ++hintsConsumed;
  }
};

struct CodeCacheContext: public Object, public ContextGlobal {
  JSG_RESOURCE_TYPE(CodeCacheContext) {}
};
JSG_DECLARE_ISOLATE_TYPE(CodeCacheIsolate, CodeCacheContext);

// Runs `fn` in a fresh isolate, optionally with a store. Opens and closes a production window
// when a store is supplied and `defer` is set.
void withIsolate(kj::Own<const CodeCacheStore> store,
    const CompilationObserver& observer,
    Defer defer,
    kj::FunctionParam<void(jsg::Lock&)> fn,
    UseNewModuleRegistry useNewModuleRegistry = UseNewModuleRegistry::NO,
    kj::Maybe<SnapshotConfig> snapshotConfig = kj::none) {
  CodeCacheIsolate isolate(v8System, newIsolateGroup(), nullptr, kj::heap<IsolateObserver>(),
      defaultExternalStringAllocator(), {}, true, kj::mv(snapshotConfig));
  isolate.runInLockScope([&](CodeCacheIsolate::Lock& lock) {
    auto& base = IsolateBase::from(lock.v8Isolate);
    if (useNewModuleRegistry) base.setUsingNewModuleRegistry();
    if (store.get() != nullptr) base.setCodeCacheStore(kj::mv(store));
    KJ_IF_SOME(codeCache, base.tryGetCodeCache()) {
      if (defer) codeCache.deferProduction();
    }
    JSG_WITHIN_CONTEXT_SCOPE(
        lock, lock.newContext<CodeCacheContext>().getHandle(lock), [&](jsg::Lock& js) { fn(js); });
    KJ_IF_SOME(codeCache, base.tryGetCodeCache()) {
      if (defer) codeCache.produceDeferred(lock.v8Isolate, observer);
    }
  });
}

// Compiles `code` as a legacy-registry bundle module.
v8::Local<v8::Module> compileLegacyModule(jsg::Lock& js,
    kj::StringPtr code,
    const CompilationObserver& observer,
    kj::StringPtr name = "main"_kj,
    kj::ArrayPtr<const int> compileHints = nullptr) {
  auto modules = ModuleRegistryImpl<CodeCacheIsolate_TypeWrapper>::from(js);
  auto path = kj::Path::parse(name);
  modules->add(path, ModuleRegistry::ModuleInfo(js, name, code, observer, compileHints));
  return KJ_REQUIRE_NONNULL(modules->resolve(js, path)).module.getHandle(js);
}

// Compiles and evaluates `code` as a legacy-registry bundle module and returns the string value
// of its `value` export, or of the exception it threw.
kj::String evalLegacyModule(
    jsg::Lock& js, kj::StringPtr code, const CompilationObserver& observer) {
  auto module = compileLegacyModule(js, code, observer);
  // NOLINTNEXTLINE(custom-raw-v8-trycatch): test: only stringifies what the module threw.
  v8::TryCatch catcher(js.v8Isolate);
  try {
    instantiateModule(js, module);
  } catch (JsExceptionThrown&) {
    return kj::str("threw ", JsValue(catcher.Exception()).toString(js));
  }
  auto ns = check(module->GetModuleNamespace()->ToObject(js.v8Context()));
  return JsValue(check(ns->Get(js.v8Context(), js.str("value"_kj)))).toString(js);
}

kj::Own<FakeStore> newStore() {
  return kj::atomicRefcounted<FakeStore>();
}

// A module whose top level calls a function, which V8 compiles lazily when it is called.
constexpr kj::StringPtr LAZY_MODULE =
    "function compute() { let s = 0; for (let i = 0; i < 4; ++i) s += i; return s; }\n"
    "export const value = String(compute());"_kj;

KJ_TEST("CodeCacheKey identifies source content and unit") {
  using Unit = CodeCacheKey::Unit;
  auto a = CodeCacheKey::compute(Unit::LEGACY_REGISTRY_ESM, "export const v = 'aaaa';"_kj);
  auto a2 = CodeCacheKey::compute(Unit::LEGACY_REGISTRY_ESM, "export const v = 'aaaa';"_kj);
  // Same length, different content.
  auto b = CodeCacheKey::compute(Unit::LEGACY_REGISTRY_ESM, "export const v = 'bbbb';"_kj);
  auto c = CodeCacheKey::compute(Unit::NEW_REGISTRY_ESM, "export const v = 'aaaa';"_kj);
  KJ_EXPECT(a == a2);
  KJ_EXPECT(a.hashCode() == a2.hashCode());
  KJ_EXPECT(!(a == b));
  KJ_EXPECT(!(a == c));
}

KJ_TEST("Legacy registry: a second isolate consumes the cache produced after evaluation") {
  auto store = newStore();
  CountingObserver observer;

  withIsolate(kj::atomicAddRef(*store), observer, Defer::YES, [&](jsg::Lock& js) {
    KJ_EXPECT(evalLegacyModule(js, LAZY_MODULE, observer) == "6");
    // Nothing is produced until the production window closes.
    KJ_EXPECT(store->state.lockShared()->inserts == 0);
  });
  {
    auto lock = store->state.lockShared();
    KJ_EXPECT(lock->lookups == 1);
    KJ_EXPECT(lock->hits == 0);
    KJ_EXPECT(lock->inserts == 1);
  }
  KJ_EXPECT(observer.generated == 1);
  KJ_EXPECT(store->claimCount() == 0);

  withIsolate(kj::atomicAddRef(*store), observer, Defer::YES,
      [&](jsg::Lock& js) { KJ_EXPECT(evalLegacyModule(js, LAZY_MODULE, observer) == "6"); });
  {
    auto lock = store->state.lockShared();
    KJ_EXPECT(lock->lookups == 2);
    KJ_EXPECT(lock->hits == 1);
    KJ_EXPECT(lock->inserts == 1);
    KJ_EXPECT(lock->rejects.size() == 0);
  }
  KJ_EXPECT(observer.found == 1);
  KJ_EXPECT(observer.rejected == 0);
}

// The legacy registry treats both borrowed and owned one-byte builtin source as Latin-1.
KJ_TEST("Legacy builtins share caches across isolates and source ownership") {
  auto store = newStore();
  CountingObserver observer;
  constexpr auto code = "export const value = 'caf\xe9';"_kj;
  for (uint i = 0; i < 2; ++i) {
    withIsolate(kj::atomicAddRef(*store), observer, Defer::YES, [&](jsg::Lock& js) {
      auto info = i == 0
          ? ModuleRegistry::ModuleInfo(
                js, "builtin", StaticExternalStringSource(code.asArray()), nullptr, observer)
          : ModuleRegistry::ModuleInfo(js, "builtin", copyToArc(code), nullptr, observer);
      auto module = info.module.getHandle(js);
      instantiateModule(js, module);
      auto ns = JsObject(check(module->GetModuleNamespace()->ToObject(js.v8Context())));
      KJ_EXPECT(ns.get(js, "value").toString(js) == "café");
    });
    auto lock = store->state.lockShared();
    KJ_EXPECT(lock->lookups == i + 1);
    KJ_EXPECT(lock->hits == i);
    KJ_EXPECT(lock->inserts == 1);
    KJ_EXPECT(lock->rejects.size() == 0);
  }
  KJ_EXPECT(observer.found == 1);
}

KJ_TEST("Legacy UTF-16 builtins consume the shared cache") {
  auto store = newStore();
  CountingObserver observer;
  static constexpr char16_t code[] = u"export const value = 'π🚀';";
  auto source = kj::arrayPtr(reinterpret_cast<const uint16_t*>(code), kj::size(code) - 1);
  for (uint i = 0; i < 2; ++i) {
    withIsolate(kj::atomicAddRef(*store), observer, Defer::YES, [&](jsg::Lock& js) {
      ModuleRegistry::ModuleInfo info(
          js, "builtin", StaticExternalStringSource(source), nullptr, observer);
      auto module = info.module.getHandle(js);
      instantiateModule(js, module);
      auto ns = JsObject(check(module->GetModuleNamespace()->ToObject(js.v8Context())));
      KJ_EXPECT(ns.get(js, "value").toString(js) == "π🚀");
    });
  }
  auto lock = store->state.lockShared();
  KJ_EXPECT(lock->hits == 1);
  KJ_EXPECT(lock->inserts == 1);
  KJ_EXPECT(lock->rejects.size() == 0);
  KJ_EXPECT(observer.found == 1);
}

KJ_TEST("Source encodings do not share builtin cache keys") {
  constexpr auto code = "export default 'é';"_kj;
  auto utf8 = CodeCacheKey::compute(CodeCacheKey::Unit::LEGACY_REGISTRY_ESM, code);
  auto latin1 = codeCacheKeyForModule(StaticExternalStringSource(code.asArray()));
  KJ_EXPECT(!(utf8 == latin1));
  static constexpr uint16_t twoByte[] = {'e', 'x', 'p', 'o', 'r', 't', ' ', 'd', 'e', 'f', 'a', 'u',
    'l', 't', ' ', '\'', 0xe9, '\'', ';'};
  auto utf16 = codeCacheKeyForModule(StaticExternalStringSource(kj::arrayPtr(twoByte)));
  auto raw =
      CodeCacheKey::compute(CodeCacheKey::Unit::ONE_BYTE_ESM, kj::asBytes(twoByte).asChars());
  KJ_EXPECT(!(utf16 == raw));
}

KJ_TEST("New registry builtins share the store across separate registries") {
  auto store = newStore();
  CountingObserver observer;
  for (uint i = 0; i < 3; ++i) {
    withIsolate(kj::atomicAddRef(*store), observer, Defer::YES, [&](jsg::Lock& js) {
      const auto base = "file:///"_url;
      modules::ModuleRegistry::Builder builder(base);
      modules::ModuleBundle::BuiltinBuilder builtins;
      constexpr auto code = "export default 'π🚀';"_kj;
      if (i == 0) {
        builtins.addEsm("test:builtin"_url, code.asArray());
      } else {
        builtins.addEsm("test:builtin"_url, copyToArc(code));
      }
      auto registry = builder.add(builtins.finish()).finish();
      auto attached = registry->attachToIsolate(js, observer);
      KJ_EXPECT(modules::ModuleRegistry::resolve(js, "test:builtin").toString(js) == "π🚀");
    }, UseNewModuleRegistry::YES);
    auto lock = store->state.lockShared();
    KJ_EXPECT(lock->lookups == i + 1);
    KJ_EXPECT(lock->hits == i);
    KJ_EXPECT(lock->inserts == 1);
    KJ_EXPECT(lock->rejects.size() == 0);
  }
  KJ_EXPECT(observer.found == 2);
}

KJ_TEST("Builtin misses outside startup do not produce code caches") {
  for (auto newRegistry: {UseNewModuleRegistry::NO, UseNewModuleRegistry::YES}) {
    auto store = newStore();
    CountingObserver observer;
    withIsolate(kj::atomicAddRef(*store), observer, Defer::NO, [&](jsg::Lock& js) {
      if (newRegistry) {
        const auto base = "file:///"_url;
        modules::ModuleRegistry::Builder builder(base);
        modules::ModuleBundle::BuiltinBuilder builtins;
        builtins.addEsm("test:builtin"_url, LAZY_MODULE.asArray());
        auto registry = builder.add(builtins.finish()).finish();
        auto attached = registry->attachToIsolate(js, observer);
        auto ns = KJ_ASSERT_NONNULL(
            modules::ModuleRegistry::tryResolveModuleNamespace(js, "test:builtin"));
        KJ_EXPECT(KJ_ASSERT_NONNULL(ns.tryCast<JsObject>()).get(js, "value").toString(js) == "6");
      } else {
        ModuleRegistry::ModuleInfo info(
            js, "builtin", StaticExternalStringSource(LAZY_MODULE.asArray()), nullptr, observer);
        auto module = info.module.getHandle(js);
        instantiateModule(js, module);
      }
    }, newRegistry);
    auto lock = store->state.lockShared();
    KJ_EXPECT(lock->lookups == 1);
    KJ_EXPECT(lock->inserts == 0);
    KJ_EXPECT(lock->claimed.size() == 0);
    KJ_EXPECT(observer.generated == 0);
  }
}

KJ_TEST("Embedded builtin caches take precedence over the store in both registries") {
  for (auto code: {"/* cache */ export default 'ASCII';"_kj,
         "/* cache */ export default 'café';"_kj, "/* cache */ export default 'π🚀';"_kj}) {
    auto store = newStore();
    CountingObserver observer;
    std::unique_ptr<v8::ScriptCompiler::CachedData> data;
    withIsolate(kj::atomicAddRef(*store), observer, Defer::NO, [&](jsg::Lock& js) {
      v8::ScriptOrigin origin(js.str("test:builtin"_kj), 0, 0, false, -1, {}, false, false, true);
      v8::ScriptCompiler::Source source(js.str(code), origin);
      auto module = check(v8::ScriptCompiler::CompileModule(js.v8Isolate, &source));
      data.reset(v8::ScriptCompiler::CreateCodeCache(module->GetUnboundModuleScript()));
      KJ_ASSERT(data != nullptr);
    });
    capnp::MallocMessageBuilder message;
    auto bundle = message.initRoot<Bundle>();
    auto module = bundle.initModules(1)[0];
    module.setName("test:builtin");
    module.setType(ModuleType::BUILTIN);
    module.setSrc(code.asBytes());
    module.setCompileCache(kj::arrayPtr(data->data, data->length));
    // Invalid syntax at the same length proves that V8 consumes the embedded cache.
    module.getSrc()[0] = ')';
    for (bool useStore: {false, true}) {
      for (auto newRegistry: {UseNewModuleRegistry::NO, UseNewModuleRegistry::YES}) {
        kj::Own<const CodeCacheStore> isolateStore;
        if (useStore) isolateStore = kj::atomicAddRef(*store);
        withIsolate(kj::mv(isolateStore), observer, Defer::YES, [&](jsg::Lock& js) {
          if (newRegistry) {
            modules::ModuleBundle::BuiltinBuilder builtins;
            modules::ModuleBundle::getBuiltInBundleFromCapnp(builtins, bundle.asReader());
            const auto base = "file:///"_url;
            modules::ModuleRegistry::Builder builder(base);
            auto registry = builder.add(builtins.finish()).finish();
            auto attached = registry->attachToIsolate(js, observer);
            KJ_EXPECT(modules::ModuleRegistry::resolve(js, "test:builtin").toString(js) != "");
          } else {
            ModuleRegistry::ModuleInfo info(js, "test:builtin", module.getSrc().asChars(),
                module.getCompileCache().asBytes(), observer);
            auto descriptor = info.module.getHandle(js);
            instantiateModule(js, descriptor);
          }
        }, newRegistry);
      }
    }
    auto lock = store->state.lockShared();
    KJ_EXPECT(lock->lookups == 0);
    KJ_EXPECT(lock->inserts == 0);
    KJ_EXPECT(observer.found == 4);
    KJ_EXPECT(observer.rejected == 0);
  }
}

KJ_TEST("The runtime cache invokes its compilation callback on misses and hits") {
  auto store = newStore();
  CountingObserver observer;
  uint calls = 0;
  for (uint i = 0; i < 2; ++i) {
    withIsolate(kj::atomicAddRef(*store), observer, Defer::YES, [&](jsg::Lock& js) {
      auto& cache = KJ_ASSERT_NONNULL(IsolateBase::from(js.v8Isolate).tryGetCodeCache());
      auto key = cache.keyFor(CodeCacheKey::Unit::LEGACY_REGISTRY_ESM, LAZY_MODULE);
      v8::ScriptOrigin origin(js.str("main"_kj), 0, 0, false, -1, {}, false, false, true);
      auto module =
          check(cache.compile<v8::Module>(js.v8Isolate, key, js.str(LAZY_MODULE), origin, observer,
              [&](v8::ScriptCompiler::Source* source, v8::ScriptCompiler::CompileOptions options) {
        ++calls;
        KJ_EXPECT(options ==
            (i == 0 ? v8::ScriptCompiler::kNoCompileOptions
                    : v8::ScriptCompiler::kConsumeCodeCache));
        KJ_EXPECT((source->GetCachedData() != nullptr) == (i == 1));
        return v8::ScriptCompiler::CompileModule(js.v8Isolate, source, options);
      }));
      instantiateModule(js, module);
    });
  }
  KJ_EXPECT(calls == 2);
  KJ_EXPECT(observer.generated == 1);
  KJ_EXPECT(observer.found == 1);
  KJ_EXPECT(store->state.lockShared()->inserts == 1);
}

KJ_TEST("Hint production and embedded bytecode bypass runtime keys and the shared slot") {
  auto store = newStore();
  CountingObserver observer;
  for (auto collect: {CollectCompileHints::YES, CollectCompileHints::NO}) {
    withIsolate(kj::atomicAddRef(*store), observer, Defer::YES, [&](jsg::Lock& js) {
      v8::ScriptOrigin origin(js.str("main"_kj), 0, 0, false, -1, {}, false, false, true);
      v8::ScriptCompiler::Source source(js.str(LAZY_MODULE), origin);
      auto module = check(v8::ScriptCompiler::CompileModule(js.v8Isolate, &source));
      std::unique_ptr<v8::ScriptCompiler::CachedData> data(
          v8::ScriptCompiler::CreateCodeCache(module->GetUnboundModuleScript()));
      KJ_ASSERT(data != nullptr);
      SharedModuleCompilationCache sharedCache;
      uint keyCalls = 0;
      if (collect) IsolateBase::from(js.v8Isolate).enableCompileHints();
      auto result = check(compileModule(js, js.str(LAZY_MODULE), origin, observer,
          [&](IsolateCodeCache&) -> kj::Maybe<CodeCacheKey> {
        ++keyCalls;
        return kj::none;
      },
          {.embeddedCache = kj::arrayPtr(data->data, data->length),
            .bundleModuleName = "main"_kj,
            .sharedCache = sharedCache}));
      instantiateModule(js, result);
      KJ_EXPECT(keyCalls == 0);
      auto hints = getProducedCompileHints(js);
      KJ_EXPECT(hints.size() == (collect ? 1 : 0));
    });
  }
  KJ_EXPECT(observer.found == 1);
  KJ_EXPECT(observer.generated == 0);
  KJ_EXPECT(store->state.lockShared()->lookups == 0);
}

KJ_TEST("Failed embedded-cache compilation does not report a cache outcome") {
  CountingObserver observer;
  std::unique_ptr<v8::ScriptCompiler::CachedData> data;
  withIsolate({}, observer, Defer::NO, [&](Lock& js) {
    v8::ScriptOrigin origin(js.str("main"_kj), 0, 0, false, -1, {}, false, false, true);
    v8::ScriptCompiler::Source source(js.str(LAZY_MODULE), origin);
    auto module = check(v8::ScriptCompiler::CompileModule(js.v8Isolate, &source));
    data.reset(v8::ScriptCompiler::CreateCodeCache(module->GetUnboundModuleScript()));
    KJ_ASSERT(data != nullptr);
  });
  withIsolate({}, observer, Defer::NO, [&](Lock& js) {
    v8::ScriptOrigin origin(js.str("main"_kj), 0, 0, false, -1, {}, false, false, true);
    // NOLINTNEXTLINE(custom-raw-v8-trycatch): test: observes an expected syntax error.
    v8::TryCatch catcher(js.v8Isolate);
    auto result = compileModule(js, js.str("export const = ;"_kj), origin, observer,
        [](IsolateCodeCache&) -> kj::Maybe<CodeCacheKey> { KJ_FAIL_ASSERT("no store installed"); },
        {.embeddedCache = kj::arrayPtr(data->data, data->length)});
    KJ_EXPECT(result.IsEmpty());
    KJ_EXPECT(catcher.HasCaught());
  });
  KJ_EXPECT(observer.found == 0);
  KJ_EXPECT(observer.rejected == 0);
  KJ_EXPECT(observer.generated == 0);
}

void compileSharedModule(
    Lock& js, const SharedModuleCompilationCache& cache, const CompilationObserver& observer) {
  v8::ScriptOrigin origin(js.str("shared"_kj), 0, 0, false, -1, {}, false, false, true);
  auto module = check(cache.compile(js, js.str(LAZY_MODULE), origin, observer));
  instantiateModule(js, module);
  auto ns = module->GetModuleNamespace().As<v8::Object>();
  KJ_EXPECT(JsValue(check(ns->Get(js.v8Context(), js.str("value"_kj)))).toString(js) == "6");
}

void expectSharedSlotRejected(size_t corruptOffset, CompatibilityCheckResult expectedCheck) {
  SharedModuleCompilationCache cache;
  CountingObserver observer;
  withIsolate({}, observer, Defer::NO, [&](Lock& js) { compileSharedModule(js, cache, observer); });
  KJ_EXPECT(observer.generated == 1);
  SharedModuleCompilationCacheTestAccess::corrupt(cache, corruptOffset);
  withIsolate({}, observer, Defer::NO, [&](Lock& js) {
    KJ_EXPECT(SharedModuleCompilationCacheTestAccess::compatibilityCheck(cache, js.v8Isolate) ==
        expectedCheck);
    compileSharedModule(js, cache, observer);
  });
  KJ_EXPECT(observer.rejected == 1);
  KJ_EXPECT(observer.generated == 2);
  KJ_EXPECT(observer.found == 0);
  withIsolate({}, observer, Defer::NO, [&](Lock& js) { compileSharedModule(js, cache, observer); });
  KJ_EXPECT(observer.found == 1);
  KJ_EXPECT(observer.generated == 2);
  KJ_EXPECT(observer.rejected == 1);
}

KJ_TEST("Shared slots replace bytecode rejected before or during compilation") {
  // The header's source hash is checked only when compiling; its flags hash is checked earlier.
  expectSharedSlotRejected(SOURCE_HASH_OFFSET, CompatibilityCheckResult::kSuccess);
  expectSharedSlotRejected(FLAGS_HASH_OFFSET, CompatibilityCheckResult::kFlagsMismatch);
}

KJ_TEST("Shared slot rejection preserves a replacement made before taking the exclusive lock") {
  SharedModuleCompilationCache cache;
  CountingObserver observer;
  CountingObserver replacementObserver;
  withIsolate({}, observer, Defer::NO, [&](Lock& js) { compileSharedModule(js, cache, observer); });
  SharedModuleCompilationCacheTestAccess::corrupt(cache, FLAGS_HASH_OFFSET);
  withIsolate({}, observer, Defer::NO, [&](Lock& js) {
    // The observer runs after the read lock is released, before the write lock is acquired.
    // Replace the rejected entry in that interval, without relying on thread scheduling.
    observer.onRejected = [&]() { compileSharedModule(js, cache, replacementObserver); };
    compileSharedModule(js, cache, observer);
    observer.onRejected = kj::none;
  });
  KJ_EXPECT(observer.rejected == 1);
  KJ_EXPECT(observer.generated == 1);
  KJ_EXPECT(replacementObserver.rejected == 1);
  KJ_EXPECT(replacementObserver.generated == 1);
  withIsolate({}, observer, Defer::NO, [&](Lock& js) { compileSharedModule(js, cache, observer); });
  KJ_EXPECT(observer.found == 1);
  KJ_EXPECT(observer.generated == 1);
}

KJ_TEST("Snapshot preparation consumes shared slots without generating or regenerating them") {
  SharedModuleCompilationCache cache;
  CountingObserver observer;
  auto snapshotCompile = [&]() {
    withIsolate({}, observer, Defer::NO,
        [&](Lock& js) {
      KJ_EXPECT(js.isPreparingSnapshot());
      compileSharedModule(js, cache, observer);
    }, UseNewModuleRegistry::NO,
        SnapshotConfig(MutableSnapshot{.artifact = kj::atomicRefcounted<SnapshotArtifact>()}));
  };
  snapshotCompile();
  KJ_EXPECT(observer.generated == 0);
  withIsolate({}, observer, Defer::NO, [&](Lock& js) { compileSharedModule(js, cache, observer); });
  KJ_EXPECT(observer.generated == 1);
  snapshotCompile();
  KJ_EXPECT(observer.found == 1);
  KJ_EXPECT(observer.generated == 1);
  SharedModuleCompilationCacheTestAccess::corrupt(cache, SOURCE_HASH_OFFSET);
  snapshotCompile();
  KJ_EXPECT(observer.rejected == 1);
  KJ_EXPECT(observer.generated == 1);
  withIsolate({}, observer, Defer::NO, [&](Lock& js) { compileSharedModule(js, cache, observer); });
  KJ_EXPECT(observer.generated == 2);
  KJ_EXPECT(observer.rejected == 1);
  KJ_EXPECT(observer.generationFailed == 0);
}

KJ_TEST("Cached CommonJS functions bind the receiving isolate's context extension") {
  auto store = newStore();
  CountingObserver observer;
  for (uint i = 0; i < 2; ++i) {
    withIsolate(kj::atomicAddRef(*store), observer, Defer::YES, [&](jsg::Lock& js) {
      auto extension = js.obj();
      extension.set(js, "provided", js.num(i));
      constexpr auto code = "globalThis.value = provided;"_kj;
      if (i == 0) {
        struct Provider final: public ModuleRegistry::CommonJsModuleInfo::CommonJsModuleProvider {
          JsRef<JsObject> context;
          Provider(Lock& js, JsObject object): context(js, object) {}
          JsObject getContext(Lock& js) override {
            return context.getHandle(js);
          }
          JsValue getExports(Lock& js) override {
            return js.global();
          }
        } provider(js, extension);
        auto fn =
            ModuleRegistry::CommonJsModuleInfo::compileEvalFunc(js, provider, "commonjs", code);
        check(fn->Call(js.v8Context(), js.v8Context()->Global(), 0, nullptr));
      } else {
        auto fn =
            modules::Module::compileEvalFunction(js, code, "commonjs"_kj, extension, observer);
        fn(js);
      }
      KJ_EXPECT(js.global().get(js, "value").toString(js) == kj::str(i));
    });
    auto lock = store->state.lockShared();
    KJ_EXPECT(lock->lookups == i + 1);
    KJ_EXPECT(lock->hits == i);
    KJ_EXPECT(lock->inserts == 1);
    KJ_EXPECT(lock->rejects.size() == 0);
  }
  KJ_EXPECT(observer.found == 1);
}

KJ_TEST("Function caches distinguish context extensions") {
  auto store = newStore();
  CountingObserver observer;
  for (uint i = 0; i < 4; ++i) {
    withIsolate(kj::atomicAddRef(*store), observer, Defer::YES, [&](jsg::Lock& js) {
      kj::Maybe<JsObject> extension;
      if (i % 2 == 1) extension = js.obj();
      auto fn = modules::Module::compileEvalFunction(
          js, "globalThis.value = 1;"_kj, "commonjs"_kj, extension, observer);
      fn(js);
    });
  }
  auto lock = store->state.lockShared();
  KJ_EXPECT(lock->lookups == 4);
  KJ_EXPECT(lock->hits == 2);
  KJ_EXPECT(lock->inserts == 2);
}

KJ_TEST("An installed code cache takes precedence over hints on a miss and a hit") {
  auto store = newStore();
  CountingObserver observer;
  // The opening parenthesis of `compute` in LAZY_MODULE.
  const int hints[] = {16};

  for (uint i = 0; i < 2; ++i) {
    withIsolate(kj::atomicAddRef(*store), observer, Defer::YES, [&](jsg::Lock& js) {
      auto module = compileLegacyModule(js, LAZY_MODULE, observer, "main", hints);
      instantiateModule(js, module);
    });
    auto lock = store->state.lockShared();
    KJ_EXPECT(lock->lookups == i + 1);
    KJ_EXPECT(lock->hits == i);
    KJ_EXPECT(lock->inserts == 1);
    KJ_EXPECT(observer.hintsConsumed == 0);
  }
  KJ_EXPECT(observer.generated == 1);
  KJ_EXPECT(observer.found == 1);
}

KJ_TEST("A cache produced after evaluation includes the functions that ran") {
  auto deferredStore = newStore();
  auto immediateStore = newStore();
  CountingObserver observer;

  withIsolate(kj::atomicAddRef(*deferredStore), observer, Defer::YES,
      [&](jsg::Lock& js) { KJ_EXPECT(evalLegacyModule(js, LAZY_MODULE, observer) == "6"); });
  // A window closed right after compilation produces the cache before anything ran.
  withIsolate(kj::atomicAddRef(*immediateStore), observer, Defer::NO, [&](jsg::Lock& js) {
    auto& codeCache = KJ_ASSERT_NONNULL(IsolateBase::from(js.v8Isolate).tryGetCodeCache());
    codeCache.deferProduction();
    compileLegacyModule(js, LAZY_MODULE, observer);
    codeCache.produceDeferred(js.v8Isolate, observer);
  });

  auto deferredSize = deferredStore->state.lockShared()->insertedSizes[0];
  auto immediateSize = immediateStore->state.lockShared()->insertedSizes[0];
  KJ_EXPECT(deferredSize > immediateSize, deferredSize, immediateSize);
}

KJ_TEST("Outside a production window, a miss is compiled from source and not cached") {
  auto store = newStore();
  CountingObserver observer;

  // As when a module is first imported during a request.
  withIsolate(kj::atomicAddRef(*store), observer, Defer::NO, [&](jsg::Lock& js) {
    KJ_EXPECT(evalLegacyModule(js, LAZY_MODULE, observer) == "6");
    KJ_EXPECT(store->claimCount() == 0);
  });
  {
    auto lock = store->state.lockShared();
    KJ_EXPECT(lock->lookups == 1);
    KJ_EXPECT(lock->inserts == 0);
  }
  KJ_EXPECT(observer.generated == 0);

  // A cache produced inside a window is still consumed outside one.
  withIsolate(kj::atomicAddRef(*store), observer, Defer::YES,
      [&](jsg::Lock& js) { KJ_EXPECT(evalLegacyModule(js, LAZY_MODULE, observer) == "6"); });
  withIsolate(kj::atomicAddRef(*store), observer, Defer::NO,
      [&](jsg::Lock& js) { KJ_EXPECT(evalLegacyModule(js, LAZY_MODULE, observer) == "6"); });
  auto lock = store->state.lockShared();
  KJ_EXPECT(lock->lookups == 3);
  KJ_EXPECT(lock->hits == 1);
  KJ_EXPECT(lock->inserts == 1);
  KJ_EXPECT(observer.found == 1);
}

KJ_TEST("Modules of the same length but different content do not share a cache") {
  auto store = newStore();
  CountingObserver observer;

  withIsolate(kj::atomicAddRef(*store), observer, Defer::YES, [&](jsg::Lock& js) {
    KJ_EXPECT(evalLegacyModule(js, "export const value = 'aaaa';"_kj, observer) == "aaaa");
  });
  withIsolate(kj::atomicAddRef(*store), observer, Defer::YES, [&](jsg::Lock& js) {
    KJ_EXPECT(evalLegacyModule(js, "export const value = 'bbbb';"_kj, observer) == "bbbb");
  });

  auto lock = store->state.lockShared();
  KJ_EXPECT(lock->hits == 0);
  KJ_EXPECT(lock->inserts == 2);
}

void expectRejected(kj::Function<CodeCacheStore::Bytes(kj::ArrayPtr<const kj::byte>)> mangle,
    kj::Maybe<CompatibilityCheckResult> expectedReason) {
  auto store = newStore();
  CountingObserver observer;
  store->state.lockExclusive()->mangle = kj::mv(mangle);

  withIsolate(kj::atomicAddRef(*store), observer, Defer::YES,
      [&](jsg::Lock& js) { KJ_EXPECT(evalLegacyModule(js, LAZY_MODULE, observer) == "6"); });
  store->state.lockExclusive()->mangle = kj::none;

  // The rejecting isolate compiles from source and is elected to produce a replacement.
  withIsolate(kj::atomicAddRef(*store), observer, Defer::YES,
      [&](jsg::Lock& js) { KJ_EXPECT(evalLegacyModule(js, LAZY_MODULE, observer) == "6"); });
  {
    auto lock = store->state.lockShared();
    KJ_ASSERT(lock->rejects.size() == 1);
    KJ_EXPECT(lock->rejects[0] == expectedReason);
    KJ_EXPECT(lock->inserts == 2);
  }
  KJ_EXPECT(observer.rejected == 1);
  KJ_EXPECT(observer.found == 0);

  // The replacement is accepted.
  withIsolate(kj::atomicAddRef(*store), observer, Defer::YES,
      [&](jsg::Lock& js) { KJ_EXPECT(evalLegacyModule(js, LAZY_MODULE, observer) == "6"); });
  KJ_EXPECT(observer.found == 1);
  KJ_EXPECT(store->state.lockShared()->rejects.size() == 1);
}

KJ_TEST("A cache whose flags hash differs is rejected before compilation") {
  // V8's SerializedCodeData header starts with 32-bit magic and version words, followed by a
  // 256-bit source hash and the 32-bit flags hash.
  expectRejected([](kj::ArrayPtr<const kj::byte> bytes) {
    auto copy = kj::heapArray(bytes);
    KJ_ASSERT(copy.size() > FLAGS_HASH_OFFSET);
    copy[FLAGS_HASH_OFFSET] ^= 1;
    return CodeCacheStore::Bytes(kj::mv(copy));
  }, CompatibilityCheckResult::kFlagsMismatch);
}

KJ_TEST("A truncated cache is rejected before compilation") {
  expectRejected([](kj::ArrayPtr<const kj::byte> bytes) {
    return CodeCacheStore::Bytes(kj::heapArray(bytes.first(16)));
  }, CompatibilityCheckResult::kInvalidHeader);
  expectRejected([](kj::ArrayPtr<const kj::byte> bytes) {
    return CodeCacheStore::Bytes(kj::heapArray(bytes.first(bytes.size() - 1)));
  }, CompatibilityCheckResult::kLengthMismatch);
}

KJ_TEST("A cache made for source of another length is rejected during compilation") {
  auto store = newStore();
  CountingObserver observer;
  store->state.lockExclusive()->singleSlot = true;

  withIsolate(kj::atomicAddRef(*store), observer, Defer::YES, [&](jsg::Lock& js) {
    KJ_EXPECT(evalLegacyModule(js, "export const value = 'a';"_kj, observer) == "a");
  });
  withIsolate(kj::atomicAddRef(*store), observer, Defer::YES, [&](jsg::Lock& js) {
    KJ_EXPECT(evalLegacyModule(js, "export const value = 'bb';"_kj, observer) == "bb");
  });

  auto lock = store->state.lockShared();
  KJ_ASSERT(lock->rejects.size() == 1);
  KJ_EXPECT(lock->rejects[0] == kj::none);
  // The cache passed the compatibility check but V8 refused it, so it was not found.
  KJ_EXPECT(observer.found == 0);
  KJ_EXPECT(observer.rejected == 1);
}

KJ_TEST("A module whose evaluation fails is not cached") {
  auto store = newStore();
  CountingObserver observer;
  constexpr auto code = "throw new Error('boom'); export const value = 1;"_kj;

  for (auto i KJ_UNUSED: kj::zeroTo(2)) {
    withIsolate(kj::atomicAddRef(*store), observer, Defer::YES, [&](jsg::Lock& js) {
      KJ_EXPECT(evalLegacyModule(js, code, observer) == "threw Error: boom");
    });
  }

  auto lock = store->state.lockShared();
  KJ_EXPECT(lock->lookups == 2);
  KJ_EXPECT(lock->inserts == 0);
  KJ_EXPECT(lock->claimed.size() == 0);
}

KJ_TEST("A cache rejected outside startup is not replaced by the requesting isolate") {
  auto store = newStore();
  CountingObserver observer;
  store->state.lockExclusive()->singleSlot = true;

  withIsolate(kj::atomicAddRef(*store), observer, Defer::YES, [&](jsg::Lock& js) {
    KJ_EXPECT(evalLegacyModule(js, "export const value = 'a';"_kj, observer) == "a");
  });
  withIsolate(kj::atomicAddRef(*store), observer, Defer::NO, [&](jsg::Lock& js) {
    KJ_EXPECT(evalLegacyModule(js, "export const value = 'bb';"_kj, observer) == "bb");
  });

  auto lock = store->state.lockShared();
  KJ_EXPECT(lock->inserts == 1);
  KJ_EXPECT(lock->entries.size() == 0);
  KJ_EXPECT(lock->claimed.size() == 0);
  KJ_EXPECT(lock->rejects.size() == 1);
  KJ_EXPECT(observer.found == 0);
  KJ_EXPECT(observer.rejected == 1);
}

KJ_TEST("Discarding or tearing down a production window releases its elections") {
  auto store = newStore();
  CountingObserver observer;

  withIsolate(kj::atomicAddRef(*store), observer, Defer::NO, [&](jsg::Lock& js) {
    auto& codeCache = KJ_ASSERT_NONNULL(IsolateBase::from(js.v8Isolate).tryGetCodeCache());
    codeCache.deferProduction();
    KJ_EXPECT(evalLegacyModule(js, LAZY_MODULE, observer) == "6");
    KJ_EXPECT(store->claimCount() == 1);
    codeCache.discardDeferred();
    KJ_EXPECT(store->claimCount() == 0);
  });

  // The window is still open when the isolate is destroyed.
  withIsolate(kj::atomicAddRef(*store), observer, Defer::NO, [&](jsg::Lock& js) {
    KJ_ASSERT_NONNULL(IsolateBase::from(js.v8Isolate).tryGetCodeCache()).deferProduction();
    KJ_EXPECT(evalLegacyModule(js, LAZY_MODULE, observer) == "6");
    KJ_EXPECT(store->claimCount() == 1);
  });
  KJ_EXPECT(store->claimCount() == 0);

  auto lock = store->state.lockShared();
  KJ_EXPECT(lock->lookups == 2);
  KJ_EXPECT(lock->inserts == 0);
}

KJ_TEST("New registry: isolates with separate registries share the store") {
  auto store = newStore();
  CountingObserver observer;
  const auto base = "file:///"_url;

  auto evalWithFreshRegistry = [&](modules::ModuleRegistry::Builder::Options options) {
    withIsolate(kj::atomicAddRef(*store), observer, Defer::YES, [&](jsg::Lock& js) {
      constexpr auto code = "export default 'abc';"_kj;
      modules::ModuleRegistry::Builder builder(base, options);
      modules::ModuleBundle::BundleBuilder bundleBuilder(base);
      bundleBuilder.addEsmModule(
          "main", jsg::copyToArc(code), modules::Module::Flags::ESM, builder.newCodeCacheKey(code));
      auto registry = builder.add(bundleBuilder.finish()).finish();
      auto attached = registry->attachToIsolate(js, observer);
      KJ_EXPECT(modules::ModuleRegistry::resolve(js, "main").toString(js) == "abc");
    }, UseNewModuleRegistry::YES);
  };

  using Options = modules::ModuleRegistry::Builder::Options;
  evalWithFreshRegistry(Options::CODE_CACHE_KEYS);
  evalWithFreshRegistry(Options::CODE_CACHE_KEYS);
  // A registry built without code cache keys does not use the store.
  evalWithFreshRegistry(Options::NONE);

  auto lock = store->state.lockShared();
  KJ_EXPECT(lock->lookups == 2);
  KJ_EXPECT(lock->hits == 1);
  KJ_EXPECT(lock->inserts == 1);
  KJ_EXPECT(observer.found == 1);
  KJ_EXPECT(observer.generated == 2);
}

kj::Arc<modules::ModuleRegistry> newLazyModuleRegistry(
    modules::ModuleRegistry::Builder::Options options) {
  const auto base = "file:///"_url;
  modules::ModuleRegistry::Builder builder(base, options);
  modules::ModuleBundle::BundleBuilder bundle(base);
  auto flags = modules::Module::Flags::ESM;
  bundle.addEsmModule(
      "main", jsg::copyToArc(LAZY_MODULE), flags, builder.newCodeCacheKey(LAZY_MODULE));
  return builder.add(bundle.finish()).finish();
}

void evalNewRegistry(jsg::Lock& js,
    const modules::ModuleRegistry& registry,
    const CompilationObserver& observer,
    CollectCompileHints produceHints) {
  if (produceHints) IsolateBase::from(js.v8Isolate).enableCompileHints();
  auto attached = registry.attachToIsolate(js, observer);
  auto ns = KJ_ASSERT_NONNULL(modules::ModuleRegistry::tryResolveModuleNamespace(js, "main"));
  KJ_EXPECT(KJ_ASSERT_NONNULL(ns.tryCast<JsObject>()).get(js, "value").toString(js) == "6");
  auto hints = getProducedCompileHints(js);
  if (produceHints) {
    KJ_ASSERT(hints.size() == 1, hints.size());
    KJ_EXPECT(hints[0].name == "main"_kj);
    KJ_ASSERT(hints[0].functionPositions.size() == 1);
    // The opening parenthesis of `compute` in LAZY_MODULE.
    KJ_EXPECT(hints[0].functionPositions[0] == 16);
  } else {
    KJ_EXPECT(hints.size() == 0);
  }
}

KJ_TEST("New registry: producing hints bypasses an empty or populated code cache store") {
  using Options = modules::ModuleRegistry::Builder::Options;
  auto store = newStore();
  CountingObserver observer;
  auto registry = newLazyModuleRegistry(Options::CODE_CACHE_KEYS);
  KJ_ASSERT(registry->getBundleCodeCacheKeys().size() == 1);
  auto eval = [&](const modules::ModuleRegistry& registry, CollectCompileHints produceHints) {
    withIsolate(kj::atomicAddRef(*store), observer, Defer::YES, [&](jsg::Lock& js) {
      evalNewRegistry(js, registry, observer, produceHints);
    }, UseNewModuleRegistry::YES);
  };

  eval(*registry, CollectCompileHints::YES);
  KJ_EXPECT(store->state.lockShared()->lookups == 0);
  KJ_EXPECT(store->state.lockShared()->inserts == 0);
  KJ_EXPECT(observer.generated == 0);

  // Ordinary compilation populates the same source key before the next production isolate.
  eval(*registry, CollectCompileHints::NO);
  KJ_EXPECT(store->state.lockShared()->entries.size() == 1);
  KJ_EXPECT(observer.generated == 1);
  eval(*registry, CollectCompileHints::YES);

  auto lock = store->state.lockShared();
  KJ_EXPECT(lock->lookups == 1);
  KJ_EXPECT(lock->hits == 0);
  KJ_EXPECT(lock->inserts == 1);
  KJ_EXPECT(lock->claimed.size() == 0);
  KJ_EXPECT(observer.found == 0);
  KJ_EXPECT(observer.generated == 1);
}

KJ_TEST("New registry: producing hints does not populate the shared module cache") {
  auto store = newStore();
  CountingObserver observer;
  auto registry = newLazyModuleRegistry(modules::ModuleRegistry::Builder::Options::NONE);
  KJ_ASSERT(registry->getBundleCodeCacheKeys().size() == 0);

  for (auto i KJ_UNUSED: kj::zeroTo(2)) {
    withIsolate(kj::atomicAddRef(*store), observer, Defer::YES, [&](jsg::Lock& js) {
      evalNewRegistry(js, *registry, observer, CollectCompileHints::YES);
    }, UseNewModuleRegistry::YES);
  }
  KJ_EXPECT(store->state.lockShared()->lookups == 0);
  KJ_EXPECT(observer.found == 0);
  KJ_EXPECT(observer.generated == 0);
}

KJ_TEST("New registry: a registry lists the keys of its bundle modules") {
  const auto base = "file:///"_url;
  constexpr auto code = "export default 'abc';"_kj;

  modules::ModuleRegistry::Builder builder(
      base, modules::ModuleRegistry::Builder::Options::CODE_CACHE_KEYS);
  auto key = KJ_ASSERT_NONNULL(builder.newCodeCacheKey(code));
  KJ_EXPECT(key == CodeCacheKey::compute(CodeCacheKey::Unit::NEW_REGISTRY_ESM, code));
  auto registry = builder.finish();
  KJ_ASSERT(registry->getBundleCodeCacheKeys().size() == 1);
  KJ_EXPECT(registry->getBundleCodeCacheKeys()[0] == key);

  modules::ModuleRegistry::Builder withoutKeys(base);
  KJ_EXPECT(withoutKeys.newCodeCacheKey(code) == kj::none);
  KJ_EXPECT(withoutKeys.finish()->getBundleCodeCacheKeys().size() == 0);
}

KJ_TEST("A prepared lookup serves the compile, and an unused one is released") {
  auto store = newStore();
  CountingObserver observer;
  constexpr auto unused = "export const value = 'unused';"_kj;

  for (auto i: kj::zeroTo(2)) {
    withIsolate(kj::atomicAddRef(*store), observer, Defer::YES, [&](jsg::Lock& js) {
      auto& codeCache = KJ_ASSERT_NONNULL(IsolateBase::from(js.v8Isolate).tryGetCodeCache());
      codeCache.prepare(CodeCacheKey::Unit::LEGACY_REGISTRY_ESM, LAZY_MODULE);
      // A source that occurs twice is looked up once.
      codeCache.prepare(CodeCacheKey::Unit::LEGACY_REGISTRY_ESM, LAZY_MODULE);
      codeCache.prepare(CodeCacheKey::Unit::LEGACY_REGISTRY_ESM, unused);
      KJ_EXPECT(store->state.lockShared()->lookups == 2 * i + 2);
      KJ_EXPECT(store->claimCount() == (i == 0 ? 2 : 1));

      // The compile uses the prepared key and lookup.
      KJ_EXPECT(evalLegacyModule(js, LAZY_MODULE, observer) == "6");
      // A second module with the same source shares the lookup, but cannot take another election.
      compileLegacyModule(js, LAZY_MODULE, observer, "other"_kj);
      KJ_EXPECT(store->state.lockShared()->lookups == 2 * i + 2);
    });
    // The unused source's election ended with the window.
    KJ_EXPECT(store->claimCount() == 0);
  }

  auto lock = store->state.lockShared();
  KJ_EXPECT(lock->hits == 1);
  KJ_EXPECT(lock->inserts == 1);
  KJ_EXPECT(observer.found == 2);
}

KJ_TEST("Lookups are prepared only inside a production window") {
  withIsolate(newStore(), CountingObserver(), Defer::NO, [&](jsg::Lock& js) {
    auto& codeCache = KJ_ASSERT_NONNULL(IsolateBase::from(js.v8Isolate).tryGetCodeCache());
    KJ_EXPECT_THROW_MESSAGE("inside a production window",
        codeCache.prepare(CodeCacheKey::Unit::LEGACY_REGISTRY_ESM, LAZY_MODULE));
  });
}

KJ_TEST("An isolate has one production window and one store") {
  withIsolate(newStore(), CountingObserver(), Defer::YES, [&](jsg::Lock& js) {
    auto& base = IsolateBase::from(js.v8Isolate);
    KJ_EXPECT_THROW_MESSAGE(
        "already open", KJ_ASSERT_NONNULL(base.tryGetCodeCache()).deferProduction());
    KJ_EXPECT_THROW_MESSAGE("already set", base.setCodeCacheStore(newStore()));
  });
}

KJ_TEST("Service-worker scripts: a second isolate consumes the cache") {
  auto store = newStore();
  CountingObserver observer;
  constexpr auto code = "function f() { return 'x' + 'y'; } f();"_kj;

  for (auto i KJ_UNUSED: kj::zeroTo(2)) {
    withIsolate(kj::atomicAddRef(*store), observer, Defer::YES, [&](jsg::Lock& js) {
      auto script = NonModuleScript::compileWorkerScript(js, code, "worker.js"_kj);
      KJ_EXPECT(script.runAndReturn(js).toString(js) == "xy");
    });
  }

  auto lock = store->state.lockShared();
  KJ_EXPECT(lock->lookups == 2);
  KJ_EXPECT(lock->hits == 1);
  KJ_EXPECT(lock->inserts == 1);
}

KJ_TEST("Internal script helpers remain uncached when a runtime store is installed") {
  auto store = newStore();
  CountingObserver observer;
  withIsolate(kj::atomicAddRef(*store), observer, Defer::YES, [&](jsg::Lock& js) {
    auto script = NonModuleScript::compile(js, "'helper result'"_kj, "helper"_kj);
    KJ_EXPECT(script.runAndReturn(js).toString(js) == "helper result");
  });
  KJ_EXPECT(store->state.lockShared()->lookups == 0);
  KJ_EXPECT(store->state.lockShared()->inserts == 0);
}

}  // namespace
}  // namespace workerd::jsg::test
