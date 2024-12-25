#pragma once

#include <workerd/rust/async/promise.h>

#include <kj/async.h>
#include <kj/mutex.h>
#include <kj/one-of.h>
#include <kj/refcount.h>

namespace workerd::rust::async {

// =======================================================================================
// CxxWaker

// CxxWaker is an abstract base class which defines an interface mirroring Rust's RawWakerVTable
// struct. Rust has four trampoline functions, defined in waker.rs, which translate Waker::clone(),
// Waker::wake(), etc. calls to the virtual member functions on this class.
//
// Rust requires Wakers to be Send and Sync, meaning all of the functions defined here may be called
// concurrently by any thread. Derived class implementations of these functions must handle this,
// which is why all of the virtual member functions are `const`-qualified.
class CxxWaker {
public:
  // Return a pointer to a new strong ref to a CxxWaker. Note that `clone()` may return nullptr,
  // in which case the Rust implementation in waker.rs will treat it as a no-op Waker. Rust
  // immediately wraps this pointer in its own Waker object, which is responsible for later
  // releasing the strong reference.
  //
  // TODO(cleanup): Build kj::Arc<T> into cxx-rs so we can return one instead of a raw pointer.
  virtual const CxxWaker* clone() const = 0;

  // Wake and drop this waker.
  virtual void wake() const = 0;

  // Wake this waker, but do not drop it.
  virtual void wake_by_ref() const = 0;

  // Drop this waker.
  virtual void drop() const = 0;
};

// =======================================================================================
// ArcWaker

// The result type for ArcWaker's Promise.
enum class WakeInstruction {
  // The `IGNORE` instruction means the Waker was dropped without ever being used.
  IGNORE,
  // The `WAKE` instruction means `wake()` was called on the Waker.
  WAKE,
};

// ArcWaker is an atomic-refcounted wrapper around a `CrossThreadPromiseFulfiller<WakeInstruction>`.
// The atomic-refcounted aspect makes it safe to call `clone()` and `drop()` concurrently, while the
// `CrossThreadPromiseFulfiller` aspect makes it safe to call `wake_by_ref()` concurrently. Finally,
// `wake()` is implemented in terms of `wake_by_ref()` and `drop()`.
//
// This class is mostly an implementation detail of RootWaker.
class ArcWaker: public kj::AtomicRefcounted,
                public kj::EnableAddRefToThis<ArcWaker>,
                public CxxWaker {
public:
  ArcWaker(kj::Own<const kj::CrossThreadPromiseFulfiller<WakeInstruction>> fulfiller);
  ~ArcWaker() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(ArcWaker);

  const CxxWaker* clone() const override;
  void wake() const override;
  void wake_by_ref() const override;
  void drop() const override;

private:
  kj::Own<const kj::CrossThreadPromiseFulfiller<WakeInstruction>> fulfiller;
};

struct PromiseArcWakerPair {
  kj::Promise<WakeInstruction> promise;
  kj::Arc<const ArcWaker> waker;
};

// TODO(now): Doc comment.
PromiseArcWakerPair newPromiseAndArcWaker(const kj::Executor& executor);

// =======================================================================================
// RootWaker

class FuturePollerBase;

// RootWaker is the waker passed to Rust's `Future::poll()` function. RootWaker itself is not
// refcounted -- instead it is intended to live locally on the stack or in a coroutine frame, and
// trying to `clone()` it will cause it to allocate an ArcWaker for the caller.
//
// This class is mostly an implementation detail of our `co_await` operator implementation for Rust
// Futures. RootWaker exists in order to optimize the case where Rust async code awaits a KJ
// promise, in which case we can make the outer KJ coroutine wait more or less directly on the inner
// KJ promise which Rust owns.
class RootWaker: public CxxWaker {
public:
  // Saves a reference to the FuturePoller which is using this RootWaker. The FuturePoller creates
  // RootWakers on the stack in `await_ready()`, so its lifetime always encloses RootWakers.
  explicit RootWaker(FuturePollerBase& futurePoller);

  // Create a new or clone an existing ArcWaker, leak its pointer, and return it. This may be called
  // by any thread.
  const CxxWaker* clone() const override;

  // Unimplemented, because Rust user code cannot consume the `std::task::Waker` we create which
  // wraps this RootWaker.
  void wake() const override;

  // Rust user code can wake us synchronously during the execution of `future.poll()` using this
  // function. This may be called by any thread.
  void wake_by_ref() const override;

  // Does not actually destroy this object. Instead, we increment a counter so we can assert that it
  // was dropped exactly once before `future.poll()` returned. This can only be called on the thread
  // which is doing the awaiting, because our implementation of `future.poll()` never transfers the
  // Waker object to a different thread.
  void drop() const override;

  // In addition to the above functions, Rust may invoke `is_current()` during `future.poll()`
  // execution. It uses this to implement a short- circuit optimization when it awaits a KJ promise.

  // True if the current thread's kj::Executor is the same as the RootWaker's.
  bool is_current() const;

  // Called by RustPromiseAwaiter's constructor to get a reference to an Event which will call
  // the current Future's `poll()` function. This is used to `.await` OwnPromiseNodes in Rust
  // without having to clone an ArcWaker.
  FuturePollerBase& getFuturePoller();

  struct State {
    // Number of times this Waker was synchronously woken during `future.poll()`.
    // Incremented by `wake_by_ref()`.
    uint wakeCount = 0;

    // Filled in if `clone()` was called on the RootWaker during `future.poll()`. This promise is
    // fulfilled by the ArcWaker clones' CrossThreadPromiseFulfiller.
    kj::Maybe<kj::Promise<WakeInstruction>> cloned;
  };

  // Used by the owner of RootWaker after `future.poll()` has returned, to retrieve the
  // RootWaker's state for further processing. This is non-const, because by the time this is
  // called, Rust has dropped all of its borrows to this class, meaning we no longer have to worry
  // about thread safety.
  //
  // This function will assert if `drop()` has not been called since RootWaker was constructed, or
  // since the last call to `reset()`.
  State reset();

private:
  FuturePollerBase& futurePoller;

  // We store the kj::Executor for the constructing thread so that we can lazily instantiate a
  // CrossThreadPromiseFulfiller from any thread in our `clone()` implementation. This also allows
  // us to guarantee that `wake_after()` will only be called from the awaiting thread, allowing us
  // to ignore thread-safety for the `wakeAfter` promise.
  const kj::Executor& executor = kj::getCurrentThreadExecutor();

  // Initialized by `clone()`, which may be called by any thread.
  kj::MutexGuarded<kj::Maybe<PromiseArcWakerPair>> cloned;

  // Incremented by `wake_by_ref()`, which may be called by any thread. All operations use relaxed
  // memory order, because this counter doesn't guard any memory.
  mutable std::atomic<uint> wakeCount { 0 };

  // Incremented by `drop()`, so we can validate that `drop()` is only called once on this object.
  //
  // Rust requires that Wakers be droppable by any thread. However, we own the implementation of
  // `poll()` to which `RootWaker&` is passed, and those implementations store the Rust
  // `std::task::Waker` object on the stack,, and never move it elsewhere. Since that object is
  // responsible for calling `RootWaker::drop()`, we know for sure that `drop()` will only ever be
  // called on the thread which constructed it. Therefore, there is no need to make `dropCount`
  // thread-safe.
  mutable uint dropCount = 0;
};

}  // namespace workerd::rust::async
