#pragma once

#include <workerd/rust/async/future.h>
#include <workerd/rust/async/waker.h>
#include <workerd/rust/async/executor-guarded.h>
#include <workerd/rust/async/linked-group.h>

namespace workerd::rust::async {

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
  // We need to keep a reference to our CoAwaitWaker so that we can arm its Event when our
  // wrapped OwnPromiseNode becomes ready.
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

// Base class for the awaitable created by `co_await` when awaiting a Rust Future in a KJ coroutine.
//
// The PromiseNode base class is a hack to implement async tracing. That is, we only implement the
// `tracePromise()` function, and, instead of calling `coroutine.awaitBegin(p)` with the Promise `p`
// that we ultimately suspend on (either a RustPromiseAwaiter's, or an ArcWakerAwaiter's), we call
// `coroutine.awaitBegin(*this)`, and decide which Promise to trace into if/when the coroutine calls
// our `tracePromise()` implementation. This primarily makes the lifetimes easier to manage: our
// RustPromiseAwaiter LinkedObjects have independent lifetimes from the CoAwaitWaker, so we mustn't
// leave references to them, or their members, lying around in the Coroutine class.
class CoAwaitWaker: public CxxWaker,
                    public kj::_::Event,
                    public kj::_::PromiseNode,
                    public LinkedGroup<CoAwaitWaker, RustPromiseAwaiter> {
public:
  // Initialize `next` with the enclosing coroutine's `Event`.
  CoAwaitWaker(
      kj::_::ExceptionOrValue& resultRef,
      kj::SourceLocation location = {});
  ~CoAwaitWaker() noexcept(false);

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
  // Event API

  void traceEvent(kj::_::TraceBuilder& builder) override;
  // fire() implemented in derived class

  // -------------------------------------------------------
  // PromiseNode API
  //
  // We only implement this for `tracePromise()`, so we can give our CoroutineBase an API to trace
  // the promise we're currently waiting on.

  void destroy() override {}  // No-op because we are allocated inside the coroutine frame
  void onReady(kj::_::Event* event) noexcept override;
  void get(kj::_::ExceptionOrValue& output) noexcept override;
  void tracePromise(kj::_::TraceBuilder& builder, bool stopAtNextEvent) override;

  // -------------------------------------------------------
  // Other stuff

  // True if `CoAwaitWaker::traceEvent()` would immediately call
  // `awaiter.tracePromise()`
  bool wouldTrace(kj::Badge<ArcWakerAwaiter>, ArcWakerAwaiter& awaiter);
  bool wouldTrace(kj::Badge<RustPromiseAwaiter>, RustPromiseAwaiter& awaiter);

  // TODO(now): Propagate value-or-exception.

  // Reject the Future with an exception. Arms the enclosing coroutine's event. The event will
  // resume the coroutine, which will then rethrow the exception from `await_resume()`. This is not
  // an expected code path, and indicates a bug.
  void internalReject(kj::Badge<ArcWakerAwaiter>, kj::Exception exception);

protected:
  // API for derived class.
  void awaitBegin();
  void awaitEnd();
  void scheduleResumption();  // TODO(now): Rename to fulfill()?

  // Called from `await_suspend()`, which is the earliest we get access to the coroutine handle.
  void setCoroutine(kj::_::CoroutineBase& coroutine);

private:
  // Helper to access `maybeCoroutine`, which is effectively always non-none.
  kj::_::CoroutineBase& getCoroutine();

  // The enclosing coroutine, which we will arm once our wrapped Future returns Ready, or an
  // internal error occurs.
  //
  // This member is a Maybe because we don't have access to the coroutine until `await_suspend()` is
  // called, which initializes this member by calling `setCoroutine()`. Since our derived classes'
  // `await_ready()` implementations do nothing but immediately return false, we can assume that
  // this Maybe is non-none effectively everywhere in the implementation of this class.
  kj::Maybe<kj::_::CoroutineBase&> maybeCoroutine;

  // This KjWaker is our actual implementation of the CxxWaker interface. We forward all calls here.
  KjWaker kjWaker;

  // HACK: We implement the PromiseNode interface to integrate with the Coroutine class' current
  // tracing implementation.
  OwnPromiseNode self { this };

  // Reference to a member of our derived class. We use this only to reject the `co_await` with an
  // exception if an internal error occurs. What is an internal error? Any condition which causes
  // `internalReject()` to be called. :)
  //
  // The referee is of course uninitialized in our constructor and destructor.
  kj::_::ExceptionOrValue& resultRef;

  kj::Maybe<ArcWakerAwaiter> arcWakerAwaiter;
};

class BoxFutureVoidAwaiter: public CoAwaitWaker {
public:
  BoxFutureVoidAwaiter(BoxFutureVoid&& future, kj::SourceLocation location = {})
      : CoAwaitWaker(result),
        future(kj::mv(future)) {}

  bool await_ready() const {
    return false;
  }

  template <typename T> requires (kj::canConvert<T&, kj::_::CoroutineBase&>())
  bool await_suspend(kj::_::stdcoro::coroutine_handle<T> handle) {
    setCoroutine(handle.promise());
    return awaitSuspendImpl();
  }

  // Unit futures return void.
  void await_resume() {
    awaitEnd();

    KJ_IF_SOME(exception, result.exception) {
      kj::throwFatalException(kj::mv(exception));
    }
  }

  kj::Maybe<kj::Own<kj::_::Event>> fire() override {
    if (!awaitSuspendImpl()) {
      scheduleResumption();
    }
    return kj::none;
  }

private:
  // Poll the wrapped Future, returning false if we should _not_ suspend, true if we should suspend.
  bool awaitSuspendImpl() {
    // TODO(perf): Check if we already have an ArcWaker from a previous suspension and give it to
    //   KjWaker for cloning if we have the last reference to it at this point. This could save
    //   memory allocations, but would depend on making XThreadFulfiller and XThreadPaf resettable
    //   to really benefit.

    if (future.poll(*this)) {
      // Future is ready, we're done.
      // TODO(now): Propagate value-or-exception.
      return false;
    }

    awaitBegin();

    return true;
  }

  BoxFutureVoid future;
  kj::_::ExceptionOr<kj::_::Void> result;
};

BoxFutureVoidAwaiter operator co_await(BoxFutureVoid future);
BoxFutureVoidAwaiter operator co_await(BoxFutureVoid& future);

}  // namespace workerd::rust::async
