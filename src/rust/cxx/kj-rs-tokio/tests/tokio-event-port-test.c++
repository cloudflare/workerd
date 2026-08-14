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
#include <atomic>
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

// =======================================================================================
// Timer precision: tokio's timer wheel (~1 ms granularity, the same as KJ's own epoll-based
// port, whose epoll_pwait takes a millisecond timeout). No high-resolution side channel.

KJ_TEST("wake() from another thread interrupts a loop cycling through short timers promptly") {
  // While the loop keeps re-arming a short timer (so wait() is always a short timed sleep), a
  // cross-thread fulfill must still get through promptly: the timer wakeup and wake() share the
  // same Notify, and the wake latch must survive interleaved timer wakeups.
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto &timer = io.getTimer();
  auto &sysClock = kj::systemPreciseMonotonicClock();

  struct Chain {
    static kj::Promise<void> run(kj::Timer &timer) {
      return timer.afterDelay(1 * kj::MILLISECONDS).then([&timer]() { return run(timer); });
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
  KJ_EXPECT(elapsed < 1000 * kj::MILLISECONDS, elapsed / kj::MILLISECONDS);
}

KJ_TEST("long sleeps are accurate") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto &timer = io.getTimer();
  auto &sysClock = kj::systemPreciseMonotonicClock();

  auto before = sysClock.now();
  timer.afterDelay(20 * kj::MILLISECONDS).wait(ws);
  auto elapsed = sysClock.now() - before;
  KJ_EXPECT(elapsed >= 20 * kj::MILLISECONDS, elapsed / kj::MILLISECONDS);
  KJ_EXPECT(elapsed < 1000 * kj::MILLISECONDS, elapsed / kj::MILLISECONDS);
}

KJ_TEST("a cancelled timer does not fire and a later one still does") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto &timer = io.getTimer();

  bool earlyFired = false;
  auto early = timer.afterDelay(5 * kj::MILLISECONDS).then([&]() {
    earlyFired = true;
  }).eagerlyEvaluate(nullptr);
  auto late = timer.afterDelay(30 * kj::MILLISECONDS);
  early = nullptr;  // cancel: deregisters from TimerImpl before the loop ever sleeps

  auto before = timer.now();
  late.wait(ws);
  KJ_EXPECT(!earlyFired);
  KJ_EXPECT(timer.now() - before >= 30 * kj::MILLISECONDS);
}

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

  auto promise = spawn_task_on_runtime(500, 7);
  // Not done yet; poll() must not sleep the task's 500 ms away. The bound is generous on
  // purpose: it distinguishes "returned without parking" from "slept until the task finished",
  // not scheduler latency.
  auto &sysClock = kj::systemPreciseMonotonicClock();
  auto before = sysClock.now();
  KJ_EXPECT(!promise.poll(ws));
  KJ_EXPECT(sysClock.now() - before < 250 * kj::MILLISECONDS);

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

kj::Timer *testTimer = nullptr;
void setTestTimer(kj::Timer *timer) {
  testTimer = timer;
}
kj::WaitScope *testWaitScope = nullptr;
void setTestWaitScope(kj::WaitScope *ws) {
  testWaitScope = ws;
}
kj::Maybe<kj::Own<kj::PromiseFulfiller<int>>> testFulfiller;
}  // namespace

// External linkage: called by the cxx bridge (kj_rs_tokio_test::kjTimerDelay etc.).
kj::Promise<void> kjTimerDelay(uint64_t ms) {
  KJ_REQUIRE(testTimer != nullptr, "setTestTimer() not called");
  return testTimer->afterDelay(ms * kj::MILLISECONDS);
}

kj::Promise<void> kjNeverPromise() {
  return kj::NEVER_DONE;
}

void nestedWait() {
  KJ_REQUIRE(testWaitScope != nullptr, "setTestWaitScope() not called");
  kj::evalLater([]() {}).wait(*testWaitScope);
}

