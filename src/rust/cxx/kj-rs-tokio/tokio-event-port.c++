#include "kj-rs-tokio/tokio-event-port.h"

#include "kj-rs/waker.h"

#include <kj/debug.h>

namespace kj_rs_tokio {

namespace {
// The port active on this loop thread, used by the arm-nudge thunk installed into
// kj_rs::futurePollArmNudge. One port per thread (KJ's one-loop-per-thread model).
thread_local TokioEventPort* activePort = nullptr;

// Captureless thunk (matches the `void(*)()` hook type) that nudges this thread's active port.
void portArmNudgeThunk() {
  if (activePort != nullptr) {
    activePort->nudge();
  }
}
}  // namespace

TokioEventPort::TokioEventPort()
    : clock(kj::systemPreciseMonotonicClock()),
      rustPort(new_tokio_port()),
      timerImpl(clock.now()) {
  // Install the same-thread arm-nudge hook so a tokio task that arms a KJ event during our
  // block_on() park reliably wakes us even when KJ's edge-triggered setRunnable() misses it.
  activePort = this;
  kj_rs::futurePollArmNudge = &portArmNudgeThunk;
}

TokioEventPort::~TokioEventPort() {
  if (activePort == this) {
    activePort = nullptr;
    kj_rs::futurePollArmNudge = nullptr;
  }
}

bool TokioEventPort::wait() {
  bool woken;
  // Bound the sleep by the next KJ timer deadline, if any. `timeoutToNextEvent()` rounds up, so
  // we always sleep until just *after* the timer is due.
  KJ_IF_SOME(timeoutNs, timerImpl.timeoutToNextEvent(clock.now(), kj::NANOSECONDS, kj::maxValue)) {
    woken = rustPort->wait_timeout_ns(timeoutNs);
  } else {
    woken = rustPort->wait_forever();
  }

  // Load-bearing: TimerImpl only fires timer events from advanceTo(). Forgetting this after a
  // wait means every kj::Timer promise silently never resolves.
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

void TokioEventPort::setRunnable(bool runnable) {
  // Called by the EventLoop (always on the loop's own thread) on empty<->runnable transitions.
  // See the member comment on `runnable` for the future scheduled-pump hook point.
  this->runnable = runnable;
  if (runnable) {
    // If we are currently parked inside wait()'s block_on, a tokio task just re-entered C++ and
    // armed a KJ event; unblock so the loop can service its queue. No-op otherwise.
    rustPort->notify_runnable();
  }
}

void TokioEventPort::nudge() {
  // Same-thread nudge: if we are parked inside wait()'s block_on, unblock so the loop services the
  // KJ event a tokio task just armed. notify_runnable() no-ops when not parked.
  rustPort->notify_runnable();
}

TokioAsyncIoContext setupTokioAsyncIo() {
  auto port = kj::heap<TokioEventPort>();
  auto loop = kj::heap<kj::EventLoop>(*port);
  auto waitScope = kj::heap<kj::WaitScope>(*loop);
  return TokioAsyncIoContext{kj::mv(port), kj::mv(loop), kj::mv(waitScope)};
}

}  // namespace kj_rs_tokio
