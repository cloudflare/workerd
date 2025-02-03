#pragma once

#include <workerd/rust/async/future.h>
#include <workerd/rust/async/waker.h>
#include <workerd/rust/async/executor-guarded.h>
#include <workerd/rust/async/linked-group.h>

#include <kj/debug.h>

namespace workerd::rust::async {

// =======================================================================================
// Opaque Rust types
//
// The following types are defined in lib.rs, and thus in lib.rs.h. lib.rs.h depends on our C++
// headers, including awaiter.h (the file you're currently reading), so we forward-declare some types
// here for use in the C++ headers.

// Wrapper around an `&std::task::Waker`, passed to `RustPromiseAwaiter::poll()`. This indirection
// is required because cxx-rs does not permit us to expose opaque Rust types to C++ defined outside
// of our own crate, like `std::task::Waker`.
struct WakerRef;

// Wrapper around an `Option<std::task::Waker>`. RustPromiseAwaiter calls `set()` with the WakerRef
// passed to `poll()` if RustPromiseAwaiter is unable to find an optimized path for awaiting its
// Promise. Later on, when its Promise becomes ready, RustPromiseAwaiter will use OptionWaker to
// call wake the wrapped Waker.
//
// Otherwise, if RustPromiseAwaiter finds an optimized path for awaiting its Promise, it calls
// `set_none()` on the OptionWaker to ensure it's empty.
struct OptionWaker;

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
// alignment using bindgen. See inside awaiter.c++ for a static_assert to remind us to re-run
// bindgen.
//
// RustPromiseAwaiter has two base classes: KJ Event, and a LinkedObject template instantiation. We
// use the Event to discover when our wrapped Promise is ready. Our Event fire() implementation
// records the fact that we are done, then wakes our Waker or arms the FuturePollEvent, if we
// have one. We access the FuturePollEvent via our LinkedObject base class mixin. It gives us the
// ability to store a weak reference to the FuturePollEvent, if we were last polled by one.
class RustPromiseAwaiter final: public kj::_::Event,
                                public LinkedObject<FuturePollEvent, RustPromiseAwaiter> {
public:
  // The Rust code which constructs RustPromiseAwaiter passes us a pointer to a OptionWaker, which can
  // be thought of as a Rust-native component RustPromiseAwaiter. Its job is to hold a clone of
  // of any non-KJ Waker that we are polled with, and forward calls to `wake()`. Ideally, we could
  // store the clone of the Waker ourselves (it's just two pointers) on the C++ side, so the
  // lifetime safety is more obvious. But, storing a reference works for now.
  RustPromiseAwaiter(OptionWaker& optionWaker, OwnPromiseNode node, kj::SourceLocation location = {});
  ~RustPromiseAwaiter() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(RustPromiseAwaiter);

  // -------------------------------------------------------
  // kj::_::Event API

  kj::Maybe<kj::Own<kj::_::Event>> fire() override;
  void traceEvent(kj::_::TraceBuilder& builder) override;

  // Helper for FuturePollEvent to report what promise it's waiting on.
  void tracePromise(kj::_::TraceBuilder& builder, bool stopAtNextEvent);

  // -------------------------------------------------------
  // API exposed to Rust code
  //
  // Additionally, see GuardedRustPromiseAwaiter below, which mediates access to this API.

  // Poll this Promise for readiness.
  //
  // If the Waker is a KjWaker, you may pass the KjWaker pointer as a second parameter. This may
  // allow the implementation of `poll()` to optimize the wake by arming a KJ Event directly when
  // the wrapped Promise becomes ready.
  //
  // If the Waker is not a KjWaker, the `maybeKjWaker` pointer argument must be nullptr.
  bool poll(const WakerRef& waker, const KjWaker* maybeKjWaker);

  // Release ownership of the OwnPromiseNode. Asserts if called before the Promise is ready; that
  // is, `poll()` must have returned true prior to calling `take_own_promise_node()`.
  OwnPromiseNode take_own_promise_node();

private:
  // The Rust code which instantiates RustPromiseAwaiter does so with a OptionWaker object right
  // next to the RustPromiseAwaiter, such that it is dropped after RustPromiseAwaiter. Thus, our
  // reference to our OptionWaker is stable. We use the OptionWaker to (optionally) store a clone of
  // the Waker with which we were last polled.
  //
  // When we wake our enclosing Future, either with the FuturePollEvent or with OptionWaker, we
  // nullify this Maybe. Therefore, this Maybe being kj::none means our OwnPromiseNode is ready, and
  // it is safe to call `node->get()` on it.
  kj::Maybe<OptionWaker&> maybeOptionWaker;

  kj::UnwindDetector unwindDetector;
  OwnPromiseNode node;
};

// We force Rust to call our `poll()` overloads using this ExecutorGuarded wrapper around the actual
// RustPromiseAwaiter class. This allows us to assume all calls that reach RustPromiseAwaiter itself
// are on the correct thread.
struct GuardedRustPromiseAwaiter: ExecutorGuarded<RustPromiseAwaiter> {
  // We need to inherit constructors or else placement-new will try to aggregate-initialize us.
  using ExecutorGuarded<RustPromiseAwaiter>::ExecutorGuarded;

