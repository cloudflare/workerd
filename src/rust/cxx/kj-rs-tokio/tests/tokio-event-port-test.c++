// Tests for TokioEventPort / setupTokioAsyncIo: a kj::EventLoop driven by a per-thread tokio
// current_thread runtime. Following kj-rs conventions, C++ KJ_TESTs drive; Rust helpers (see
// tests/lib.rs, bridged by workerd-cxx) provide async behaviors.

#include "kj-rs-tokio-test/lib.rs.h"
#include "kj-rs-tokio/tokio-event-port.h"

#include <kj/async.h>
#include <kj/debug.h>
#include <kj/test.h>
#include <kj/thread.h>
#include <kj/vector.h>

#include <algorithm>
#include <chrono>
#include <thread>

namespace kj_rs_tokio_test {
namespace {

using kj_rs_tokio::setupTokioAsyncIo;

void delayMillis(uint64_t millis) {
  std::this_thread::sleep_for(std::chrono::milliseconds(millis));
}

// =======================================================================================
// Basics: the KJ event loop works as usual on top of the tokio-backed port.

KJ_TEST("promises resolve on a TokioEventPort loop") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();

  KJ_EXPECT(kj::evalLater([]() { return 123; }).wait(ws) == 123);
  KJ_EXPECT(kj::Promise<int>(42).wait(ws) == 42);
}

KJ_TEST("evalLater ordering is preserved") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();

  kj::Vector<int> order;
  // eagerlyEvaluate arms each promise immediately (KJ promises are otherwise lazy: continuations
  // only get scheduled once awaited), so all three events sit in the queue in creation order.
  auto p1 = kj::evalLater([&]() { order.add(1); }).eagerlyEvaluate(nullptr);
  auto p2 = kj::evalLater([&]() { order.add(2); }).eagerlyEvaluate(nullptr);
  auto p3 = kj::evalLater([&]() { order.add(3); }).eagerlyEvaluate(nullptr);

  // Waiting on the last promise runs all three events in FIFO order.
  p3.wait(ws);
  p1.wait(ws);
  p2.wait(ws);

  KJ_ASSERT(order.size() == 3);
  KJ_EXPECT(order[0] == 1);
  KJ_EXPECT(order[1] == 2);
  KJ_EXPECT(order[2] == 3);
}

KJ_TEST("promise chains resolve") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();

  auto promise =
      kj::evalLater([]() { return 1; }).then([](int v) {
    return kj::Promise<int>(v + 1);
  }).then([](int v) { return v * 10; });
  KJ_EXPECT(promise.wait(ws) == 20);
}

KJ_TEST("evalLast fires when the loop would sleep") {
  // kj::evalLast events live on the would-sleep queue, which is only serviced through the
  // EventLoop::poll() path (EventLoop::wait() switches to poll() when would-sleep waiters
  // exist). This test hangs or misorders if the port's poll() is broken.
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();

  kj::Vector<int> order;
  auto last = kj::evalLast([&]() { order.add(2); });
  auto later = kj::evalLater([&]() { order.add(1); });

  later.wait(ws);
  // evalLast must not have run yet: the loop never ran out of work while waiting on `later`.
  KJ_ASSERT(order.size() == 1);
  KJ_EXPECT(order[0] == 1);

  last.wait(ws);
  KJ_ASSERT(order.size() == 2);
  KJ_EXPECT(order[1] == 2);
}

// =======================================================================================
// Timers: kj::TimerImpl fed by the port; advanceTo() after every wait()/poll().

KJ_TEST("timer.afterDelay fires with real elapsed time") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto &timer = io.getTimer();

  auto &sysClock = kj::systemPreciseMonotonicClock();
  auto before = sysClock.now();
  auto timerBefore = timer.now();

  // If the port forgot timerImpl.advanceTo() after waits, this would never resolve (caught by
  // the test timeout).
  timer.afterDelay(30 * kj::MILLISECONDS).wait(ws);

  KJ_EXPECT(sysClock.now() - before >= 30 * kj::MILLISECONDS);
  // Timer time is synced to the monotonic clock at each wait return.
  KJ_EXPECT(timer.now() - timerBefore >= 30 * kj::MILLISECONDS);
}

