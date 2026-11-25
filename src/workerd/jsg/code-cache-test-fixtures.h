// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// Fixtures for the tests of V8 code caching through a CodeCacheStore: an in-memory store that
// records how it is used, a compilation observer that counts outcomes, and isolates to compile
// modules in.

#include "code-cache.h"
#include "compilation.h"
#include "jsg-test.h"
#include "modules-new.h"
#include "script.h"

#include <v8-version.h>

#include <kj/map.h>
#include <kj/mutex.h>

namespace workerd::jsg::test {

WD_STRONG_BOOL(Defer);
WD_STRONG_BOOL(UseNewModuleRegistry);

using CompatibilityCheckResult = v8::ScriptCompiler::CachedData::CompatibilityCheckResult;

#if V8_MAJOR_VERSION > 15 || (V8_MAJOR_VERSION == 15 && V8_MINOR_VERSION >= 9)
// V8's SerializedCodeData header starts with 32-bit magic, payload length, trusted payload length
// and version words, followed by a 256-bit source hash and the 32-bit flags hash.
constexpr size_t SOURCE_HASH_OFFSET = 4 * sizeof(uint32_t);
#else
// V8's SerializedCodeData header starts with 32-bit magic and version words, followed by a
// 256-bit source hash and the 32-bit flags hash.
constexpr size_t SOURCE_HASH_OFFSET = 2 * sizeof(uint32_t);
#endif
constexpr size_t FLAGS_HASH_OFFSET = SOURCE_HASH_OFFSET + 32;

inline V8System v8System;

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
    // Leaves produced and hit caches with the isolate, which gives them back through release(),
    // instead of keeping them in `entries`.
    bool handOver = false;
    // With `handOver`, hands hits over undecoded, for Held::decode() to decode.
    bool lazyHits = false;
    // Makes Held::decode() fail.
    bool decodeFails = false;
    uint decodes = 0;
    // The decoded copies of hits that the isolate freed.
    uint decodedFreed = 0;
    // How many caches an isolate may produce, if bounded (see mayProduce()).
    kj::Maybe<uint> maxInserts;
    // The keys of the caches release() was given.
    kj::Vector<CodeCacheKey> released;
    ProductionTime productionTime = ProductionTime::END_OF_STARTUP;
    bool cachesBuiltins = true;
  };

  kj::MutexGuarded<State> state;

  LookupResult lookup(const CodeCacheKey& key, MayElect mayElect) const override {
    auto lock = state.lockExclusive();
    ++lock->lookups;
    auto slot = slotFor(*lock, key);
    KJ_IF_SOME(bytes, lock->entries.find(slot)) {
      ++lock->hits;
      if (!lock->handOver) return {.hit = bytes.addRef()};
      auto held = kj::heap<FakeHeld>(*this, slot, kj::mv(bytes));
      lock->entries.erase(slot);
      if (lock->lazyHits) return {.held = kj::mv(held)};
      return {.hit = held->bytes.addRef(), .held = kj::mv(held)};
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

  void release(kj::Array<kj::Own<Held>> released) const override {
    auto lock = state.lockExclusive();
    for (auto& held: released) {
      auto& fake = kj::downcast<FakeHeld>(*held);
      lock->released.add(fake.key);
      lock->entries.upsert(fake.key, kj::mv(fake.bytes));
    }
  }

  ProductionTime getProductionTime() const override {
    return state.lockShared()->productionTime;
  }

  bool cachesBuiltins() const override {
    return state.lockShared()->cachesBuiltins;
  }

  bool mayProduce() const override {
    auto lock = state.lockShared();
    KJ_IF_SOME(max, lock->maxInserts) {
      return lock->inserts < max;
    }
    return true;
  }

  uint claimCount() const {
    return state.lockShared()->claimed.size();
  }

 private:
  struct FakeHeld final: public Held {
    FakeHeld(const FakeStore& store, CodeCacheKey key, kj::Arc<Bytes> bytes)
        : store(store),
          key(key),
          bytes(kj::mv(bytes)) {}

    LookupResult decode(MayElect mayElect) override {
      auto lock = store.state.lockExclusive();
      ++lock->decodes;
      if (!lock->decodeFails) {
        // A copy, which counts itself freed.
        auto copy = kj::heap<Bytes>(kj::heapArray<const kj::byte>(bytes->asPtr()))
                        .attach(kj::defer(
                            [&store = store]() { ++store.state.lockExclusive()->decodedFreed; }));
        return {.hit = kj::Arc<Bytes>(kj::Own<const Bytes>(kj::mv(copy)))};
      }
      lock->rejects.add(kj::none);
      if (!mayElect) return {};
      return {.producer = store.tryClaim(*lock, key)};
    }

    const FakeStore& store;
    CodeCacheKey key;
    kj::Arc<Bytes> bytes;
  };

  class FakeProducer final: public Producer {
   public:
    FakeProducer(const FakeStore& store, CodeCacheKey key): store(store), key(key) {}
    ~FakeProducer() noexcept(false) {
      store.state.lockExclusive()->claimed.eraseMatch(key);
    }

    kj::Maybe<kj::Own<Held>> insert(Bytes bytes) override {
      auto lock = store.state.lockExclusive();
      ++lock->inserts;
      lock->insertedSizes.add(bytes.size());
      KJ_IF_SOME(m, lock->mangle) {
        bytes = m(bytes);
      }
      auto arc = kj::arc<Bytes>(kj::mv(bytes));
      if (lock->handOver) return kj::Own<Held>(kj::heap<FakeHeld>(store, key, kj::mv(arc)));
      lock->entries.upsert(key, kj::mv(arc));
      return kj::none;
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
inline void withIsolate(kj::Own<const CodeCacheStore> store,
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
inline v8::Local<v8::Module> compileLegacyModule(jsg::Lock& js,
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
inline kj::String evalLegacyModule(
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

inline kj::Own<FakeStore> newStore() {
  return kj::atomicRefcounted<FakeStore>();
}

// A module whose top level calls a function, which V8 compiles lazily when it is called.
constexpr kj::StringPtr LAZY_MODULE =
    "function compute() { let s = 0; for (let i = 0; i < 4; ++i) s += i; return s; }\n"
    "export const value = String(compute());"_kj;

}  // namespace workerd::jsg::test
