#include "waker.h"

#include "awaiter.h"

namespace kj_rs {

// Definition of the arm-nudge hook declared in waker.h; null until an integrating event port
// (kj-rs-tokio's TokioEventPort) installs itself. Thread-local: one loop/port per thread.
thread_local void (*futurePollArmNudge)() = nullptr;

// =======================================================================================
// PollWaker
//
// These are defined here rather than inline in waker.h because they reach into FuturePollEvent,
// which is only a complete type once awaiter.h is included.

PollWaker::PollWaker(FuturePollEvent& futurePollEvent)
    : holder(FuturePollEventHolder{futurePollEvent}) {}

PollWaker::~PollWaker() noexcept(false) {}

void PollWaker::wakeByRef() const {
  // Synchronous same-turn wake during `future.poll()`: arm the FuturePollEvent so it re-polls.
  // armDepthFirst() is idempotent and safe to call from within the event's own fire(), so this
  // works whether we were reached from onReady() (first poll) or fire() (a subsequent poll). No
  // arm-nudge needed here: the loop is running this very poll, not parked.
  KJ_IF_SOME(futurePollEvent, tryGetFuturePollEvent()) {
    futurePollEvent.armDepthFirst();
  }
}

kj::Maybe<kj::Rc<FutureWakerCell>> PollWaker::cloneCell() const {
  // Rust wants a waker it can retain and wake later. Hand out a strong reference to a
  // FutureWakerCell bound to the FuturePollEvent being polled; waking it arms that event.
  KJ_IF_SOME(futurePollEvent, tryGetFuturePollEvent()) {
    return futurePollEvent.cloneWakerCell();
  }
  return kj::none;
}

kj::Maybe<FuturePollEvent&> PollWaker::tryGetFuturePollEvent() const {
  KJ_IF_SOME(h, holder.tryGet()) {
    return h.futurePollEvent;
  }
  return kj::none;
}

}  // namespace kj_rs