void fulfillTestFulfiller(int32_t value) {
  KJ_ASSERT_NONNULL(testFulfiller, "setTestFulfiller() not called")->fulfill(kj::cp(value));
}

namespace {

KJ_TEST("context destruction with a spawned task HOLDING a KJ timer promise is clean") {
  // Unlike the test below, the spawned task itself owns live KJ objects (TimerPromiseAdapter in
  // the port's TimerImpl, armed RustPromiseAwaiter Event on the loop) at teardown. The LocalSet
  // cancellation that drops them must run while the timer and loop still exist.
  {
    auto io = setupTokioAsyncIo();
    auto &ws = io.getWaitScope();
    setTestTimer(&io.getTimer());
    KJ_DEFER(setTestTimer(nullptr));
    spawn_task_holding_kj_timer();
    // Let the task start: it registers the timer and arms its awaiter, then parks.
    kj::evalLater([]() {}).wait(ws);
    ws.poll();
  }
  {
    auto io = setupTokioAsyncIo();
    KJ_EXPECT(kj::evalLater([]() { return 7; }).wait(io.getWaitScope()) == 7);
  }
}

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

KJ_TEST("context destruction with a spawned task awaiting a KJ promise is clean") {
  // The task owns an OwnPromiseNode plus an armed RustPromiseAwaiter event registered with the
  // loop. Cancellation must drop them while the loop still exists (else ~Event unlinks from a
  // freed queue).
  {
    auto io = setupTokioAsyncIo();
    auto &ws = io.getWaitScope();
    spawn_task_awaiting_kj_never_promise();
    kj::evalLater([]() {}).wait(ws);
    ws.poll();
  }
  auto io = setupTokioAsyncIo();
  KJ_EXPECT(kj::evalLater([]() { return 8; }).wait(io.getWaitScope()) == 8);
}

KJ_TEST("one TokioEventPort per thread") {
  auto io = setupTokioAsyncIo();
  // A second port on this thread is refused (KJ's one-loop-per-thread model): the C++ side
  // KJ_REQUIREs it and the Rust side asserts it; either surfaces as a kj::Exception.
  KJ_EXPECT_THROW(FAILED, kj::heap<kj_rs_tokio::TokioEventPort>());
  // The failed construction must not have disturbed the live port.
  KJ_EXPECT(kj::evalLater([]() { return 1; }).wait(io.getWaitScope()) == 1);
  KJ_EXPECT(spawn_task_on_runtime(1, 4).wait(io.getWaitScope()) == 4);
}

KJ_TEST("bridged future woken from a plain std::thread while the loop is parked") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();

  // The wake comes from a thread with neither a KJ loop nor a tokio context, ~20ms after the
  // loop parks in the port's block_on: FutureWakerCell's cross-thread fulfiller -> this loop's
  // Executor -> TokioEventPort::wake() -> unpark. A lost hop hangs this wait().
  std_thread_wake_future().wait(ws);
  // And again, back to back, so the second wake targets a freshly renewed fulfiller.
  std_thread_wake_future().wait(ws);
}

// =======================================================================================
// A tokio task hands KJ work by means other than a bridged waker while the loop is parked in
// wait(): KJ reports it to the port (setRunnable(true) for events, the TimerImpl SleepHooks for
// timers) and the park must end. Each test is bounded by a long timer that turns "hangs forever"
// into a failed assertion.

KJ_TEST("a spawned task fulfilling a kj::PromiseFulfiller while the loop is parked wakes it") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto &timer = io.getTimer();
  auto &sysClock = kj::systemPreciseMonotonicClock();

  auto paf = kj::newPromiseAndFulfiller<int>();
  testFulfiller = kj::mv(paf.fulfiller);
  KJ_DEFER(testFulfiller = kj::none);

  // The task fulfills ~20 ms into the park; the 10 s timer is the "we hung" bound. Without KJ
  // reporting the arm (setRunnable(true)) it sits unserviced until that timer.
  task_fulfills_kj_fulfiller(20, 42);
  auto before = sysClock.now();
  int value = paf.promise
                  .exclusiveJoin(timer.afterDelay(10 * kj::SECONDS).then([]() -> int {
    KJ_FAIL_ASSERT("KJ event armed by a tokio task was not serviced while the loop was parked");
  })).wait(ws);
  KJ_EXPECT(value == 42);
  KJ_EXPECT(sysClock.now() - before < 2 * kj::SECONDS);
}

