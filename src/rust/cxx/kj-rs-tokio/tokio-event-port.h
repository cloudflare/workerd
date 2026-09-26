#pragma once
// TokioEventPort: a kj::EventPort backed by a per-thread tokio current_thread runtime. C++ keeps
// creating and awaiting kj::Promises exactly as it always has; the port decides *who sleeps* and
// who drives whom. It has two modes, fixed at construction:
//
//  - KJ-driven (LoopDriver::KJ, `setupTokioAsyncIo()`): the KJ loop is the thread's scheduler.
//    `promise.wait()` works; when the loop would block in the OS, wait() parks the thread inside
//    `tokio::Runtime::block_on`, driving the whole tokio scheduler, so every Rust task on the
//    loop's runtime makes progress while C++ is "blocked". This is the mode of kj_test binaries
//    and of workerd's inspector and fallback-service threads (via kj::setupAsyncIo()).
//  - tokio-driven (LoopDriver::TOKIO, `newTokioDrivenContext()`, owned by the Rust
//    `kj_rs_tokio::Runtime`): tokio's scheduler owns the thread (`Runtime::block_on` in Rust
//    `main`) and the KJ loop is one participant, run to idle by a Rust driver task through
//    TokioAsyncIoContext::runTurns() / pollTurns() / prepareToYield() (runtime.rs) whenever it
//    has work, with tokio running only while it is idle -- the KJ-driven schedule with the roles
//    swapped, so events run in the same order in both modes. Nothing may
//    block the loop thread: wait() throws. poll() advances the timer and reports the wake latch
//    without touching tokio (the driver is already inside block_on; a nested one would panic).
//    setRunnable(true) and a sooner timer deadline always signal the driver, which may be
//    parked on the port's Notify; a spurious signal costs one idle iteration, a lost one hangs.
//
// Notes on the kj::EventPort contract (see kj/async.h):
//  - wait()/poll() return true iff wake() latched. This is load-bearing: kj::Executor's
//    executeAsync and kj::newPromiseAndCrossThreadFulfiller only get drained on `true`. In
//    tokio-driven mode the drain happens inside pollTurns() (WaitScope::poll() -> EventLoop::poll()
//    -> this->poll()), which the driver calls whenever the queue runs dry.
//  - The kj::TimerImpl is advanced after every wait()/poll() and before every runTurns()/
//    pollTurns(). Timer precision is tokio's timer wheel (~1 ms), the same granularity KJ's own
//    epoll-based port has.
//  - One TokioEventPort (and hence one runtime, one kj::EventLoop) per thread, matching KJ's
//    one-loop-per-thread model. The port owns its EventLoop: the loop is constructed on the port
//    and destroyed before any of the port's other members, so the port can always ask the loop
//    whether it needs service and can always cancel spawned tasks while the loop and timer are
//    alive.
//  - Tokio tasks on this runtime may use KJ freely -- fulfill a PromiseFulfiller, arm a timer,
//    add to a TaskSet, create or await bridged promises, wake bridged futures. Every one of those
//    either arms a KJ event or moves the next KJ timer deadline, and KJ reports both to the port
//    while it is not turning: kj::EventLoop reports setRunnable(false) before calling wait() (and
//    the driver's run(0) does the same in tokio-driven mode) and so setRunnable(true) on the first
//    arm, and the port is the kj::TimerImpl's SleepHooks for the duration of a park or yield
//    (updateNextTimerEvent() on a changed earliest deadline; kj::Timer::now() reads the live
//    clock). Either report ends the park. The one thing a task must never do is re-enter
//    `promise.wait()` on this thread: KJ-driven, that nests block_on inside block_on, which tokio
//    rejects (the panic surfaces as a kj::Exception); tokio-driven, wait() throws.
//
// Object relationships (one loop thread):
//
//   TokioAsyncIoContext                       -- kj::setupAsyncIo() analogue
//       ├── Own<TokioEventPort>               -- the kj::EventPort; one per thread
//       │       ├── kj::TimerImpl             -- fed from wait()/poll()/runTurns()/pollTurns();
//       │       │                                 declared BEFORE the Rust half so it outlives
//       │       │                                 cancelled tasks
//       │       ├── rust::Box<TokioPort>      -- Sync but not Send; wake() is cross-thread
//       │       │       ├── tokio::Runtime    -- KJ-driven only (current_thread, I/O + time
//       │       │       │                        drivers); tokio-driven, the Rust Runtime owns it
//       │       │       └── Arc<SharedState>  -- Notify + `woken` latch + `in_wait` flag; the
//       │       │                                ONE thing every wake source touches
//       │       └── Own<kj::EventLoop>        -- constructed on the port; declared LAST so it
//       │                                        is destroyed first
//       │   (the port is also the TimerImpl's SleepHooks while parked or yielded)
//       └── Own<kj::WaitScope>                -- keeps the loop current on the thread
//                                                (kj::currentEventLoop()) for its whole life
//
//   Thread-locals (loop thread only), all installed by construction / cleared by destruction:
//       LOOP_RUNTIME_HANDLE                   -- for kj_rs_tokio::current_handle()
//       LOOP_LOCAL_SET: Rc<LocalSet>          -- what kj_rs_tokio::spawn() enqueues onto and
//                                                wait()/poll() (KJ-driven) or Runtime::block_on
//                                                (tokio-driven) drive. Not a TokioPort member
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
// the loop (asserting its queue is empty), the Rust half, and finally the timer.

