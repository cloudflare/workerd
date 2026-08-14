#pragma once
// TokioEventPort: a kj::EventPort backed by a per-thread tokio current_thread runtime.
// Ownership inversion, not replacement: C++ keeps creating and awaiting kj::Promises exactly
// as today; what changes is *who sleeps*. When the KJ loop would block in the OS, wait()
// parks the thread inside `tokio::Runtime::block_on`, driving the whole tokio scheduler, so
// every Rust task on the loop's runtime makes progress while C++ is "blocked".
//
// Notes on the kj::EventPort contract (see kj/async.h):
//  - wait()/poll() return true iff wake() latched. This is load-bearing: kj::Executor's
//    executeAsync and kj::newPromiseAndCrossThreadFulfiller only get drained on `true`.
//  - The kj::TimerImpl is advanced after every wait()/poll(). Timer precision is tokio's timer
//    wheel (~1 ms), the same granularity KJ's own epoll-based port has.
//  - One TokioEventPort (and hence one runtime, one kj::EventLoop) per thread, matching KJ's
//    one-loop-per-thread model. The port owns its EventLoop: the loop is constructed on the port
//    and destroyed before any of the port's other members, so the port can always ask the loop
//    whether it needs service (below) and can always cancel spawned tasks while the loop and
//    timer are alive.
//  - Tokio tasks on this runtime may use KJ freely -- fulfill a PromiseFulfiller, arm a timer,
//    add to a TaskSet, create or await bridged promises, wake bridged futures. Every one of those
//    either arms a KJ event or moves the next KJ timer deadline, and KJ reports both to the port
//    while it sleeps: kj::EventLoop reports setRunnable(false) before calling wait() and so
//    setRunnable(true) on the first arm during it, and the port is the kj::TimerImpl's SleepHooks
//    for the duration of the park (updateNextTimerEvent() on a changed earliest deadline;
//    kj::Timer::now() reads the live clock). Either report ends the park. The one thing a task
//    must never do is re-enter `promise.wait()` / `waitScope.poll()` on this thread: that nests
//    block_on inside block_on, which tokio rejects (the panic surfaces as a kj::Exception).
//
// Object relationships (one loop thread):
//
//   TokioAsyncIoContext                       -- kj::setupAsyncIo() analogue
//       ├── Own<TokioEventPort>               -- the kj::EventPort; one per thread
//       │       ├── kj::TimerImpl             -- fed from wait()/poll(); declared BEFORE the
//       │       │                                 Rust half so it outlives cancelled tasks
//       │       ├── rust::Box<TokioPort>      -- Send + Sync (wake() is called cross-thread)
//       │       │       ├── tokio::Runtime (current_thread, I/O + time drivers)
//       │       │       └── Arc<SharedState>  -- Notify + `woken` latch + `in_wait` flag; the
//       │       │                                ONE thing every wake source touches
//       │       └── Own<kj::EventLoop>        -- constructed on the port; declared LAST so it
//       │                                        is destroyed first
//       │   (the port is also the TimerImpl's SleepHooks while parked)
//       └── Own<kj::WaitScope>
//
//   Thread-locals (loop thread only), all installed by construction / cleared by destruction:
//       LOOP_RUNTIME_HANDLE                   -- for kj_rs_tokio::current_handle()
//       LOOP_LOCAL_SET: Rc<LocalSet>          -- what kj_rs_tokio::spawn() enqueues onto and
//                                                wait()/poll() drive. Not a TokioPort member
//                                                because LocalSet is !Send; dropped by
//                                                TokioEventPort's destructor
//       activePort                            -- enforces one port per thread
//
//   Wake sources -> SharedState.notify: wake() from any thread (kj::Executor,
//   CrossThreadPromiseFulfiller); on the loop thread setRunnable(true) and the SleepHooks timer
//   callback; and the planned timer deadline via tokio's timer wheel.
//
// Teardown order is structural, not a convention: ~TokioEventPort cancels the LocalSet's spawned
// tasks FIRST (they may own KJ promises whose destructors need the loop and timer), then destroys
// the loop (asserting its queue is empty), the runtime, and finally the timer.

#include "kj-rs-tokio/ffi.rs.h"

#include <kj/async.h>
#include <kj/exception.h>
#include <kj/time.h>
#include <kj/timer.h>

namespace kj_rs_tokio {

class TokioEventPort final: public kj::EventPort, private kj::TimerImpl::SleepHooks {
 public:
  TokioEventPort();
  ~TokioEventPort() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(TokioEventPort);

  // kj::EventPort implementation. setRunnable(true) during wait() -- an event armed by a tokio
  // task running inside the park -- ends the park; kj::EventLoop guarantees setRunnable(false)
  // right before wait(), so that edge is always delivered.
  bool wait() override;
  bool poll() override;
  void wake() const override;
  void setRunnable(bool runnable) override;

