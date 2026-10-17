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

TokioEventPort::TokioEventPort()
    : ownerThread(kj::ThreadId::current()),
      clock(requireNoActivePort()),
      timerImpl(clock.now()),
      rustPort(new_tokio_port()),
      rustWaker(new_tokio_waker(*rustPort)),
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

bool TokioEventPort::wait() {
  assertOwnerThread();
  // Sleep until just *after* the next KJ timer is due (`timeoutToNextEvent()` rounds up), or
  // until tokio has work. No task runs under the sleep, so no KJ timer can be armed under it.
  KJ_IF_SOME(timeoutNs, timerImpl.timeoutToNextEvent(clock.now(), kj::NANOSECONDS, kj::maxValue)) {
    rustPort->park_timeout_ns(timeoutNs);
  } else {
    rustPort->park_forever();
  }
  return tick();
}

bool TokioEventPort::poll() {
  assertOwnerThread();
  return tick();
}

bool TokioEventPort::tick() {
  // The loop has woken: advance the timer first, so the batch of tokio tasks sees the time it
  // runs at (as KJ events do) and the KJ timers now due are armed ahead of whatever the batch
  // arms. Load-bearing: TimerImpl only fires timer events from advanceTo(). Forgetting this after
  // a wait means every kj::Timer promise silently never resolves.
  timerImpl.advanceTo(clock.now());
  return rustPort->tick();
}

void TokioEventPort::wake() const {
  // Callable from any thread; the waker latches the flag and ends a concurrent (or the next)
  // wait(). It must not touch `rustPort`, which is the loop thread's alone.
  rustWaker->wake();
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
