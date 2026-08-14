#pragma once

#include "kj-rs/executor-guarded.h"

#include <kj/async.h>
#include <kj/mutex.h>
#include <kj/refcount.h>

namespace kj_rs {

class FuturePollEvent;

// Hook invoked when a waker arms its FuturePollEvent (below) from the owning loop's thread. An
// integrating kj::EventPort that drives tokio tasks inside its own wait() (see kj-rs-tokio)
// installs this to nudge itself out of a blocking park: a tokio task completing during the port's
// block_on() may arm a KJ event same-thread, and KJ's edge-triggered setRunnable() misses that
// arm when the loop's runnable state is already set (e.g. left true by a prior timer). Null
// (no-op) by default; thread-local, one integrating port per loop thread.
extern thread_local void (*futurePollArmNudge)();

// =======================================================================================
// FutureWakerCell

// FutureWakerCell is the thread-safe "waker cell" behind every Rust waker that outlives a single
// `Future::poll()` call. `std::task::Waker` is `Send + Sync`, so safe Rust may clone, wake, and
// drop it from any thread; the cell upholds that contract:
//
//   - clone/drop are atomic refcount operations (kj::AtomicRefcounted, handed across the FFI as
//     `kj::Arc<FutureWakerCell>`).
//   - wakeByRef() checks whether it is running on the owning event loop's thread. On the owning
//     thread — the overwhelmingly common case, e.g. every tokio I/O readiness event under
//     kj-rs-tokio — it arms the owning FuturePollEvent's `kj::_::Event` directly via
//     `Event::armDepthFirst()`, which is idempotent across same-turn arms and safe to call from
//     within the event's own `fire()` (turn() unlinks the event before firing). This is the same
//     way a RustPromiseAwaiter leaf arms the FuturePollEvent when its Promise becomes ready — a
//     wake is just another leaf arming the same event. From any other thread, wakeByRef()
//     fulfills a `kj::CrossThreadPromiseFulfiller` instead (safe from threads with no KJ event
//     loop at all, e.g. tokio's blocking pool); the promise side lives in the FuturePollEvent and
//     arms it from the owning thread, where the weak event link below is safe to read. The
//     consumed fulfiller is renewed at the top of the next poll (see PollWaker's constructor), so
//     repeated cross-thread wakes coalesce into the pending poll — exactly the coalescing the
//     Waker contract permits.
//
// Neutralize-on-drop: the cell's link to the Event is weak — a `kj::Maybe<Event&>` invalidated
// (structurally, by the owning FuturePollEvent's RAII guard; see awaiter.h) when that event is
// destroyed. The link is only ever read or written on the owning thread (same-thread wakes,
// neutralize()), so it needs no synchronization; a cell reference that Rust retains past the
// Future's lifetime (e.g. parked in a channel's AtomicWaker) observes a dead link — or, cross-
// thread, a fulfiller whose promise side died with the event — and the wake is a safe no-op
// rather than arming a freed Event.
//
// Ownership only ever crosses the FFI as real `kj::Arc<FutureWakerCell>` handles (PollWaker::
// cloneCell(), addRef()). Rust's RawWakerVTable island (waker.rs) carries its handle in the
// RawWaker data slot, disowning/reowning it at the vtable edge — the one place `std::task::Waker`
// forces a raw pointer.
class FutureWakerCell final: public kj::AtomicRefcounted {
 public:
  explicit FutureWakerCell(kj::_::Event& event)
      : executor(kj::getCurrentThreadExecutor().addRef()),
        event(event) {}

  // Called by `~FuturePollEvent` (owning thread) to neutralize this cell and every outstanding
  // Rust reference to it, making any subsequent same-thread wake a safe no-op. (Cross-thread
  // wakes neutralize independently: fulfilling the cell's fulfiller after the event destroyed
  // the promise side is already a no-op.) Const because `kj::Arc` (like the FFI) only hands out
  // const access; `event` is owning-thread-only interior state.
  void neutralize() const {
    event = kj::none;
  }

  // Wake from any thread: arm the owning FuturePollEvent (directly on the owning thread, via the
  // cross-thread fulfiller otherwise), or no-op if it has been neutralized. Const because Rust
  // reaches it through `&self`.
  void wakeByRef() const {
    if (isCurrent(*executor)) {
      // Owning thread: arm the event directly. `event` is only touched on this thread.
      KJ_IF_SOME(e, event) {
        e.armDepthFirst();
        // Nudge an integrating event port out of a blocking park (see `futurePollArmNudge`).
        // No-op unless a port installed the hook and is currently parked.
        if (futurePollArmNudge != nullptr) {
          futurePollArmNudge();
        }
      }
    } else {
      // Foreign thread (possibly one with no KJ event loop): deliver through the cross-thread
      // fulfiller. Fulfilling twice before the owning loop renews it, or after the promise side
      // died with the event, is a documented no-op — wakes coalesce.
      auto lock = crossThreadWake.lockShared();
      if (*lock != nullptr) {
        (*lock)->fulfill();
      }
    }
  }

