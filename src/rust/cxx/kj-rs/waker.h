#pragma once

#include "kj-rs/executor-guarded.h"
#include "promise.h"

#include <kj/async.h>
#include <kj/mutex.h>
#include <kj/one-of.h>
#include <kj/refcount.h>

#include <atomic>

namespace kj_rs {

using kj::uint;

class FuturePollEvent;
class FutureWakerCell;

// =======================================================================================
// PollWaker

// PollWaker is the waker C++ passes to `Future::poll()`. It lives on the stack / in a coroutine
// frame for the duration of a single poll, and Rust only ever borrows it (waker.rs wraps it in a
// Waker whose drop is a no-op).
//
//   - wakeByRef() is a synchronous same-turn wake: it arms the FuturePollEvent directly. Arming
//     during poll() (whether poll was reached via onReady() or fire()) is idempotent and causes
//     an immediate re-poll.
//   - cloneCell() is how Rust retains a waker past the poll: it hands out a strong reference to
//     the event's FutureWakerCell, so a later wake arms the same event.
//   - tryGetFuturePollEvent() lets RustPromiseAwaiter (which helps Rust `.await` KJ Promises)
//     arm the event directly instead of going through a waker, when possible.
class PollWaker final {
 public:
  // `futurePollEvent` is the FuturePollEvent responsible for calling `Future::poll()`, and must
  // outlive this PollWaker.
  explicit PollWaker(FuturePollEvent& futurePollEvent);
  ~PollWaker() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(PollWaker);

  // Synchronous same-turn wake: arm the associated FuturePollEvent so it re-polls.
  void wakeByRef() const;

  // Get-or-create the event's FutureWakerCell and hand out a new strong reference to it, for Rust
  // to retain and wake later. Returns kj::none if the current thread's kj::Executor is not the
  // one which owns the FuturePollEvent (cannot normally happen in the single-thread world); Rust
  // then mints a no-op waker.
  kj::Maybe<kj::Rc<FutureWakerCell>> cloneCell() const;

  // The FuturePollEvent whose poll() this waker was created for, if the current thread's
  // kj::Executor is the one which owns it.
  kj::Maybe<FuturePollEvent&> tryGetFuturePollEvent() const;

 private:
  struct FuturePollEventHolder {
    FuturePollEvent& futurePollEvent;
  };
  ExecutorGuarded<FuturePollEventHolder> holder;
};

// =======================================================================================
// KjWaker

class FuturePollEvent;

// Hook invoked when a waker arms its FuturePollEvent (below). An integrating kj::EventPort that
// drives tokio tasks inside its own wait() (see kj-rs-tokio) installs this to nudge itself out of
// a blocking park: a tokio task completing during the port's block_on() may arm a KJ event
// same-thread, and KJ's edge-triggered setRunnable() misses that arm when the loop's runnable
// state is already set (e.g. left true by a prior timer). Null (no-op) by default; thread-local,
// one integrating port per loop thread.
extern thread_local void (*futurePollArmNudge)();

// =======================================================================================
// FutureWakerCell

// FutureWakerCell is the same-thread "waker cell" behind every Rust waker that outlives a single
// `Future::poll()` call, refcounted via the non-atomic kj::Refcounted (single-thread axiom: a
// waker is never woken, cloned, or dropped from another thread).
//
// The cell holds a bare pointer to the owning FuturePollEvent's `kj::_::Event`. `wakeByRef()`
// arms that Event directly via `Event::armDepthFirst()`, which is idempotent across same-turn
// arms and safe to call from within the event's own `fire()` (turn() unlinks the event before
// firing). This is the same way a RustPromiseAwaiter leaf arms the FuturePollEvent when its
// Promise becomes ready — a wake is just another leaf arming the same event.
//
// Neutralize-on-drop: the cell's link to the Event is weak — a `kj::Maybe<Event&>` invalidated
// (structurally, by the owning FuturePollEvent's RAII guard; see awaiter.h) when that event is
// destroyed. A cell reference that Rust retains past the Future's lifetime (e.g. parked in a
// channel's AtomicWaker) therefore observes a dead link on a later wake and is a safe no-op,
// rather than arming a freed Event.
//
// Ownership only ever crosses the FFI as real `kj::Rc<FutureWakerCell>` handles (PollWaker::
// cloneCell(), addRef()). Rust's RawWakerVTable island (waker.rs) carries its handle in the
// RawWaker data slot, disowning/reowning it at the vtable edge — the one place `std::task::Waker`
// forces a raw pointer.
class FutureWakerCell final: public kj::Refcounted {
 public:
  explicit FutureWakerCell(kj::_::Event& event): event(event) {}

  // Called by `~FuturePollEvent` to neutralize this cell and every outstanding Rust reference to
  // it, making any subsequent wake a safe no-op.
  void neutralize() {
    event = kj::none;
  }

  // Arm the owning FuturePollEvent, or no-op if it has been neutralized. Const because Rust
  // reaches it through `&self`; in the single-thread world it only ever runs on the owning event
  // loop's thread.
  void wakeByRef() const {
    KJ_IF_SOME(e, event) {
      e.armDepthFirst();
      // Nudge an integrating event port out of a blocking park (see `futurePollArmNudge`). No-op
      // unless a port installed the hook and is currently parked.
      if (futurePollArmNudge != nullptr) {
        futurePollArmNudge();
      }
    }
  }

  // Hand out a new strong reference. Const + const_cast because Rust reaches it through `&self`:
  // cells are always heap-allocated non-const (kj::rc in cloneWakerCell()), and the non-atomic
  // refcount bump is safe under the single-thread axiom.
  kj::Rc<FutureWakerCell> addRef() const {
    return const_cast<FutureWakerCell&>(*this).addRefToThis();
  }

  // Re-own a strong reference previously surrendered to a raw pointer: waker.rs disowns the
  // kj::Rc it parks in a RawWaker data slot, and its vtable's drop calls this to reclaim it.
  // Exposed to Rust as an `unsafe fn`: `this` must carry exactly such a surrendered reference,
  // and dropping the returned handle releases it.
  kj::Rc<FutureWakerCell> reown() const {
    return kj::Rc<FutureWakerCell>::reown(&const_cast<FutureWakerCell&>(*this));
  }

 private:
  // Weak, owner-invalidated reference to the owning FuturePollEvent's Event base: non-owning (the
  // event lives in the promise graph; the cell must observe its death, never extend its life) and
  // nulled by `neutralize()` when that event is destroyed. Reads in the `const` wake functions
  // are single-thread so no synchronization is required.
  kj::Maybe<kj::_::Event&> event;
};

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
