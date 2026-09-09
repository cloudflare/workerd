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

  done = true;

  KJ_IF_SOME(futurePollEvent, weakPollEvent.tryGet()) {
    // Optimized path: we're still linked to a FuturePollEvent. Arm it directly.
    futurePollEvent.armDepthFirst();
    clearPollEvent();
  } else KJ_IF_SOME(waker, storedWaker) {
    auto owned = kj::mv(waker);
    storedWaker = kj::none;
    owned->wake();
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
  //   suspended at least once, we may be able to check for that through LazyArcWaker, but this path
  //   doesn't have access to one.

  if (done) {
    return true;
  }

  bool haveEquivalentClone = false;
  KJ_IF_SOME(stored, storedWaker) {
    haveEquivalentClone = stored->will_wake(waker);
  }
  if (!haveEquivalentClone) {
    storedWaker = clone_waker(waker);
  }
  clearPollEvent();
  return false;
}

bool RustPromiseAwaiter::poll(const WakerRef& waker, const PollWaker& pollWaker) {
  KJ_IF_SOME(futurePollEvent, pollWaker.tryGetFuturePollEvent()) {
    if (done) return true;
    storedWaker = kj::none;
    setPollEvent(futurePollEvent);
    return false;
  }
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
      wakerCell{kj::arc<FutureWakerCell>(*this)} {}

FuturePollEvent::~FuturePollEvent() noexcept(false) {
  invalidateWeak();
  for (;;) {
    auto it = leaves.begin();
    if (it == leaves.end()) break;
    leaves.remove(*it);
  }
}

kj::Arc<FutureWakerCell> FuturePollEvent::cloneWakerCell() {
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
