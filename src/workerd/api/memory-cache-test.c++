// Regression test: a FallbackDoneCallback returned by getWithFallback() must
// remain safe to invoke even after the SharedMemoryCache::Use that created it
// has been destroyed. Previously, the callback captured a bare pointer to the
// Use, leading to a use-after-free when the callback outlived the Use.
//
// This is representative of production behavior: MemoryCache::read() on a
// shared cache can queue fallback callbacks across isolates via
// CrossThreadPromiseFulfiller. If one worker's fallback fails,
// handleFallbackFailure() ships a new FallbackDoneCallback to the next queued
// worker — which may be on a different thread. If the originating worker's
// isolate is torn down before that callback fires, the Use is destroyed while
// the callback is still live. This test simulates that sequence: obtain a
// callback, destroy the Use, then invoke it.

#include "memory-cache-v2-test.h"
#include "memory-cache.h"

#include <workerd/io/trace.h>
#include <workerd/tests/test-fixture.h>
#include <workerd/util/autogate.h>

#include <kj/test.h>
#include <kj/thread.h>

namespace workerd::api {
namespace {

static SharedMemoryCache::Limits testLimits() {
  return {
    .maxKeys = 100,
    .maxValueSize = 1024,
    .maxTotalValueSize = 10240,
  };
}

static bool memoryCacheV2Enabled() {
  return util::Autogate::isEnabled(util::AutogateKey::MEMORY_CACHE_V2);
}

KJ_TEST("MemoryCacheProvider captures its implementation at construction") {
  const auto& clock = kj::systemCoarseMonotonicClock();
  MemoryCacheProvider provider(clock);
  KJ_EXPECT(isMemoryCacheV2ForTest(provider) == memoryCacheV2Enabled());
}

KJ_TEST("V2 serializes concurrent final release and acquisition") {
  auto cacheNamespace = MemoryCacheNamespace::create(MemoryCachePolicy{kj::none});
  auto run = [&cacheNamespace]() {
    for (size_t i = 0; i < 1000; ++i) {
      auto binding = cacheNamespace->getBinding("shared"_kj, testLimits());
    }
  };
  {
    kj::Thread first(run);
    kj::Thread second(run);
  }
  auto first = cacheNamespace->getBinding("shared"_kj, testLimits());
  auto second = cacheNamespace->getBinding("shared"_kj, testLimits());
  KJ_EXPECT(getMemoryCacheV2StatsForTest(*first).bindings == 2);
}

KJ_TEST("V2 provider teardown does not invalidate a live binding") {
  if (!memoryCacheV2Enabled()) return;

  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment&) -> kj::Promise<void> {
    kj::Own<MemoryCacheUse> use;
    kj::Own<MemoryCacheUse> privateUse;
    {
      MemoryCacheProvider provider(kj::systemCoarseMonotonicClock());
      use = provider.getUse("shared"_kj, testLimits());
      privateUse = provider.getUse(kj::none, testLimits());
    }

    SpanBuilder span(nullptr);
    auto result = use->getWithFallback(kj::str("key"), span);
    KJ_ASSERT(result.is<kj::Promise<SharedMemoryCache::Use::GetWithFallbackOutcome>>());
    return kj::mv(result.get<kj::Promise<SharedMemoryCache::Use::GetWithFallbackOutcome>>())
        .then([use = kj::mv(use), privateUse = kj::mv(privateUse)](
                  SharedMemoryCache::Use::GetWithFallbackOutcome outcome) mutable {
      (void)use;
      (void)privateUse;
      KJ_ASSERT(outcome.is<SharedMemoryCache::Use::FallbackDoneCallback>());
      SpanBuilder span(nullptr);
      outcome.get<SharedMemoryCache::Use::FallbackDoneCallback>()(kj::none, span);
    });
  });
}

