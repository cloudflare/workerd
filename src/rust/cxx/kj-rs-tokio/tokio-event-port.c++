#include "kj-rs-tokio/tokio-event-port.h"

#include <kj/debug.h>

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
  return kj::systemPreciseMonotonicClock();
}
}  // namespace

TokioEventPort::TokioEventPort()
    : clock(requireNoActivePort()),
      timerImpl(clock.now()),
      rustPort(new_tokio_port()),
      loop(kj::heap<kj::EventLoop>(*this)) {
  activePort = this;
}

TokioEventPort::~TokioEventPort() noexcept(false) {
  // Cancel spawned tasks while the loop and timer -- our members, destroyed after this body --
  // are still alive: a cancelled task's destructors may unlink an armed kj::_::Event from the
  // loop or deregister a kj::TimerImpl timer.
  cancelSpawnedTasks();
  if (activePort == this) {
    activePort = nullptr;
  }
}

void TokioEventPort::cancelSpawnedTasks() {
  // Guarded so a task-drop panic (surfaced as a kj::Exception by the cxx fork) does not
  // std::terminate if the caller is already unwinding (this runs from destructors).
  unwindDetector.catchExceptionsIfUnwinding([this]() { rustPort->cancel_spawned_tasks(); });
}

void TokioEventPort::setRunnable(bool runnable) {
  // Called by the EventLoop on the loop thread on empty <-> runnable transitions. The loop
  // reports `false` right before it calls wait(), so a `true` that arrives while we are parked
  // means a tokio task armed a KJ event: hand the thread back to KJ.
  if (runnable) {
    rustPort->notify_kj_service();
  }
}

void TokioEventPort::updateNextTimerEvent(kj::Maybe<kj::TimePoint> time) {
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
  bool woken;
  // Bound the sleep by the next KJ timer deadline, if any, and remember which deadline that was
  // so updateNextTimerEvent() can spot a sooner one armed during the park. `timeoutToNextEvent()`
  // rounds up, so we always sleep until just *after* the timer is due.
  plannedNextEvent = timerImpl.nextEvent();
  KJ_DEFER(plannedNextEvent = kj::none);
  // While parked, be the timer's sleep hooks: tokio tasks arming KJ timers mid-park get live
  // time from now() and re-plan the sleep if their deadline is sooner. advanceTo() below clears
  // the hooks (KJ's contract for setSleeping()).
  timerImpl.setSleeping(*this);
  KJ_IF_SOME(timeoutNs, timerImpl.timeoutToNextEvent(clock.now(), kj::NANOSECONDS, kj::maxValue)) {
    woken = rustPort->wait_timeout_ns(timeoutNs);
  } else {
    woken = rustPort->wait_forever();
  }

  // Load-bearing: TimerImpl only fires timer events from advanceTo(). Forgetting this after a
  // wait means every kj::Timer promise silently never resolves. (This also clears the sleep
  // hooks installed above.)
  timerImpl.advanceTo(clock.now());
  return woken;
}

bool TokioEventPort::poll() {
  bool woken = rustPort->poll();
  timerImpl.advanceTo(clock.now());
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

TokioAsyncIoContext setupTokioAsyncIo() {
  auto port = kj::heap<TokioEventPort>();
  auto waitScope = kj::heap<kj::WaitScope>(port->getLoop());
  return TokioAsyncIoContext(kj::mv(port), kj::mv(waitScope));
}

}  // namespace kj_rs_tokio
