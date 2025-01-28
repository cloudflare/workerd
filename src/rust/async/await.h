#pragma once

#include <workerd/rust/async/future.h>
#include <workerd/rust/async/waker.h>
#include <workerd/rust/async/executor-guarded.h>
#include <workerd/rust/async/linked-group.h>

#include <kj/debug.h>

namespace workerd::rust::async {

// =======================================================================================
// ArcWakerAwaiter

// ArcWakerAwaiter is an awaiter intended to await Promises associated with the ArcWaker produced
// when a KjWaker is cloned.
//
// TODO(perf): This is only an Event because we need to handle the case where all the Wakers are
//   dropped and we receive a WakeInstruction::IGNORE. If we could somehow disarm the
//   CrossThreadPromiseFulfillers inside ArcWaker when it's dropped, we could avoid requiring this
//   separate Event, and connect the ArcWaker promise directly to the CoAwaitWaker's Event.
class ArcWakerAwaiter final: public kj::_::Event {
public:
  ArcWakerAwaiter(CoAwaitWaker& coAwaitWaker, OwnPromiseNode node, kj::SourceLocation location = {});
  ~ArcWakerAwaiter() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(ArcWakerAwaiter);

  kj::Maybe<kj::Own<kj::_::Event>> fire() override;
  void traceEvent(kj::_::TraceBuilder& builder) override;

  // Helper for CoAwaitWaker to report what promise it's waiting on.
  void tracePromise(kj::_::TraceBuilder& builder, bool stopAtNextEvent);

private:
  // We need to keep a reference to our CoAwaitWaker so that we can decide whether our
  // `traceEvent()` implementation should forward to its Future poll() Event. Additionally, we need
  // to be able to arm the Future poll() Event when our wrapped OwnPromiseNode becomes ready.
  //
  // Safety: It is safe to store a bare reference to our CoAwaitWaker, because this object
  // (ArcWakerAwaiter) lives inside of CoAwaitWaker, and thus our lifetime is encompassed by
  // the CoAwaitWaker's. Note that if this ever changes in the future, we could switch to
  // making ArcWakerAwaiter a
  CoAwaitWaker& coAwaitWaker;

