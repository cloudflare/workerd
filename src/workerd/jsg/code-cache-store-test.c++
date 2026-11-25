// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Tests of what a CodeCacheStore decides for the isolates that use it: whether hits are decoded
// only when a compile uses them, and when the isolate produces its caches.

#include "code-cache-test-fixtures.h"

namespace workerd::jsg::test {
namespace {

KJ_TEST("A window the embedder closes captures functions first run after startup") {
  // Returns the size of the cache produced for REQUEST_MODULE, after calling `later` between the
  // end of startup and production if `runLater` is set.
  auto producedSize = [](bool runLater) {
    auto store = newEmbedderProducedStore();
    CountingObserver observer;
    withIsolate(kj::atomicAddRef(*store), observer, Defer::NO, [&](jsg::Lock& js) {
      auto& codeCache = KJ_ASSERT_NONNULL(IsolateBase::from(js.v8Isolate).tryGetCodeCache());
      codeCache.deferProduction();
      auto module = startModule(js, REQUEST_MODULE, observer);
      codeCache.finishStartup(js.v8Isolate, observer);
      KJ_EXPECT(store->state.lockShared()->inserts == 0);
      if (runLater) KJ_EXPECT(callExport(js, module, "later"_kj) == "10");
      codeCache.produceDeferred(js.v8Isolate, observer);
      // Producing again does nothing, since the window is closed.
      codeCache.produceDeferred(js.v8Isolate, observer);
    });
    auto lock = store->state.lockShared();
    KJ_ASSERT(lock->insertedSizes.size() == 1);
    return lock->insertedSizes[0];
  };
  KJ_EXPECT(producedSize(true) > producedSize(false));
}

KJ_TEST("Code compiled after startup joins the window only if the embedder closes it") {
  constexpr auto imported = "export const value = 'imported';"_kj;
  for (auto productionTime: {CodeCacheStore::ProductionTime::END_OF_STARTUP,
         CodeCacheStore::ProductionTime::ON_EMBEDDER_REQUEST}) {
    auto store = newStore();
    store->state.lockExclusive()->productionTime = productionTime;
    CountingObserver observer;
    withIsolate(kj::atomicAddRef(*store), observer, Defer::NO, [&](jsg::Lock& js) {
      auto& codeCache = KJ_ASSERT_NONNULL(IsolateBase::from(js.v8Isolate).tryGetCodeCache());
      codeCache.deferProduction();
      startModule(js, LAZY_MODULE, observer);
      codeCache.finishStartup(js.v8Isolate, observer);
      // A second script would open its own window.
      if (productionTime == CodeCacheStore::ProductionTime::ON_EMBEDDER_REQUEST) {
        codeCache.deferProduction();
      }
      startModule(js, imported, observer, "imported"_kj);
      codeCache.produceDeferred(js.v8Isolate, observer);
    });
    auto lock = store->state.lockShared();
    KJ_EXPECT(lock->lookups == 2);
    KJ_EXPECT(lock->inserts ==
        (productionTime == CodeCacheStore::ProductionTime::END_OF_STARTUP ? 1 : 2));
    KJ_EXPECT(store->claimCount() == 0);
  }
}

KJ_TEST("Tearing down an isolate whose embedder never closed the window releases it") {
  auto store = newEmbedderProducedStore();
  CountingObserver observer;
  withIsolate(kj::atomicAddRef(*store), observer, Defer::NO, [&](jsg::Lock& js) {
    auto& codeCache = KJ_ASSERT_NONNULL(IsolateBase::from(js.v8Isolate).tryGetCodeCache());
    codeCache.deferProduction();
    startModule(js, LAZY_MODULE, observer);
    codeCache.finishStartup(js.v8Isolate, observer);
    KJ_EXPECT(store->claimCount() == 1);
  });
  KJ_EXPECT(store->claimCount() == 0);
  KJ_EXPECT(store->state.lockShared()->inserts == 0);
}

kj::Own<FakeStore> newLazyStore() {
  auto store = newStore();
  store->state.lockExclusive()->lazyHits = true;
  return store;
}

constexpr kj::StringPtr OTHER_MODULE = "export const value = 'other';"_kj;

// Publishes caches of LAZY_MODULE and OTHER_MODULE to `store`.
void publishTwoModules(FakeStore& store, const CompilationObserver& observer) {
  withIsolate(kj::atomicAddRef(store), observer, Defer::YES, [&](jsg::Lock& js) {
    KJ_EXPECT(evalLegacyModule(js, LAZY_MODULE, observer) == "6");
    compileLegacyModule(js, OTHER_MODULE, observer, "other"_kj);
  });
  KJ_EXPECT(store.state.lockShared()->entries.size() == 2);
}

KJ_TEST("A lazy hit is decoded only for a compile that uses it") {
  auto store = newLazyStore();
  CountingObserver observer;
  publishTwoModules(*store, observer);

  withIsolate(kj::atomicAddRef(*store), observer, Defer::NO, [&](jsg::Lock& js) {
    auto& codeCache = KJ_ASSERT_NONNULL(IsolateBase::from(js.v8Isolate).tryGetCodeCache());
    codeCache.deferProduction();
    codeCache.prepare(CodeCacheKey::Unit::LEGACY_REGISTRY_ESM, LAZY_MODULE);
    codeCache.prepare(CodeCacheKey::Unit::LEGACY_REGISTRY_ESM, OTHER_MODULE);
    KJ_EXPECT(store->state.lockShared()->decodes == 0);
    KJ_EXPECT(evalLegacyModule(js, LAZY_MODULE, observer) == "6");
    KJ_EXPECT(store->state.lockShared()->decodes == 1);
    codeCache.produceDeferred(js.v8Isolate, observer);
  });
  auto lock = store->state.lockShared();
  KJ_EXPECT(lock->decodes == 1);
  KJ_EXPECT(lock->entries.size() == 2);
  KJ_EXPECT(observer.found == 1);
}

KJ_TEST("A lazy hit that fails to decode is a miss, and the isolate replaces it") {
  auto store = newLazyStore();
  CountingObserver observer;
  publishTwoModules(*store, observer);
  store->state.lockExclusive()->decodeFails = true;

  withIsolate(kj::atomicAddRef(*store), observer, Defer::YES,
      [&](jsg::Lock& js) { KJ_EXPECT(evalLegacyModule(js, LAZY_MODULE, observer) == "6"); });
  auto lock = store->state.lockShared();
  KJ_EXPECT(lock->decodes == 1);
  // The store counts the failure; V8 rejected nothing.
  KJ_EXPECT(lock->rejects.size() == 1);
  KJ_EXPECT(observer.rejected == 0);
  KJ_EXPECT(observer.found == 0);
  // The replacement, and the cache of OTHER_MODULE that the first isolate published.
  KJ_EXPECT(lock->inserts == 3);
  KJ_EXPECT(lock->entries.size() == 2);
}

KJ_TEST("A lazy hit serves one compile, which frees its decoded bytes") {
  auto store = newLazyStore();
  store->state.lockExclusive()->productionTime =
      CodeCacheStore::ProductionTime::ON_EMBEDDER_REQUEST;
  CountingObserver observer;
  publishTwoModules(*store, observer);

  withIsolate(kj::atomicAddRef(*store), observer, Defer::NO, [&](jsg::Lock& js) {
    auto& codeCache = KJ_ASSERT_NONNULL(IsolateBase::from(js.v8Isolate).tryGetCodeCache());
    codeCache.deferProduction();
    codeCache.prepare(CodeCacheKey::Unit::LEGACY_REGISTRY_ESM, LAZY_MODULE);
    codeCache.prepare(CodeCacheKey::Unit::LEGACY_REGISTRY_ESM, OTHER_MODULE);
    startModule(js, LAZY_MODULE, observer);
    // The compile that used the hit freed its bytes.
    KJ_EXPECT(store->state.lockShared()->decodedFreed == 1);
    codeCache.finishStartup(js.v8Isolate, observer);
    KJ_EXPECT(observer.found == 1);
    // Identical source compiled later looks the key up again, and decodes its own copy.
    auto lookups = store->state.lockShared()->lookups;
    startModule(js, LAZY_MODULE, observer, "copy"_kj);
    KJ_EXPECT(observer.found == 2);
    KJ_EXPECT(store->state.lockShared()->lookups == lookups + 1);
    KJ_EXPECT(store->state.lockShared()->decodedFreed == 2);
    codeCache.produceDeferred(js.v8Isolate, observer);
  });
  auto lock = store->state.lockShared();
  // The prepared hit of OTHER_MODULE was never decoded.
  KJ_EXPECT(lock->decodes == 2);
  KJ_EXPECT(lock->entries.size() == 2);
}

KJ_TEST("A failed startup discards the window only if no startup finished in it") {
  auto store = newEmbedderProducedStore();
  CountingObserver observer;
  withIsolate(kj::atomicAddRef(*store), observer, Defer::NO, [&](jsg::Lock& js) {
    auto& codeCache = KJ_ASSERT_NONNULL(IsolateBase::from(js.v8Isolate).tryGetCodeCache());
    codeCache.deferProduction();
    startModule(js, LAZY_MODULE, observer);
    codeCache.failStartup();
    KJ_EXPECT(!codeCache.isWindowOpen());
    KJ_EXPECT(store->claimCount() == 0);

    codeCache.deferProduction();
    startModule(js, REQUEST_MODULE, observer, "second"_kj);
    codeCache.finishStartup(js.v8Isolate, observer);
    // A later startup in the same window fails.
    codeCache.failStartup();
    KJ_EXPECT(codeCache.isWindowOpen());
    codeCache.produceDeferred(js.v8Isolate, observer);
  });
  auto lock = store->state.lockShared();
  KJ_EXPECT(lock->lookups == 2);
  KJ_EXPECT(lock->inserts == 1);
}

KJ_TEST("A store that stops production leaves the window's other caches unproduced") {
  auto store = newEmbedderProducedStore();
  store->state.lockExclusive()->maxInserts = 1;
  CountingObserver observer;
  withIsolate(kj::atomicAddRef(*store), observer, Defer::NO, [&](jsg::Lock& js) {
    auto& codeCache = KJ_ASSERT_NONNULL(IsolateBase::from(js.v8Isolate).tryGetCodeCache());
    codeCache.deferProduction();
    startModule(js, LAZY_MODULE, observer);
    startModule(js, OTHER_MODULE, observer, "other"_kj);
    codeCache.finishStartup(js.v8Isolate, observer);
    KJ_EXPECT(store->claimCount() == 2);
    codeCache.produceDeferred(js.v8Isolate, observer);
  });
  KJ_EXPECT(store->state.lockShared()->inserts == 1);
  KJ_EXPECT(observer.generated == 1);
  KJ_EXPECT(store->claimCount() == 0);
}

KJ_TEST("Once production ended, no window opens") {
  auto store = newEmbedderProducedStore();
  CountingObserver observer;
  // An earlier isolate produced the cache of OTHER_MODULE.
  withIsolate(kj::atomicAddRef(*store), observer, Defer::NO, [&](jsg::Lock& js) {
    auto& codeCache = KJ_ASSERT_NONNULL(IsolateBase::from(js.v8Isolate).tryGetCodeCache());
    codeCache.deferProduction();
    startModule(js, OTHER_MODULE, observer, "other"_kj);
    codeCache.produceDeferred(js.v8Isolate, observer);
  });
  KJ_ASSERT(store->state.lockShared()->inserts == 1);

  withIsolate(kj::atomicAddRef(*store), observer, Defer::NO, [&](jsg::Lock& js) {
    auto& codeCache = KJ_ASSERT_NONNULL(IsolateBase::from(js.v8Isolate).tryGetCodeCache());
    codeCache.deferProduction();
    startModule(js, LAZY_MODULE, observer);
    codeCache.endProduction();
    KJ_EXPECT(!codeCache.isWindowOpen());
    KJ_EXPECT(store->claimCount() == 0);

    // Also when production ends before the window would open, as for a startup that is late. The
    // script still prepares and compiles its code, and consumes caches, without being elected.
    codeCache.deferProduction();
    KJ_EXPECT(!codeCache.isWindowOpen());
    codeCache.prepare(CodeCacheKey::Unit::LEGACY_REGISTRY_ESM, OTHER_MODULE);
    codeCache.prepare(CodeCacheKey::Unit::LEGACY_REGISTRY_ESM, REQUEST_MODULE);
    startModule(js, OTHER_MODULE, observer, "other"_kj);
    startModule(js, REQUEST_MODULE, observer, "request"_kj);
    KJ_EXPECT(observer.found == 1);
    KJ_EXPECT(store->claimCount() == 0);
    codeCache.produceDeferred(js.v8Isolate, observer);
  });
  auto lock = store->state.lockShared();
  KJ_EXPECT(lock->inserts == 1);
  // OTHER_MODULE once in each isolate, LAZY_MODULE, and REQUEST_MODULE when it compiled.
  KJ_EXPECT(lock->lookups == 4);
}

}  // namespace
}  // namespace workerd::jsg::test
