#pragma once

#include "kj-rs/executor-guarded.h"

#include <kj/async.h>
#include <kj/refcount.h>

namespace kj_rs {

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

}  // namespace kj_rs
