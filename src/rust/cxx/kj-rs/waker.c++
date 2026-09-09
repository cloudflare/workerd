#include "waker.h"

#include "awaiter.h"

namespace kj_rs {

// =======================================================================================
// CrossThreadWakeSink

// The per-loop owner of a sink: created by kj::EventLoopLocal on first use, destroyed with the
// loop. The drain coroutine it starts arms a fresh fulfiller, sleeps until a foreign thread
// fulfills it, replays the queued wakes from the owning thread, and repeats.
//
// The drain runs as a detached ("daemon") task of the loop rather than as a member here, for
// teardown order: ~EventLoop destroys its daemons FIRST, then disconnects its Executor -- which
// logs an error for any cross-thread fulfiller reply still queued -- and only then destroys its
// EventLoopLocals (this Holder). Cancelling the drain with the daemons destroys the promise side
// of the armed fulfiller in time, so a wake that arrived just before the loop died is dequeued
// quietly instead of being reported as an undrained reply.
struct CrossThreadWakeSink::Holder {
  kj::Arc<CrossThreadWakeSink> sink = kj::arc<CrossThreadWakeSink>();

  Holder() {
    sink->ensureDrain();
  }

  // Runs during ~EventLoop, after the drain daemon is gone: later enqueues drop their cells.
  ~Holder() noexcept(false) {
    sink->close();
  }
};

kj::Promise<void> CrossThreadWakeSink::drainLoop(kj::Arc<CrossThreadWakeSink> sink) {
  // Co-owns the sink so the frame never outlives what it reads. The frame's destruction --
  // cancellation by ~EventLoop or by cancelAllDetached() -- is what marks the drain stopped, so
  // ensureDrain() can restart it; a bare `drainRunning = false` at the end would miss both.
  KJ_DEFER(sink->drainRunning.store(false, std::memory_order_relaxed));
  for (;;) {
    auto paf = kj::newPromiseAndCrossThreadFulfiller<void>();
    sink->arm(kj::mv(paf.fulfiller));
    co_await paf.promise;
    for (auto& cell: sink->takePending()) {
      // Same-thread now: arms the event, or no-op if the event was destroyed meanwhile.
      cell->wakeByRef();
    }
  }
}

void CrossThreadWakeSink::ensureDrain() const {
  if (drainRunning.load(std::memory_order_relaxed)) return;
  drainRunning.store(true, std::memory_order_relaxed);
  drainLoop(kj::Arc<CrossThreadWakeSink>(const_cast<CrossThreadWakeSink&>(*this).addRefToThis()))
      .detach([](kj::Exception&& exception) {
    // Only kj::newPromiseAndCrossThreadFulfiller() can throw here, and only if the loop has no
    // executor left; nothing to recover, but never silently. (The frame's KJ_DEFER has already
    // marked the drain stopped, so a later ensureDrain() retries.)
    KJ_LOG(ERROR, "kj-rs cross-thread wake drain stopped", exception);
  });
}

kj::Arc<CrossThreadWakeSink> CrossThreadWakeSink::forCurrentLoop() {
  // One sink per event loop, created on first use and destroyed with the loop. (A function-local
  // static so it can name the private Holder; EventLoopLocal only requires static storage.)
  static const kj::EventLoopLocal<Holder> loopSink;
  auto& sink = loopSink->sink;
  sink->ensureDrain();
  return sink.addRef();
}

void CrossThreadWakeSink::enqueue(kj::Arc<FutureWakerCell> cell) const {
  auto lock = state.lockExclusive();
  if (lock->closed) return;  // loop gone: drop the reference, nothing to wake
  lock->pending.add(kj::mv(cell));
  KJ_IF_SOME(fulfiller, lock->fulfiller) {
    // First wake since the drain armed: fire it (and consume it; the drain installs the next).
    // fulfill() is thread-safe and fine to call under our lock: the fulfiller never calls back.
    fulfiller->fulfill();
    lock->fulfiller = kj::none;
  }
}

void CrossThreadWakeSink::arm(
    kj::Own<const kj::CrossThreadPromiseFulfiller<void>> fulfiller) const {
  auto lock = state.lockExclusive();
  if (!lock->pending.empty()) {
    // Wakes arrived between the last drain and this arm: fire immediately.
    fulfiller->fulfill();
    return;
  }
  lock->fulfiller = kj::mv(fulfiller);
}

kj::Vector<kj::Arc<FutureWakerCell>> CrossThreadWakeSink::takePending() const {
  auto lock = state.lockExclusive();
  auto result = kj::mv(lock->pending);
  lock->pending = kj::Vector<kj::Arc<FutureWakerCell>>();
  return result;
}

void CrossThreadWakeSink::close() const {
  auto lock = state.lockExclusive();
  lock->closed = true;
  // Queued cells that never got replayed: their events are dying with this loop anyway.
  lock->pending = kj::Vector<kj::Arc<FutureWakerCell>>();
}

// =======================================================================================
// FutureWakerCell

// Defined here rather than inline in waker.h because arming the event requires FuturePollEvent to
// be a complete type, which it only is once awaiter.h is included.
void FutureWakerCell::wakeByRef() const {
  if (executor->isCurrent()) {
    // Owning thread: arm the event directly. `event` is only touched on this thread. (An event
    // port that drives other work inside its own wait() -- kj-rs-tokio -- learns of the arm
    // through EventLoop::setRunnable(true); nothing here needs to know about it.)
    KJ_IF_SOME(e, event) {
      e.armDepthFirst();
    }
  } else if (alive.load(std::memory_order_acquire)) {
    // Foreign thread (possibly one with no KJ event loop): hand ourselves to the owning loop's
    // sink, whose drain replays this wake on the owning thread. A closed sink (loop gone) drops
    // the reference; repeated wakes before the drain runs coalesce.
    sink->enqueue(addRef());
  }
  // else: our event is already gone (neutralized); nothing to wake, so do not bother the loop.
}

// =======================================================================================
// PollWaker

PollWaker::PollWaker(FuturePollEvent& futurePollEvent): cell(futurePollEvent.cloneWakerCell()) {
  // Every poll starts on the owning thread: the cheapest reliable place to make sure the loop's
  // cross-thread drain is (still) running -- see CrossThreadWakeSink::ensureDrain().
  cell->ensureCrossThreadDrain();
}

PollWaker::~PollWaker() noexcept(false) {}

void PollWaker::wakeByRef() const {
  // Delegate to the cell, which handles both threads: on the owning thread this is a synchronous
  // same-turn wake (armDepthFirst() is idempotent and safe from within the event's own fire(),
  // so it works whether we were reached from onReady() or fire(), and causes an immediate
  // re-poll); from a foreign thread it goes through the cross-thread fulfiller — `&Waker` is
  // Sync, so even this borrowed waker may legally be woken from another thread during the poll.
  cell->wakeByRef();
}

kj::Arc<FutureWakerCell> PollWaker::cloneCell() const {
  // Rust wants a waker it can retain and wake later: hand out a strong reference to the event's
  // FutureWakerCell. Atomic refcount, safe from any thread.
  return cell->addRef();
}

kj::Maybe<FuturePollEvent&> PollWaker::tryGetFuturePollEvent() const {
  return cell->tryGetFuturePollEvent();
}

}  // namespace kj_rs
