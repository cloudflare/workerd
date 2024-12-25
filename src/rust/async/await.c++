#include <workerd/rust/async/await.h>

namespace workerd::rust::async {

// =======================================================================================
// ArcWakerAwaiter

ArcWakerAwaiter::ArcWakerAwaiter(FuturePollerBase& futurePoller, OwnPromiseNode node, kj::SourceLocation location)
    : Event(location),
      futurePoller(futurePoller),
      node(kj::mv(node)) {
  this->node->setSelfPointer(&this->node);
  this->node->onReady(this);
  // TODO(perf): If `this->isNext()` is true, can we immediately resume? Or should we check if
  //   the enclosing coroutine has suspended at least once?
  futurePoller.beginTrace(this->node);
}

ArcWakerAwaiter::~ArcWakerAwaiter() noexcept(false) {
  futurePoller.endTrace(node);

  unwindDetector.catchExceptionsIfUnwinding([this]() {
    node = nullptr;
  });
}

// Validity-check the Promise's result, then fire the BaseFutureAwaiterBase Event to poll the
// wrapped Future again.
kj::Maybe<kj::Own<kj::_::Event>> ArcWakerAwaiter::fire() {
  futurePoller.endTrace(node);

  kj::_::ExceptionOr<WakeInstruction> result;

  node->get(result);
  KJ_IF_SOME(exception, kj::runCatchingExceptions([this]() {
    node = nullptr;
  })) {
    result.addException(kj::mv(exception));
  }

  // We should only ever receive a WakeInstruction, never an exception. But if we do, propagate
  // it to the coroutine.
  KJ_IF_SOME(exception, result.exception) {
    futurePoller.reject(kj::mv(exception));
    return kj::none;
  }

  auto value = KJ_ASSERT_NONNULL(result.value);

  if (value == WakeInstruction::WAKE) {
    // This was an actual wakeup.
    futurePoller.armDepthFirst();
  } else {
    // All of our Wakers were dropped. We are awaiting the Rust equivalent of kj::NEVER_DONE.
  }

  return kj::none;
}

void ArcWakerAwaiter::traceEvent(kj::_::TraceBuilder& builder) {
  if (node.get() != nullptr) {
    node->tracePromise(builder, true);
  }
  futurePoller.traceEvent(builder);
}

// =================================================================================================
// RustPromiseAwaiter

RustPromiseAwaiter::RustPromiseAwaiter(const RootWaker& rootWaker, OwnPromiseNode node, kj::SourceLocation location)
    : Event(location),
      // TODO(now): const cast
      futurePoller(const_cast<RootWaker&>(rootWaker).getFuturePoller()),
      node(kj::mv(node)),
      done(false) {
  this->node->setSelfPointer(&this->node);
  this->node->onReady(this);
  // TODO(perf): If `this->isNext()` is true, can we immediately resume? Or should we check if
  //   the enclosing coroutine has suspended at least once?
  futurePoller.beginTrace(this->node);
}

RustPromiseAwaiter::~RustPromiseAwaiter() noexcept(false) {
  futurePoller.endTrace(node);

  unwindDetector.catchExceptionsIfUnwinding([this]() {
    node = nullptr;
  });
}

kj::Maybe<kj::Own<kj::_::Event>> RustPromiseAwaiter::fire() {
  futurePoller.endTrace(node);
  done = true;
  futurePoller.armDepthFirst();
  return kj::none;
}

void RustPromiseAwaiter::traceEvent(kj::_::TraceBuilder& builder) {
  if (node.get() != nullptr) {
    node->tracePromise(builder, true);
  }
  futurePoller.traceEvent(builder);
}

bool RustPromiseAwaiter::poll(const RootWaker& rootWaker) {
  // TODO(now): const cast, and can we do something smarter?
  KJ_ASSERT(&const_cast<RootWaker&>(rootWaker).getFuturePoller() == &futurePoller);
  return done;
}

void rust_promise_awaiter_new_in_place(RustPromiseAwaiter* ptr, const RootWaker& rootWaker, OwnPromiseNode node) {
  kj::ctor(*ptr, rootWaker, kj::mv(node));
}
void rust_promise_awaiter_drop_in_place(RustPromiseAwaiter* ptr) {
  kj::dtor(*ptr);
}

// =======================================================================================
// FuturePollerBase

FuturePollerBase::FuturePollerBase(
    kj::_::Event& next, kj::_::ExceptionOrValue& resultRef, kj::SourceLocation location)
    : Event(location),
      next(next),
      resultRef(resultRef) {}

void FuturePollerBase::beginTrace(OwnPromiseNode& node) {
  if (promiseNodeForTrace == kj::none) {
    promiseNodeForTrace = node;
  }
}

void FuturePollerBase::endTrace(OwnPromiseNode& node) {
  KJ_IF_SOME(myNode, promiseNodeForTrace) {
    if (myNode.get() == node.get()) {
      promiseNodeForTrace = kj::none;
    }
  }
}

void FuturePollerBase::reject(kj::Exception exception) {
  resultRef.addException(kj::mv(exception));
  next.armDepthFirst();
}

void FuturePollerBase::traceEvent(kj::_::TraceBuilder& builder) {
  KJ_IF_SOME(node, promiseNodeForTrace) {
    node->tracePromise(builder, true);
  }
  next.traceEvent(builder);
}

BoxFutureVoidAwaiter operator co_await(kj::_::CoroutineBase::Await<BoxFutureVoid> await) {
  return BoxFutureVoidAwaiter{await.coroutine, kj::mv(await.awaitable)};
}

BoxFutureVoidAwaiter operator co_await(kj::_::CoroutineBase::Await<BoxFutureVoid&> await) {
  return BoxFutureVoidAwaiter{await.coroutine, kj::mv(await.awaitable)};
}

}  // namespace workerd::rust::async
