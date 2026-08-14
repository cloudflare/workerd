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

RustPromiseAwaiter::RustPromiseAwaiter(
    OptionWaker& optionWaker, OwnPromiseNode nodeParam, kj::SourceLocation location)
    : Event(location),
      maybeOptionWaker(optionWaker),
      node(kj::mv(nodeParam)) {
  node->setSelfPointer(&node);
  node->onReady(this);
}

RustPromiseAwaiter::~RustPromiseAwaiter() noexcept(false) {
  // Sever our weak link to any FuturePollEvent before we go away, so it can't trace into or arm a
  // destroyed awaiter. Our `tracePromise()` implementation also checks for a null `node`, so even
  // between clearPollEvent() and node reset we are safe to trace.
  clearPollEvent();
  unwindDetector.catchExceptionsIfUnwinding([this]() { node = nullptr; });
}

void RustPromiseAwaiter::setPollEvent(FuturePollEvent& futurePollEvent) {
  KJ_IF_SOME(old, maybePollEvent) {
    if (&old == &futurePollEvent) return;
    old.leaves.remove(*this);
  }
  futurePollEvent.leaves.add(*this);
  maybePollEvent = futurePollEvent;
}

void RustPromiseAwaiter::clearPollEvent() {
  KJ_IF_SOME(old, maybePollEvent) {
    old.leaves.remove(*this);
    maybePollEvent = kj::none;
  }
}

void RustPromiseAwaiter::fire() {
  // Safety: Our Event can only fire on the event loop which was active when our Event base class
  // was constructed. Therefore, we don't need to check that we're on the correct event loop.

  // Nullify our `maybeOptionWaker` to signal that we are done.
  KJ_DEFER(maybeOptionWaker = kj::none);

  KJ_IF_SOME(futurePollEvent, maybePollEvent) {
    // Optimized path: we're still linked to a FuturePollEvent. Arm it directly.
    futurePollEvent.armDepthFirst();
    clearPollEvent();
  } else KJ_IF_SOME(optionWaker, maybeOptionWaker) {
    // We use wake_if_some() rather than an unconditional wake because the OptionWaker may be empty. This
    // happens when poll() took the optimized path (clearing the OptionWaker and linking to a
    // FuturePollEvent instead), but the FuturePollEvent was destroyed before our Promise fired.
    // In that case there's nothing to wake; KJ_DEFER above will set maybeOptionWaker = kj::none,
    // so our owner's next poll() will see that and return true.
    //
    // When the OptionWaker IS populated (the unoptimized path stored a cloned Waker), this wakes
    // it normally.
    optionWaker.wake_if_some();
  } else {
    // maybeOptionWaker is already kj::none, meaning fire() was already called (it's set by
    // KJ_DEFER above). This shouldn't happen since KJ Events fire at most once, but doing nothing
    // is safe: poll() will see maybeOptionWaker == kj::none and return true.
  }
}

void RustPromiseAwaiter::traceEvent(kj::_::TraceBuilder& builder) {
  if (node.get() != nullptr) {
    node->tracePromise(builder, true);
  }
  // TODO(someday): Can we add an entry for the `.await` expression in Rust here?
  KJ_IF_SOME(futurePollEvent, maybePollEvent) {
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

  KJ_IF_SOME(optionWaker, maybeOptionWaker) {
    // Our Promise is not yet ready.

    // Tell our OptionWaker to store a clone of whatever Waker we were given.
    optionWaker.set(waker);

    // Clearing our weak reference to the FuturePollEvent (if we have one) tells our fire()
    // implementation to use our OptionWaker to perform the wake.
    clearPollEvent();

    return false;
  } else {
    // Our Promise is ready.
    return true;
  }
}

bool RustPromiseAwaiter::poll(const WakerRef& waker, const PollWaker& pollWaker) {
  KJ_IF_SOME(futurePollEvent, pollWaker.tryGetFuturePollEvent()) {
    KJ_IF_SOME(optionWaker, maybeOptionWaker) {
      // Our Promise is not yet ready, and we have an optimized wake path. The Future which is
      // polling our Promise is in turn being polled by a `co_await` expression somewhere up the
      // stack from us. We can arrange to arm the `co_await` expression's KJ Event directly when
      // our Promise is ready.

      // Drop any Waker stored in OptionWaker. We'll use our weak link to the FuturePollEvent to
      // wake instead.
      //
      // Note: this leaves OptionWaker empty while maybeOptionWaker is still Some(ref). If the
      // FuturePollEvent is later destroyed (severing our weak link) before our Promise fires,
      // fire() will find no linked FuturePollEvent AND an empty OptionWaker. fire() handles this
      // via wake_if_some(), which is a no-op on an empty OptionWaker.
      optionWaker.set_none();

      // Store a weak reference to the current `co_await` expression's Future polling Event. The
      // reference is weak, and will be cleared if the `co_await` expression happens to end before
      // our Promise is ready. In the more likely case that our Promise becomes ready while the
      // `co_await` expression is still active, we'll arm its Event so it can `poll()` us again.
      setPollEvent(futurePollEvent);

      return false;
    } else {
      // Our Promise is ready.
      return true;
    }
  }
  // The PollWaker exposes no FuturePollEvent (its owning thread's kj::Executor is not ours --
  // cannot normally happen in the single-thread world). Fall back to the generic path.
  return poll(waker);
}

OwnPromiseNode RustPromiseAwaiter::take_own_promise_node() {
  KJ_ASSERT(maybeOptionWaker == kj::none,
      "take_own_promise_node() should only be called after poll() "
      "returns true");
  KJ_ASSERT(node.get() != nullptr, "take_own_promise_node() should only be called once");
  return kj::mv(node);
}

void guarded_rust_promise_awaiter_new_in_place(
    GuardedRustPromiseAwaiter* ptr, OptionWaker* optionWaker, OwnPromiseNode node) {
  kj::ctor(*ptr, *optionWaker, kj::mv(node));
}
void guarded_rust_promise_awaiter_drop_in_place(GuardedRustPromiseAwaiter* ptr) {
  kj::dtor(*ptr);
}

// =======================================================================================
// FuturePollEvent

FuturePollEvent::~FuturePollEvent() noexcept(false) {
  // Our FutureWakerCell (if any) is neutralized by the wakerCell guard's destructor during member
  // destruction, so any waker reference Rust retained past our lifetime observes a dead weak link
  // on a later wake and is a safe no-op, rather than arming this freed event.

  // Sever our weak links to all leaves, so a RustPromiseAwaiter that outlives us (e.g. a stashed
  // PromiseFuture) never arms this freed event.
  for (;;) {
    auto it = leaves.begin();
    if (it == leaves.end()) break;
    auto& leaf = *it;
    leaves.remove(leaf);
    leaf.maybePollEvent = kj::none;
  }
}

kj::Rc<FutureWakerCell> FuturePollEvent::cloneWakerCell() {
  // Lazily create the cell, bound to this event.
  if (wakerCell.cell == nullptr) {
    wakerCell.cell = kj::rc<FutureWakerCell>(*this);
  }
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
