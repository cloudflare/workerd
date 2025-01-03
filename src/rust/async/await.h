#include <workerd/rust/async/future.h>
#include <workerd/rust/async/waker.h>

#include <kj/debug.h>
#include <kj/mutex.h>

namespace workerd::rust::async {

// TODO(cleanup): Code duplication with kj::_::PromiseAwaiterBase. If BaseFutureAwaiterBase could
//   somehow implement CoroutineBase's interface, we could fold this into one class.
// TODO(perf): This is only an Event because we need to handle the case where all the Wakers are
//   dropped and we receive a WakeInstruction::IGNORE. If we could somehow disarm the
//   CrossThreadPromiseFulfillers inside ArcWaker when it's dropped, we could avoid this
//   indirection.
class ArcWakerAwaiter final: public kj::_::Event {
public:
  ArcWakerAwaiter(PollEvent& pollEvent, OwnPromiseNode node, kj::SourceLocation location = {})
      : Event(location),
        pollEvent(pollEvent),
        node(kj::mv(node)) {
    this->node->setSelfPointer(&this->node);
    this->node->onReady(this);
    // TODO(perf): If `this->isNext()` is true, can we immediately resume? Or should we check if
    //   the enclosing coroutine has suspended at least once?
    pollEvent.beginTrace(this->node);
  }

  ~ArcWakerAwaiter() noexcept(false) {
    pollEvent.endTrace(node);

    unwindDetector.catchExceptionsIfUnwinding([this]() {
      node = nullptr;
    });
  }

  // Validity-check the Promise's result, then fire the BaseFutureAwaiterBase Event to poll the
  // wrapped Future again.
  kj::Maybe<kj::Own<kj::_::Event>> fire() override {
    pollEvent.endTrace(node);

    node->get(result);
    KJ_IF_SOME(exception, kj::runCatchingExceptions([this]() {
      node = nullptr;
    })) {
      result.addException(kj::mv(exception));
    }

    // We should only ever receive a WakeInstruction, never an exception. But if we do, propagate
    // it to the coroutine.
    if (result.exception != kj::none) {
      // TODO(perf): Call `pollEvent.fire()` directly?
      // Note: Event::armDepthFirst() is idempotent.
      pollEvent.armDepthFirst();
      return kj::none;
    }

    KJ_IF_SOME(value, result.value) {
      if (value == WakeInstruction::WAKE) {
        // TODO(perf): Call `pollEvent.fire()` directly?
        pollEvent.armDepthFirst();
      } else {
        // All of our Wakers were dropped. We are awaiting the Rust equivalent of kj::NEVER_DONE.
      }
    }

    return kj::none;
  }

  void traceEvent(kj::_::TraceBuilder& builder) override {
    if (node.get() != nullptr) {
      node->tracePromise(builder, true);
    }
    pollEvent.traceEvent(builder);
  }

  kj::Maybe<kj::Exception> tryGetException() {
    return kj::mv(result.exception);
  }

private:
  PollEvent& pollEvent;

  kj::UnwindDetector unwindDetector;
  kj::_::OwnPromiseNode node;
  kj::_::ExceptionOr<WakeInstruction> result;
};

class OwnPromiseNodeFuture: public kj::_::Event {
public:
  OwnPromiseNodeFuture(RootWaker& waker, kj::_::OwnPromiseNode node, kj::SourceLocation location = {})
      : Event(location),
        pollEvent(waker.getPollEvent()),
        node(kj::mv(node)) {
    this->node->setSelfPointer(&this->node);
    this->node->onReady(this);
    // TODO(perf): If `this->isNext()` is true, can we immediately resume? Or should we check if
    //   the enclosing coroutine has suspended at least once?
    pollEvent.beginTrace(this->node);
  }

  ~OwnPromiseNodeFuture() noexcept(false) {
    pollEvent.endTrace(node);

    unwindDetector.catchExceptionsIfUnwinding([this]() {
      node = nullptr;
    });
  }

  kj::Maybe<kj::Own<kj::_::Event>> fire() override {
    pollEvent.endTrace(node);
    done = true;
    // TODO(perf): Call `pollEvent.fire()` directly?
    pollEvent.armDepthFirst();
    return kj::none;
  }

  void traceEvent(kj::_::TraceBuilder& builder) override {
    if (node.get() != nullptr) {
      node->tracePromise(builder, true);
    }
    pollEvent.traceEvent(builder);
  }

  // TODO(now): Figure out how to return value-or-exception to Rust code.

private:
  PollEvent& pollEvent;

  kj::UnwindDetector unwindDetector;
  kj::_::OwnPromiseNode node;
  bool done;
};

class BoxFutureVoidAwaiter: public PollEvent {
public:
  BoxFutureVoidAwaiter(kj::_::CoroutineBase& coroutine, BoxFutureVoid&& future, kj::SourceLocation location = {})
      : PollEvent(coroutine, location),
        coroutine(coroutine),
        future(kj::mv(future)) {}
  ~BoxFutureVoidAwaiter() noexcept(false) {
    coroutine.awaitEnd();
  }