KJ_TEST("multiple timers fire in deadline order") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto &timer = io.getTimer();

  kj::Vector<int> order;
  auto p3 = timer.afterDelay(30 * kj::MILLISECONDS).then([&]() {
    order.add(3);
  }).eagerlyEvaluate(nullptr);
  auto p1 =
      timer.afterDelay(5 * kj::MILLISECONDS).then([&]() { order.add(1); }).eagerlyEvaluate(nullptr);
  auto p2 = timer.afterDelay(15 * kj::MILLISECONDS).then([&]() {
    order.add(2);
  }).eagerlyEvaluate(nullptr);

  p3.wait(ws);
  KJ_ASSERT(order.size() == 3);
  KJ_EXPECT(order[0] == 1);
  KJ_EXPECT(order[1] == 2);
  KJ_EXPECT(order[2] == 3);
  p1.wait(ws);
  p2.wait(ws);
}

KJ_TEST("timer fires while blocked waiting on a cross-thread event") {
  // The port must bound each sleep by timeoutToNextEvent(): the loop first wakes at the timer
  // deadline (long before the cross-thread fulfill), fires the timer, then goes back to sleep.
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto &timer = io.getTimer();

  auto paf = kj::newPromiseAndCrossThreadFulfiller<void>();
  bool timerFired = false;
  auto timerPromise = timer.afterDelay(10 * kj::MILLISECONDS).then([&]() {
    timerFired = true;
  }).eagerlyEvaluate(nullptr);

  kj::Thread thread([fulfiller = kj::mv(paf.fulfiller)]() mutable {
    delayMillis(100);
    fulfiller->fulfill();
  });

  paf.promise.wait(ws);
  KJ_EXPECT(timerFired);
  timerPromise.wait(ws);
}

#if !_WIN32
// =======================================================================================
// Sub-millisecond timer precision: the high-resolution short-sleep path (unix-only:
// mach_wait_until on macOS, clock_nanosleep(TIMER_ABSTIME) on Linux). Without it, tokio's
// ~1 ms timer wheel quantizes a 100 µs kj::Timer delay to ~1.1 ms (a ~10x slowdown observed in
// workerd's Timed100us stream-piping benchmarks). Windows stays on the wheel, so these bounds
// don't apply there.

KJ_TEST("sub-millisecond afterDelay is not quantized to tokio's ~1ms timer wheel") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto &timer = io.getTimer();
  auto &sysClock = kj::systemPreciseMonotonicClock();

  // Warm-up: the first short wait lazily spawns the high-res timer thread.
  timer.afterDelay(100 * kj::MICROSECONDS).wait(ws);

  auto sampleDelays = [&](kj::Duration delay) {
    kj::Vector<kj::Duration> samples;
    for (int i = 0; i < 31; i++) {
      auto timerBefore = timer.now();
      auto before = sysClock.now();
      timer.afterDelay(delay).wait(ws);
      samples.add(sysClock.now() - before);
      // Never-early contract, checked on the timer's own clock: afterDelay computes its deadline
      // from timer.now(), which only advances when the loop wakes, so it lags the wall clock
      // slightly -- wall-clock elapsed can legitimately undershoot `delay` by that staleness
      // (observed as a 492 us "500 us" sleep under ASan).
      KJ_EXPECT(timer.now() - timerBefore >= delay, timer.now() - timerBefore, delay);
    }
    std::sort(samples.begin(), samples.end());
    return samples;
  };

  // Bound rationale: the wheel-quantized failure mode is a FLOOR -- tokio rounds every
  // sub-millisecond delay up to the next ~1 ms tick, so under quantization no sample can complete
  // in less than ~1 ms. The high-res path completes a 100 µs sleep in ~115-150 µs on idle
  // hardware; loaded CI runners (macOS VMs especially) inflate the median well past 500 µs, but
  // even there some of 31 samples land near the ideal. So assert on the MINIMUM sample: load
  // noise can't push all 31 samples up, while the wheel floor pushes every one of them >= ~1 ms.
  auto samples100 = sampleDelays(100 * kj::MICROSECONDS);
  KJ_EXPECT(samples100[0] < 500 * kj::MICROSECONDS, samples100[0] / kj::MICROSECONDS);

  auto samples500 = sampleDelays(500 * kj::MICROSECONDS);
  KJ_EXPECT(samples500[0] < 1000 * kj::MICROSECONDS, samples500[0] / kj::MICROSECONDS);
}

