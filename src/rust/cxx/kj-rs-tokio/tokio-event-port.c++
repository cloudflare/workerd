#include "kj-rs-tokio/tokio-event-port.h"

#include "kj-rs-tokio/ffi.rs.h"

#include <kj/debug.h>

#include <cstdlib>

namespace kj_rs_tokio {

namespace {
// How often a loop that stays busy gives tokio a turn: often enough that a timer or a socket on
// the runtime is served within tens of milliseconds of its event however long KJ stays busy, and
// seldom enough that the turns cost a busy loop no measurable time.
constexpr kj::Duration BUSY_POLL_INTERVAL = 20 * kj::MILLISECONDS;

// The port active on this loop thread, to enforce one port per thread (KJ's one-loop-per-thread
// model); set by the constructor, cleared by the destructor.
thread_local TokioEventPort* activePort = nullptr;

// Enforce one port per thread on the C++ side too (the Rust half asserts the same when it
// registers its runtime handle), so the `activePort` invariant is guaranteed where the pointer
// lives rather than relied upon from across the bridge.
const kj::MonotonicClock& requireNoActivePort() {
  KJ_REQUIRE(activePort == nullptr,
      "only one TokioEventPort may exist per thread (KJ's one-loop-per-thread model)");
  KJ_REQUIRE(kj::tryGetCurrentThreadExecutor() == kj::none,
      "cannot create a TokioEventPort while another KJ event loop is current");
  return kj::systemPreciseMonotonicClock();
}
}  // namespace

TokioEventPort::TokioEventPort()
    : ownerThread(kj::ThreadId::current()),
      clock(requireNoActivePort()),
      coarseClock(kj::systemCoarseMonotonicClock()),
      timerImpl(clock.now()),
      rustPort(new_tokio_port()),
      loop(kj::heap<kj::EventLoop>(*this)),
      lastTokioTurn(coarseClock.now()) {
  activePort = this;
}

TokioEventPort::~TokioEventPort() noexcept(false) {
  if (ownerThread != kj::ThreadId::current()) {
    KJ_LOG(FATAL, "TokioEventPort destroyed on a thread other than its owner");
    std::abort();
  }
  // Cancel spawned tasks while the loop and timer -- our members, destroyed after this body --
  // are still alive: a cancelled task's destructors may unlink an armed kj::_::Event from the
  // loop or deregister a kj::TimerImpl timer.
  cancelSpawnedTasks();
  if (activePort == this) {
    activePort = nullptr;
  }
}

void TokioEventPort::assertOwnerThread() const {
  KJ_REQUIRE(ownerThread == kj::ThreadId::current(),
      "TokioEventPort used from a thread other than its owner");
}

void TokioEventPort::cancelSpawnedTasks() {
  assertOwnerThread();
  // Guarded so a task-drop panic (surfaced as a kj::Exception by the cxx fork) does not
  // std::terminate if the caller is already unwinding (this runs from destructors).
  unwindDetector.catchExceptionsIfUnwinding([this]() { rustPort->cancel_spawned_tasks(); });
}

void TokioEventPort::setRunnable(bool runnable) {
  assertOwnerThread();
  // Called by the EventLoop on the loop thread on empty <-> runnable transitions. The loop
  // reports `false` right before it calls wait(), so a `true` that arrives while we are parked
  // means a tokio task armed a KJ event: hand the thread back to KJ.
  if (runnable) {
    rustPort->notify_kj_service();
  }
}

void TokioEventPort::updateNextTimerEvent(kj::Maybe<kj::TimePoint> time) {
  assertOwnerThread();
  // A timer was armed or cancelled while we sleep. Only a deadline sooner than the one this
  // wait() was planned against needs the thread back: KJ must re-plan the sleep.
  KJ_IF_SOME(next, time) {
    bool sooner =
        plannedNextEvent.map([&](kj::TimePoint planned) { return next < planned; }).orDefault(true);
    if (sooner) {
      rustPort->notify_kj_service();
    }
  }
}

bool TokioEventPort::wait() {
  assertOwnerThread();
  // Bound the sleep by the next KJ timer deadline, if any, and remember which deadline that was
  // so updateNextTimerEvent() can spot a sooner one armed during the park. `timeoutToNextEvent()`
  // rounds up, so we always sleep until just *after* the timer is due.
  plannedNextEvent = timerImpl.nextEvent();
  KJ_DEFER(plannedNextEvent = kj::none);
  // While parked, be the timer's sleep hooks: tokio tasks arming KJ timers mid-park get live
  // time from now() and re-plan the sleep if their deadline is sooner. The advanceTo() deferred
  // below clears the hooks (KJ's contract for setSleeping()).
  timerImpl.setSleeping(*this);
  // Load-bearing: TimerImpl only fires timer events from advanceTo(). Forgetting this after a
  // wait means every kj::Timer promise silently never resolves. Deferred so a wait that throws
  // also clears the sleep hooks.
  KJ_DEFER({
    timerImpl.advanceTo(clock.now());
    lastTokioTurn = coarseClock.now();
    turning = true;
  });
  KJ_IF_SOME(timeoutNs, timerImpl.timeoutToNextEvent(clock.now(), kj::NANOSECONDS, kj::maxValue)) {
    return rustPort->wait_timeout_ns(timeoutNs);
  } else {
    return rustPort->wait_forever();
  }
}

bool TokioEventPort::poll() {
  assertOwnerThread();
  // KJ polls after every turn of a promise.wait() (busy polling, which TokioAsyncIoContext
  // enables), and from waitScope.poll() and promise.poll() only once the queue is empty. A busy
  // poll runs tokio every BUSY_POLL_INTERVAL, on the coarse clock: other busy polls cost a turn
  // little more than that clock's read.
  bool due = coarseClock.now() - lastTokioTurn >= BUSY_POLL_INTERVAL;
  bool woken;
  if (loop->isRunnable()) {
    turning = true;
    // Every busy poll reports the wake latch, so that cross-thread events reach KJ within a turn
    // whether tokio runs or not.
    if (!due) {
      return rustPort->take_wake_latch();
    }
    woken = rustPort->poll();
    // The KJ timer stays put while KJ has work queued: a due timer fires once the queue drains,
    // as on a loop that does not busy poll, so a timer of zero still fires after the work queued
    // ahead of it.
    lastTokioTurn = coarseClock.now();
    return woken;
  } else {
    // A poll that finds the queue empty always runs tokio. A waitScope.poll() that empties the
    // queue within its turn limit ends with such a poll, and so does a promise.poll() whose
    // promise is still pending then; one whose promise becomes ready while queued events run
    // returns without polling the port. KJ also polls this way after the busy turn that emptied
    // the queue, and then waits, and the wait runs tokio as it parks. KJ cannot tell us
    // which this is, so after turns that a wait() or a busy poll left queued the poll yields to
    // tokio only once, unless BUSY_POLL_INTERVAL has passed: its ready tasks and drivers run,
    // without the full pump that would double the cost of going idle. Any other such poll is
    // the full pump. So a waitScope.poll() that runs turns left queued gives tokio one yield,
    // and the next one, with nothing queued in between, the full pump.
    bool afterTurns = turning;
    turning = false;
    woken = afterTurns && !due ? rustPort->poll_once() : rustPort->poll();
  }
  timerImpl.advanceTo(clock.now());
  lastTokioTurn = coarseClock.now();
  return woken;
}

void TokioEventPort::wake() const {
  // Callable from any thread; the Rust side latches the flag and unblocks a concurrent wait().
  rustPort->wake();
}

TokioAsyncIoContext::~TokioAsyncIoContext() noexcept(false) {
  // Cancel spawned tasks while the WaitScope is still alive as well (the port's destructor
  // repeats this harmlessly). A moved-from context (null port) owns nothing.
  if (port.get() != nullptr) {
    port->cancelSpawnedTasks();
  }
}

void TokioAsyncIoContext::blockOn(::rust::Box<BlockOnTask> task) {
  block_on_task_join(kj::mv(task)).wait(*waitScope);
}

TokioAsyncIoContext setupTokioAsyncIo() {
  return TokioAsyncIoContext();
}

kj::Own<TokioAsyncIoContext> newTokioAsyncIoContext() {
  return kj::heap<TokioAsyncIoContext>();
}

}  // namespace kj_rs_tokio
