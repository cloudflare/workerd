// Tests for the C++ side of the memory cache: binding lifetime, singleflight
// leader/waiter handoff, and the FallbackDoneCallback contract. The storage and
// coordination logic itself is covered by the Rust crate's own tests.

#include "memory-cache-v2-test.h"
#include "memory-cache.h"

#include <workerd/io/trace.h>
#include <workerd/tests/test-fixture.h>

#include <kj/test.h>
#include <kj/thread.h>

namespace workerd::api {
namespace {

using Outcome = MemoryCacheUse::GetWithFallbackOutcome;
using FallbackDoneCallback = MemoryCacheUse::FallbackDoneCallback;

static MemoryCacheLimits testLimits() {
  return {
    .maxKeys = 100,
    .maxValueSize = 1024,
    .maxTotalValueSize = 10240,
  };
}

KJ_TEST("serializes concurrent final release and acquisition") {
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

KJ_TEST("provider teardown does not invalidate a live binding") {
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
    KJ_ASSERT(result.is<kj::Promise<Outcome>>());
    return kj::mv(result.get<kj::Promise<Outcome>>())
        .then([use = kj::mv(use), privateUse = kj::mv(privateUse)](Outcome outcome) mutable {
      (void)use;
      (void)privateUse;
      KJ_ASSERT(outcome.is<FallbackDoneCallback>());
      SpanBuilder span(nullptr);
      outcome.get<FallbackDoneCallback>()(kj::none, span);
    });
  });
}

KJ_TEST("canceled waiters unlink immediately") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment&) -> kj::Promise<void> {
    auto cacheNamespace = MemoryCacheNamespace::create(MemoryCachePolicy{kj::none});
    auto cache = cacheNamespace->getBinding("shared"_kj, testLimits());
    auto key = kj::str("key");
    SpanBuilder span(nullptr);
    auto leader = cache->getWithFallback(key, span);
    KJ_ASSERT(leader.is<kj::Promise<Outcome>>());
    {
      auto follower = cache->getWithFallback(key, span);
      KJ_ASSERT(follower.is<kj::Promise<Outcome>>());
      KJ_EXPECT(getMemoryCacheV2StatsForTest(*cache).waiters == 1);
    }
    KJ_EXPECT(getMemoryCacheV2StatsForTest(*cache).waiters == 0);
    KJ_EXPECT(getMemoryCacheV2StatsForTest(*cache).canceledWaiters == 1);

    return kj::mv(leader.get<kj::Promise<Outcome>>())
        .then([cache = kj::mv(cache)](Outcome outcome) mutable {
      KJ_ASSERT(outcome.is<FallbackDoneCallback>());
      SpanBuilder span(nullptr);
      outcome.get<FallbackDoneCallback>()(kj::none, span);
      auto stats = getMemoryCacheV2StatsForTest(*cache);
      KJ_EXPECT(stats.inFlightFallbacks == 0);
      KJ_EXPECT(stats.waiters == 0);
      KJ_EXPECT(stats.canceledWaiters == 1);
    });
  });
}

KJ_TEST("abandoned fallback token promotes the next waiter") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment&) -> kj::Promise<void> {
    auto cacheNamespace = MemoryCacheNamespace::create(MemoryCachePolicy{kj::none});
    auto cache = cacheNamespace->getBinding("shared"_kj, testLimits());
    auto key = kj::str("key");
    SpanBuilder span(nullptr);
    auto leader = cache->getWithFallback(key, span);
    auto follower = cache->getWithFallback(key, span);
    KJ_ASSERT(leader.is<kj::Promise<Outcome>>());
    KJ_ASSERT(follower.is<kj::Promise<Outcome>>());
    auto followerPromise = kj::mv(follower.get<kj::Promise<Outcome>>());
    return kj::mv(leader.get<kj::Promise<Outcome>>())
        .then([follower = kj::mv(followerPromise), cache = kj::mv(cache)](Outcome outcome) mutable {
      KJ_ASSERT(outcome.is<FallbackDoneCallback>());
      { auto abandoned = kj::mv(outcome.get<FallbackDoneCallback>()); }
      return kj::mv(follower).then([cache = kj::mv(cache)](Outcome outcome) {
        KJ_ASSERT(outcome.is<FallbackDoneCallback>());
        SpanBuilder span(nullptr);
        outcome.get<FallbackDoneCallback>()(kj::none, span);
        KJ_EXPECT(getMemoryCacheV2StatsForTest(*cache).inFlightFallbacks == 0);
      });
    });
  });
}