#include <rust/cxx.h>

#include <kj/async.h>
#include <kj/exception.h>
#include <kj/time.h>
#include <kj/timer.h>

namespace kj_rs_tokio {

// The Rust half of the port (port.rs), declared by the generated bridge header; only the .c++
// file needs its definition.
struct TokioPort;

// Who owns the loop thread's schedule (see the file comment).
enum class LoopDriver {
  KJ,
  TOKIO,
};

class TokioEventPort final: public kj::EventPort, private kj::TimerImpl::SleepHooks {
 public:
  // KJ-driven: builds the Rust half, and with it the thread's tokio runtime.
  TokioEventPort();
  // Tokio-driven: `rustPort` was built by the Rust Runtime, which owns the tokio runtime.
  explicit TokioEventPort(::rust::Box<TokioPort> rustPort);
  ~TokioEventPort() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(TokioEventPort);

  LoopDriver getDriver() const {
    return driver;
  }

  // kj::EventPort implementation. setRunnable(true) while the port is not turning -- an event
  // armed by a tokio task -- hands the thread back to KJ: KJ-driven it ends the park inside
  // wait(); tokio-driven it wakes the driver. kj::EventLoop guarantees setRunnable(false) right
  // before wait(), and the driver's run(0) after every cycle, so that edge is always delivered.
  // wait() throws in tokio-driven mode.
  bool wait() override;
  bool poll() override;
  void wake() const override;
  void setRunnable(bool runnable) override;

  // The kj::EventLoop this port drives. Owned by the port (see the file comment).
  kj::EventLoop &getLoop() {
    assertOwnerThread();
    return *loop;
  }

  // Timer fed by this port. now() is frozen while KJ events run and advances only when the loop
  // waits/polls or the driver runs it, preserving stock KJ timer semantics (timers would silently
  // never fire otherwise).
  kj::Timer &getTimer() {
    assertOwnerThread();
    return timerImpl;
  }

  // The Rust half (wake state, and KJ-driven the runtime). Rust code on this thread can also reach
  // the runtime via kj_rs_tokio::current_handle() / kj_rs_tokio::spawn().
  const TokioPort &getRustPort() const {
    assertOwnerThread();
    return *rustPort;
  }

  // Cancel every task spawned onto this thread's LocalSet (kj_rs_tokio::spawn()), running their
  // destructors now. The destructor does this itself, first; TokioAsyncIoContext also does it
  // while its WaitScope is still alive. This terminal operation is a no-op when repeated.
  void cancelSpawnedTasks();

  // Tokio-driven mode, for TokioAsyncIoContext's driver primitives: fire the timers that are due
  // (the timer's now() becomes the live clock, and the sleep hooks are cleared).
  void advanceTimer();

  // Tokio-driven mode: the driver is about to give up the thread (yield or park). Installs the
  // timer's sleep hooks so tasks that arm KJ timers meanwhile see live now() and a sooner
  // deadline signals the driver; returns the nanoseconds until the next timer deadline, if any,
  // rounded up so a park bounded by it ends just after the timer is due.
  kj::Maybe<uint64_t> prepareToYield();

 private:
  TokioEventPort(::rust::Box<TokioPort> rustPort, LoopDriver driver);

