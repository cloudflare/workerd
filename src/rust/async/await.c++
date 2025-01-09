#include <workerd/rust/async/await.h>
#include <workerd/rust/async/lib.rs.h>

namespace workerd::rust::async {

// =======================================================================================
// ArcWakerAwaiter

ArcWakerAwaiter::ArcWakerAwaiter(FuturePollerBase& futurePoller, OwnPromiseNode node, kj::SourceLocation location)
    : Event(location),
      futurePoller(futurePoller),
      node(kj::mv(node)) {
  this->node->setSelfPointer(&this->node);
  this->node->onReady(this);
  // TODO(perf): If `this->isNext()` is true, can we immediately resume? Or should we check if
  //   the enclosing coroutine has suspended at least once?
  futurePoller.beginTrace(this->node);
}

ArcWakerAwaiter::~ArcWakerAwaiter() noexcept(false) {
  futurePoller.endTrace(node);

  unwindDetector.catchExceptionsIfUnwinding([this]() {
    node = nullptr;
  });
}

// Validity-check the Promise's result, then fire the BaseFutureAwaiterBase Event to poll the
// wrapped Future again.
kj::Maybe<kj::Own<kj::_::Event>> ArcWakerAwaiter::fire() {
  futurePoller.endTrace(node);

  kj::_::ExceptionOr<WakeInstruction> result;

  node->get(result);
  KJ_IF_SOME(exception, kj::runCatchingExceptions([this]() {
    node = nullptr;
  })) {
    result.addException(kj::mv(exception));
  }

  // We should only ever receive a WakeInstruction, never an exception. But if we do, propagate
  // it to the coroutine.
  KJ_IF_SOME(exception, result.exception) {
    futurePoller.reject(kj::mv(exception));
    return kj::none;
  }

  auto value = KJ_ASSERT_NONNULL(result.value);

  if (value == WakeInstruction::WAKE) {
    // This was an actual wakeup.
    futurePoller.armDepthFirst();
  } else {
    // All of our Wakers were dropped. We are awaiting the Rust equivalent of kj::NEVER_DONE.
  }

  return kj::none;
}

void ArcWakerAwaiter::traceEvent(kj::_::TraceBuilder& builder) {
  if (node.get() != nullptr) {
    node->tracePromise(builder, true);
  }
  futurePoller.traceEvent(builder);
}

// =================================================================================================
// RustPromiseAwaiter

static_assert(sizeof(RustPromiseAwaiter) == sizeof(uint64_t) * 14,
    "RustPromiseAwaiter size changed, you must re-run bindgen");

// To initialize the object, Rust needs to know the size and alignment of RustPromiseAwaiter. To
// that end, we use bindgen to generate an opaque FFI type of known size for RustPromiseAwaiter in
// await_h.rs. There is a static_assert in await.c++ which ensures
//
// TODO(now): Automate this?

#if 0

bindgen \
    --rust-target 1.83.0 \
    --disable-name-namespacing \
    --generate "types" \
    --allowlist-type "^workerd::rust::async_::RustPromiseAwaiter$" \
    --opaque-type ".*" \
    --no-derive-copy \
    ./await.h \
    -o ./await.h.rs \
    -- \
    -x c++ \
    -std=c++23 \
    -stdlib=libc++ \
    -Wno-pragma-once-outside-header \
    -I $(bazel info bazel-bin)/external/capnp-cpp/src/kj/_virtual_includes/kj \
    -I $(bazel info bazel-bin)/external/capnp-cpp/src/kj/_virtual_includes/kj-async \
    -I $(bazel info bazel-bin)/external/crates_vendor__cxx-1.0.133/_virtual_includes/cxx_cc \
    -I $(bazel info bazel-bin)/src/rust/async/_virtual_includes/async@cxx

#endif

RustPromiseAwaiter::RustPromiseAwaiter(OwnPromiseNode nodeParam, kj::SourceLocation location)
    : Event(location),
      node(kj::mv(nodeParam)),
      done(false) {
  node->setSelfPointer(&node);
  node->onReady(this);
}

RustPromiseAwaiter::~RustPromiseAwaiter() noexcept(false) {
  // End tracing.
  KJ_IF_SOME(kjWaker, maybeKjWaker) {
    kjWaker.getFuturePoller().endTrace(node);
  }

  unwindDetector.catchExceptionsIfUnwinding([this]() {
    node = nullptr;
  });
}

kj::Maybe<kj::Own<kj::_::Event>> RustPromiseAwaiter::fire() {
  done = true;

  KJ_IF_SOME(kjWaker, maybeKjWaker) {
    kjWaker.getFuturePoller().endTrace(node);
  }

  KJ_SWITCH_ONEOF(currentWaker) {
    KJ_CASE_ONEOF(_, Uninit) {
      // We were constructed, and our Event even fired, but our owner still didn't `poll()` us yet.
      // This is currently an unlikely case given how the rest of the code is written, but doing
      // nothing here is the right thing regardless: `poll()` will see `done == true` if/when it is
      // eventually called.
    }
    KJ_CASE_ONEOF(event, Event*) {
      event->armDepthFirst();
    }
    KJ_CASE_ONEOF(rustWaker, const RustWaker*) {
      rustWaker->wake_by_ref();
    }
  }

  // We don't need our Waker pointer anymore. `done` is true, so the next call to `poll()` will not
  // register a new one.
  currentWaker = Uninit{};

  return kj::none;
}

void RustPromiseAwaiter::traceEvent(kj::_::TraceBuilder& builder) {
  if (node.get() != nullptr) {
    node->tracePromise(builder, true);
  }
  KJ_IF_SOME(kjWaker, maybeKjWaker) {
    kjWaker.getFuturePoller().traceEvent(builder);
  }
}

bool RustPromiseAwaiter::poll_with_kj_waker(const KjWaker& waker) {
  // TODO(perf): If `this->isNext()` is true, meaning our event is next in line to fire, can we
  //   disarm it, set `done = true`, etc.? If we can only suspend if our enclosing KJ coroutine has
  //   suspended at least once, we may be able to check for that through KjWaker.

  if (done) {
    // TODO(now): Propagate value-or-exception.
    return true;
  }

  // Safety: const_cast is okay, because `waker.is_current()` means we are running on the same
  // event loop that the `waker.getFuturePoller()` Event is a member of, and we only use KjWaker
  // to access that Event.
  // TODO(now): Scope this mutable access tighter somehow.
  KJ_ASSERT(waker.is_current());
  auto& mutWaker = const_cast<KjWaker&>(waker);

  KJ_IF_SOME(previousWaker, maybeKjWaker) {
    // We assert that we are polled with the same `KjWaker` pointer value every time. To us, the
    // `KjWaker` pointer identifies the KJ coroutine which transitively owns us. We rely on this
    // ownership to guarantee that it is safe for this `RustPromiseAwaiter` to store the `KjWaker`
    // pointer, and later to use it to arm the KJ coroutine event when our PromiseNode becomes
    // ready.
    //
    // If we are now being polled with a different `KjWaker`, that means that ownership of the
    // enclosing Future must have passed from one KJ coroutine's `co_await` expression to another,
    // and it is very likely that `previousWaker` is now dangling. Our `co_await` implementation
    // does not allow cross-coroutine ownership transfer, so we can assert here.
    //
    // TODO(now): This assumes that `KjWaker` is only used in `co_await` expressions. Perhaps it
    //   should be renamed? I used to call it AwaitWaker, but I didn't like that. KjAwaitWaker?
    //   KjStaticWaker, to reflect the stability of its address relative to us? KjCoroutineWaker?
    KJ_ASSERT(&previousWaker == &mutWaker,
        "RustPromiseAwaiters may be awaited by only one coroutine");
  } else {
    // First call to `poll()`.
    mutWaker.getFuturePoller().beginTrace(node);
  }

  maybeKjWaker = mutWaker;

  currentWaker = &static_cast<Event&>(mutWaker.getFuturePoller());

  return false;
}

bool RustPromiseAwaiter::poll(const RustWaker* waker) {
  // TODO(perf): If `this->isNext()` is true, meaning our event is next in line to fire, can we
  //   disarm it, set `done = true`, etc.? If we can only suspend if our enclosing KJ coroutine has
  //   suspended at least once, we may be able to check for that through KjWaker, but this path
  //   doesn't have access to one.

  if (done) {
    // TODO(now): Propagate value-or-exception.
    return true;
  }

  KJ_IF_SOME(previousWaker, currentWaker.tryGet<const RustWaker*>()) {
    KJ_ASSERT(previousWaker == waker);
  } else {
    // TODO(now): Safety comment.
    currentWaker = waker;
  }

  return false;
}

void rust_promise_awaiter_new_in_place(RustPromiseAwaiter* ptr, OwnPromiseNode node) {
  kj::ctor(*ptr, kj::mv(node));
}
void rust_promise_awaiter_drop_in_place(RustPromiseAwaiter* ptr) {
  kj::dtor(*ptr);
}

// =======================================================================================
// FuturePollerBase

FuturePollerBase::FuturePollerBase(
    kj::_::Event& next, kj::_::ExceptionOrValue& resultRef, kj::SourceLocation location)
    : Event(location),
      next(next),
      resultRef(resultRef) {}

void FuturePollerBase::beginTrace(OwnPromiseNode& node) {
  if (promiseNodeForTrace == kj::none) {
    promiseNodeForTrace = node;
  }
}

void FuturePollerBase::endTrace(OwnPromiseNode& node) {
  KJ_IF_SOME(myNode, promiseNodeForTrace) {
    if (myNode.get() == node.get()) {
      promiseNodeForTrace = kj::none;
    }
  }
}

void FuturePollerBase::reject(kj::Exception exception) {
  resultRef.addException(kj::mv(exception));
  next.armDepthFirst();
}

void FuturePollerBase::traceEvent(kj::_::TraceBuilder& builder) {
  KJ_IF_SOME(node, promiseNodeForTrace) {
    node->tracePromise(builder, true);
  }
  next.traceEvent(builder);
}

BoxFutureVoidAwaiter operator co_await(kj::_::CoroutineBase::Await<BoxFutureVoid> await) {
  return BoxFutureVoidAwaiter{await.coroutine, kj::mv(await.awaitable)};
}

BoxFutureVoidAwaiter operator co_await(kj::_::CoroutineBase::Await<BoxFutureVoid&> await) {
  return BoxFutureVoidAwaiter{await.coroutine, kj::mv(await.awaitable)};
}

}  // namespace workerd::rust::async
