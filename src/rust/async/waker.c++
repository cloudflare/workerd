#include <workerd/rust/async/waker.h>
#include <workerd/rust/async/leak.h>

#include <kj/debug.h>

namespace workerd::rust::async {

// =======================================================================================
// ArcWaker

ArcWaker::ArcWaker(kj::Own<const kj::CrossThreadPromiseFulfiller<WakeInstruction>> fulfiller)
    : fulfiller(kj::mv(fulfiller)) {}
ArcWaker::~ArcWaker() noexcept(false) {
  // We can't leave the promise hanging or else the fulfiller's destructor will reject it for us.
  // So, settle the promise with our no-op ignore value in case we're still waiting here.
  fulfiller->fulfill(WakeInstruction::IGNORE);
}

const CxxWaker* ArcWaker::clone() const {
  return leak(addRefToThis());
}
void ArcWaker::wake() const {
  wake_by_ref();
  drop();
}
void ArcWaker::wake_by_ref() const {
  fulfiller->fulfill(WakeInstruction::WAKE);
}
void ArcWaker::drop() const {
  auto drop = unleak(this);
}

PromiseArcWakerPair newPromiseAndArcWaker(const kj::Executor& executor) {
  // TODO(perf): newPromiseAndCrossThreadFulfiller() makes two heap allocations, but it is probably
  //   optimizable to one.
  auto [promise, fulfiller] = kj::newPromiseAndCrossThreadFulfiller<WakeInstruction>(executor);
  return {
    .promise = kj::mv(promise),
    // TODO(perf): This heap allocation could also probably be collapsed into the fulfiller's.
    .waker = kj::arc<const ArcWaker>(kj::mv(fulfiller)),
  };
}

// =======================================================================================
// RootWaker

PollEvent::PollEvent(kj::_::Event& next, kj::SourceLocation location)
    : Event(location),
      next(next) {}

void PollEvent::beginTrace(OwnPromiseNode& node) {
  if (promiseNodeForTrace == kj::none) {
    promiseNodeForTrace = node;
  }
}

void PollEvent::endTrace(OwnPromiseNode& node) {
  KJ_IF_SOME(myNode, promiseNodeForTrace) {
    if (myNode.get() == node.get()) {
      promiseNodeForTrace = kj::none;
    }
  }
}

void PollEvent::traceEvent(kj::_::TraceBuilder& builder) {
  KJ_IF_SOME(node, promiseNodeForTrace) {
    node->tracePromise(builder, true);
  }
  next.traceEvent(builder);
}

RootWaker::RootWaker(PollEvent& pollEvent): pollEvent(pollEvent) {}

const CxxWaker* RootWaker::clone() const {
  // Rust code wants to suspend and wait for something other than an OwnPromiseNode from the same
  // thread as this RootWaker. We'll start handing out ArcWakers if we haven't already been woken
  // synchronously.

  if (wakeCount.load(std::memory_order_relaxed) > 0) {
    // We were already woken synchronously, so there's no point handing out more wakers for the
    // current call to `Future::poll()`. We can hand out a noop waker by returning nullptr.
    return nullptr;
  }

  auto lock = cloned.lockExclusive();

  if (*lock == kj::none) {
    // We haven't been cloned before, so make a new ArcWaker.
    *lock = newPromiseAndArcWaker(executor);
  }

  return KJ_ASSERT_NONNULL(*lock).waker->clone();
}

void RootWaker::wake() const {
  // RootWakers are only exposed to Rust by const borrow, meaning Rust can never arrange to call
  // `wake()`, which drops `self`, on this object.
  KJ_UNIMPLEMENTED("Rust user code should never have a consumable reference to RootWaker");
}

void RootWaker::wake_by_ref() const {
  // Woken synchronously during a call to `future.poll(awaitWaker)`.
  wakeCount.fetch_add(1, std::memory_order_relaxed);
}

void RootWaker::drop() const {
  ++dropCount;
}

bool RootWaker::is_current() const {
  return &executor == &kj::getCurrentThreadExecutor();
}

RootWaker::State RootWaker::reset() {
  // Getting the state without a lock is safe, because this function is only called after
  // `future.poll(awaitWaker)` has returned, meaning Rust has dropped its reference.
  KJ_ASSERT(dropCount == 1);
  KJ_DEFER(dropCount = 0);
  KJ_DEFER(wakeCount.store(0, std::memory_order_relaxed));
  KJ_DEFER(cloned.getWithoutLock() = kj::none);
  return {
    .wakeCount = wakeCount.load(std::memory_order_relaxed),
    .cloned = kj::mv(cloned.getWithoutLock()),
  };
}

}  // namespace workerd::rust::async
