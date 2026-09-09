#include "waker.h"

#include "awaiter.h"

namespace kj_rs {

// =======================================================================================
// CrossThreadWakeSink

struct CrossThreadWakeSink::Holder {
  kj::Arc<CrossThreadWakeSink> sink = kj::arc<CrossThreadWakeSink>();

  Holder() {
    sink->ensureDrain();
  }

  ~Holder() noexcept(false) {
    sink->close();
  }
};

kj::Promise<void> CrossThreadWakeSink::drainLoop(kj::Arc<CrossThreadWakeSink> sink) {
  KJ_DEFER(sink->drainRunning.store(false, std::memory_order_relaxed));
  for (;;) {
    auto paf = kj::newPromiseAndCrossThreadFulfiller<void>();
    sink->arm(kj::mv(paf.fulfiller));
    co_await paf.promise;
    for (auto& cell: sink->takePending()) {
      cell->wakeByRef();
    }
  }
}

void CrossThreadWakeSink::ensureDrain() const {
  if (drainRunning.load(std::memory_order_relaxed)) return;
  drainRunning.store(true, std::memory_order_relaxed);
  drainLoop(kj::Arc<CrossThreadWakeSink>(const_cast<CrossThreadWakeSink&>(*this).addRefToThis()))
      .detach([](kj::Exception&& exception) {
    KJ_LOG(ERROR, "kj-rs cross-thread wake drain stopped", exception);
  });
}

kj::Arc<CrossThreadWakeSink> CrossThreadWakeSink::forCurrentLoop() {
  static const kj::EventLoopLocal<Holder> loopSink;
  auto& sink = loopSink->sink;
  sink->ensureDrain();
  return sink.addRef();
}

void CrossThreadWakeSink::enqueue(kj::Arc<FutureWakerCell> cell) const {
  auto lock = state.lockExclusive();
  if (lock->closed) return;
  lock->pending.add(kj::mv(cell));
  KJ_IF_SOME(fulfiller, lock->fulfiller) {
    fulfiller->fulfill();
    lock->fulfiller = kj::none;
  }
}

void CrossThreadWakeSink::arm(
    kj::Own<const kj::CrossThreadPromiseFulfiller<void>> fulfiller) const {
  auto lock = state.lockExclusive();
  if (!lock->pending.empty()) {
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
  lock->pending = kj::Vector<kj::Arc<FutureWakerCell>>();
}

// =======================================================================================
// FutureWakerCell

void FutureWakerCell::wakeByRef() const {
  if (executor->isCurrent()) {
    KJ_IF_SOME(e, event) {
      e.armDepthFirst();
    }
  } else if (alive.load(std::memory_order_acquire)) {
    sink->enqueue(addRef());
  }
}

// =======================================================================================
// PollWaker

PollWaker::PollWaker(FuturePollEvent& futurePollEvent): cell(futurePollEvent.cloneWakerCell()) {
  cell->ensureCrossThreadDrain();
}

PollWaker::~PollWaker() noexcept(false) {}

void PollWaker::wakeByRef() const {
  cell->wakeByRef();
}

kj::Arc<FutureWakerCell> PollWaker::cloneCell() const {
  return cell->addRef();
}

kj::Maybe<FuturePollEvent&> PollWaker::tryGetFuturePollEvent() const {
  return cell->tryGetFuturePollEvent();
}

}  // namespace kj_rs