  // Install a fresh cross-thread fulfiller, replacing any consumed one. Called on the owning
  // thread by FuturePollEvent (at construction and at the top of each poll); the lock is only
  // ever contended by a concurrent foreign-thread wakeByRef().
  void replaceCrossThreadFulfiller(
      kj::Own<const kj::CrossThreadPromiseFulfiller<void>> fulfiller) const {
    *crossThreadWake.lockExclusive() = kj::mv(fulfiller);
  }

  // True if the current cross-thread fulfiller has been consumed (or discarded) and should be
  // renewed before the next park. Owning thread only.
  bool needsFreshCrossThreadFulfiller() const {
    auto lock = crossThreadWake.lockShared();
    return *lock == nullptr || !(*lock)->isWaiting();
  }

  // Hand out a new strong reference. Const + const_cast because Rust reaches it through `&self`:
  // cells are always heap-allocated non-const (kj::arc in FuturePollEvent), and the atomic
  // refcount bump is safe from any thread.
  kj::Arc<FutureWakerCell> addRef() const {
    return const_cast<FutureWakerCell&>(*this).addRefToThis();
  }

  // Re-own a strong reference previously surrendered to a raw pointer: waker.rs disowns the
  // kj::Arc it parks in a RawWaker data slot, and its vtable's drop calls this to reclaim it.
  // Exposed to Rust as an `unsafe fn`: `this` must carry exactly such a surrendered reference,
  // and dropping the returned handle releases it.
  kj::Arc<FutureWakerCell> reown() const {
    return kj::Arc<FutureWakerCell>::reown(this);
  }

 private:
  // The owning event loop's executor, used to route wakes: captured at construction (which
  // happens on the owning thread), immutable afterwards, safe to read from any thread. Owned via
  // addRef() so a cell retained by Rust past loop teardown still has a valid Executor to ask
  // (isCurrent() then reports false and the wake takes the — dead, no-op — fulfiller path).
  kj::Own<const kj::Executor> executor;

  // Weak, owner-invalidated reference to the owning FuturePollEvent's Event base: non-owning (the
  // event lives in the promise graph; the cell must observe its death, never extend its life) and
  // nulled by `neutralize()` when that event is destroyed. Only read and written on the owning
  // thread, so no synchronization is required; mutable because the cell is only ever reached
  // const (kj::Arc / FFI `&self`).
  mutable kj::Maybe<kj::_::Event&> event;

  // Cross-thread wake delivery: fulfilled by foreign-thread wakeByRef(), renewed by the owning
  // thread (replaceCrossThreadFulfiller). The promise side lives in the FuturePollEvent; if the
  // event dies first, fulfilling is a safe no-op. Mutex-guarded because the owning thread's
  // renewal races with foreign-thread fulfills; same-thread wakes never touch it.
  kj::MutexGuarded<kj::Own<const kj::CrossThreadPromiseFulfiller<void>>> crossThreadWake;
};

// =======================================================================================
// PollWaker

// PollWaker is the waker C++ passes to `Future::poll()`. It lives on the stack / in a coroutine
// frame for the duration of a single poll, and Rust only ever borrows it (waker.rs wraps it in a
// Waker whose drop is a no-op).
//
//   - wakeByRef() delegates to the event's FutureWakerCell, which handles both the same-thread
//     (synchronous same-turn re-poll) and foreign-thread cases — `&Waker` is Sync, so even the
//     borrowed waker may legally be woken from another thread during the poll.
//   - cloneCell() is how Rust retains a waker past the poll: it hands out a strong reference to
//     the event's FutureWakerCell, so a later wake from any thread arms the same event.
//   - tryGetFuturePollEvent() lets RustPromiseAwaiter (which helps Rust `.await` KJ Promises)
//     arm the event directly instead of going through a waker, when possible (owning thread
//     only).
class PollWaker final {
 public:
  // `futurePollEvent` is the FuturePollEvent responsible for calling `Future::poll()`, and must
  // outlive this PollWaker. Construction happens on the owning thread at the top of each poll,
  // and renews the cell's cross-thread fulfiller if a foreign-thread wake consumed it.
  explicit PollWaker(FuturePollEvent& futurePollEvent);
  ~PollWaker() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(PollWaker);

  // Wake from any thread: arm the associated FuturePollEvent so it (re-)polls.
  void wakeByRef() const;

  // Hand out a new strong reference to the event's FutureWakerCell, for Rust to retain and wake
  // later. Safe from any thread (atomic refcount).
  kj::Arc<FutureWakerCell> cloneCell() const;

  // The FuturePollEvent whose poll() this waker was created for, if the current thread's
  // kj::Executor is the one which owns it.
  kj::Maybe<FuturePollEvent&> tryGetFuturePollEvent() const;

 private:
  struct FuturePollEventHolder {
    FuturePollEvent& futurePollEvent;
  };
  ExecutorGuarded<FuturePollEventHolder> holder;

  // The event's cell, cached here so wakeByRef()/cloneCell() work from any thread without going
  // through the executor-guarded holder. Valid for this PollWaker's whole life: the cell is
  // created eagerly with the FuturePollEvent, which outlives the poll.
  const FutureWakerCell& cell;
};

}  // namespace kj_rs