KJ_TEST("V2 canceled waiters unlink immediately") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment&) -> kj::Promise<void> {
    auto cacheNamespace = MemoryCacheNamespace::create(MemoryCachePolicy{kj::none});
    auto cache = cacheNamespace->getBinding("shared"_kj, testLimits());
    auto key = kj::str("key");
    SpanBuilder span(nullptr);
    auto leader = cache->getWithFallback(key, span);
    KJ_ASSERT(leader.is<kj::Promise<SharedMemoryCache::Use::GetWithFallbackOutcome>>());
    {
      auto follower = cache->getWithFallback(key, span);
      KJ_ASSERT(follower.is<kj::Promise<SharedMemoryCache::Use::GetWithFallbackOutcome>>());
      KJ_EXPECT(getMemoryCacheV2StatsForTest(*cache).waiters == 1);
    }
    KJ_EXPECT(getMemoryCacheV2StatsForTest(*cache).waiters == 0);
    KJ_EXPECT(getMemoryCacheV2StatsForTest(*cache).canceledWaiters == 1);

    return kj::mv(leader.get<kj::Promise<SharedMemoryCache::Use::GetWithFallbackOutcome>>())
        .then([cache = kj::mv(cache)](
                  SharedMemoryCache::Use::GetWithFallbackOutcome outcome) mutable {
      KJ_ASSERT(outcome.is<SharedMemoryCache::Use::FallbackDoneCallback>());
      SpanBuilder span(nullptr);
      outcome.get<SharedMemoryCache::Use::FallbackDoneCallback>()(kj::none, span);
      auto stats = getMemoryCacheV2StatsForTest(*cache);
      KJ_EXPECT(stats.inFlightFallbacks == 0);
      KJ_EXPECT(stats.waiters == 0);
      KJ_EXPECT(stats.canceledWaiters == 1);
    });
  });
}

KJ_TEST("V2 abandoned fallback token promotes the next waiter") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment&) -> kj::Promise<void> {
    auto cacheNamespace = MemoryCacheNamespace::create(MemoryCachePolicy{kj::none});
    auto cache = cacheNamespace->getBinding("shared"_kj, testLimits());
    auto key = kj::str("key");
    SpanBuilder span(nullptr);
    auto leader = cache->getWithFallback(key, span);
    auto follower = cache->getWithFallback(key, span);
    KJ_ASSERT(leader.is<kj::Promise<SharedMemoryCache::Use::GetWithFallbackOutcome>>());
    KJ_ASSERT(follower.is<kj::Promise<SharedMemoryCache::Use::GetWithFallbackOutcome>>());
    auto followerPromise =
        kj::mv(follower.get<kj::Promise<SharedMemoryCache::Use::GetWithFallbackOutcome>>());
    return kj::mv(leader.get<kj::Promise<SharedMemoryCache::Use::GetWithFallbackOutcome>>())
        .then([follower = kj::mv(followerPromise), cache = kj::mv(cache)](
                  SharedMemoryCache::Use::GetWithFallbackOutcome outcome) mutable {
      KJ_ASSERT(outcome.is<SharedMemoryCache::Use::FallbackDoneCallback>());
      { auto abandoned = kj::mv(outcome.get<SharedMemoryCache::Use::FallbackDoneCallback>()); }
      return kj::mv(follower).then(
          [cache = kj::mv(cache)](SharedMemoryCache::Use::GetWithFallbackOutcome outcome) {
        KJ_ASSERT(outcome.is<SharedMemoryCache::Use::FallbackDoneCallback>());
        SpanBuilder span(nullptr);
        outcome.get<SharedMemoryCache::Use::FallbackDoneCallback>()(kj::none, span);
        KJ_EXPECT(getMemoryCacheV2StatsForTest(*cache).inFlightFallbacks == 0);
      });
    });
  });
}

KJ_TEST("V2 fallback callback is one-shot") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment&) -> kj::Promise<void> {
    auto cacheNamespace = MemoryCacheNamespace::create(MemoryCachePolicy{kj::none});
    auto cache = cacheNamespace->getBinding("shared"_kj, testLimits());
    auto key = kj::str("key");
    SpanBuilder span(nullptr);
    auto leader = cache->getWithFallback(key, span);
    KJ_ASSERT(leader.is<kj::Promise<SharedMemoryCache::Use::GetWithFallbackOutcome>>());
    return kj::mv(leader.get<kj::Promise<SharedMemoryCache::Use::GetWithFallbackOutcome>>())
        .then([](SharedMemoryCache::Use::GetWithFallbackOutcome outcome) {
      KJ_ASSERT(outcome.is<SharedMemoryCache::Use::FallbackDoneCallback>());
      auto callback = kj::mv(outcome.get<SharedMemoryCache::Use::FallbackDoneCallback>());
      SpanBuilder span(nullptr);
      callback(kj::none, span);
      KJ_EXPECT_THROW_MESSAGE(
          "memory cache fallback callback invoked more than once", callback(kj::none, span));
    });
  });
}

