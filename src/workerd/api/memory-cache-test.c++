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

#include "memory-cache.h"

#include <workerd/io/trace.h>

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
