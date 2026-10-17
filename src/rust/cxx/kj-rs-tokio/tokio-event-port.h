#pragma once
// TokioEventPort: a kj::EventPort backed by a per-thread tokio `LocalEventLoop`.
// Ownership inversion, not replacement: C++ keeps creating and awaiting kj::Promises exactly
// as today; what changes is *who sleeps*. When the KJ loop would block in the OS, wait() parks
// the thread in tokio's driver (`LocalEventLoop::park`) and then runs one bounded batch of tokio
// tasks (`tick`); poll() is the batch alone. Tokio tasks therefore never run while the thread
// sleeps, only between KJ's own turns, and whatever they arm in KJ, KJ runs next.
//
// Notes on the kj::EventPort contract (see kj/async.h):
//  - wait()/poll() return true iff wake() latched. This is load-bearing: kj::Executor's
//    executeAsync and kj::newPromiseAndCrossThreadFulfiller only get drained on `true`.
//  - The kj::TimerImpl is advanced after every wait()/poll(). Timer precision is tokio's timer
//    wheel (~1 ms), the same granularity KJ's own epoll-based port has.
//  - One TokioEventPort (and hence one runtime, one kj::EventLoop) per thread, matching KJ's
//    one-loop-per-thread model. The port owns its EventLoop: the loop is constructed on the port
//    and destroyed before any of the port's other members, so the port can always cancel spawned
//    tasks while the loop and timer are alive.
//  - Tokio tasks on this runtime may use KJ freely -- fulfill a PromiseFulfiller, arm a timer,
//    add to a TaskSet, create or await bridged promises, wake bridged futures. The batch they run
//    in returns to KJ, which then runs what they armed. The one thing a task must never do is
//    re-enter `promise.wait()` / `waitScope.poll()` on this thread: the loop is already inside a
//    wait() or poll(), and KJ refuses the nested one with a kj::Exception.
//
// Object relationships (one loop thread):
//
//   TokioAsyncIoContext                       -- kj::setupAsyncIo() analogue
//       ├── Own<TokioEventPort>               -- the kj::EventPort; one per thread
//       │       ├── kj::TimerImpl             -- fed from wait()/poll(); declared BEFORE the
//       │       │                                 Rust half so it outlives cancelled tasks
//       │       ├── rust::Box<TokioPort>      -- the tokio LocalEventLoop; loop thread only
//       │       ├── rust::Box<TokioWaker>     -- Send + Sync: wake() from any thread, through
//       │       │                                tokio's Unparker and the `woken` latch
//       │       └── Own<kj::EventLoop>        -- constructed on the port; declared LAST so it
//       │                                        is destroyed first
//       └── Own<kj::WaitScope>
//
//   Thread-locals (loop thread only), installed by construction / cleared by destruction:
//       LOOP_RUNTIME_HANDLE                   -- for kj_rs_tokio::current_handle()
//       activePort                            -- enforces one port per thread
//
// Teardown order is structural, not a convention: ~TokioEventPort shuts the tokio runtime down
// FIRST, dropping its spawned tasks (they may own KJ promises whose destructors need the loop and
// timer), then destroys the loop (asserting its queue is empty) and finally the timer.

#include <rust/cxx.h>

#include <kj/async.h>
#include <kj/exception.h>
#include <kj/time.h>
#include <kj/timer.h>

namespace kj_rs_tokio {

// Rust types (port.rs, runtime.rs), defined by the generated bridge header. That header includes
// this one, so they are only declared here.
struct TokioPort;
struct TokioWaker;
struct BlockOnTask;

class TokioEventPort final: public kj::EventPort {
 public:
  TokioEventPort();
  ~TokioEventPort() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(TokioEventPort);

  // kj::EventPort implementation.
  bool wait() override;
  bool poll() override;
  void wake() const override;

  // The kj::EventLoop this port drives. Owned by the port (see the file comment).
  kj::EventLoop &getLoop() {
    assertOwnerThread();
    return *loop;
  }

  // Timer fed by this port. now() is frozen while KJ events and tokio tasks run and advances
  // only when the loop wakes from a wait() or enters a poll(), preserving stock KJ timer
  // semantics (the port calls timerImpl.advanceTo() there; timers would silently never fire
  // otherwise).
  kj::Timer &getTimer() {
    assertOwnerThread();
    return timerImpl;
  }

  // The Rust half (the tokio loop and wake state). Rust code on this thread can also reach the
  // runtime via kj_rs_tokio::current_handle() / kj_rs_tokio::spawn().
  const TokioPort &getRustPort() const {
    assertOwnerThread();
    return *rustPort;
  }

  // Shut the tokio runtime down, dropping every task spawned onto it (kj_rs_tokio::spawn()) now.
  // The destructor does this itself, first; TokioAsyncIoContext also does it while its WaitScope
  // is still alive. This terminal operation is a no-op when repeated.
  void cancelSpawnedTasks();

 private:
  // The second half of wait(), and all of poll(): advance the timer to now, then run one batch
  // of tokio tasks. Returns the wake latch.
  bool tick();

  void assertOwnerThread() const;

  const kj::ThreadId ownerThread;
  const kj::MonotonicClock &clock;
  // Declaration order is destruction order in reverse, and it matters: `loop` is destroyed
  // first, then `rustPort` (the runtime), then `timerImpl`. The destructor body cancels spawned
  // tasks before any of them go.
  kj::TimerImpl timerImpl;
  ::rust::Box<TokioPort> rustPort;
  ::rust::Box<TokioWaker> rustWaker;
  kj::Own<kj::EventLoop> loop;

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
  TokioAsyncIoContext()
      : port(kj::heap<TokioEventPort>()),
        waitScope(kj::heap<kj::WaitScope>(port->getLoop())) {}
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
  kj::WaitScope &getWaitScope() {
    return *waitScope;
  }
  kj::Timer &getTimer() {
    return port->getTimer();
  }

  // For `kj_rs_tokio::Runtime::block_on` (runtime.rs): blocks in promise.wait() on the WaitScope
  // until the task block_on spawned has ended (`task` awaits it).
  void blockOn(::rust::Box<BlockOnTask> task);
};

TokioAsyncIoContext setupTokioAsyncIo();

// The same on the heap, for the Rust `kj_rs_tokio::Runtime` to own.
kj::Own<TokioAsyncIoContext> newTokioAsyncIoContext();

}  // namespace kj_rs_tokio