KJ_TEST("a spawned task fulfilling a kj::PromiseFulfiller during a wait-forever park wakes it") {
  // No timer at all: wait() plans to sleep forever. A hang here is a real hang, so this test
  // guards itself with a cross-thread kill switch instead of a KJ timer.
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();

  auto paf = kj::newPromiseAndFulfiller<int>();
  testFulfiller = kj::mv(paf.fulfiller);
  KJ_DEFER(testFulfiller = kj::none);

  auto bound = kj::newPromiseAndCrossThreadFulfiller<int>();
  std::atomic<bool> done{false};
  kj::Thread watchdog([&done, fulfiller = kj::mv(bound.fulfiller)]() mutable {
    for (int i = 0; i < 1000 && !done.load(); i++) delayMillis(10);
    if (!done.load()) fulfiller->fulfill(-1);
  });

  task_fulfills_kj_fulfiller(20, 7);
  int value = paf.promise.exclusiveJoin(kj::mv(bound.promise)).wait(ws);
  done.store(true);
  KJ_EXPECT(
      value == 7, "KJ event armed by a tokio task was not serviced during a wait-forever park");
}

KJ_TEST("a KJ timer armed by a spawned task during the park is honored at its own deadline") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto &timer = io.getTimer();
  auto &sysClock = kj::systemPreciseMonotonicClock();
  setTestTimer(&timer);
  KJ_DEFER(setTestTimer(nullptr));

  // wait() plans against the 10 s bound; 10 ms (wall clock) into the park the task arms a 20 ms
  // KJ timer and awaits it to completion. The park must end at that timer, not at 10 s.
  //
  // The port is the TimerImpl's SleepHooks while parked, so kj::Timer::now() reads the live
  // clock: the task's afterDelay(20ms) is measured from the moment it is armed (~10 ms in), and
  // the timer is due at ~30 ms of wall time -- KJ's intended semantics for code using the timer
  // while the loop sleeps (see kj::TimerImpl::setSleeping).
  auto before = sysClock.now();
  task_awaits_kj_timer(10, 20)
      .exclusiveJoin(timer.afterDelay(10 * kj::SECONDS).then([]() {
    KJ_FAIL_ASSERT("KJ timer armed by a tokio task during the park was not honored");
  })).wait(ws);
  auto elapsed = sysClock.now() - before;
  KJ_EXPECT(elapsed >= 30 * kj::MILLISECONDS, elapsed / kj::MILLISECONDS);
  KJ_EXPECT(elapsed < 2 * kj::SECONDS, elapsed / kj::MILLISECONDS);
}

KJ_TEST("a KJ timer armed by a spawned task during a wait-forever park is honored") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  setTestTimer(&io.getTimer());
  KJ_DEFER(setTestTimer(nullptr));

  auto bound = kj::newPromiseAndCrossThreadFulfiller<void>();
  std::atomic<bool> done{false};
  kj::Thread watchdog([&done, fulfiller = kj::mv(bound.fulfiller)]() mutable {
    for (int i = 0; i < 1000 && !done.load(); i++) delayMillis(10);
    if (!done.load()) fulfiller->reject(KJ_EXCEPTION(FAILED, "park never ended"));
  });

  task_awaits_kj_timer(10, 20).exclusiveJoin(kj::mv(bound.promise)).wait(ws);
  done.store(true);
}

