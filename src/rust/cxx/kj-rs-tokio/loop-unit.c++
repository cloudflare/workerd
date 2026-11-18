#include "kj-rs-tokio/loop-unit.h"

#include "kj-rs-tokio/ffi.rs.h"

#include <kj/debug.h>

namespace kj_rs_tokio {

UnitEventPort::UnitEventPort(::rust::Box<UnitWake> wake)
    : clock(kj::systemPreciseMonotonicClock()),
      timerImpl(clock.now()),
      rustWake(kj::mv(wake)) {}

UnitEventPort::~UnitEventPort() noexcept(false) = default;

uint64_t UnitEventPort::nextTimerNs() {
  return timerImpl.timeoutToNextEvent(clock.now(), kj::NANOSECONDS, kj::maxValue)
      .orDefault(UNIT_NO_TIMER);
}

void UnitEventPort::block() {
  rustWake->block(nextTimerNs());
}

bool UnitEventPort::wait() {
  block();
  return poll();
}

bool UnitEventPort::poll() {
  timerImpl.advanceTo(clock.now());
  return rustWake->take();
}

void UnitEventPort::wake() const {
  rustWake->wake();
}

LoopUnitContext::LoopUnitContext(::rust::Box<UnitWake> wake)
    : port(kj::mv(wake)),
      loop(kj::heap<kj::EventLoop>(port)) {}

LoopUnitContext::~LoopUnitContext() noexcept(false) {
  // The root promise and the pin check may own objects of the loop. The loop itself must be
  // destroyed with no loop entered: its destructor enters it.
  kj::WaitScope waitScope(*loop);
  root = kj::none;
  pinCheck = kj::none;
}

bool LoopUnitContext::isPinned() {
  KJ_IF_SOME(check, pinCheck) {
    return check();
  }
  return false;
}

void LoopUnitContext::start(::rust::Box<UnitTask> task) {
  KJ_REQUIRE(root == kj::none, "the unit was started already");
  kj::WaitScope waitScope(*loop);
  auto future = unit_task_start(kj::mv(task), *this);
  root = unit_future_run(kj::mv(future))
             .then([this]() { done = true; }, [this](kj::Exception&& exception) {
    failure = kj::mv(exception);
    done = true;
  }).eagerlyEvaluate(nullptr);
}

uint8_t LoopUnitContext::turn(uint32_t maxTurns) {
  kj::WaitScope waitScope(*loop);
  for (;;) {
    // Turns events and, each time none is left, polls the port: the timer advances and
    // whatever other threads sent the loop is delivered.
    waitScope.poll(maxTurns);
    if (done) return UNIT_DONE;
    bool pinned = isPinned();
    if (loop->isRunnable()) {
      // A pinned unit keeps its thread.
      if (pinned) continue;
      return UNIT_YIELD;
    }
    return pinned ? UNIT_PINNED : UNIT_IDLE;
  }
}

void LoopUnitContext::turnPinned() {
  kj::WaitScope waitScope(*loop);
  for (;;) {
    waitScope.poll();
    if (done || !isPinned()) return;
    // The latch stays set for the poll() above to report.
    port.block();
  }
}

void LoopUnitContext::finish() {
  {
    kj::WaitScope waitScope(*loop);
    root = kj::none;
  }
  KJ_IF_SOME(exception, failure) {
    auto thrown = kj::mv(exception);
    failure = kj::none;
    kj::throwFatalException(kj::mv(thrown));
  }
}

kj::Own<LoopUnitContext> newLoopUnitContext(::rust::Box<UnitWake> wake) {
  return kj::heap<LoopUnitContext>(kj::mv(wake));
}

}  // namespace kj_rs_tokio
