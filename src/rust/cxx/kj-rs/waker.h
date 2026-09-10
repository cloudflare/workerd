#pragma once

#include "promise.h"

#include <kj/async.h>
#include <kj/mutex.h>
#include <kj/one-of.h>
#include <kj/refcount.h>

#include <atomic>

namespace kj_rs {

using kj::uint;

// =======================================================================================
// KjWaker

class FuturePollEvent;

// KjWaker is an abstract base class which defines an interface mirroring Rust's RawWakerVTable
// struct. Rust has four trampoline functions, defined in waker.rs, which translate Waker::clone(),
// Waker::wake(), etc. calls to the virtual member functions on this class.
//
// Rust requires Wakers to be Send and Sync, meaning all of the functions defined here may be called
// concurrently by any thread. Derived class implementations of these functions must handle this,
// which is why all of the virtual member functions are `const`-qualified.
class KjWaker {
 public:
  // Return a pointer to a new strong ref to a KjWaker. Note that `clone()` may return nullptr,
  // in which case the Rust implementation in waker.rs will treat it as a no-op Waker. Rust
  // immediately wraps this pointer in its own Waker object, which is responsible for later
  // releasing the strong reference.
  //
  // TODO(cleanup): Build kj::Arc<T> into cxx-rs so we can return one instead of a raw pointer.
  virtual const KjWaker* clone() const = 0;

  // Wake and drop this waker.
  virtual void wake() const = 0;

  // Wake this waker, but do not drop it.
  virtual void wake_by_ref() const = 0;

  // Drop this waker.
  virtual void drop() const = 0;

  // If this KjWaker implementation has an associated FuturePollEvent, C++ code can request access
  // to it here. The RustPromiseAwaiter class (which helps Rust `.await` KJ Promises) uses this to
  // optimize awaits, when possible.
  virtual kj::Maybe<FuturePollEvent&> tryGetFuturePollEvent() const {
    return kj::none;
  }
};

// =======================================================================================
// ArcWakerPromiseNode

class ArcWaker;

class ArcWakerPromiseNode: public kj::_::PromiseNode {
 public:
  ArcWakerPromiseNode(kj::Promise<void> promise);
  KJ_DISALLOW_COPY_AND_MOVE(ArcWakerPromiseNode);

  void destroy() noexcept override;
  void onReady(kj::_::Event* event) noexcept override;
  void get(kj::_::ExceptionOrValue& output) noexcept override;
  void tracePromise(kj::_::TraceBuilder& builder, bool stopAtNextEvent) override;

 private:
  kj::Arc<const ArcWaker> owner = nullptr;
  OwnPromiseNode node;

  friend class ArcWaker;
};

// =======================================================================================
// ArcWaker

class ArcWaker;

struct PromiseArcWakerPair {
  kj::Promise<void> promise;
  kj::Arc<const ArcWaker> waker;
};

// ArcWaker is an atomic-refcounted wrapper around a `CrossThreadPromiseFulfiller<void>`.
// The atomic-refcounted aspect makes it safe to call `clone()` and `drop()` concurrently, while the
// `CrossThreadPromiseFulfiller` aspect makes it safe to call `wake_by_ref()` concurrently. Finally,
// `wake()` is implemented in terms of `wake_by_ref()` and `drop()`.
//
// This class is mostly an implementation detail of LazyArcWaker.
class ArcWaker: public kj::AtomicRefcounted, public KjWaker {
 public:
  // Construct a new promise and ArcWaker promise pair, with the Promise to be scheduled on the
  // event loop associated with `executor`.
  static PromiseArcWakerPair create(const kj::Executor& executor);

  ArcWaker(kj::Badge<ArcWaker>, kj::PromiseCrossThreadFulfillerPair<void> paf);
  KJ_DISALLOW_COPY_AND_MOVE(ArcWaker);

  const KjWaker* clone() const override;
  void wake() const override;
  void wake_by_ref() const override;
  void drop() const override;

 private:
  kj::Promise<void> getPromise();

  ArcWakerPromiseNode node;
  kj::Own<const kj::CrossThreadPromiseFulfiller<void>> fulfiller;
};

// =======================================================================================
// LazyArcWaker

// LazyArcWaker is intended to live locally on the stack or in a coroutine frame. Trying to
// `clone()` it will cause it to allocate an ArcWaker for the caller.
class LazyArcWaker: public KjWaker {
 public:
  // Create a new or clone an existing ArcWaker, leak its pointer, and return it. This may be called
  // by any thread.
  const KjWaker* clone() const override;

  // Unimplemented, because Rust user code cannot consume the `std::task::Waker` we create which
  // wraps this LazyArcWaker.
  void wake() const override;

  // Rust user code can wake us synchronously during the execution of `future.poll()` using this
  // function. This may be called by any thread.
  void wake_by_ref() const override;

  // Does not actually destroy this object. Instead, we increment a counter so we can assert that it
  // was dropped exactly once before `future.poll()` returned. This can only be called on the thread
  // which is doing the awaiting, because our implementation of `future.poll()` never transfers the
  // Waker object to a different thread.
  void drop() const override;

  // Used by the owner of LazyArcWaker after `future.poll()` has returned, to retrieve the
  // LazyArcWaker's state for further processing. This is non-const, because by the time this is
  // called, Rust has dropped all of its borrows to this class, meaning we no longer have to worry
  // about thread safety.
  //
  // This function will assert if `drop()` has not been called since LazyArcWaker was constructed,
  // or since the last call to `reset()`.
  //
  // Returns `kj::none` the LazyArcWaker was neither woken nor cloned before being dropped. Returns
  // `kj::READY_NOW` if the LazyArcWaker was synchronously woken. Otherwise, if `clone()` was
  // called, return the promise associated with the cloned ArcWaker.
  kj::Maybe<kj::Promise<void>> reset();

 private:
  // We store the kj::Executor for the constructing thread so that we can lazily instantiate a
  // CrossThreadPromiseFulfiller from any thread in our `clone()` implementation.
  const kj::Executor& executor = kj::getCurrentThreadExecutor();

  // Initialized by `clone()`, which may be called by any thread. This could almost be a
  // `kj::Lazy<T>`, but we need to be able to detect when we haven't been cloned.
  kj::MutexGuarded<kj::Maybe<PromiseArcWakerPair>> cloned;

  // Incremented by `wake_by_ref()`, which may be called by any thread. All operations use relaxed
  // memory order, because this counter does not guard any memory.
  mutable std::atomic<uint> wakeCount{0};

  // Incremented by `drop()`, so we can validate that `drop()` is only called once on this object.
  //
  // Rust requires that Wakers be droppable by any thread. However, we own the implementation of
  // `poll()` to which `LazyArcWaker&` is passed, and those implementations store the Rust
  // `std::task::Waker` object on the stack,, and never move it elsewhere. Since that object is
  // responsible for calling `LazyArcWaker::drop()`, we know for sure that `drop()` will only ever be
  // called on the thread which constructed it. Therefore, there is no need to make `dropCount`
  // thread-safe.
  mutable uint dropCount = 0;
};

}  // namespace kj_rs
