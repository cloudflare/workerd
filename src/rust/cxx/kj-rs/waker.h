#pragma once

#include <kj/async.h>
#include <kj/debug.h>
#include <kj/mutex.h>
#include <kj/refcount.h>
#include <kj/vector.h>

#include <atomic>

namespace kj_rs {

class FuturePollEvent;
class FutureWakerCell;

// =======================================================================================
// CrossThreadWakeSink

// The one cross-thread doorway into a KJ event loop for Rust wakers. Each event loop owns one
// sink, and every retained waker created on that loop shares it. Foreign wakes enqueue their
// cell and fulfill the drain coroutine's cross-thread fulfiller; the drain then replays the wake
// on the owning thread.
class CrossThreadWakeSink final: public kj::AtomicRefcounted {
 public:
  static kj::Arc<CrossThreadWakeSink> forCurrentLoop();
  void enqueue(kj::Arc<FutureWakerCell> cell) const;
  void ensureDrain() const;

 private:
  struct Holder;

  static kj::Promise<void> drainLoop(kj::Arc<CrossThreadWakeSink> sink);
  void arm(kj::Own<const kj::CrossThreadPromiseFulfiller<void>> fulfiller) const;
  kj::Vector<kj::Arc<FutureWakerCell>> takePending() const;
  void close() const;

  mutable std::atomic<bool> drainRunning{false};

  struct State {
    kj::Vector<kj::Arc<FutureWakerCell>> pending;
    kj::Maybe<kj::Own<const kj::CrossThreadPromiseFulfiller<void>>> fulfiller;
    bool closed = false;
  };
  kj::MutexGuarded<State> state;
};

// =======================================================================================
// FutureWakerCell

// Thread-safe shared state behind a retained Rust waker. The executor routes foreign wakes
// through the loop's sink, while neutralize() severs the non-owning event edge at teardown.
class FutureWakerCell final: public kj::AtomicRefcounted {
 public:
  explicit FutureWakerCell(FuturePollEvent& event)
      : executor(kj::getCurrentThreadExecutor().addRef()),
        sink(CrossThreadWakeSink::forCurrentLoop()),
        event(event) {}

  void neutralize() const {
    KJ_DREQUIRE(executor->isCurrent() || kj::tryGetCurrentThreadExecutor() == kj::none,
        "FutureWakerCell neutralized off its owning thread");
    event = kj::none;
    alive.store(false, std::memory_order_release);
  }

  void ensureCrossThreadDrain() const {
    sink->ensureDrain();
  }

  void wakeByRef() const;

  kj::Maybe<FuturePollEvent&> tryGetFuturePollEvent() const {
    if (executor->isCurrent()) {
      return event;
    }
    return kj::none;
  }

  kj::Arc<FutureWakerCell> addRef() const {
    return const_cast<FutureWakerCell&>(*this).addRefToThis();
  }

  kj::Arc<FutureWakerCell> reown() const {
    return kj::Arc<FutureWakerCell>::reown(this);
  }

 private:
  kj::Own<const kj::Executor> executor;
  kj::Arc<CrossThreadWakeSink> sink;
  mutable std::atomic<bool> alive{true};
  mutable kj::Maybe<FuturePollEvent&> event;
};

// =======================================================================================
// PollWaker

// Stack-owned waker passed to `Future::poll()`. Clones retain the associated FutureWakerCell.
class PollWaker final {
 public:
  explicit PollWaker(FuturePollEvent& futurePollEvent);
  ~PollWaker() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(PollWaker);

  void wakeByRef() const;
  kj::Arc<FutureWakerCell> cloneCell() const;
  kj::Maybe<FuturePollEvent&> tryGetFuturePollEvent() const;

 private:
  kj::Arc<FutureWakerCell> cell;
};

}  // namespace kj_rs