  kj::UnwindDetector unwindDetector;
  OwnPromiseNode node;
};

// =======================================================================================
// RustPromiseAwaiter

// RustPromiseAwaiter allows Rust `async` blocks to `.await` KJ promises. Rust code creates one in
// the block's storage at the point where the `.await` expression is evaluated, similar to how
// `kj::_::PromiseAwaiter` is created in the KJ coroutine frame when C++ `co_await`s a promise.
//
// To elaborate, RustPromiseAwaiter is part of the IntoFuture trait implementation for the
// OwnPromiseNode class, and `.await` expressions implicitly call `.into_future()`. So,
// RustPromiseAwaiter can be thought of a "Promise-to-Future" adapter. This also means that
// RustPromiseAwaiter can be constructed outside of `.await` expressions, and potentially _not_
// driven to complete readiness. Our implementation must be able to handle this case.
//
// Rust knows how big RustPromiseAwaiter is because we generate a Rust type of equal size and
// alignment using bindgen. See inside await.c++ for a static_assert to remind us to re-run bindgen.
//
// RustPromiseAwaiter has two base classes: KJ Event, and a LinkedObject template
// instantiation. We use the Event to discover when our wrapped Promise is ready. Our Event fire()
// implementation records the fact that we are done, then wakes our Waker or arms the CoAwaitWaker
// Event, if we have one. We access the CoAwaitWaker via our LinkedObject base class mixin. It
// gives us the ability to store a weak reference to the CoAwaitWaker, if we were last polled by
// one's KjWaker.
class RustPromiseAwaiter final: public kj::_::Event,
                                public LinkedObject<CoAwaitWaker, RustPromiseAwaiter> {
public:
  // The Rust code which constructs RustPromiseAwaiter passes us a pointer to a RustWaker, which can
  // be thought of as a Rust-native component RustPromiseAwaiter. Its job is to hold a clone of
  // of any non-KJ Waker that we are polled with, and forward calls to `wake()`. Ideally, we could
  // store the clone of the Waker ourselves (it's just two pointers) on the C++ side, so the
  // lifetime safety is more obvious. But, storing a reference works for now.
  RustPromiseAwaiter(RustWaker& rustWaker, OwnPromiseNode node, kj::SourceLocation location = {});
  ~RustPromiseAwaiter() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(RustPromiseAwaiter);

  // -------------------------------------------------------
  // kj::_::Event API

  kj::Maybe<kj::Own<kj::_::Event>> fire() override;
  void traceEvent(kj::_::TraceBuilder& builder) override;

  // Helpers for CoAwaitWaker to report what promise it's waiting on.
  void tracePromise(kj::_::TraceBuilder& builder, bool stopAtNextEvent);

  // -------------------------------------------------------
  // poll() API exposed to Rust code
  //
  // Additionally, see GuardedRustPromiseAwaiter below, which mediates access to this API.

  // Poll this Promise for readiness using a KjWaker.
  //
  // If we are polled by a Future being driven by a KJ coroutine's `co_await` expression, then we
  // have an optimization opportunity: when our wrapped Promise becomes ready, we can arm the
  // `co_await` expression's `CoAwaitWaker` directly to tell it to poll us again. This allows
  // the Rust call site of `poll()` to avoid having to clone its Waker, which can be high overhead.
  //
  // Preconditions:
  // - `waker.is_current()` is true: the KjWaker is associated with the event loop running on the
  //   current thread.
  // - `(*rustWaker).is_none()` is true.
  bool poll_with_co_await_waker(const CoAwaitWaker& waker);

  // Poll this Promise for readiness using a clone of a Waker stored on the Rust side of the FFI
  // boundary.
  //
  // Preconditions: `(*rustWaker).is_some()` is true.
  bool poll();

private:
  // Private API to set or query done-ness.
  void setDone();
  bool isDone() const;

  // The Rust code which instantiates RustPromiseAwaiter does so with a RustWaker object right next
  // to the RustPromiseAwaiter, such that its lifetime encompasses RustPromiseAwaiter's. Thus, our
  // reference to our RustWaker is stable. We use the RustWaker to wake our enclosing Future if we
  // were last polled with a non-KjWaker. The Rust code which calls our `poll*()` functions stores a
  // clone of the actual `std::task::Waker` inside the RustWaker before calling poll(), and clears
  // the RustWaker before calling `poll_with_co_await_waker()`.
  //
  // When we wake our enclosing Future, either with CoAwaitWaker or with RustWaker, we nullify
  // this Maybe. Therefore, this Maybe being kj::none means our OwnPromiseNode is ready, and it is
  // safe to call `node->get()` on it.
  kj::Maybe<RustWaker&> rustWaker;

  kj::UnwindDetector unwindDetector;
  OwnPromiseNode node;
};

// We force Rust to call our `poll()` overloads using this ExecutorGuarded wrapper around the actual
// RustPromiseAwaiter class. This allows us to assume all calls that reach RustPromiseAwaiter itself
// are on the correct thread.
struct GuardedRustPromiseAwaiter: ExecutorGuarded<RustPromiseAwaiter> {
  // We need to inherit constructors or else placement-new will try to aggregate-initialize us.
  using ExecutorGuarded<RustPromiseAwaiter>::ExecutorGuarded;