  // kj::TimerImpl::SleepHooks, installed by wait() and prepareToYield() (KJ clears them in
  // advanceTo()). KJ calls updateNextTimerEvent() whenever the earliest deadline changes; a
  // deadline sooner than the one the sleep was planned against ends it. While installed,
  // kj::Timer::now() reads the live clock through getTimeWhileSleeping().
  void updateNextTimerEvent(kj::Maybe<kj::TimePoint> time) override;
  kj::TimePoint getTimeWhileSleeping() override {
    assertOwnerThread();
    return clock.now();
  }

  void assertOwnerThread() const;

  const kj::ThreadId ownerThread;
  const LoopDriver driver;
  const kj::MonotonicClock &clock;
  // Declaration order is destruction order in reverse, and it matters: `loop` is destroyed
  // first, then `rustPort` (KJ-driven, the runtime), then `timerImpl`. The destructor body
  // cancels spawned tasks before any of them go.
  kj::TimerImpl timerImpl;
  ::rust::Box<TokioPort> rustPort;
  kj::Own<kj::EventLoop> loop;

  // The earliest timer deadline the current sleep was planned from (none = no timer; sleep
  // forever). updateNextTimerEvent() compares against it so only a *sooner* deadline ends the
  // sleep. Loop thread only; meaningful only while the sleep hooks are installed.
  kj::Maybe<kj::TimePoint> plannedNextEvent;

  // Guards the Rust call in cancelSpawnedTasks(): a cancelled task's drop can throw (a panic
  // surfaced as a kj::Exception by the cxx fork); if the caller is already unwinding, swallow
  // rather than std::terminate (same pattern as ~RustPromiseAwaiter in kj-rs/awaiter.c++).
  kj::UnwindDetector unwindDetector;
};

// Mirrors the shape of kj::setupAsyncIo() (see kj/async-io.h) for the tokio-backed loop. Owns
// the event port (and thus the kj::EventLoop, and KJ-driven the per-thread tokio runtime) and the
// kj::WaitScope.
//
// Teardown: spawned tasks are cancelled first, while the WaitScope is still alive (the port's
// own destructor would do it too, but by then the WaitScope is gone), then the WaitScope, then
// the port (loop, Rust half, timer -- see TokioEventPort). Bridged Rust futures that C++ co_awaits
// are separate: they are cancelled through KJ promise destruction and never outlive the loop.
struct TokioAsyncIoContext {
  TokioAsyncIoContext(kj::Own<TokioEventPort> port, kj::Own<kj::WaitScope> waitScope)
      : port(kj::mv(port)),
        waitScope(kj::mv(waitScope)) {}
  ~TokioAsyncIoContext() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(TokioAsyncIoContext);

  kj::Own<TokioEventPort> port;
  kj::Own<kj::WaitScope> waitScope;

  TokioEventPort &getPort() {
    return *port;
  }
  kj::EventLoop &getLoop() {
    return port->getLoop();
  }
  // Tokio-driven, `promise.wait()` on it throws (TokioEventPort::wait()); `promise.poll()` works.
  kj::WaitScope &getWaitScope() {
    return *waitScope;
  }
  kj::Timer &getTimer() {
    return port->getTimer();
  }
  const TokioPort &getRustPort() const {
    return port->getRustPort();
  }

  // The tokio-driven driver's primitives (runtime.rs). Loop thread only, never from inside a KJ
  // event callback. Each fires due timers first and reports whether the queue still has events.
  //
  // runTurns: up to `maxTurns` events. pollTurns: the same through kj::WaitScope::poll(), which
  // additionally polls the port when the queue runs dry -- the only path that drains cross-thread
  // kj::Executor events and promotes kj::yieldUntilWouldSleep() waiters -- followed by a run(0)
  // so the loop's runnable state is reported as false before the driver gives up the thread.
  bool runTurns(uint32_t maxTurns);
  bool pollTurns(uint32_t maxTurns);
  // See TokioEventPort::prepareToYield().
  kj::Maybe<uint64_t> prepareToYield();
};

// KJ-driven: the context whose WaitScope the thread blocks on.
TokioAsyncIoContext setupTokioAsyncIo();

// Tokio-driven: the context the Rust `kj_rs_tokio::Runtime` drives; `rustPort` is the Rust half
// it built, whose tokio runtime it owns.
kj::Own<TokioAsyncIoContext> newTokioDrivenContext(::rust::Box<TokioPort> rustPort);

}  // namespace kj_rs_tokio