  bool await_ready() {
    // TODO(perf): Check if we already have an ArcWaker from a previous suspension and give it to
    //   RootWaker for cloning if we have the last reference to it at this point. This could save
    //   memory allocations, but would depend on making XThreadFulfiller and XThreadPaf resettable
    //   to really benefit.
    RootWaker waker(*this);

    if (future.poll(waker)) {
      // Future is ready, we're done.
      // TODO(now): Propagate value-or-exception.
      return true;
    }

    auto state = waker.reset();

    if (state.wakeCount > 0) {
      // The future returned Pending, but synchronously called `wake_by_ref()` on the RootWaker,
      // indicating it wants to immediately be polled again. We should arm our event right now,
      // which will call `await_ready()` again on the event loop.
      armDepthFirst();
    } else KJ_IF_SOME(cloned, state.cloned) {
      // The future returned Pending and cloned an ArcWaker to notify us later. We'll arrange for
      // the ArcWaker's promise to arm our event once it's fulfilled.
      arcWakerAwaiter.emplace(*this, kj::_::PromiseNode::from(kj::mv(cloned.promise)));
    } else {
      // The future returned Pending, did not call `wake_by_ref()` on the RootWaker, and did not
      // clone an ArcWaker. We are awaiting the Rust equivalent of kj::NEVER_DONE.
      // TODO(now): Once DoneDetector is in service, this code path could be taken by awaiting KJ
      //   promises in the Future.
    }

    return false;
  }

  // We already arranged to be scheduled in await_ready(), nothing to do here.
  void await_suspend(kj::_::stdcoro::coroutine_handle<>) {}

  // Validity-check the Promise's result, then poll our wrapped Future again. If it's ready, or if
  // there was an exception, schedule the coroutine by arming its event.
  kj::Maybe<kj::Own<kj::_::Event>> fire() override {
    // Check for an internal error.
    KJ_IF_SOME(exception, tryGetArcWakerException()) {
      // This _should_ be a dead code path, because ArcWaker never rejects its promise, and
      // always fulfills it on destruction.
      result.addException(kj::mv(exception));
      // TODO(perf): Call `coroutine.fire()` directly?
      coroutine.armDepthFirst();
      return kj::none;
    }

    // Poll the Future.
    if (await_ready()) {
      // TODO(perf): Call `coroutine.fire()` directly?
      coroutine.armDepthFirst();
    }

    return kj::none;
  }

  // Unit futures return void.
  void await_resume() {
    KJ_IF_SOME(exception, result.exception) {
      kj::throwFatalException(kj::mv(exception));
    }
  }

private:
  kj::Maybe<kj::Exception> tryGetArcWakerException() {
    KJ_IF_SOME(awa, arcWakerAwaiter) {
      return awa.tryGetException();
    }
    return kj::none;
  }

  kj::_::CoroutineBase& coroutine;
  BoxFutureVoid future;
  kj::_::ExceptionOr<kj::_::Void> result;

  kj::Maybe<ArcWakerAwaiter> arcWakerAwaiter;
};

BoxFutureVoidAwaiter operator co_await(kj::_::CoroutineBase::Await<BoxFutureVoid> await);
BoxFutureVoidAwaiter operator co_await(kj::_::CoroutineBase::Await<BoxFutureVoid&> await);

#if 0

// Invoke like so:

bindgen \
    --rust-target 1.83.0 \
    --enable-cxx-namespaces \
    --allowlist-type "workerd::rust::async_::OwnPromiseNodeFuture" \
    ./promise.h.rs \
    -- \
    -x c++ \
    -std=c++23 \
    -stdlib=libc++ \
    -Wno-pragma-once-outside-header \
    -I $(bazel info bazel-bin)/external/capnp-cpp/src/kj/_virtual_includes/kj \
    -I $(bazel info bazel-bin)/external/capnp-cpp/src/kj/_virtual_includes/kj-async \
    -I $(bazel info bazel-bin)/external/crates_vendor__cxx-1.0.133/_virtual_includes/cxx_cc

// Notes:
// - The `--allowlist-type` argument is a regex. It matches on bindgen's mangled fully-qualified
//   name of the type, _not_ C++'s fully-qualified named. Since `async` is a keyword in Rust, that
//   is why it's spelled `...::async_::...`, rather than `...::async::...`.
// - If you pass `--enable-cxx-namespaces` to bindgen, you'll get Rust modules defined such that the
//   type is named `root::workerd::rust::async_::OwnPromiseNodeFuture`.
// - If you don't pass `--enable-cxx-namespaces`, the Rust name will be
//   `workerd_rust_async_OwnPromiseNodeFuture`.

#endif

}  // namespace workerd::rust::async