KJ_TEST("sequential 100us timers run at ~100us each, not ~1ms each") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto &timer = io.getTimer();
  auto &sysClock = kj::systemPreciseMonotonicClock();

  timer.afterDelay(100 * kj::MICROSECONDS).wait(ws);  // warm-up

  constexpr int kIterations = 200;
  kj::Duration minIteration = kj::maxValue;
  auto timerBefore = timer.now();
  for (int i = 0; i < kIterations; i++) {
    auto iterBefore = sysClock.now();
    timer.afterDelay(100 * kj::MICROSECONDS).wait(ws);
    minIteration = kj::min(minIteration, sysClock.now() - iterBefore);
  }
  // Lower bound on the timer's own clock (see the sampling test above for why wall-clock elapsed
  // can undershoot).
  auto timerElapsed = timer.now() - timerBefore;

  // Ideal total is 20 ms; measured is ~25-40 ms idle but >140 ms on loaded macOS CI VMs, so a
  // total-time bound can't separate load from the wheel-quantized failure mode (>= ~220 ms).
  // Quantization is a floor, though: under it EVERY iteration takes >= ~1 ms, while under mere
  // load at least one of 200 iterations still lands near the ~100 µs ideal. Assert on the
  // fastest iteration.
  KJ_EXPECT(timerElapsed >= kIterations * 100 * kj::MICROSECONDS, timerElapsed / kj::MILLISECONDS);
  KJ_EXPECT(minIteration < 500 * kj::MICROSECONDS, minIteration / kj::MICROSECONDS);
}

KJ_TEST("wake() from another thread interrupts short-timer waits promptly") {
  // While the loop is continuously in the high-res short-wait path (a re-chaining 200 µs
  // timer), a cross-thread fulfill must still get through promptly: the high-res wakeup and
  // wake() share the same Notify, and the wake latch must survive interleaved timer wakeups.
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto &timer = io.getTimer();
  auto &sysClock = kj::systemPreciseMonotonicClock();

  struct Chain {
    static kj::Promise<void> run(kj::Timer &timer) {
      return timer.afterDelay(200 * kj::MICROSECONDS).then([&timer]() { return run(timer); });
    }
  };
  auto keepBusy = Chain::run(timer).eagerlyEvaluate(nullptr);

  auto paf = kj::newPromiseAndCrossThreadFulfiller<int>();
  auto before = sysClock.now();
  kj::Thread thread([fulfiller = kj::mv(paf.fulfiller)]() mutable {
    delayMillis(5);
    fulfiller->fulfill(7);
  });

  KJ_EXPECT(paf.promise.wait(ws) == 7);
  auto elapsed = sysClock.now() - before;
  KJ_EXPECT(elapsed >= 5 * kj::MILLISECONDS, elapsed / kj::MILLISECONDS);
  KJ_EXPECT(elapsed < 100 * kj::MILLISECONDS, elapsed / kj::MILLISECONDS);
}

KJ_TEST("long sleeps remain accurate alongside the high-res short-sleep path") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto &timer = io.getTimer();
  auto &sysClock = kj::systemPreciseMonotonicClock();

  // Prime the high-res thread, then take a long sleep: it must go through the plain tokio
  // path (no spin, no early wake from stale short-timer state).
  timer.afterDelay(100 * kj::MICROSECONDS).wait(ws);

  auto before = sysClock.now();
  timer.afterDelay(20 * kj::MILLISECONDS).wait(ws);
  auto elapsed = sysClock.now() - before;
  KJ_EXPECT(elapsed >= 20 * kj::MILLISECONDS, elapsed / kj::MILLISECONDS);
  KJ_EXPECT(elapsed < 500 * kj::MILLISECONDS, elapsed / kj::MILLISECONDS);
}
#endif  // !_WIN32

// =======================================================================================
// Cross-thread: the wake() -> wait()-returns-true latch is what drains kj::Executor events and
// cross-thread fulfillers.

KJ_TEST("executeAsync from another thread runs on the tokio-ported loop") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();

  const kj::Executor &executor = kj::getCurrentThreadExecutor();
  auto paf = kj::newPromiseAndFulfiller<int>();
  auto fulfiller = kj::mv(paf.fulfiller);

  kj::Thread thread([&executor, &fulfiller]() {
    // A plain portless loop so this thread can wait on the cross-thread promise.
    kj::EventLoop loop;
    kj::WaitScope threadWs(loop);
    kj::uint result = executor
                          .executeAsync([&fulfiller]() {
      // Runs on the main (tokio-ported) loop.
      fulfiller->fulfill(42);
      return 99u;
    }).wait(threadWs);
    KJ_ASSERT(result == 99);
  });

  // While we are blocked here, the other thread's executeAsync must wake the port (wake() ->
  // wait() returns true -> executor drained).
  KJ_EXPECT(paf.promise.wait(ws) == 42);
}

