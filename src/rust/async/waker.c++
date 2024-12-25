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
// AwaitWaker

const CxxWaker* AwaitWaker::clone() const {
  // Rust code wants to suspend and wait for something other than an OwnPromiseNode from the same
  // thread as this AwaitWaker. We'll start handing out ArcWakers if we haven't already been woken
  // synchronously.

  auto lock = state.lockExclusive();

  if (lock->wakeCount > 0) {
    // We were already woken synchronously, so there's no point handing out more wakers for the
    // current call to `Future::poll()`. We can hand out a noop waker by returning nullptr.
    return nullptr;
  }

  if (lock->cloned == kj::none) {
    // We haven't been cloned before, so make a new ArcWaker.
    lock->cloned = newPromiseAndArcWaker(executor);
  }

  return KJ_ASSERT_NONNULL(lock->cloned).waker->clone();
}

void AwaitWaker::wake() const {
  // AwaitWakers are only exposed to Rust by const borrow, meaning Rust can never arrange to call
  // `wake()`, which drops `self`, on this object.
  KJ_UNIMPLEMENTED("Rust user code should never have a consumable reference to AwaitWaker");
}

void AwaitWaker::wake_by_ref() const {
  // Woken synchronously during a call to `future.poll(awaitWaker)`.
  auto lock = state.lockExclusive();
  ++lock->wakeCount;
}

void AwaitWaker::drop() const {
  ++dropCount;
}

bool AwaitWaker::is_current() const {
  return &executor == &kj::getCurrentThreadExecutor();
}

void AwaitWaker::wake_after(OwnPromiseNode& node) const {
  // If our kj::Executor isn't current on this thread, Rust should not have called us.
  KJ_ASSERT(is_current());

  // TODO(perf): There shouldn't be any need to store `wakeAfter` behind the mutex, since only one
  //   thread can call this function at a time.
  auto lock = state.lockExclusive();

  // TODO(now): If we already have a `wakeAfter` promise, Rust must be trying to await two
  //   OwnPromiseNodes at once. Figure out how to support this case. Options:
  //   1. Rust learns how to own a kj::_::Event in OwnPromiseNodeFuture, possibly with the `moveit!`
  //      macro. See comment in IntoFuture trait implementation for OwnPromiseNode.
  //   2. We maintain a set of kj::_::Events here in AwaitWaker, one for each promise. We could
  //      store one directly in AwaitWaker for optimizing the common case, then fall back to a heap-
  //      allocated list of kj::_::Events.
  //   3. We give PromiseNode an isReady() function, and actually poll them. If we had an
  //      `Event::isArmed()` function, we could implement this today, but we'd break KJ's orddering
  //      guarantees...
  KJ_REQUIRE(lock->wakeAfter == kj::none,
      "AwaitWaker does not yet know how to await multiple promises at once");

  lock->wakeAfter = node;
}

AwaitWaker::State AwaitWaker::reset() {
  // Getting the state without a lock is safe, because this function is only called after
  // `future.poll(awaitWaker)` has returned, meaning Rust has dropped its reference.
  KJ_ASSERT(dropCount == 1);
  KJ_DEFER(dropCount = 0);
  auto result = kj::mv(state.getWithoutLock());
  state.getWithoutLock() = State{};
  return result;
}

}  // namespace workerd::rust::async