  // The kj::EventLoop this port drives. Owned by the port (see the file comment).
  kj::EventLoop &getLoop() {
    return *loop;
  }

  // Timer fed by this port. now() is frozen while KJ events run and advances only when the loop
  // waits/polls, preserving stock KJ timer semantics (the port calls timerImpl.advanceTo() after
  // every wait()/poll() return; timers would silently never fire otherwise).
  kj::Timer &getTimer() {
    return timerImpl;
  }

  // The Rust half (runtime + wake state). Rust code on this thread can also reach the runtime
  // via kj_rs_tokio::current_handle() / kj_rs_tokio::spawn().
  const TokioPort &getRustPort() const {
    return *rustPort;
  }

  // Cancel every task spawned onto this thread's LocalSet (kj_rs_tokio::spawn()), running their
  // destructors now. The destructor does this itself, first; TokioAsyncIoContext also does it
  // while its WaitScope is still alive. Idempotent.
  void cancelSpawnedTasks();

 private:
  // kj::TimerImpl::SleepHooks, installed by wait() for the duration of the park (KJ clears them
  // in advanceTo()). KJ calls updateNextTimerEvent() whenever the earliest deadline changes; a
  // deadline sooner than the one this wait() was planned against ends the park. While installed,
  // kj::Timer::now() reads the live clock through getTimeWhileSleeping().
  void updateNextTimerEvent(kj::Maybe<kj::TimePoint> time) override;
  kj::TimePoint getTimeWhileSleeping() override {
    return clock.now();
  }

  const kj::MonotonicClock &clock;
  // Declaration order is destruction order in reverse, and it matters: `loop` is destroyed
  // first, then `rustPort` (the runtime), then `timerImpl`. The destructor body cancels spawned
  // tasks before any of them go.
  kj::TimerImpl timerImpl;
  ::rust::Box<TokioPort> rustPort;
  kj::Own<kj::EventLoop> loop;

  // The earliest timer deadline wait() computed its sleep from (none = no timer; sleep forever).
  // updateNextTimerEvent() compares against it so only a *sooner* deadline ends the park. Loop
  // thread only; meaningful only inside wait().
  kj::Maybe<kj::TimePoint> plannedNextEvent;

  // Guards the Rust call in cancelSpawnedTasks(): a cancelled task's drop can throw (a panic
  // surfaced as a kj::Exception by the cxx fork); if the caller is already unwinding, swallow
  // rather than std::terminate (same pattern as ~RustPromiseAwaiter in kj-rs/awaiter.c++).
  kj::UnwindDetector unwindDetector;
};

// Mirrors the shape of kj::setupAsyncIo() (see kj/async-io.h) for the tokio-backed loop. Owns
// the event port (and thus the per-thread tokio runtime and the kj::EventLoop) and the
// kj::WaitScope.
//
// Teardown: spawned tasks are cancelled first, while the WaitScope is still alive (the port's
// own destructor would do it too, but by then the WaitScope is gone), then the WaitScope, then
// the port (loop, runtime, timer -- see TokioEventPort). Bridged Rust futures that C++ co_awaits
// are separate: they are cancelled through KJ promise destruction and never outlive the loop.
struct TokioAsyncIoContext {
  TokioAsyncIoContext(kj::Own<TokioEventPort> port, kj::Own<kj::WaitScope> waitScope)
      : port(kj::mv(port)),
        waitScope(kj::mv(waitScope)) {}
  ~TokioAsyncIoContext() noexcept(false);
  // Move-CONSTRUCTIBLE only (setupTokioAsyncIo() returns by value; a moved-from context owns
  // nothing and its destructor is a no-op). Move-ASSIGNMENT is deleted on purpose: it would
  // destroy the existing members via their kj::Owns without running ~TokioAsyncIoContext first.
  // Return-by-value needs only the move constructor, so nothing is lost.
  TokioAsyncIoContext(TokioAsyncIoContext &&) = default;
  TokioAsyncIoContext &operator=(TokioAsyncIoContext &&) = delete;
  KJ_DISALLOW_COPY(TokioAsyncIoContext);

  kj::Own<TokioEventPort> port;
  kj::Own<kj::WaitScope> waitScope;

  TokioEventPort &getPort() {
    return *port;
  }
  kj::EventLoop &getLoop() {
    return port->getLoop();
  }
  kj::WaitScope &getWaitScope() {
    return *waitScope;
  }
  kj::Timer &getTimer() {
    return port->getTimer();
  }
};

TokioAsyncIoContext setupTokioAsyncIo();

}  // namespace kj_rs_tokio