KJ_TEST("cross-thread bridged wake arriving while the loop is busy (not parked) is delivered") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto &timer = io.getTimer();
  auto &sysClock = kj::systemPreciseMonotonicClock();

  // The wake lands ~20 ms in; keep the loop turning events (never parking) for ~60 ms. The wake
  // then travels cell -> cross-thread fulfiller -> Executor -> port.wake(); the loop only sees
  // the latch through busy-poll's port.poll() (enabled here; KJ's default never busy-polls),
  // never through wait().
  ws.setBusyPollInterval(8);
  auto woken = std_thread_wake_future();
  auto busyUntil = sysClock.now() + 60 * kj::MILLISECONDS;
  [&]() -> kj::Promise<void> {
    while (sysClock.now() < busyUntil) {
      co_await kj::evalLater([]() {});
    }
  }().wait(ws);

  woken
      .exclusiveJoin(timer.afterDelay(10 * kj::SECONDS).then([]() {
    KJ_FAIL_ASSERT("cross-thread wake during a busy loop was lost");
  })).wait(ws);
}

KJ_TEST("a bare TokioEventPort (no context) tears down cleanly with tasks holding KJ objects") {
  // The port owns its loop and cancels spawned tasks before destroying anything, so even without
  // a TokioAsyncIoContext a task holding a KJ timer promise and an armed awaiter event is dropped
  // while the loop and timer are alive. ASAN target.
  {
    auto port = kj::heap<kj_rs_tokio::TokioEventPort>();
    kj::WaitScope ws(port->getLoop());
    setTestTimer(&port->getTimer());
    KJ_DEFER(setTestTimer(nullptr));
    spawn_task_holding_kj_timer();
    spawn_task_awaiting_kj_never_promise();
    kj::evalLater([]() {}).wait(ws);
    ws.poll();
    // `ws` is destroyed first (declared after `port`), then the port: tasks, loop, runtime, timer.
  }
  auto io = setupTokioAsyncIo();
  KJ_EXPECT(kj::evalLater([]() { return 9; }).wait(io.getWaitScope()) == 9);
}

KJ_TEST("a task that yields forever keeps poll() bounded and does not starve the KJ loop") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();

  spawn_yield_loop_task();
  // poll() lets ready tasks run for a bounded number of turns (POLL_YIELD_BUDGET) and returns.
  ws.poll();
  ws.poll();
  // The KJ side still makes progress alongside the spinning task.
  KJ_EXPECT(kj::evalLater([]() { return 21; }).wait(ws) == 21);
  KJ_EXPECT(spawn_task_on_runtime(1, 9).wait(ws) == 9);
  // The spinning task is cancelled at teardown (LocalSet cancellation, above).
}

KJ_TEST("a spawned task re-entering promise.wait() gets a kj::Exception, not an abort") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  setTestWaitScope(&ws);
  KJ_DEFER(setTestWaitScope(nullptr));

  // Documented in tokio-event-port.h: nesting block_on inside block_on is rejected by tokio;
  // the panic must reach the task as a catchable exception (an Err across the bridge), and the
  // outer loop must stay healthy.
  nested_wait_from_task().wait(ws);
  KJ_EXPECT(kj::evalLater([]() { return 3; }).wait(ws) == 3);
}

KJ_TEST("already-due timer returns promptly from wait()") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto &timer = io.getTimer();

  // timeoutToNextEvent() is zero: wait() must not sleep; it returns and advanceTo() fires the
  // timer.
  auto start = kj::systemPreciseMonotonicClock().now();
  timer.afterDelay(0 * kj::MILLISECONDS).wait(ws);
  timer.atTime(timer.now()).wait(ws);
  KJ_EXPECT(kj::systemPreciseMonotonicClock().now() - start < 200 * kj::MILLISECONDS);
}

