#pragma once

#include <workerd/rust/async/future.h>
#include <workerd/rust/async/waker.h>
#include <workerd/rust/async/executor-guarded.h>
#include <workerd/rust/async/linked-group.h>

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
  ArcWakerAwaiter(FutureAwaiterBase& futureAwaiter, OwnPromiseNode node, kj::SourceLocation location = {});
  ~ArcWakerAwaiter() noexcept(false);

  kj::Maybe<kj::Own<kj::_::Event>> fire() override;
  void traceEvent(kj::_::TraceBuilder& builder) override;

  // Helper for FutureAwaiter to report what promise it's waiting on.
  void tracePromise(kj::_::TraceBuilder& builder);

private:
  FutureAwaiterBase& futureAwaiter;
  kj::UnwindDetector unwindDetector;
  kj::_::OwnPromiseNode node;
};

// =======================================================================================
// RustPromiseAwaiter

// RustPromiseAwaiter allows Rust `async` blocks to `.await` KJ promises. Rust code creates one in
// the block's storage at the point where the `.await` expression is evaluated, similar to how
// `kj::_::PromiseAwaiter` is created in the KJ coroutine frame when C++ `co_await`s a promise.
//
// Rust knows how big RustPromiseAwaiter is because we generate a Rust type of equal size and
// alignment using bindgen. See inside await.c++ for a static_assert to remind us to re-run bindgen.
class RustPromiseAwaiter final: public kj::_::Event,
                                public LinkedObject<FutureAwaiterBase, RustPromiseAwaiter> {
public:
  RustPromiseAwaiter(OwnPromiseNode node, kj::SourceLocation location = {});
  ~RustPromiseAwaiter() noexcept(false);

  kj::Maybe<kj::Own<kj::_::Event>> fire() override;
  void traceEvent(kj::_::TraceBuilder& builder) override;

  // Helper for FutureAwaiter to report what promise it's waiting on.
  void tracePromise(kj::_::TraceBuilder& builder);

  // Called by Rust.
  bool poll_with_kj_waker(const KjWaker& waker);
  bool poll(const RustWaker* waker);

private:
  // If we are polled by a Future being driven by a KJ coroutine's `co_await` expression, then we
  // have an optimization opportunity: when our wrapped Promise becomes ready, we can arm the
  // `co_await` expression's `FutureAwaiterBase` directly to tell it to poll us again. This allows
  // us to avoid cloning the Waker which our `poll()` caller passed to us, which is a higher
  // overhead path.
  //
  // If we are polled, directly or indirectly, by a KJ coroutine's `co_await` expression, and that
  // `co_await` expression completes before the promise we are waiting for here is ready, the
  // temporary FutureAwaiter object created by the `co_await` expression needs to be able to erase
  // our reference to it, or else our reference will become dangling.
  friend class FutureAwaiterBase;

  kj::Maybe<const RustWaker&> rustWaker;

  kj::UnwindDetector unwindDetector;
  OwnPromiseNode node;

  // TODO(perf): Set `rustWaker` in constructor and communicate done-ness by nullifying it, which
  //   saves one bool.
  bool done;
};

struct GuardedRustPromiseAwaiter: ExecutorGuarded<RustPromiseAwaiter> {
  bool poll_with_kj_waker(const KjWaker& waker) {
    return get().poll_with_kj_waker(waker);
  }
  bool poll(const RustWaker* waker) {
    return get().poll(waker);
  }
};
using PtrGuardedRustPromiseAwaiter = GuardedRustPromiseAwaiter*;

void guarded_rust_promise_awaiter_new_in_place(PtrGuardedRustPromiseAwaiter, OwnPromiseNode);
void guarded_rust_promise_awaiter_drop_in_place(PtrGuardedRustPromiseAwaiter);

// =======================================================================================
// FutureAwaiterBase

// Base class for the awaitable created by `co_await` when awaiting a Rust Future in a KJ coroutine.
class FutureAwaiterBase: public kj::_::Event,
                         public LinkedGroup<FutureAwaiterBase, RustPromiseAwaiter> {
public:
  // Initialize `next` with the enclosing coroutine's `Event`.
  FutureAwaiterBase(
      kj::_::Event& next, kj::_::ExceptionOrValue& resultRef, kj::SourceLocation location = {});

  // TODO(now): fulfill()

  // Reject the Future with an exception. Arms the enclosing coroutine's event. The event will
  // resume the coroutine, which will then rethrow the exception from `await_resume()`. This is not
  // an expected code path, and indicates a bug.
  void internalReject(kj::Exception exception);

  void traceEvent(kj::_::TraceBuilder& builder) override;

protected:
  virtual kj::Maybe<kj::Own<kj::_::Event>> fire() override = 0;

  // The enclosing coroutine event, which we will arm once our wrapped Future returns Ready, or an
  // internal error occurs.
  kj::_::Event& next;

  // Reference to a member of our derived class. We use this only to reject the `co_await` with an
  // exception if an internal error occurs. What is an internal error? Any condition which causes
  // `internalReject()` to be called. :)
  //
  // The referee is of course uninitialized in our constructor and destructor.
  kj::_::ExceptionOrValue& resultRef;

  kj::Maybe<ArcWakerAwaiter> arcWakerAwaiter;
};

class BoxFutureVoidAwaiter: public FutureAwaiterBase {
public:
  BoxFutureVoidAwaiter(kj::_::CoroutineBase& coroutine, BoxFutureVoid&& future, kj::SourceLocation location = {})
      : FutureAwaiterBase(coroutine, result),
        coroutine(coroutine),
        future(kj::mv(future)) {}
  ~BoxFutureVoidAwaiter() noexcept(false) {
    // TODO(now): Where is the awaitBegin()? I seem to have lost it.
    coroutine.awaitEnd();
  }

  bool await_ready() {
    // TODO(perf): Check if we already have an ArcWaker from a previous suspension and give it to
    //   KjWaker for cloning if we have the last reference to it at this point. This could save
    //   memory allocations, but would depend on making XThreadFulfiller and XThreadPaf resettable
    //   to really benefit.

    if (future.poll(waker)) {
      // Future is ready, we're done.
      // TODO(now): Propagate value-or-exception.
      return true;
    }

    auto state = waker.reset();

    if (state.wakeCount > 0) {
      // The future returned Pending, but synchronously called `wake_by_ref()` on the KjWaker,
      // indicating it wants to immediately be polled again. We should arm our event right now,
      // which will call `await_ready()` again on the event loop.
      armDepthFirst();
    } else KJ_IF_SOME(promise, state.cloned) {
      // The future returned Pending and cloned an ArcWaker to notify us later. We'll arrange for
      // the ArcWaker's promise to arm our event once it's fulfilled.
      arcWakerAwaiter.emplace(*this, kj::_::PromiseNode::from(kj::mv(promise)));
    } else {
      // The future returned Pending, did not call `wake_by_ref()` on the KjWaker, and did not
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
  KjWaker waker { *this };
  BoxFutureVoid future;
  kj::_::ExceptionOr<kj::_::Void> result;
};

BoxFutureVoidAwaiter operator co_await(kj::_::CoroutineBase::Await<BoxFutureVoid> await);
BoxFutureVoidAwaiter operator co_await(kj::_::CoroutineBase::Await<BoxFutureVoid&> await);

}  // namespace workerd::rust::async