KJ_TEST("fallback callback is one-shot") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment&) -> kj::Promise<void> {
    auto cacheNamespace = MemoryCacheNamespace::create(MemoryCachePolicy{kj::none});
    auto cache = cacheNamespace->getBinding("shared"_kj, testLimits());
    auto key = kj::str("key");
    SpanBuilder span(nullptr);
    auto leader = cache->getWithFallback(key, span);
    KJ_ASSERT(leader.is<kj::Promise<Outcome>>());
    return kj::mv(leader.get<kj::Promise<Outcome>>()).then([](Outcome outcome) {
      KJ_ASSERT(outcome.is<FallbackDoneCallback>());
      auto callback = kj::mv(outcome.get<FallbackDoneCallback>());
      SpanBuilder span(nullptr);
      callback(kj::none, span);
      KJ_EXPECT_THROW_MESSAGE(
          "memory cache fallback callback invoked more than once", callback(kj::none, span));
    });
  });
}

KJ_TEST("canceled fallback waiters do not overflow the stack") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment&) -> kj::Promise<void> {
    auto cacheNamespace = MemoryCacheNamespace::create(MemoryCachePolicy{kj::none});
    auto cache = cacheNamespace->getBinding("shared"_kj, testLimits());
    auto key = kj::str("test-key");
    SpanBuilder span(nullptr);

    auto leader = cache->getWithFallback(key, span);
    KJ_ASSERT(leader.is<kj::Promise<Outcome>>());
    auto leaderCallback = kj::mv(leader.get<kj::Promise<Outcome>>()).then([](Outcome outcome) {
      KJ_ASSERT(outcome.is<FallbackDoneCallback>());
      return kj::mv(outcome.get<FallbackDoneCallback>());
    });

    constexpr size_t waiterCount = 50'000;
    for (size_t i = 0; i < waiterCount; ++i) {
      auto waiter = cache->getWithFallback(key, span);
      KJ_ASSERT(waiter.is<kj::Promise<Outcome>>());
    }

    KJ_EXPECT(getMemoryCacheV2StatsForTest(*cache).waiters == 0);
    KJ_EXPECT(getMemoryCacheV2StatsForTest(*cache).canceledWaiters == waiterCount);

    return leaderCallback.then([cache = kj::mv(cache)](FallbackDoneCallback callback) mutable {
      SpanBuilder span(nullptr);
      callback(kj::none, span);
      KJ_EXPECT(getMemoryCacheV2StatsForTest(*cache).inFlightFallbacks == 0);
    });
  });
}

KJ_TEST("fallback callback stores the value and survives binding destruction") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto cacheNamespace = MemoryCacheNamespace::create(MemoryCachePolicy{kj::none});
    auto key = kj::str("test-key");
    SpanBuilder span(nullptr);

    // Keep a second binding alive so the shared cache outlives the first one.
    auto reader = cacheNamespace->getBinding("shared"_kj, testLimits());

    auto leader = [&]() {
      auto writer = cacheNamespace->getBinding("shared"_kj, testLimits());
      auto result = writer->getWithFallback(key, span);
      KJ_ASSERT(result.is<kj::Promise<Outcome>>());
      return kj::mv(result.get<kj::Promise<Outcome>>());
      // `writer` is destroyed here while the leader promise is still pending.
    }();

    // Completing the fallback emits trace spans, which requires running inside
    // the I/O context like the JS caller does in production.
    return leader.then([context = kj::addRef(env.context), reader = kj::mv(reader),
                           key = kj::mv(key)](Outcome outcome) mutable {
      KJ_ASSERT(outcome.is<FallbackDoneCallback>());
      return context->run([outcome = kj::mv(outcome), reader = kj::mv(reader), key = kj::mv(key)](
                              Worker::Lock&) mutable {
        SpanBuilder span(nullptr);
        outcome.get<FallbackDoneCallback>()(
            MemoryCacheUse::FallbackResult{kj::heapArray<kj::byte>({1, 2, 3}), kj::none}, span);

        auto cached = KJ_ASSERT_NONNULL(reader->getWithoutFallback(key, span));
        KJ_EXPECT(cached->asBytes() == kj::ArrayPtr<const kj::byte>({1, 2, 3}));
      });
    });
  });
}

}  // namespace
}  // namespace workerd::api
