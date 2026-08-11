#include "waker.h"

#include <kj/debug.h>

namespace kj_rs {

// =======================================================================================
// ArcWakerPromiseNode

ArcWakerPromiseNode::ArcWakerPromiseNode(kj::Promise<void> promise)
    : node(PromiseNode::from(kj::mv(promise))) {
  node->setSelfPointer(&node);
}

void ArcWakerPromiseNode::destroy() noexcept {
  auto drop = kj::mv(owner);
}

void ArcWakerPromiseNode::onReady(kj::_::Event* event) noexcept {
  node->onReady(event);
}

void ArcWakerPromiseNode::get(kj::_::ExceptionOrValue& output) noexcept {
  node->get(output);
  KJ_IF_SOME(exception, kj::runCatchingExceptions([this]() { node = nullptr; })) {
    output.addException(kj::mv(exception));
  }
}

void ArcWakerPromiseNode::tracePromise(kj::_::TraceBuilder& builder, bool stopAtNextEvent) {
  // TODO(someday): Is it possible to get the address of the Rust code which cloned our Waker?

  if (node.get() != nullptr) {
    node->tracePromise(builder, stopAtNextEvent);
  }
}

// =======================================================================================
// ArcWaker

PromiseArcWakerPair ArcWaker::create(const kj::Executor& executor) {
  // TODO(perf): newPromiseAndCrossThreadFulfiller() makes two heap allocations, but it is probably
  //   optimizable to one.
  // TODO(perf): This heap allocation could also probably be collapsed into the fulfiller's.
  auto waker =
      kj::arc<ArcWaker>(kj::Badge<ArcWaker>(), executor.newPromiseAndCrossThreadFulfiller<void>());
  auto promise = const_cast<ArcWaker*>(waker.get())->getPromise();
  return {
    .promise = kj::mv(promise),
    .waker = kj::mv(waker),
  };
}

kj::Promise<void> ArcWaker::getPromise() {
  KJ_REQUIRE(node.owner == nullptr);
  node.owner = addRefToThis();
  return kj::_::PromiseNode::to<kj::Promise<void>>(OwnPromiseNode(&node));
}

ArcWaker::ArcWaker(kj::Badge<ArcWaker>, kj::PromiseCrossThreadFulfillerPair<void> paf)
    : node(kj::mv(paf.promise)),
      fulfiller(kj::mv(paf.fulfiller)) {}

const KjWaker* ArcWaker::clone() const {
  return addRefToThis().disown();
}
void ArcWaker::wake() const {
  wake_by_ref();
  drop();
}
void ArcWaker::wake_by_ref() const {
  fulfiller->fulfill();
}
void ArcWaker::drop() const {
  auto drop = kj::Arc<const ArcWaker>::reown(this);
}

// =======================================================================================
// LazyArcWaker

const KjWaker* LazyArcWaker::clone() const {
  // Rust code wants to suspend and wait for something. We'll start handing out ArcWakers if we
  // haven't already been woken synchronously.

  if (wakeCount.load(std::memory_order_relaxed) > 0) {
    // We were already woken synchronously, so there's no point handing out more wakers for the
    // current call to `Future::poll()`. We can hand out a noop waker by returning nullptr.
    return nullptr;
  }

  auto lock = cloned.lockExclusive();

  if (*lock == kj::none) {
    // We haven't been cloned before, so make a new ArcWaker.
    *lock = ArcWaker::create(executor);
  }

  return KJ_ASSERT_NONNULL(*lock).waker->clone();
}

void LazyArcWaker::wake() const {
  // LazyArcWakers are only exposed to Rust by const borrow, meaning Rust can never arrange to call
  // `wake()`, which drops `self`, on this object.
  KJ_UNIMPLEMENTED("Rust user code should never have possess a consumable "
                   "reference to LazyArcWaker");
}

void LazyArcWaker::wake_by_ref() const {
  // Woken synchronously during a call to `future.poll(awaitWaker)`.
  wakeCount.fetch_add(1, std::memory_order_relaxed);
}

void LazyArcWaker::drop() const {
  ++dropCount;
}

kj::Maybe<kj::Promise<void>> LazyArcWaker::reset() {
  // This function is only called after `future.poll(awaitWaker)` has returned, meaning Rust has
  // dropped its reference. Thus, we don't need to worry about thread-safety here, and can call
  // `cloned.getWithoutLock()`, for example.

  KJ_ASSERT(dropCount == 1);
  KJ_DEFER(dropCount = 0);
  KJ_DEFER(wakeCount.store(0, std::memory_order_relaxed));

  // Reset the ArcWaker on our way out. Since we only return the ArcWaker's promise to our caller,
  // we ensure that Rust owns the only remaining ArcWaker clones, if any.
  //
  // TODO(perf): If ArcWakers were resettable, we could instead return the ArcWaker for our caller
  //   to cache for later use.
  KJ_DEFER(cloned.getWithoutLock() = kj::none);

  if (wakeCount.load(std::memory_order_relaxed) > 0) {
    // The future returned Pending, but synchronously called `wake_by_ref()` on the LazyArcWaker,
    // indicating it wants to immediately be polled again. We should arm our event right now,
    // which will call `await_ready()` again on the event loop.
    return kj::Promise<void>(kj::READY_NOW);
  } else KJ_IF_SOME(arcWakerPair, cloned.getWithoutLock()) {
    // The future returned Pending and cloned an ArcWaker to notify us later. We'll arrange for
    // the ArcWaker's promise to arm our event once it's fulfilled.
    return kj::mv(arcWakerPair.promise);
  } else {
    // The future returned Pending, did not call `wake_by_ref()` on the LazyArcWaker, and did not
    // clone an ArcWaker. Rust is either awaiting a KJ promise, or the Rust equivalent of
    // kj::NEVER_DONE.
    return kj::none;
  }
}

}  // namespace kj_rs
