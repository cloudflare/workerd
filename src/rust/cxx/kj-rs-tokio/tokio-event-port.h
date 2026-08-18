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
//  - The kj::TimerImpl is advanced after every wait()/poll(). Sub-millisecond deadlines
//    bypass tokio's ~1ms timer wheel via a dedicated high-resolution timer thread (see
//    HiResTimer in port.rs).
//  - One TokioEventPort (and hence one runtime) per thread, matching KJ's one-loop-per-thread
//    model. The port, loop, and WaitScope must live and die on that thread.
//  - What a tokio task on this runtime may do with KJ, today: it may complete a *bridged*
//    future — make ready anything whose readiness reaches the loop through a Rust waker (a
//    kj_rs::FutureWakerCell clone), e.g. send on a channel/oneshot that a bridged future is
//    awaiting. Those wakes escape a parked wait() via the arm-nudge hook (see nudge()). It must
//    NOT arm KJ events by any other means while the loop may be parked: re-entering C++ to
//    fulfill a PromiseFulfiller / add to a TaskSet / resolve a promise chain, creating or
//    awaiting a bridged (eager-by-default) kj promise, or arming a KJ timer all arm events that
//    KJ's edge-filtered setRunnable() does not report during a park (lastRunnableState is stale-
//    true), and nothing else touches the port's parked block_on — the arm sits unserviced until
//    an unrelated wakeup (under wait-forever: possibly forever). Generalizing the nudge so tasks
//    can use KJ promises freely is future work; until then, task->KJ communication must be
//    waker-mediated only. Tasks must also never re-enter `promise.wait()` /
//    `waitScope.poll()` on this thread: that nests block_on inside block_on, which tokio
//    rejects (the panic surfaces as a kj::Exception).

#include "kj-rs-tokio/ffi.rs.h"

#include <kj/async.h>
#include <kj/time.h>
#include <kj/timer.h>

namespace kj_rs_tokio {

class TokioEventPort final: public kj::EventPort {
 public:
  TokioEventPort();
  ~TokioEventPort();
  KJ_DISALLOW_COPY_AND_MOVE(TokioEventPort);

  // kj::EventPort implementation.
  bool wait() override;
  bool poll() override;
  void wake() const override;
  void setRunnable(bool runnable) override;

  // Nudge this port's Rust half out of a blocking wait() when a same-thread FutureWakerCell arm
  // happens during block_on() that KJ's edge-triggered setRunnable() would miss (installed as
  // kj_rs::futurePollArmNudge for this thread while the port lives). Idempotent: notify_runnable()
  // no-ops unless the loop is parked.
  void nudge();

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

 private:
  const kj::MonotonicClock &clock;
  ::rust::Box<TokioPort> rustPort;
  kj::TimerImpl timerImpl;

  // Recorded runnable state, updated by setRunnable() on empty<->runnable transitions. Today
  // its only job is to nudge a concurrent wait() out of its block_on (a tokio task may have
  // re-entered C++ and armed a KJ event while the loop slept); it is also the hook point for a
  // future scheduled-pump model where Rust owns the thread and setRunnable(true) schedules a
  // bounded waitScope.poll() pump task.
  bool runnable = false;
};

}  // namespace kj_rs_tokio