KJ_TEST("two tokio-ported loops on two threads executeAsync into each other concurrently") {
  // Both directions at once, N round trips each way, so both ports' wake paths (Executor ->
  // wake() -> block_on unpark) race under load. TSAN target. A lost wake hangs the test.
  //
  // Protocol: each side publishes its Executor, runs N round trips into the peer while its own
  // loop services the peer's round trips, then fulfills the peer's "I'm finished" cross-thread
  // fulfiller and waits (on its own loop, still servicing) for the peer's. Only when both have
  // finished does either loop go away, so no call is ever left without a live target.
  constexpr kj::uint N = 200;
  using ExecSlot = kj::MutexGuarded<kj::Maybe<kj::Own<const kj::Executor>>>;
  using FulfillerSlot =
      kj::MutexGuarded<kj::Maybe<kj::Own<const kj::CrossThreadPromiseFulfiller<void>>>>;
  ExecSlot mainExecSlot;
  ExecSlot otherExecSlot;
  // Each side creates its own PAF (the promise stays on its loop) and hands the fulfiller over.
  FulfillerSlot mainFinishedSlot;   // fulfilled by `other` when it is done
  FulfillerSlot otherFinishedSlot;  // fulfilled by main when it is done

  auto takeFromSlot = [](auto &slot) {
    auto lock = slot.lockExclusive();
    lock.wait([](auto &v) { return v != kj::none; });
    return kj::mv(KJ_ASSERT_NONNULL(*lock));
  };

  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto mainFinished = kj::newPromiseAndCrossThreadFulfiller<void>();
  *mainFinishedSlot.lockExclusive() = kj::mv(mainFinished.fulfiller);
  *mainExecSlot.lockExclusive() = kj::getCurrentThreadExecutor().addRef();

  kj::Thread other([&]() noexcept {
    auto io2 = setupTokioAsyncIo();
    auto &ws2 = io2.getWaitScope();
    auto otherFinished = kj::newPromiseAndCrossThreadFulfiller<void>();
    *otherFinishedSlot.lockExclusive() = kj::mv(otherFinished.fulfiller);
    *otherExecSlot.lockExclusive() = kj::getCurrentThreadExecutor().addRef();

    auto mainExec = takeFromSlot(mainExecSlot);
    for (kj::uint i = 0; i < N; i++) {
      KJ_ASSERT(mainExec->executeAsync([i]() { return i; }).wait(ws2) == i);
    }
    takeFromSlot(mainFinishedSlot)->fulfill();
    // Keep servicing main's round trips until it reports finished.
    otherFinished.promise.wait(ws2);
  });

  auto otherExec = takeFromSlot(otherExecSlot);
  for (kj::uint i = 0; i < N; i++) {
    KJ_EXPECT(otherExec->executeAsync([i]() { return i * 2; }).wait(ws) == i * 2);
  }
  takeFromSlot(otherFinishedSlot)->fulfill();
  mainFinished.promise.wait(ws);
}

KJ_TEST("a spawned task that panics surfaces as a kj::Exception, not an abort") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();

  // tokio isolates a spawned task's panic into a JoinError; the bridged future maps it to an
  // Err, so co_await throws rather than aborting the process.
  kj::Maybe<kj::Exception> maybeException;
  try {
    spawn_panicking_task().wait(ws);
  } catch (...) {
    maybeException = kj::getCaughtExceptionAsKj();
  }
  KJ_EXPECT(maybeException != kj::none, "a panicking spawned task must throw");

  // The loop is still healthy afterward.
  KJ_EXPECT(kj::evalLater([]() { return 11; }).wait(ws) == 11);
}

KJ_TEST("a detached spawned task (JoinHandle dropped) still runs to completion") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();

  uint64_t before = completed_task_count();
  spawn_detached_completing_task();  // drops the JoinHandle immediately
  // Drive the loop until the detached task has run (bounded; each afterDelay lets the runtime
  // turn). Dropping the handle must NOT have cancelled it.
  for (int i = 0; i < 200 && completed_task_count() == before; i++) {
    io.getTimer().afterDelay(2 * kj::MILLISECONDS).wait(ws);
  }
  KJ_EXPECT(completed_task_count() == before + 1);
}

}  // namespace
}  // namespace kj_rs_tokio_test
