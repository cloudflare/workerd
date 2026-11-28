#include "kj-rs-tokio/tokio-event-port.h"

#include "kj-rs-tokio/ffi.rs.h"

#include <kj/debug.h>

#include <cstdlib>

namespace kj_rs_tokio {

namespace {
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

// See `kj_rs_tokio::Progress` (progress.rs) for why the busy half of the count is a KJ event.
class TokioEventPort::ProgressProbe final: public kj::_::Event {
 public:
  explicit ProgressProbe(const TokioPort& rustPort)
      : kj::_::Event(kj::SourceLocation()),
        rustPort(rustPort) {}

  void traceEvent(kj::_::TraceBuilder&) override {}

 private:
  void fire() override {
    rustPort.count_progress();
    // Back behind every depth-first and breadth-first event armed so far, to fire once more per
    // pass through the queue. If there are none, the loop has run out of work: leave the queue,
    // so that it runs its `evalLast()` events and sleeps. The port arms the probe again once the
    // loop has events to turn.
    armBreadthFirst();
    if (isNext()) {
      disarm();
    }
  }

  const TokioPort& rustPort;
};

TokioEventPort::TokioEventPort()
    : ownerThread(kj::ThreadId::current()),
      clock(requireNoActivePort()),
      timerImpl(clock.now()),
      rustPort(new_tokio_port()),
      loop(kj::heap<kj::EventLoop>(*this)) {
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
  // A task may hold a `kj_rs::SendKjFuture` or `kj_rs::LoopChecked`, which must be dropped with its
  // loop current, but the port's owner may already have left the loop's scope, e.g. by destroying
  // its WaitScope.
  kj::Maybe<kj::WaitScope> scope;
  if (kj::tryGetCurrentThreadExecutor() == kj::none) {
    scope.emplace(*loop);
  }
  // Guarded so a task-drop panic (surfaced as a kj::Exception by the cxx fork) does not
  // std::terminate if the caller is already unwinding (this runs from destructors).
  unwindDetector.catchExceptionsIfUnwinding([this]() { rustPort->cancel_spawned_tasks(); });
}

void TokioEventPort::startProgressProbe() {
  assertOwnerThread();
  if (progressProbe == kj::none) {
    progressProbe = kj::heap<ProgressProbe>(*rustPort);
  }
  armProgressProbe();
}

void TokioEventPort::armProgressProbe() {
  KJ_IF_SOME(probe, progressProbe) {
    probe->armBreadthFirst();
  }
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
  KJ_DEFER(timerImpl.advanceTo(clock.now()));
  bool woken;
  KJ_IF_SOME(timeoutNs, timerImpl.timeoutToNextEvent(clock.now(), kj::NANOSECONDS, kj::maxValue)) {
    woken = rustPort->wait_timeout_ns(timeoutNs);
  } else {
    woken = rustPort->wait_forever();
  }
  // Whatever ended the park may start a busy stretch of the loop: events armed during it, the
  // timers advanceTo() fires below, the cross-thread events KJ takes in once this returns. The
  // probe counts it, or, if the loop finds nothing to turn, leaves the queue at once.
  armProgressProbe();
  return woken;
}

bool TokioEventPort::poll() {
  assertOwnerThread();
  bool woken = rustPort->poll();
  timerImpl.advanceTo(clock.now());
  // Only if the loop has events to turn. `WaitScope::poll()` returns, and KJ arms a
  // `yieldUntilWouldSleep()` waiter, only once a poll leaves the loop with none, which a probe
  // armed regardless would prevent. That rules out arming it for `woken` too, ahead of the
  // cross-thread events KJ takes in once this returns: a wake can outlive its event, which KJ
  // cancels without consuming the wake, and another thread that keeps submitting and cancelling
  // would keep the loop runnable for good. So the probe counts neither the busy stretch that a
  // cross-thread event starts here nor one that a waiter starts (see `kj_rs_tokio::Progress`).
  if (loop->isRunnable()) {
    armProgressProbe();
  }
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

void startProgressProbe() {
  KJ_REQUIRE(activePort != nullptr, "no TokioEventPort drives this thread");
  activePort->startProgressProbe();
}

}  // namespace kj_rs_tokio