KJ_TEST("V2 canceled fallback waiters do not overflow the stack") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment&) -> kj::Promise<void> {
    auto cacheNamespace = MemoryCacheNamespace::create(MemoryCachePolicy{kj::none});
    auto cache = cacheNamespace->getBinding("shared"_kj, testLimits());
    auto key = kj::str("test-key");
    SpanBuilder span(nullptr);

    auto leader = cache->getWithFallback(key, span);
    KJ_ASSERT(leader.is<kj::Promise<MemoryCacheUse::GetWithFallbackOutcome>>());
    auto leaderCallback = kj::mv(leader.get<kj::Promise<MemoryCacheUse::GetWithFallbackOutcome>>())
                              .then([](MemoryCacheUse::GetWithFallbackOutcome outcome) {
      KJ_ASSERT(outcome.is<MemoryCacheUse::FallbackDoneCallback>());
      return kj::mv(outcome.get<MemoryCacheUse::FallbackDoneCallback>());
    });

    constexpr size_t waiterCount = 50'000;
    for (size_t i = 0; i < waiterCount; ++i) {
      auto waiter = cache->getWithFallback(key, span);
      KJ_ASSERT(waiter.is<kj::Promise<MemoryCacheUse::GetWithFallbackOutcome>>());
    }

    KJ_EXPECT(getMemoryCacheV2StatsForTest(*cache).waiters == 0);
    KJ_EXPECT(getMemoryCacheV2StatsForTest(*cache).canceledWaiters == waiterCount);

    return leaderCallback.then(
        [cache = kj::mv(cache)](MemoryCacheUse::FallbackDoneCallback callback) mutable {
      SpanBuilder span(nullptr);
      callback(kj::none, span);
      KJ_EXPECT(getMemoryCacheV2StatsForTest(*cache).inFlightFallbacks == 0);
    });
  });
}

KJ_TEST("regression: FallbackDoneCallback survives Use destruction") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  const auto& clock = kj::systemCoarseMonotonicClock();
  auto cache = SharedMemoryCache::create(kj::none, "test-cache"_kj, kj::none, clock);

  auto limits = testLimits();
  auto key = kj::str("test-key");

  SpanBuilder noopSpan(nullptr);

  kj::Maybe<SharedMemoryCache::Use::FallbackDoneCallback> savedCallback;

  {
    SharedMemoryCache::Use useA(kj::atomicAddRef(*cache), limits);

    // Trigger a cache miss and save the callback.
    auto result = useA.getWithFallback(key, noopSpan);
    KJ_ASSERT(result.is<kj::Promise<SharedMemoryCache::Use::GetWithFallbackOutcome>>());
    auto& promise = result.get<kj::Promise<SharedMemoryCache::Use::GetWithFallbackOutcome>>();
    KJ_ASSERT(promise.poll(waitScope));
    auto outcome = promise.wait(waitScope);
    KJ_ASSERT(outcome.is<SharedMemoryCache::Use::FallbackDoneCallback>());
    savedCallback = kj::mv(outcome.get<SharedMemoryCache::Use::FallbackDoneCallback>());
  }

  auto& callback = KJ_ASSERT_NONNULL(savedCallback);
  callback(kj::none, noopSpan);

  // If we reach here without crashing, the fix is working. The InProgress
  // entry should have been cleaned up since there are no waiters.

  // Verify the cache is still functional after the callback.
  {
    SharedMemoryCache::Use useC(kj::atomicAddRef(*cache), limits);
    auto cached = useC.getWithoutFallback(key, noopSpan);
    // Key should not be in cache (fallback failed, no value stored).
    KJ_ASSERT(cached == kj::none);
  }
}

}  // namespace
}  // namespace workerd::api