  bool poll(const WakerRef& waker, const KjWaker* maybeKjWaker) {
    return get().poll(waker, maybeKjWaker);
  }
  OwnPromiseNode take_own_promise_node() {
    return get().take_own_promise_node();
  }
};

using PtrGuardedRustPromiseAwaiter = GuardedRustPromiseAwaiter*;

void guarded_rust_promise_awaiter_new_in_place(
    PtrGuardedRustPromiseAwaiter, OptionWaker*, OwnPromiseNode);
void guarded_rust_promise_awaiter_drop_in_place(PtrGuardedRustPromiseAwaiter);

// =======================================================================================
// FuturePollEvent

// Base class for `BoxFutureAwaiter<T>`. `BoxFutureAwaiter<T>` implements the type-specific
// `Event::fire()` override which actually polls the `BoxFuture<T>`; this class implements all other
// base class virtual functions.
//
// A FuturePollEvent contains an optional ArcWakerPromiseAwaiter and a list of zero or more
// RustPromiseAwaiters. These "sub-Promise awaiters" all wrap a KJ Promise of some sort, and arrange
// to arm the FuturePollEvent when their Promises become ready.
//
// The PromiseNode base class is a hack to implement async tracing. That is, we only implement the
// `tracePromise()` function, and decide which Promise to trace into if/when the coroutine calls our
// `tracePromise()` implementation. This primarily makes the lifetimes easier to manage: our
// RustPromiseAwaiter LinkedObjects have independent lifetimes from the FuturePollEvent, so we
// mustn't leave references to them, or their members, lying around in the Coroutine class.
class FuturePollEvent: public kj::_::Event,
                       public kj::_::PromiseNode,
                       public LinkedGroup<FuturePollEvent, RustPromiseAwaiter> {
public:
  FuturePollEvent(kj::SourceLocation location = {}): Event(location) {}

  // -------------------------------------------------------
  // PromiseNode API
  //
  // HACK: We only implement this interface for `tracePromise()`, which is the only function
  // CoroutineBase uses on its `promiseNodeForTrace` reference.

  void destroy() override {}  // No-op because we are allocated inside the coroutine frame
  void onReady(kj::_::Event* event) noexcept override;
  void get(kj::_::ExceptionOrValue& output) noexcept override;
  void tracePromise(kj::_::TraceBuilder& builder, bool stopAtNextEvent) override;

protected:
  // PollScope is a LazyArcWaker which is associated with a specific FuturePollEvent, allowing
  // optimized Promise `.await`s. Additionally, PollScope's destructor arranges to await any
  // ArcWaker promise which was lazily created.
  //
  // Used by BoxFutureAwaiter<T>, our derived class.
  class PollScope;

private:
  // Private API for PollScope.
  void enterPollScope() noexcept;
  void exitPollScope(kj::Maybe<kj::Promise<void>> maybeLazyArcWakerPromise);

  kj::Maybe<OwnPromiseNode> arcWakerPromise;
};

class FuturePollEvent::PollScope: public LazyArcWaker {
public:
  // `futurePollEvent` is the FuturePollEvent responsible for calling `Future::poll()`, and must
  // outlive this PollScope.
  PollScope(FuturePollEvent& futurePollEvent);
  ~PollScope() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(PollScope);

  // The Event which is using this PollScope to poll() a Future. Waking this FuturePollEvent's
  // PollScope arms this Event (possibly via a cross-thread promise fulfiller). We also arm the
  // Event directly in the RustPromiseAwaiter class, to more optimally `.await` KJ Promises from
  // within Rust. If the current thread's kj::Executor is not the same as the one which owns the
  // FuturePollEvent, this function returns kj::none.
  kj::Maybe<FuturePollEvent&> tryGetFuturePollEvent() const override;

private:
  struct FuturePollEventHolder {
    FuturePollEvent& futurePollEvent;
  };
  ExecutorGuarded<FuturePollEventHolder> holder;
};

// =======================================================================================
// BoxFutureAwaiter, LazyBoxFutureAwaiter, and operator co_await implementations

// BoxFutureAwaiter<T> is a Future poll() Event, and is the inner implementation of our co_await
// syntax. It wraps a BoxFuture<T> and captures a reference to its enclosing KJ coroutine, arranging
// to continuously call `BoxFuture<T>::poll()` on the KJ event loop until the Future produces a
// result, after which it arms the enclosing KJ coroutine's Event.
template <typename T>
class BoxFutureAwaiter final: public FuturePollEvent {
public:
  BoxFutureAwaiter(
      kj::_::CoroutineBase& coroutine,
      BoxFuture<T> future,
      kj::SourceLocation location = {})
      : FuturePollEvent(location),
        coroutine(coroutine),
        future(kj::mv(future)) {}
  ~BoxFutureAwaiter() noexcept(false) {
    coroutine.clearPromiseNodeForTrace();
  }
  KJ_DISALLOW_COPY_AND_MOVE(BoxFutureAwaiter);

  // Poll the wrapped Future, returning false if we should _not_ suspend, true if we should suspend.
  bool awaitSuspendImpl() {
    // TODO(perf): Check if we already have an ArcWaker from a previous suspension and give it to
    //   LazyArcWaker for cloning if we have the last reference to it at this point. This could save
    //   memory allocations, but would depend on making XThreadFulfiller and XThreadPaf resettable
    //   to really benefit.

    {
      PollScope pollScope(*this);

      if (future.poll(pollScope, result)) {
        // Future is ready, we're done.
        return false;
      }
    }

    // Integrate with our enclosing coroutine's tracing.
    coroutine.setPromiseNodeForTrace(promiseNodeForTrace);

    return true;
  }

  RemoveFallible<T> awaitResumeImpl() {
    coroutine.clearPromiseNodeForTrace();
    return kj::_::convertToReturn(kj::mv(result));
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
  // HACK: FuturePollEvent implements the PromiseNode interface to integrate with the Coroutine
  // class' current tracing implementation.
  OwnPromiseNode promiseNodeForTrace { this };
  kj::_::ExceptionOr<kj::_::FixVoid<RemoveFallible<T>>> result;
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
  RemoveFallible<T> await_resume() {
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
