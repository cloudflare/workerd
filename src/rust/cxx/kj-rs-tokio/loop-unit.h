#pragma once
// LoopUnitContext: a KJ event loop that no thread owns. A tokio task drives it (unit.rs,
// `LoopUnit`): whichever worker thread of a multi-thread tokio runtime polls the task enters the
// loop, turns its events until none is left, and leaves it again before the poll returns. Many
// such loops share few threads, and a loop that becomes ready runs on whichever worker picks its
// task up.
//
// This is the m:n arrangement KJ provides for (kj/async-unix.h, `UnixEventPort::getPollableFd()`:
// "drive multiple event loops with fewer threads"), built on the facts that make it possible:
//  - A kj::EventLoop is bound to a thread only while a kj::WaitScope for it exists there
//    (`EventLoop::enterScope()` / `leaveScope()`); constructing one binds nothing. turn() makes a
//    WaitScope for the duration of the call.
//  - kj::Executor and cross-thread fulfillers follow the loop, not a thread: they reach it
//    through its port's wake(), which here wakes the task.
//  - kj::EventLoopLocal keys by loop.
//  - KJ's thread-locals (the current loop, the firing event, the exception callback stack) are
//    scoped to stack frames, and no frame of the loop survives turn().
//
// The last point has one exception, a suspended fiber (kj::FiberPool, kj::newFiber), whose stack
// keeps frames across turns. Resuming one on another thread is not safe in general: a frame may
// have cached the address of a thread-local, and C++'s exception state is per thread. So a unit
// can be pinned: while `setPinCheck()`'s function says so, turn() does not go idle but returns
// PINNED, and the driver then stays on the thread, blocking it in turnPinned() until the unit
// is no longer pinned. Whoever starts a fiber on the loop is responsible for pinning it for the
// fiber's life.
//
// Where TokioEventPort (tokio-event-port.h) parks a thread it owns inside a tokio runtime of its
// own, this port never sleeps on its own account: poll() only reports the wake latch and
// advances the timer, the driver asks for the next timer deadline and has tokio wake the task
// then, and I/O is whatever tokio resources the loop's code holds, registered with the shared
// runtime's driver and waking the loop through wake() from whichever thread turns that driver.
//
// Everything created on the loop is destroyed with the loop entered: the root promise by
// finish() or the destructor, which enter it themselves.

#include <rust/cxx.h>

#include <kj/async.h>
#include <kj/exception.h>
#include <kj/function.h>
#include <kj/time.h>
#include <kj/timer.h>

namespace kj_rs_tokio {

// Rust types (unit.rs), defined by the generated bridge header.
struct UnitWake;
struct UnitTask;
struct UnitFuture;

// What turn() found (`UnitTurn` in unit.rs).
constexpr uint8_t UNIT_DONE = 0;
constexpr uint8_t UNIT_IDLE = 1;
constexpr uint8_t UNIT_YIELD = 2;
constexpr uint8_t UNIT_PINNED = 3;

// `nextTimerNs()` when no timer is pending.
constexpr uint64_t UNIT_NO_TIMER = kj::maxValue;

class UnitEventPort final: public kj::EventPort {
 public:
  explicit UnitEventPort(::rust::Box<UnitWake> wake);
  ~UnitEventPort() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(UnitEventPort);

  // Blocks the calling thread until wake() was called or the next timer is due, and leaves the
  // wake latch as it is. Only a pinned unit sleeps this way (LoopUnitContext::turnPinned()).
  void block();

  // block(), then poll().
  bool wait() override;
  // Advances the timer and takes the wake latch.
  bool poll() override;
  // Callable from any thread: latches, wakes the driving task and ends a wait().
  void wake() const override;

  kj::Timer& getTimer() {
    return timerImpl;
  }
  // Nanoseconds until the earliest timer is due, rounded up; UNIT_NO_TIMER without one.
  uint64_t nextTimerNs();

 private:
  const kj::MonotonicClock& clock;
  kj::TimerImpl timerImpl;
  ::rust::Box<UnitWake> rustWake;
};

class LoopUnitContext {
 public:
  explicit LoopUnitContext(::rust::Box<UnitWake> wake);
  // Enters the loop to destroy what is left on it, so it must not run on a thread that has
  // another loop entered.
  ~LoopUnitContext() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(LoopUnitContext);

  // The loop's timer: now() is frozen while events run, as with every KJ port.
  kj::Timer& getTimer() {
    return port.getTimer();
  }

  // `check` says whether the unit must stay on the thread it is on (see the file comment). It
  // is called with the loop entered.
  void setPinCheck(kj::Function<bool()> check) {
    pinCheck = kj::mv(check);
  }

  // Makes the root future from `task`, with the loop entered, and starts it. Once.
  void start(::rust::Box<UnitTask> task);

  // Enters the loop and turns events until the root future is done (UNIT_DONE), none is left
  // and the port has nothing more for the loop (UNIT_IDLE, or UNIT_PINNED if the unit is
  // pinned), or `maxTurns` were turned (UNIT_YIELD; never while pinned).
  uint8_t turn(uint32_t maxTurns);

  // After UNIT_PINNED, on the same thread: turns events, blocking the thread whenever none is
  // left, until the unit is not pinned any more or the root future is done.
  void turnPinned();

  // See UnitEventPort.
  uint64_t nextTimerNs() {
    return port.nextTimerNs();
  }

  // After UNIT_DONE: destroys the root promise with the loop entered and throws the exception
  // it failed with, if any.
  void finish();

 private:
  bool isPinned();

  UnitEventPort port;
  kj::Own<kj::EventLoop> loop;
  kj::Maybe<kj::Function<bool()>> pinCheck;
  bool done = false;
  kj::Maybe<kj::Exception> failure;
  // Declared last: destroyed first, with the loop entered.
  kj::Maybe<kj::Promise<void>> root;
};

kj::Own<LoopUnitContext> newLoopUnitContext(::rust::Box<UnitWake> wake);

}  // namespace kj_rs_tokio
