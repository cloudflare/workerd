#include "awaiter.h"

#include <kj-rs/ffi.rs.h>

#include <kj/debug.h>

namespace kj_rs {

// =================================================================================================
// RustPromiseAwaiter

// To own RustPromiseAwaiters, Rust needs to know the size and alignment of RustPromiseAwaiter. To
// that end, we use bindgen to generate an opaque FFI type of known size for RustPromiseAwaiter in
// awaiter.h.rs.
//
static_assert(sizeof(GuardedRustPromiseAwaiter) == sizeof(GuardedRustPromiseAwaiterRepr),
    "GuardedRustPromiseAwaiter size changed, you must update lib.rs ffi");
static_assert(alignof(GuardedRustPromiseAwaiter) == alignof(GuardedRustPromiseAwaiterRepr),
    "GuardedRustPromiseAwaiter alignment changed, you must update lib.rs ffi");

RustPromiseAwaiter::RustPromiseAwaiter(OwnPromiseNode nodeParam, kj::SourceLocation location)
    : Event(location),
      node(kj::mv(nodeParam)) {
  node->setSelfPointer(&node);
  node->onReady(this);
}

RustPromiseAwaiter::~RustPromiseAwaiter() noexcept(false) {
  // Sever our weak link to any FuturePollEvent before we go away, so it can't trace into or arm a
  // destroyed awaiter. (Our stored Waker clone, if any, drops itself — rust::Box.) Our
  // `tracePromise()` implementation also checks for a null `node`, so even between
  // clearPollEvent() and node reset we are safe to trace.
  clearPollEvent();
  unwindDetector.catchExceptionsIfUnwinding([this]() { node = nullptr; });
}

void RustPromiseAwaiter::setPollEvent(FuturePollEvent& futurePollEvent) {
  KJ_IF_SOME(old, weakPollEvent.tryGet()) {
    if (&old == &futurePollEvent) return;
    old.leaves.remove(*this);
  }
  futurePollEvent.leaves.add(*this);
  weakPollEvent = futurePollEvent.addWeakRef();
}

void RustPromiseAwaiter::clearPollEvent() {
  KJ_IF_SOME(old, weakPollEvent.tryGet()) {
    old.leaves.remove(*this);
  }
  weakPollEvent = nullptr;
}

void RustPromiseAwaiter::fire() {
  // Safety: Our Event can only fire on the event loop which was active when our Event base class
  // was constructed. Therefore, we don't need to check that we're on the correct event loop.

  // Our Promise is ready; poll() returns true and take_own_promise_node() is legal from here on.
  done = true;

  KJ_IF_SOME(futurePollEvent, weakPollEvent.tryGet()) {
    // Optimized path: we're still linked to a FuturePollEvent. Arm it directly.
    futurePollEvent.armDepthFirst();
    clearPollEvent();
  } else KJ_IF_SOME(waker, storedWaker) {
    // Generic path: wake our owned clone of the Waker we were last polled with, then drop it.
    // Move it out first so a wake that synchronously re-enters poll() sees consistent state.
    auto owned = kj::mv(waker);
    storedWaker = kj::none;
    owned->wake();
  } else {
    // Neither wake path is armed: poll() took the optimized path (dropping the stored Waker
    // clone), but the FuturePollEvent was destroyed before our Promise fired, expiring the link.
    // There's nothing to wake; our owner's next poll() will see `done` and return true.
  }
}

void RustPromiseAwaiter::traceEvent(kj::_::TraceBuilder& builder) {
  if (node.get() != nullptr) {
    node->tracePromise(builder, true);
  }
  // TODO(someday): Can we add an entry for the `.await` expression in Rust here?
  KJ_IF_SOME(futurePollEvent, weakPollEvent.tryGet()) {
    futurePollEvent.traceEvent(builder);
  }
}

void RustPromiseAwaiter::tracePromise(kj::_::TraceBuilder& builder, bool stopAtNextEvent) {
  if (stopAtNextEvent) return;

  if (node.get() != nullptr) {
    node->tracePromise(builder, stopAtNextEvent);
  }
  // TODO(someday): Can we add an entry for the `.await` expression in Rust here?
}

bool RustPromiseAwaiter::poll(const WakerRef& waker) {
  // TODO(perf): If `this->isNext()` is true, meaning our event is next in line to fire, can we
  //   disarm it, set `done = true`, etc.? If we can only suspend if our enclosing KJ coroutine has
  //   suspended at least once, we may be able to check for that through PollWaker, but this path
  //   doesn't have access to one.

  if (done) {
    // Our Promise is ready.
    return true;
  }

  // Store our own clone of the Waker we were polled with — unless the clone we already hold would
  // wake the same task (`Waker::will_wake`), in which case keep it and skip the clone.
  bool haveEquivalentClone = false;
  KJ_IF_SOME(stored, storedWaker) {
    haveEquivalentClone = stored->will_wake(waker);
  }
  if (!haveEquivalentClone) {
    storedWaker = clone_waker(waker);
  }

  // Clearing our weak reference to the FuturePollEvent (if we have one) tells our fire()
  // implementation to use the stored Waker to perform the wake.
  clearPollEvent();

  return false;
}

bool RustPromiseAwaiter::poll(const WakerRef& waker, const PollWaker& pollWaker) {
  KJ_IF_SOME(futurePollEvent, pollWaker.tryGetFuturePollEvent()) {
    if (done) {
      // Our Promise is ready.
      return true;
    }

    // Our Promise is not yet ready, and we have an optimized wake path. The Future which is
    // polling our Promise is in turn being polled by a `co_await` expression somewhere up the
    // stack from us. We can arrange to arm the `co_await` expression's KJ Event directly when
    // our Promise is ready.

    // Drop any stored Waker clone. We'll use our weak link to the FuturePollEvent to wake
    // instead. (If the FuturePollEvent is destroyed before our Promise fires, expiring the link,
    // fire() finds neither wake path and does nothing — our owner's next poll() sees `done`.)
    storedWaker = kj::none;

    // Store a weak reference to the current `co_await` expression's Future polling Event. It will
    // expire if the `co_await` expression happens to end before our Promise is ready. In the more
    // likely case that our Promise becomes ready while the `co_await` expression is still active,
    // we'll arm its Event so it can `poll()` us again.
    setPollEvent(futurePollEvent);

    return false;
  }
  // The PollWaker exposes no FuturePollEvent (its owning thread's kj::Executor is not ours --
  // cannot normally happen in the single-thread world). Fall back to the generic path.
  return poll(waker);
}

OwnPromiseNode RustPromiseAwaiter::take_own_promise_node() {
  KJ_ASSERT(done,
      "take_own_promise_node() should only be called after poll() "
      "returns true");
  KJ_ASSERT(node.get() != nullptr, "take_own_promise_node() should only be called once");
  return kj::mv(node);
}

void guarded_rust_promise_awaiter_new_in_place(
    GuardedRustPromiseAwaiter* ptr, OwnPromiseNode node) {
  kj::ctor(*ptr, kj::mv(node));
}
void guarded_rust_promise_awaiter_drop_in_place(GuardedRustPromiseAwaiter* ptr) {
  kj::dtor(*ptr);
}

// =======================================================================================
// FuturePollEvent

FuturePollEvent::FuturePollEvent(kj::SourceLocation location)
    : Event(location),
      // Created eagerly (not lazily on first clone): `&Waker` is Sync, so even the borrowed
      // per-poll waker may be cloned or woken from a foreign thread during the very first poll,
      // and both paths need the cell to already exist. One small allocation per awaited future,
      // next to the coroutine frame and promise nodes already being allocated; the cross-thread
      // machinery itself is shared per loop (CrossThreadWakeSink), not allocated here.
      wakerCell{kj::arc<FutureWakerCell>(*this)} {}

FuturePollEvent::~FuturePollEvent() noexcept(false) {
  // Expire every weak reference to us up front — the leaves' `weakPollEvent` links — so nothing
  // can reach a half-destroyed event. (PtrTarget's own destructor would do this too, but only
  // after our members are gone; invalidating first closes even that window.)
  invalidateWeak();

  // Our FutureWakerCell is neutralized by the wakerCell guard's destructor during member
  // destruction, so any waker reference Rust retained past our lifetime observes a dead weak link
  // on a later wake — same-thread directly, cross-thread once the loop's sink replays it — and is
  // a safe no-op, rather than arming this freed event.

  // Unlink all leaves. Their weak links to us are already expired (above); the list link is the
  // one piece only we can sever, and kj::ListLink asserts on a leaf's destruction if we miss one.
  for (;;) {
    auto it = leaves.begin();
    if (it == leaves.end()) break;
    leaves.remove(*it);
  }
}

kj::Arc<FutureWakerCell> FuturePollEvent::cloneWakerCell() {
  // Hand out a new strong reference for Rust to retain.
  return wakerCell.cell.addRef();
}

void FuturePollEvent::tracePromise(kj::_::TraceBuilder& builder, bool stopAtNextEvent) {
  if (stopAtNextEvent) return;

  // FuturePollEvent is inherently a "join". Even though it polls only one Future, that Future may in
  // turn poll any number of different Futures and Promises.
  //
  // When tracing, we can only pick one branch to follow. Arbitrarily, I'm following the first
  // RustPromiseAwaiter branch, similar to how ExclusiveJoinPromiseNode chooses its left branch. In
  // the common case, this will be whatever OwnPromiseNode our Rust Future is currently `.await`ing.
  if (!leaves.empty()) {
    // Our Rust Future is awaiting an OwnPromiseNode. We'll pick the first one in our list.
    leaves.front().tracePromise(builder, false);
  }
}

}  // namespace kj_rs
