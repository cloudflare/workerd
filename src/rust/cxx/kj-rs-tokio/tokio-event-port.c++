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

TokioEventPort::TokioEventPort(): TokioEventPort(new_tokio_port(), LoopDriver::KJ) {}

TokioEventPort::TokioEventPort(::rust::Box<TokioPort> rustPort)
    : TokioEventPort(kj::mv(rustPort), LoopDriver::TOKIO) {}

TokioEventPort::TokioEventPort(::rust::Box<TokioPort> rustPort, LoopDriver driver)
    : ownerThread(kj::ThreadId::current()),
      driver(driver),
      clock(requireNoActivePort()),
      timerImpl(clock.now()),
      rustPort(kj::mv(rustPort)),
      loop(kj::heap<kj::EventLoop>(*this)) {
  activePort = this;
}

TokioEventPort::~TokioEventPort() noexcept(false) {
  KJ_ASSERT(ownerThread == kj::ThreadId::current(),
      "TokioEventPort destroyed on a thread other than its owner") {
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
  // reports `false` right before it sleeps, so a `true` that arrives while KJ is not turning
  // means a tokio task armed a KJ event: hand the thread back to KJ (KJ-driven, end the park;
  // tokio-driven, wake the driver). A `true` reported while KJ is turning is redundant -- the
  // driver runs the loop to idle -- and costs the driver one idle cycle.
  if (runnable) {
    rustPort->notify_kj_service();
  }
}

void TokioEventPort::updateNextTimerEvent(kj::Maybe<kj::TimePoint> time) {
  assertOwnerThread();
  // A timer was armed or cancelled while we sleep. Only a deadline sooner than the one this
  // sleep was planned against needs the thread back: KJ must re-plan the sleep.
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
  KJ_REQUIRE(driver == LoopDriver::KJ,
      "promise.wait() is not allowed on a tokio-driven KJ event loop: nothing may block the loop "
      "thread. Await the promise from a task instead.");
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
  assertOwnerThread();
  // Tokio-driven, the caller is already inside the runtime's block_on (the driver task), so tokio
  // cannot be pumped from here; the wake latch is all KJ needs from this call.
  bool woken = driver == LoopDriver::KJ ? rustPort->poll() : rustPort->take_wake_latch();
  timerImpl.advanceTo(clock.now());
  return woken;
}

void TokioEventPort::wake() const {
  // Callable from any thread; the Rust side latches the flag and unblocks a concurrent wait().
  rustPort->wake();
}

void TokioEventPort::advanceTimer() {
  assertOwnerThread();
  KJ_REQUIRE(driver == LoopDriver::TOKIO, "advanceTimer() is the tokio-driven driver's");
  timerImpl.advanceTo(clock.now());
}

kj::Maybe<uint64_t> TokioEventPort::prepareToYield() {
  assertOwnerThread();
  KJ_REQUIRE(driver == LoopDriver::TOKIO, "prepareToYield() is the tokio-driven driver's");
  // Same as the KJ-driven wait()'s preamble; the next advanceTimer() clears the hooks.
  timerImpl.setSleeping(*this);
  plannedNextEvent = timerImpl.nextEvent();
  return timerImpl.timeoutToNextEvent(clock.now(), kj::NANOSECONDS, kj::maxValue);
}

TokioAsyncIoContext::~TokioAsyncIoContext() noexcept(false) {
  // Cancel spawned tasks while the WaitScope is still alive as well (the port's destructor
  // repeats this harmlessly). A moved-from context (null port) owns nothing.
  if (port.get() != nullptr) {
    port->cancelSpawnedTasks();
  }
}

bool TokioAsyncIoContext::runTurns(uint32_t maxTurns) {
  port->advanceTimer();
  auto& loop = port->getLoop();
  loop.run(maxTurns);
  return loop.isRunnable();
}

bool TokioAsyncIoContext::pollTurns(uint32_t maxTurns) {
  port->advanceTimer();
  waitScope->poll(maxTurns);
  // WaitScope::poll() leaves the loop's last-reported runnable state wherever the turns left it;
  // run(0) re-reports it, so that an event armed while the driver is away produces the
  // setRunnable(true) edge that wakes it.
  auto& loop = port->getLoop();
  loop.run(0);
  return loop.isRunnable();
}

kj::Maybe<uint64_t> TokioAsyncIoContext::prepareToYield() {
  return port->prepareToYield();
}

TokioAsyncIoContext setupTokioAsyncIo() {
  auto port = kj::heap<TokioEventPort>();
  auto waitScope = kj::heap<kj::WaitScope>(port->getLoop());
  return TokioAsyncIoContext(kj::mv(port), kj::mv(waitScope));
}

kj::Own<TokioAsyncIoContext> newTokioDrivenContext(::rust::Box<TokioPort> rustPort) {
  auto port = kj::heap<TokioEventPort>(kj::mv(rustPort));
  auto waitScope = kj::heap<kj::WaitScope>(port->getLoop());
  return kj::heap<TokioAsyncIoContext>(kj::mv(port), kj::mv(waitScope));
}

}  // namespace kj_rs_tokio