  bool poll_with_co_await_waker(const CoAwaitWaker& waker) {
    return get().poll_with_co_await_waker(waker);
  }
  bool poll() {
    return get().poll();
  }
};

using PtrGuardedRustPromiseAwaiter = GuardedRustPromiseAwaiter*;

void guarded_rust_promise_awaiter_new_in_place(
    PtrGuardedRustPromiseAwaiter, RustWaker*, OwnPromiseNode);
void guarded_rust_promise_awaiter_drop_in_place(PtrGuardedRustPromiseAwaiter);

// =======================================================================================
// CoAwaitWaker

// A CxxWaker implementation which provides an optimized path for awaiting KJ Promises in Rust. It
// consists of a KjWaker, an Event reference, and a set of "sub-Promise awaiters".
//
// The Event in question is responsible for calling `Future::poll()`, elsewhere I call it "the
// Future poll() Event". It owns this CoAwaitWaker in an object lifetime sense.
//
// The sub-Promise awaiters comprise an optional ArcWakerAwaiter and a list of zero or more
// RustPromiseAwaiters. These sub-Promise awaiters all wrap a KJ Promise of some sort, and arrange
// to arm the Future poll() Event when their Promises become ready.
//
// The PromiseNode base class is a hack to implement async tracing. That is, we only implement the
// `tracePromise()` function, and decide which Promise to trace into if/when the coroutine calls our
// `tracePromise()` implementation. This primarily makes the lifetimes easier to manage: our
// RustPromiseAwaiter LinkedObjects have independent lifetimes from the CoAwaitWaker, so we mustn't
// leave references to them, or their members, lying around in the Coroutine class.
class CoAwaitWaker: public CxxWaker,
                    public kj::_::PromiseNode,
                    public LinkedGroup<CoAwaitWaker, RustPromiseAwaiter> {
public:
  CoAwaitWaker(kj::_::Event& futurePoller);

  // After constructing a CoAwaitWaker, pass it by reference to `BoxFuture<T>::poll()`. If `poll()`
  // returns Pending, call this `suspend()` function to arrange to arm the Future poll() Event when
  // we are woken.
  //
  // TODO(cleanup): Make a RAII PollScope instead? Call this from the Rust side?
  void suspend();

  // The Event which is using this CoAwaitWaker to poll() a Future. Waking the CoAwaitWaker
  // arms this Event (possibly via a cross-thread promise fulfiller). We also arm the Event directly
  // in the RustPromiseAwaiter class, to more optimally await KJ Promises from within Rust.
  kj::_::Event& getFuturePollEvent();

  // True if our `tracePromise()` implementation would choose the given awaiter's promise for
  // tracing. If our wrapped Future is awaiting multiple other Promises and/or Futures, our
  // `tracePromise()` implementation might choose a different branch to go down.
  bool wouldTrace(kj::Badge<ArcWakerAwaiter>, ArcWakerAwaiter& awaiter);
  bool wouldTrace(kj::Badge<RustPromiseAwaiter>, RustPromiseAwaiter& awaiter);

  // TODO(now): Propagate value-or-exception.

  // -------------------------------------------------------
  // API exposed to Rust

  // True if the current thread's kj::Executor is the same as the one that was active when this
  // CoAwaitWaker was constructed. This allows Rust to optimize Promise `.await`s.
  bool is_current() const;

  // -------------------------------------------------------
  // CxxWaker API
  //
  // Our CxxWaker implementation just forwards everything to our KjWaker member.

  const CxxWaker* clone() const override;
  void wake() const override;
  void wake_by_ref() const override;
  void drop() const override;

  // -------------------------------------------------------
  // PromiseNode API
  //
  // HACK: We only implement this interface for `tracePromise()`, which is the only function
  // CoroutineBase uses on its `promiseNodeForTrace` reference.

  void destroy() override {}  // No-op because we are allocated inside the coroutine frame
  void onReady(kj::_::Event* event) noexcept override;
  void get(kj::_::ExceptionOrValue& output) noexcept override;
  void tracePromise(kj::_::TraceBuilder& builder, bool stopAtNextEvent) override;

private:
  kj::_::Event& futurePoller;

  // This KjWaker is our actual implementation of the CxxWaker interface. We forward all calls here.
  KjWaker kjWaker;

  // TODO(now): Can this be moved into KjWaker?
  kj::Maybe<ArcWakerAwaiter> arcWakerAwaiter;
};

// =======================================================================================
// BoxFutureAwaiter, LazyBoxFutureAwaiter, and operator co_await implementations

// BoxFutureAwaiter<T> is a Future poll() Event, and is the inner implementation of our co_await
// syntax. It wraps a BoxFuture<T> and captures a reference to its enclosing KJ coroutine, arranging
// to continuously call `BoxFuture<T>::poll()` on the KJ event loop until the Future produces a
// result, after which it arms the enclosing KJ coroutine's Event.
template <typename T>
class BoxFutureAwaiter final: public kj::_::Event {
public:
  BoxFutureAwaiter(
      kj::_::CoroutineBase& coroutine,
      BoxFuture<T> future,
      kj::SourceLocation location = {})
      : Event(location),
        coroutine(coroutine),
        coAwaitWaker(*this),
        future(kj::mv(future)) {}
  ~BoxFutureAwaiter() noexcept(false) {
    coroutine.clearPromiseNodeForTrace();
  }
  KJ_DISALLOW_COPY_AND_MOVE(BoxFutureAwaiter);

