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
    : holder(FuturePollEventHolder{futurePollEvent}),
      cell(futurePollEvent.wakerCellRef()) {
  // Every poll starts on the owning thread; renew the cell's cross-thread fulfiller if a
  // foreign-thread wake consumed it getting us here. Doing it at poll-top (rather than inside
  // the consumed promise's own continuation) means we never destroy a promise chain from within
  // its own continuation frame.
  futurePollEvent.ensureCrossThreadWakeArmed();
}

PollWaker::~PollWaker() noexcept(false) {}

void PollWaker::wakeByRef() const {
  // Delegate to the cell, which handles both threads: on the owning thread this is a synchronous
  // same-turn wake (armDepthFirst() is idempotent and safe from within the event's own fire(),
  // so it works whether we were reached from onReady() or fire(), and causes an immediate
  // re-poll); from a foreign thread it goes through the cross-thread fulfiller — `&Waker` is
  // Sync, so even this borrowed waker may legally be woken from another thread during the poll.
  cell.wakeByRef();
}

kj::Arc<FutureWakerCell> PollWaker::cloneCell() const {
  // Rust wants a waker it can retain and wake later: hand out a strong reference to the event's
  // FutureWakerCell. Atomic refcount, safe from any thread.
  return cell.addRef();
}

kj::Maybe<FuturePollEvent&> PollWaker::tryGetFuturePollEvent() const {
  KJ_IF_SOME(h, holder.tryGet()) {
    return h.futurePollEvent;
  }
  return kj::none;
}

}  // namespace kj_rs
