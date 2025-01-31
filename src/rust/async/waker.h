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
// This class is mostly an implementation detail of KjWaker.
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

// Construct a new promise and ArcWaker promise pair, with the Promise to be scheduled on the event
// loop associated with `executor`.
PromiseArcWakerPair newPromiseAndArcWaker(const kj::Executor& executor);

// =======================================================================================
// KjWaker

class CoAwaitWaker;

// KjWaker is intended to live locally on the stack or in a coroutine frame. Trying to `clone()` it
// will cause it to allocate an ArcWaker for the caller.
class KjWaker: public CxxWaker {
public:
  // Create a new or clone an existing ArcWaker, leak its pointer, and return it. This may be called
  // by any thread.
  const CxxWaker* clone() const override;

  // Unimplemented, because Rust user code cannot consume the `std::task::Waker` we create which
  // wraps this KjWaker.
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

  struct State {
    // Number of times this Waker was synchronously woken during `future.poll()`.
    // Incremented by `wake_by_ref()`.
    uint wakeCount = 0;

    // Filled in if `clone()` was called on the KjWaker during `future.poll()`. This promise is
    // fulfilled by the ArcWaker clones' CrossThreadPromiseFulfiller.
    kj::Maybe<kj::Promise<WakeInstruction>> cloned;
  };

  // Used by the owner of KjWaker after `future.poll()` has returned, to retrieve the
  // KjWaker's state for further processing. This is non-const, because by the time this is
  // called, Rust has dropped all of its borrows to this class, meaning we no longer have to worry
  // about thread safety.
  //
  // This function will assert if `drop()` has not been called since KjWaker was constructed, or
  // since the last call to `reset()`.
  State reset();

  // Access the Executor that was active when this KjWaker was constructed. This a convenience
  // function to help CoAwaitWaker figure out if it can optimize Rust Promise `.await`s
  // without having to store its own Executor reference.
  const kj::Executor& getExecutor() const;

private:
  // We store the kj::Executor for the constructing thread so that we can lazily instantiate a
  // CrossThreadPromiseFulfiller from any thread in our `clone()` implementation.
  const kj::Executor& executor = kj::getCurrentThreadExecutor();

  // Initialized by `clone()`, which may be called by any thread.
  kj::MutexGuarded<kj::Maybe<PromiseArcWakerPair>> cloned;

  // Incremented by `wake_by_ref()`, which may be called by any thread. All operations use relaxed
  // memory order, because this counter does not guard any memory.
  mutable std::atomic<uint> wakeCount { 0 };

  // Incremented by `drop()`, so we can validate that `drop()` is only called once on this object.
  //
  // Rust requires that Wakers be droppable by any thread. However, we own the implementation of
  // `poll()` to which `KjWaker&` is passed, and those implementations store the Rust
  // `std::task::Waker` object on the stack,, and never move it elsewhere. Since that object is
  // responsible for calling `KjWaker::drop()`, we know for sure that `drop()` will only ever be
  // called on the thread which constructed it. Therefore, there is no need to make `dropCount`
  // thread-safe.
  mutable uint dropCount = 0;
};

// =======================================================================================
// OptionWaker

// An opaque Rust type defined in lib.rs, and thus in lib.rs.h. lib.rs.h depends on our C++ headers,
// including waker.h (the file you're currently reading), so we forward-declare OptionWaker here for
// use in the C++ headers.
//
// This class is a wrapper around an arbitrary `std::task::Waker`. It has one function on it:
// `wake_by_ref()`. RustPromiseAwaiter calls it when the KJ promise it is currently waiting on
// becomes ready. In this way, Rust is able to await KJ promises using any Waker of their choosing,
// not just a KjWaker.
struct OptionWaker;

// Wrapper around an arbitrary `&std::task::Waker`.
struct WakerRef;

}  // namespace workerd::rust::async