  // Poll the wrapped Future, returning false if we should _not_ suspend, true if we should suspend.
  bool awaitSuspendImpl() {
    // TODO(perf): Check if we already have an ArcWaker from a previous suspension and give it to
    //   KjWaker for cloning if we have the last reference to it at this point. This could save
    //   memory allocations, but would depend on making XThreadFulfiller and XThreadPaf resettable
    //   to really benefit.

    if (future.poll(coAwaitWaker)) {
      // Future is ready, we're done.
      // TODO(now): Propagate value-or-exception.
      return false;
    }

    coAwaitWaker.suspend();

    // Integrate with our enclosing coroutine's tracing.
    coroutine.setPromiseNodeForTrace(promiseNodeForTrace);

    return true;
  }

  void awaitResumeImpl() {
    coroutine.clearPromiseNodeForTrace();
  }

  // -------------------------------------------------------
  // Event API

  void traceEvent(kj::_::TraceBuilder& builder) override {
    // Just defer to our enclosing Coroutine. It will immediately call our CoAwaitWaker's
    // `tracePromise()` implementation.
    static_cast<Event&>(coroutine).traceEvent(builder);
  }

private:
  kj::Maybe<kj::Own<kj::_::Event>> fire() override {
    if (!awaitSuspendImpl()) {
      coroutine.armDepthFirst();
    }
    return kj::none;
  }

  kj::_::CoroutineBase& coroutine;
  CoAwaitWaker coAwaitWaker;
  // HACK: CoAwaitWaker implements the PromiseNode interface to integrate with the Coroutine class'
  // current tracing implementation.
  OwnPromiseNode promiseNodeForTrace { &coAwaitWaker };

  BoxFuture<T> future;
};

// LazyBoxFutureAwaiter<T> is the outer implementation of our co_await syntax, providing the
// await_ready(), await_suspend(), await_resume() facade expected by the compiler.
//
// LazyBoxFutureAwaiter is a type with two stages. At first, it merely wraps a BoxFuture<T>. Once
// its await_suspend() function is called, it transitions to wrap a BoxFutureAwaiter<T>, our inner
// awaiter implementation. We do this because we don't get a reference to our enclosing
// coroutine until await_suspend() is called, and our awaiter implementation is greatly simplified
// if we can avoid using a Maybe. So, we defer the real awaiter instantiation to await_suspend().
template <typename T>
class LazyBoxFutureAwaiter {
public:
  LazyBoxFutureAwaiter(BoxFuture<T>&& future): impl(kj::mv(future)) {}

  // Always return false, so our await_suspend() is guaranteed to be called.
  bool await_ready() const { return false; }

  // Initialize our wrapped Awaiter and forward to `BoxFutureAwaiter<T>::awaitSuspendImpl()`.
  template <typename U> requires (kj::canConvert<U&, kj::_::CoroutineBase&>())
  bool await_suspend(kj::_::stdcoro::coroutine_handle<U> handle) {
    auto future = kj::mv(KJ_ASSERT_NONNULL(impl.template tryGet<BoxFuture<T>>()));
    return impl.template init<BoxFutureAwaiter<T>>(handle.promise(), kj::mv(future))
        .awaitSuspendImpl();
  }

  // Forward to our wrapped `BoxFutureAwaiter<T>::awaitResumeImpl()`.
  void await_resume() {
    return KJ_ASSERT_NONNULL(impl.template tryGet<BoxFutureAwaiter<T>>()).awaitResumeImpl();
  }

private:
  kj::OneOf<BoxFuture<T>, BoxFutureAwaiter<T>> impl;
};

template <typename T>
LazyBoxFutureAwaiter<T> operator co_await(BoxFuture<T> future) {
  return kj::mv(future);
}
template <typename T>
LazyBoxFutureAwaiter<T> operator co_await(BoxFuture<T>& future) {
  return kj::mv(future);
}

}  // namespace workerd::rust::async
