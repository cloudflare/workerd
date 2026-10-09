// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "code-cache.h"
#include "jsg-test.h"
#include "modules-new.h"
#include "script.h"

#include <kj/map.h>
#include <kj/mutex.h>

namespace workerd::jsg::test {

WD_STRONG_BOOL(Defer);
WD_STRONG_BOOL(UseNewModuleRegistry);

namespace {

using CompatibilityCheckResult = v8::ScriptCompiler::CachedData::CompatibilityCheckResult;

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

  LookupResult lookup(const CodeCacheKey& key) const override {
    auto lock = state.lockExclusive();
    ++lock->lookups;
    auto slot = slotFor(*lock, key);
    KJ_IF_SOME(bytes, lock->entries.find(slot)) {
      ++lock->hits;
      return {.hit = bytes.addRef()};
    }
    return {.producer = tryClaim(*lock, slot)};
  }

  kj::Maybe<kj::Own<Producer>> reject(const CodeCacheKey& key,
      const Bytes& rejected,
      kj::Maybe<CompatibilityCheckResult> reason) const override {
    auto lock = state.lockExclusive();
    lock->rejects.add(reason);
    auto slot = slotFor(*lock, key);
    KJ_IF_SOME(bytes, lock->entries.find(slot)) {
      if (bytes.get() == &rejected) lock->entries.erase(slot);
    }
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

  void onCompileCacheFound(v8::Isolate*) const override {
    ++found;
  }
  void onCompileCacheRejected(v8::Isolate*) const override {
    ++rejected;
  }
  void onCompileCacheGenerated(v8::Isolate*) const override {
    ++generated;
  }
  void onCompileCacheGenerationFailed(v8::Isolate*) const override {
    ++generationFailed;
  }
};

struct CodeCacheContext: public Object, public ContextGlobal {
  JSG_RESOURCE_TYPE(CodeCacheContext) {}
};
JSG_DECLARE_ISOLATE_TYPE(CodeCacheIsolate, CodeCacheContext);

// Runs `fn` in a context of a fresh isolate that uses `store`, inside a production window if
// `defer` is set, and closes the window afterwards.
void withIsolate(kj::Own<const CodeCacheStore> store,
    const CompilationObserver& observer,
    Defer defer,
    kj::FunctionParam<void(jsg::Lock&)> fn,
    UseNewModuleRegistry useNewModuleRegistry = UseNewModuleRegistry::NO) {
  CodeCacheIsolate isolate(v8System, newIsolateGroup(), nullptr, kj::heap<IsolateObserver>());
  isolate.runInLockScope([&](CodeCacheIsolate::Lock& lock) {
    auto& base = IsolateBase::from(lock.v8Isolate);
    if (useNewModuleRegistry) base.setUsingNewModuleRegistry();
    base.setCodeCacheStore(kj::mv(store));
    auto& codeCache = KJ_ASSERT_NONNULL(base.tryGetCodeCache());
    if (defer) codeCache.deferProduction();
    JSG_WITHIN_CONTEXT_SCOPE(
        lock, lock.newContext<CodeCacheContext>().getHandle(lock), [&](jsg::Lock& js) { fn(js); });
    if (defer) codeCache.produceDeferred(lock.v8Isolate, observer);
  });
}

// Compiles and evaluates `code` as a legacy-registry bundle module and returns the string value
// of its `value` export, or of the exception it threw.
kj::String evalLegacyModule(
    jsg::Lock& js, kj::StringPtr code, const CompilationObserver& observer) {
  auto modules = ModuleRegistryImpl<CodeCacheIsolate_TypeWrapper>::from(js);
  auto path = kj::Path::parse("main");
  modules->add(path, ModuleRegistry::ModuleInfo(js, "main", code, observer));
  auto& info = KJ_REQUIRE_NONNULL(modules->resolve(js, path));
  auto module = info.module.getHandle(js);
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

KJ_TEST("A cache produced after evaluation includes the functions that ran") {
  auto deferredStore = newStore();
  auto immediateStore = newStore();
  CountingObserver observer;

  withIsolate(kj::atomicAddRef(*deferredStore), observer, Defer::YES,
      [&](jsg::Lock& js) { KJ_EXPECT(evalLegacyModule(js, LAZY_MODULE, observer) == "6"); });
  // Without a production window, the cache is produced right after compilation.
  withIsolate(kj::atomicAddRef(*immediateStore), observer, Defer::NO,
      [&](jsg::Lock& js) { KJ_EXPECT(evalLegacyModule(js, LAZY_MODULE, observer) == "6"); });

  auto deferredSize = deferredStore->state.lockShared()->insertedSizes[0];
  auto immediateSize = immediateStore->state.lockShared()->insertedSizes[0];
  KJ_EXPECT(deferredSize > immediateSize, deferredSize, immediateSize);
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
  // The code cache header is a sequence of 32-bit words: magic number, version hash, source hash,
  // flags hash, read-only snapshot checksum, payload length and payload checksum.
  expectRejected([](kj::ArrayPtr<const kj::byte> bytes) {
    auto copy = kj::heapArray(bytes);
    copy[12] ^= 1;
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
  KJ_EXPECT(observer.found == 1);
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

  auto evalWithFreshRegistry = [&]() {
    withIsolate(kj::atomicAddRef(*store), observer, Defer::YES, [&](jsg::Lock& js) {
      modules::ModuleBundle::BundleBuilder bundleBuilder(base);
      bundleBuilder.addEsmModule("main", jsg::copyToArc("export default 'abc';"_kj));
      auto registry = modules::ModuleRegistry::Builder(base).add(bundleBuilder.finish()).finish();
      auto attached = registry->attachToIsolate(js, observer);
      KJ_EXPECT(modules::ModuleRegistry::resolve(js, "main").toString(js) == "abc");
    }, UseNewModuleRegistry::YES);
  };

  evalWithFreshRegistry();
  evalWithFreshRegistry();

  auto lock = store->state.lockShared();
  KJ_EXPECT(lock->lookups == 2);
  KJ_EXPECT(lock->hits == 1);
  KJ_EXPECT(lock->inserts == 1);
  KJ_EXPECT(observer.found == 1);
  KJ_EXPECT(observer.generated == 1);
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

}  // namespace
}  // namespace workerd::jsg::test
