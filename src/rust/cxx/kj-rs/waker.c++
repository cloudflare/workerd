#include "waker.h"

#include "awaiter.h"

#include <kj/debug.h>

namespace kj_rs {

// Optional wake hook for event ports integrating another scheduler.
thread_local void (*futurePollArmNudge)() = nullptr;

// PollWaker reaches into FuturePollEvent, whose definition is provided by awaiter.h.
PollWaker::PollWaker(FuturePollEvent& futurePollEvent)
    : holder(FuturePollEventHolder{futurePollEvent}) {}

PollWaker::~PollWaker() noexcept(false) {}

void PollWaker::wakeByRef() const {
  KJ_IF_SOME(futurePollEvent, tryGetFuturePollEvent()) {
    futurePollEvent.armDepthFirst();
  }
}

kj::Maybe<kj::Rc<FutureWakerCell>> PollWaker::cloneCell() const {
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