KJ_TEST("cross-thread fulfiller wakes a blocked wait()") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();

  auto paf = kj::newPromiseAndCrossThreadFulfiller<int>();
  kj::Thread thread([fulfiller = kj::mv(paf.fulfiller)]() mutable {
    delayMillis(10);  // Give the main thread time to actually block in wait().
    fulfiller->fulfill(123);
  });

  KJ_EXPECT(paf.promise.wait(ws) == 123);
}

// =======================================================================================
// Rust integration: spawned tokio tasks run while C++ is blocked in promise.wait(), and the
// existing kj-rs future<->promise bridge works unchanged under the new port.

KJ_TEST("Rust task spawned on the loop's runtime completes while C++ is "
        "blocked in wait()") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();

  KJ_EXPECT(has_loop_runtime_handle());

  uint64_t completedBefore = completed_task_count();
  auto promise = spawn_task_on_runtime(20, 42);
  // This top-level wait() parks the thread inside the tokio runtime's block_on, which is what
  // drives the spawned task (and its tokio sleep) to completion.
  KJ_EXPECT(promise.wait(ws) == 42);
  KJ_EXPECT(completed_task_count() == completedBefore + 1);
}

KJ_TEST("tokio timers inside spawned tasks work") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();

  auto &sysClock = kj::systemPreciseMonotonicClock();
  auto before = sysClock.now();
  tokio_sleep_on_runtime(25).wait(ws);
  KJ_EXPECT(sysClock.now() - before >= 25 * kj::MILLISECONDS);
}

KJ_TEST("promise.poll() pumps the tokio scheduler without blocking") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();

  auto promise = spawn_task_on_runtime(30, 7);
  // Not done yet; poll() must not sleep the 30ms away.
  auto &sysClock = kj::systemPreciseMonotonicClock();
  auto before = sysClock.now();
  KJ_EXPECT(!promise.poll(ws));
  KJ_EXPECT(sysClock.now() - before < 30 * kj::MILLISECONDS);

  KJ_EXPECT(promise.wait(ws) == 7);
}

KJ_TEST("kj-rs bridged Rust future with same-thread delayed waker works under the new "
        "port") {
  // A bridged Rust future that suspends, then is re-driven by a wake delivered from a task on the
  // loop's own tokio runtime (same thread, via the TokioEventPort). Exercises the future⇄promise
  // bridge's asynchronous same-thread re-drive path under the new port.
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();

  threaded_wake_future().wait(ws);

  []() -> kj::Promise<void> { co_await threaded_wake_future(); }().wait(ws);
}

KJ_TEST("KJ coroutine can co_await spawned Rust tasks and KJ timers together") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto &timer = io.getTimer();

  int result = [&timer]() -> kj::Promise<int> {
    co_await timer.afterDelay(5 * kj::MILLISECONDS);
    uint32_t value = co_await spawn_task_on_runtime(5, 11);
    co_await timer.afterDelay(5 * kj::MILLISECONDS);
    co_return static_cast<int>(value) + 1;
  }().wait(ws);
  KJ_EXPECT(result == 12);
}

// =======================================================================================
// Teardown.

KJ_TEST("context destruction with pending spawned tasks and armed timers is "
        "clean") {
  {
    auto io = setupTokioAsyncIo();
    auto &ws = io.getWaitScope();
    auto &timer = io.getTimer();

    // A detached, never-completing Rust task: must be dropped by the runtime at teardown.
    spawn_pending_task();

    // Armed timer and an in-flight spawned task; their promises are destroyed (canceling the
    // KJ side) before the context itself, per declaration order.
    auto timerPromise = timer.afterDelay(60 * kj::SECONDS);
    auto spawnedPromise = spawn_task_on_runtime(60'000, 1);
    KJ_EXPECT(!spawnedPromise.poll(ws));
    KJ_EXPECT(!timerPromise.poll(ws));
  }

  // The thread is fully cleaned up: a fresh context on the same thread works.
  {
    auto io = setupTokioAsyncIo();
    auto &ws = io.getWaitScope();
    KJ_EXPECT(kj::evalLater([]() { return 5; }).wait(ws) == 5);
    KJ_EXPECT(has_loop_runtime_handle());
  }
  KJ_EXPECT(!has_loop_runtime_handle());
}

}  // namespace
}  // namespace kj_rs_tokio_test
