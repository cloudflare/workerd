#include "memory-cache.h"

#include <workerd/io/trace.h>
#include <workerd/tests/test-fixture.h>

#include <kj/test.h>

namespace workerd::api {
namespace {

static SharedMemoryCache::Limits testLimits() {
  return {
    .maxKeys = 100,
    .maxValueSize = 1024,
    .maxTotalValueSize = 10240,
  };
}

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

// Regression test: when a fallback fails, handleFallbackFailure() offers it to the next queued
// waiter. When a waiter who can't take it, fulfill() destroys the fallback instead, and the
// fallback reports the failure. Ensure that we can handle such reports even with a long queue of
// canceled waiters, which is best achieved by walking the queue iteratively.
KJ_TEST("regression: failing a fallback with a queue of canceled waiters") {
  TestFixture fixture;

  const auto& clock = kj::systemCoarseMonotonicClock();
  auto cache = SharedMemoryCache::create(kj::none, "test-cache"_kj, kj::none, clock);

  auto limits = testLimits();
  auto key = kj::str("test-key");

  SpanBuilder noopSpan(nullptr);

  SharedMemoryCache::Use use(kj::atomicAddRef(*cache), limits);

  // Enough waiters that recursing over them cannot fit on the stack: with the 8 MiB a thread has,
  // anything above 80 bytes of stack per waiter overflows and the recursive hand-off spent several
  // frames on each one.
  constexpr size_t WAITER_COUNT = 100'000;

  kj::Maybe<kj::Promise<SharedMemoryCache::Use::GetWithFallbackOutcome>> maybeFallbackPromise;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    // The first read misses, so it is the one handed the fallback.
    auto miss = use.getWithFallback(key, noopSpan);
    maybeFallbackPromise =
        kj::mv(miss.get<kj::Promise<SharedMemoryCache::Use::GetWithFallbackOutcome>>());

    // All following reads coalesce onto the same key and queue up behind that fallback. Dropping
    // the waiter promises models the requests being canceled: their fulfillers stay on the queue,
    // but can't accept a fallback anymore.
    for (size_t i = 0; i < WAITER_COUNT; i++) {
      auto waiter = use.getWithFallback(key, noopSpan);
      KJ_ASSERT(waiter.is<kj::Promise<SharedMemoryCache::Use::GetWithFallbackOutcome>>());
    }
  });

  // The fallback is an immediate value that outlives the IoContext it was created for, just as a
  // fallback shipped to another isolate does.
  auto& fallbackPromise = KJ_ASSERT_NONNULL(maybeFallbackPromise);
  auto outcome = fallbackPromise.wait(fixture.getWaitScope());
  auto callback = kj::mv(outcome.get<SharedMemoryCache::Use::FallbackDoneCallback>());

  // Fail the fallback. It is offered to each of the canceled waiters in turn.
  callback(kj::none, noopSpan);

  // The entire queue drained and the in-progress entry was retired, so a new read misses and is
  // handed a fresh fallback. (Had the entry survived, this read would try to queue behind it and
  // fail without an IoContext.)
  auto result = use.getWithFallback(key, noopSpan);
  auto& promise = result.get<kj::Promise<SharedMemoryCache::Use::GetWithFallbackOutcome>>();
  auto retryOutcome = promise.wait(fixture.getWaitScope());
  KJ_EXPECT(retryOutcome.is<SharedMemoryCache::Use::FallbackDoneCallback>());
}

}  // namespace
}  // namespace workerd::api
