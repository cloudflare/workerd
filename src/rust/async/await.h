#pragma once

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
  ArcWakerAwaiter(FuturePollerBase& futurePoller, OwnPromiseNode node, kj::SourceLocation location = {});
  ~ArcWakerAwaiter() noexcept(false);

  kj::Maybe<kj::Own<kj::_::Event>> fire() override;
  void traceEvent(kj::_::TraceBuilder& builder) override;

private:
  FuturePollerBase& futurePoller;
  kj::UnwindDetector unwindDetector;
  kj::_::OwnPromiseNode node;
};

// =======================================================================================
// RustPromiseAwaiter

// RustPromiseAwaiter allows Rust `async` blocks to `.await` KJ promises. Rust code creates one in
// the block's storage at the point where the `.await` expression is evaluated, similar to how
// `kj::_::PromiseAwaiter` is created in the KJ coroutine frame when C++ `co_await`s a promise.
//
// To initialize the object, Rust needs to know the size and alignment of RustPromiseAwaiter. To
// that end, I used bindgen to generate an opaque FFI type in await_h.rs using the command below.
//
// TODO(now): Automate this?

#if 0

bindgen \
    --rust-target 1.83.0 \
    --disable-name-namespacing \
    --generate "types" \
    --allowlist-type "workerd::rust::async_::RustPromiseAwaiter" \
    --opaque-type ".*" \
    --no-derive-copy \
    ./await.h \
    -o ./await.h.rs \
    -- \
    -x c++ \
    -std=c++23 \
    -stdlib=libc++ \
    -Wno-pragma-once-outside-header \
    -I $(bazel info bazel-bin)/external/capnp-cpp/src/kj/_virtual_includes/kj \
    -I $(bazel info bazel-bin)/external/capnp-cpp/src/kj/_virtual_includes/kj-async \
    -I $(bazel info bazel-bin)/external/crates_vendor__cxx-1.0.133/_virtual_includes/cxx_cc \
    -I $(bazel info bazel-bin)/src/rust/async/_virtual_includes/async@cxx

#endif

class RustPromiseAwaiter final: public kj::_::Event {
public:
  RustPromiseAwaiter(const RootWaker& rootWaker, OwnPromiseNode node, kj::SourceLocation location = {});
  ~RustPromiseAwaiter() noexcept(false);

  kj::Maybe<kj::Own<kj::_::Event>> fire() override;
  void traceEvent(kj::_::TraceBuilder& builder) override;

  // Called by Rust.
  bool poll(const RootWaker& cx);

private:
  FuturePollerBase& futurePoller;
  kj::UnwindDetector unwindDetector;
  kj::_::OwnPromiseNode node;
  bool done;
};

using PtrRustPromiseAwaiter = RustPromiseAwaiter*;

void rust_promise_awaiter_new_in_place(PtrRustPromiseAwaiter, const RootWaker&, OwnPromiseNode);
void rust_promise_awaiter_drop_in_place(PtrRustPromiseAwaiter);

// =======================================================================================
// FuturePollerBase

// Base class for the awaitable created by `co_await` when awaiting a Rust Future in a KJ coroutine.
class FuturePollerBase: public kj::_::Event {
public:
  // Initialize `next` with the enclosing coroutine's `Event`.
  FuturePollerBase(
      kj::_::Event& next, kj::_::ExceptionOrValue& resultRef, kj::SourceLocation location = {});

  // When we `poll()` a Future, our RootWaker will either be cloned (creating an ArcWaker
  // promise), or the Future will `.await` some number of KJ promises itself, or both. The awaiter
  // objects which wrap those two kinds of promises, use `beginTrace()` and `endTrace()` to connect
  // the promise they're wrapping to the enclosing coroutine for tracing purposes.
  void beginTrace(OwnPromiseNode& node);
  void endTrace(OwnPromiseNode& node);

  // TODO(now): fulfill()

  // Reject the Future with an exception. Arms the enclosing coroutine's event. The event will
  // resume the coroutine, which will then rethrow the exception from `await_resume()`.
  void reject(kj::Exception exception);

  void traceEvent(kj::_::TraceBuilder& builder) override;

protected:
  virtual kj::Maybe<kj::Own<kj::_::Event>> fire() override = 0;

  kj::_::Event& next;
  kj::_::ExceptionOrValue& resultRef;

  kj::Maybe<OwnPromiseNode&> promiseNodeForTrace;
};

class BoxFutureVoidAwaiter: public FuturePollerBase {
public:
  BoxFutureVoidAwaiter(kj::_::CoroutineBase& coroutine, BoxFutureVoid&& future, kj::SourceLocation location = {})
      : FuturePollerBase(coroutine, result),
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
    } else KJ_IF_SOME(promise, state.cloned) {
      // The future returned Pending and cloned an ArcWaker to notify us later. We'll arrange for
      // the ArcWaker's promise to arm our event once it's fulfilled.
      arcWakerAwaiter.emplace(*this, kj::_::PromiseNode::from(kj::mv(promise)));
    } else {
      // The future returned Pending, did not call `wake_by_ref()` on the RootWaker, and did not
      // clone an ArcWaker. Rust is either awaiting a KJ promise, or the Rust equivalent of
      // kj::NEVER_DONE.
    }

    return false;
  }

  // We already arranged to be scheduled in await_ready(), nothing to do here.
  void await_suspend(kj::_::stdcoro::coroutine_handle<>) {}

  // Unit futures return void.
  void await_resume() {
    KJ_IF_SOME(exception, result.exception) {
      kj::throwFatalException(kj::mv(exception));
    }
  }

  kj::Maybe<kj::Own<kj::_::Event>> fire() override {
    if (await_ready()) {
      // TODO(perf): Call `coroutine.fire()` directly?
      coroutine.armDepthFirst();
    }
    return kj::none;
  }

private:
  kj::_::CoroutineBase& coroutine;
  BoxFutureVoid future;
  kj::_::ExceptionOr<kj::_::Void> result;

  kj::Maybe<ArcWakerAwaiter> arcWakerAwaiter;
};

BoxFutureVoidAwaiter operator co_await(kj::_::CoroutineBase::Await<BoxFutureVoid> await);
BoxFutureVoidAwaiter operator co_await(kj::_::CoroutineBase::Await<BoxFutureVoid&> await);

}  // namespace workerd::rust::async
