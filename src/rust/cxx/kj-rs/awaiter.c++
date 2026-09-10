#include "awaiter.h"

#include <kj-rs/lib.rs.h>

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
  // Our `tracePromise()` implementation checks for a null `node`, so we don't have to sever our
  // LinkedGroup relationship before destroying `node`. If our FuturePollEvent (our LinkedGroup)
  // tries to trace us between now and our destructor completing, `tracePromise()` will ignore the
  // null `node`.
  unwindDetector.catchExceptionsIfUnwinding([this]() { node = nullptr; });
}

void RustPromiseAwaiter::fire() {
  // Safety: Our Event can only fire on the event loop which was active when our Event base class
  // was constructed. Therefore, we don't need to check that we're on the correct event loop.

  // Nullify our `maybeOptionWaker` to signal that we are done.
  KJ_DEFER(maybeOptionWaker = kj::none);

  KJ_IF_SOME(futurePollEvent, linkedGroup().tryGet()) {
    // Optimized path: we're still linked to a FuturePollEvent. Arm it directly.
    futurePollEvent.armDepthFirst();
    linkedGroup().set(kj::none);
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
  KJ_IF_SOME(futurePollEvent, linkedGroup().tryGet()) {
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

bool RustPromiseAwaiter::poll(const WakerRef& waker, const KjWaker* maybeKjWaker) {
  // TODO(perf): If `this->isNext()` is true, meaning our event is next in line to fire, can we
  //   disarm it, set `done = true`, etc.? If we can only suspend if our enclosing KJ coroutine has
  //   suspended at least once, we may be able to check for that through LazyArcWaker, but this path
  //   doesn't have access to one.

  KJ_IF_SOME(optionWaker, maybeOptionWaker) {
    // Our Promise is not yet ready.

    // Check for an optimized wake path.
    KJ_IF_SOME(kjWaker, maybeKjWaker) {
      KJ_IF_SOME(futurePollEvent, kjWaker.tryGetFuturePollEvent()) {
        // Optimized path. The Future which is polling our Promise is in turn being polled by a
        // `co_await` expression somewhere up the stack from us. We can arrange to arm the
        // `co_await` expression's KJ Event directly when our Promise is ready.

        // Drop any Waker stored in OptionWaker. We'll use the LinkedGroup to wake instead.
        //
        // Note: this leaves OptionWaker empty while maybeOptionWaker is still Some(ref). If the
        // FuturePollEvent is later destroyed (severing the LinkedGroup link) before our Promise
        // fires, fire() will find no LinkedGroup AND an empty OptionWaker. fire() handles this
        // via wake_if_some(), which is a no-op on an empty OptionWaker.
        optionWaker.set_none();

        // Store a reference to the current `co_await` expression's Future polling Event. The
        // reference is weak, and will be cleared if the `co_await` expression happens to end before
        // our Promise is ready. In the more likely case that our Promise becomes ready while the
        // `co_await` expression is still active, we'll arm its Event so it can `poll()` us again.
        linkedGroup().set(futurePollEvent);

        return false;
      }
    }

    // Unoptimized fallback path.

    // Tell our OptionWaker to store a clone of whatever Waker we were given.
    optionWaker.set(waker);

    // Clearing our reference to the FuturePollEvent (if we have one) tells our fire()
    // implementation to use our OptionWaker to perform the wake.
    linkedGroup().set(kj::none);

    return false;
  } else {
    // Our Promise is ready.
    return true;
  }
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

void FuturePollEvent::exitPollScope(kj::Maybe<kj::Promise<void>> maybePromise) {
  // Await any LazyArcWaker promise that got created during the call to `poll()`. Note that if a
  // Future returns Ready _and_ synchronously wakes its Waker, the work done to await the
  // LazyArcWaker promise is wasted, since we will immediately tear the entire BoxFutureAwaiter<T>
  // down. However, that's an unlikely case, and this work here isn't likely to be a significant
  // source of overhead.
  KJ_IF_SOME(promise, maybePromise) {
    auto& node = arcWakerPromise.emplace(kj::_::PromiseNode::from(kj::mv(promise)));
    node->setSelfPointer(&node);
    node->onReady(this);
  }
}

void FuturePollEvent::enterPollScope() noexcept {
  // Clear out any previous LazyArcWaker promise the FuturePollEvent was holding onto. Note that
  // since there is no code path which rejects this Promise, this is not strictly required for
  // correctness, but nevertheless serves as a useful assertion.
  KJ_IF_SOME(node, arcWakerPromise) {
    kj::_::ExceptionOr<kj::_::Void> output;

    node->get(output);
    KJ_IF_SOME(exception, kj::runCatchingExceptions([this]() { arcWakerPromise = kj::none; })) {
      output.addException(kj::mv(exception));
    }

    // NOTE: `node` is now dangling.

    KJ_IF_SOME(exception, output.exception) {
      // We should only ever receive a WakeInstruction, never an exception. If we do receive an
      // exception, it would be because our ArcWaker implementation allowed its cross-thread promise
      // fulfiller to be destroyed without being fulfilled, or because we foolishly added an
      // explicit call to the fulfiller's reject() function. Either way, it is a programming error,
      // so we abort the process here by re-throwing across a noexcept boundary. This avoids having
      // implement the ability to "reject" the Future poll() Event.
      kj::throwFatalException(kj::mv(exception));
    }
  }
}

void FuturePollEvent::tracePromise(kj::_::TraceBuilder& builder, bool stopAtNextEvent) {
  if (stopAtNextEvent) return;

  // FuturePollEvent is inherently a "join". Even though it polls only one Future, that Future may in
  // turn poll any number of different Futures and Promises.
  //
  // When tracing, we can only pick one branch to follow. Arbitrarily, I'm following the first
  // RustPromiseAwaiter branch, similar to how ExclusiveJoinPromiseNode chooses its left branch. In
  // the common case, this will be whatever OwnPromiseNode our Rust Future is currently `.await`ing.
  auto rustPromiseAwaiters = linkedObjects();
  if (rustPromiseAwaiters.begin() != rustPromiseAwaiters.end()) {
    // Our Rust Future is awaiting an OwnPromiseNode. We'll pick the first one in our list.
    rustPromiseAwaiters.front().tracePromise(builder, false);
  } else KJ_IF_SOME(node, arcWakerPromise) {
    // Our Rust Future is not awaiting any OwnPromiseNode, and instead cloned our Waker. We'll trace
    // our ArcWaker Promise instead.
    if (node.get() != nullptr) {
      node->tracePromise(builder, false);
    }
  }
}

FuturePollEvent::PollScope::PollScope(FuturePollEvent& futurePollEvent): holder(futurePollEvent) {
  futurePollEvent.enterPollScope();
}

FuturePollEvent::PollScope::~PollScope() noexcept(false) {
  holder.get().futurePollEvent.exitPollScope(reset());
}

kj::Maybe<FuturePollEvent&> FuturePollEvent::PollScope::tryGetFuturePollEvent() const {
  KJ_IF_SOME(h, holder.tryGet()) {
    return h.futurePollEvent;
  } else {
    return kj::none;
  }
}

}  // namespace kj_rs
