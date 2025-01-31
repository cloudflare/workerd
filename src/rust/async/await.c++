#include <workerd/rust/async/await.h>
#include <workerd/rust/async/lib.rs.h>

#include <kj/debug.h>

namespace workerd::rust::async {

// =======================================================================================
// ArcWakerAwaiter

ArcWakerAwaiter::ArcWakerAwaiter(
    CoAwaitWaker& coAwaitWaker, OwnPromiseNode nodeParam, kj::SourceLocation location)
    : Event(location),
      coAwaitWaker(coAwaitWaker),
      node(kj::mv(nodeParam)) {
  node->setSelfPointer(&node);
  node->onReady(this);
  // TODO(perf): If `this->isNext()` is true, can we immediately resume? Or should we check if
  //   the enclosing coroutine has suspended at least once?
}

ArcWakerAwaiter::~ArcWakerAwaiter() noexcept(false) {
  unwindDetector.catchExceptionsIfUnwinding([this]() {
    node = nullptr;
  });
}

// Validity-check the Promise's result, then fire the CoAwaitWaker Event to poll the
// wrapped Future again.
kj::Maybe<kj::Own<kj::_::Event>> ArcWakerAwaiter::fire() {
  kj::_::ExceptionOr<WakeInstruction> result;

  node->get(result);
  KJ_IF_SOME(exception, kj::runCatchingExceptions([this]() {
    node = nullptr;
  })) {
    result.addException(kj::mv(exception));
  }

  [&result]() noexcept {
    KJ_IF_SOME(exception, result.exception) {
      // We should only ever receive a WakeInstruction, never an exception. If we do receive an
      // exception, it would be because our ArcWaker implementation allowed its cross-thread promise
      // fulfiller to be destroyed without being fulfilled, or because we foolishly added an
      // explicit call to the fulfiller's reject() function. Either way, it is a programming error,
      // so we abort the process here by re-throwing across a noexcept boundary. This avoids having
      // implement the ability to "reject" the Future poll() Event.
      kj::throwFatalException(kj::mv(exception));
    }
  }();

  auto value = KJ_ASSERT_NONNULL(result.value);

  if (value == WakeInstruction::WAKE) {
    // This was an actual wakeup.
    coAwaitWaker.getFuturePollEvent().armDepthFirst();
  } else {
    // All of our Wakers were dropped. We are awaiting the Rust equivalent of kj::NEVER_DONE.
  }

  return kj::none;
}

void ArcWakerAwaiter::traceEvent(kj::_::TraceBuilder& builder) {
  if (coAwaitWaker.wouldTrace({}, *this)) {
    // Our associated futurePollEvent's `traceEvent()` implementation would call our
    // `tracePromise()` function. Just forward the call to the futurePollEvent.
    coAwaitWaker.getFuturePollEvent().traceEvent(builder);
  } else {
    // Our CoAwaitWaker would choose a different branch to trace, so just record our own trace
    // address(es) and stop here.
    tracePromise(builder, false);
  }
}

void ArcWakerAwaiter::tracePromise(kj::_::TraceBuilder& builder, bool stopAtNextEvent) {
  // TODO(someday): Is it possible to get the address of the Rust code which cloned our Waker?

  if (node.get() != nullptr) {
    node->tracePromise(builder, stopAtNextEvent);
  }
}

// =================================================================================================
// RustPromiseAwaiter

// To own RustPromiseAwaiters, Rust needs to know the size and alignment of RustPromiseAwaiter. To
// that end, we use bindgen to generate an opaque FFI type of known size for RustPromiseAwaiter in
// await.h.rs.
//
// Our use of bindgen is non-automated, and the generated await.hs.rs file must be manually
// regenerated whenever the size and alignment of RustPromiseAwaiter changes. To remind us to do so,
// we have these static_asserts.
//
// If you are reading this because a static_assert fired:
//
//   1. Scroll down to find a sample `bindgen` command line invocation.
//   2. Run the command in this directory.
//   3. Read the new await.hs.rs and adjust the constants in these static_asserts with the new size
//      or alignment.
//   4. Commit the changes here with the new await.hs.rs file.
//
// It would be nice to automate this someday. `rules_rust` has some bindgen rules, but it adds a few
// thousand years to the build times due to its hermetic dependency on LLVM. It's possible to
// provide our own toolchain, but I became fatigued in the attempt.
static_assert(sizeof(GuardedRustPromiseAwaiter) == sizeof(uint64_t) * 16,
    "GuardedRustPromiseAwaiter size changed, you must re-run bindgen");
static_assert(alignof(GuardedRustPromiseAwaiter) == alignof(uint64_t) * 1,
    "GuardedRustPromiseAwaiter alignment changed, you must re-run bindgen");

// Notes about the bindgen command below:
//
//   - `--generate "types"` inhibits the generation of any binding other than types.
//   - We use `--allow-list-type` and `--blocklist-type` regexes to select specific types.
//   - `--blocklist-type` seems to be necessary if your allowlisted type has nested types.
//   - The allowlist/blocklist regexes are applied to an intermediate mangling of the types' paths
//     in C++. In particular, C++ namespaces are replaced with Rust module names. Since `async` is
//     a keyword in Rust, bindgen mangles the corresponding Rust module to `async_`. Meanwhile,
//     nested types are mangled to `T_Nested`, despite being `T::Nested` in C++.
//   - `--opaque-type` tells bindgen to generate a type containing a single array of words, rather
//     than named members which alias the members in C++.
//
// The end result is a Rust file which defines Rust equivalents for our selected C++ types. The
// types will have the same size and alignment as our C++ types, but do not provide data member
// access, nor does bindgen define any member functions or special functions for the type. Instead,
// we define the entire interface for the types in our `cxxbridge` FFI module.
//
// We do it this way because in our philosophy on cross-language safety, the only structs which both
// languages are allowed to mutate are those generated by our `cxxbridge` macro. RustPromiseAwaiter
// is a C++ class, so we don't let Rust mutate its internal data members.

#if 0

bindgen \
    --rust-target 1.83.0 \
    --disable-name-namespacing \
    --generate "types" \
    --allowlist-type "workerd::rust::async_::GuardedRustPromiseAwaiter" \
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

RustPromiseAwaiter::RustPromiseAwaiter(OptionWaker& optionWaker, OwnPromiseNode nodeParam, kj::SourceLocation location)
    : Event(location),
      optionWaker(optionWaker),
      node(kj::mv(nodeParam)) {
  node->setSelfPointer(&node);
  node->onReady(this);
}

RustPromiseAwaiter::~RustPromiseAwaiter() noexcept(false) {
  // Our `tracePromise()` implementation checks for a null `node`, so we don't have to sever our
  // LinkedGroup relationship before destroying `node`. If our CoAwaitWaker (our LinkedGroup) tries
  // to trace us between now and our destructor completing, `tracePromise()` will ignore the null
  // `node`.
  unwindDetector.catchExceptionsIfUnwinding([this]() {
    node = nullptr;
  });
}

kj::Maybe<kj::Own<kj::_::Event>> RustPromiseAwaiter::fire() {
  // Safety: Our Event can only fire on the event loop which was active when our Event base class
  // was constructed. Therefore, we don't need to check that we're on the correct event loop.

  // `setDone()` nullifies our `optionWaker` Maybe, but we might need the OptionWaker first, so defer
  // this call to `setDone()`.
  KJ_DEFER(setDone());

  KJ_IF_SOME(coAwaitWaker, linkedGroup().tryGet()) {
    coAwaitWaker.getFuturePollEvent().armDepthFirst();
    linkedGroup().set(kj::none);
  } else KJ_IF_SOME(waker, optionWaker) {
    // This call to `waker.wake()` consumes OptionWaker's inner Waker. If we call it more than once,
    // it will panic. Fortunately, we only call it once.
    waker.wake();
  } else {
    // We were constructed, and our Event even fired, but our owner still didn't `poll()` us yet.
    // This is currently an unlikely case given how the rest of the code is written, but doing
    // nothing here is the right thing regardless: `poll()` will see `isDone() == true` if/when it
    // is eventually called.
  }

  return kj::none;
}

void RustPromiseAwaiter::traceEvent(kj::_::TraceBuilder& builder) {
  KJ_IF_SOME(coAwaitWaker, linkedGroup().tryGet()) {
    if (coAwaitWaker.wouldTrace({}, *this)) {
      // We are associated with a CoAwaitWaker, and we are at the head of the its list of Promises,
      // meaning its `tracePromise()` implementation would forward to our `tracePromise()`. We can
      // therefore forward this `traceEvent()` call to the coroutine's `traceEvent()` to generate a
      // slightly longer trace with this node in it.
      coAwaitWaker.getFuturePollEvent().traceEvent(builder);
      return;
    }
  }

  // If we get here, we either don't have a CoAwaitWaker, or we do but it wouldn't trace our
  // Promise because the Future in question is polling more than one Promise. We'll just trace our
  // Promise, and not trace into the CoAwaitWaker.
  tracePromise(builder, false);
}

void RustPromiseAwaiter::tracePromise(kj::_::TraceBuilder& builder, bool stopAtNextEvent) {
  // We ignore `stopAtNextEvent`, because `CoAwaitWaker` is our only possible caller. And if
  // it's calling us, it wants us to trace our promise, not ignore the call.

  if (node.get() != nullptr) {
    node->tracePromise(builder, stopAtNextEvent);
  }

  // TODO(now): Can we add an entry for the `.await` expression in Rust here?
}

void RustPromiseAwaiter::setDone() {
  optionWaker = kj::none;
}

bool RustPromiseAwaiter::isDone() const {
  return optionWaker == kj::none;
}

bool RustPromiseAwaiter::poll(const WakerRef& waker, const CxxWaker* maybeCxxWaker) {
  // TODO(perf): If `this->isNext()` is true, meaning our event is next in line to fire, can we
  //   disarm it, set `done = true`, etc.? If we can only suspend if our enclosing KJ coroutine has
  //   suspended at least once, we may be able to check for that through KjWaker, but this path
  //   doesn't have access to one.

  if (isDone()) {
    return true;
  }

  auto& optionWakerRef = KJ_ASSERT_NONNULL(
      optionWaker, "isDone() returned false so we must have a OptionWaker");

  KJ_IF_SOME(cxxWaker, maybeCxxWaker) {
    KJ_IF_SOME(coAwaitWaker, kj::dynamicDowncastIfAvailable<const CoAwaitWaker>(cxxWaker)) {
      if (coAwaitWaker.is_current()) {
        // Optimized path. The Future which is polling our Promise is in turn being polled by a
        // `co_await` expression. We can arm the `co_await` expression's KJ Event directly when our
        // Promise is ready.

        // If we had an opaque Waker stored in OptionWaker before, drop it now, as we won't be
        // needing it.
        optionWakerRef.set_none();

        // Store a reference to the current `co_await` expression's Future polling Event. The
        // reference is weak, and will be cleared if the `co_await` expression happens to end before
        // our Promise is ready. In the more likely case that our Promise becomes ready while the
        // `co_await` expression is still active, we'll arm its Event so it can `poll()` us again.
        //
        // Safety: const_cast is okay, because `coAwaitWaker.is_current()` means we are running on
        // the same event loop that the CoAwaitWaker's Event base class is a member of.
        auto& mutCoAwaitWaker = const_cast<CoAwaitWaker&>(coAwaitWaker);
        linkedGroup().set(mutCoAwaitWaker);

        return false;
      }
    }
  }

  // Unoptimized fallback path.

  // Tell our OptionWaker to store a clone of whatever Waker we were given.
  optionWakerRef.set(waker);

  // Clearing our reference to the CoAwaitWaker (if we have one) tells our fire() implementation to
  // use our OptionWaker to perform the wake.
  linkedGroup().set(kj::none);

  return false;
}

OwnPromiseNode RustPromiseAwaiter::take_own_promise_node() {
  KJ_ASSERT(isDone(), "take_own_promise_node() should only be called after poll() returns true");
  KJ_ASSERT(node.get() != nullptr, "take_own_promise_node() should only be called once");
  return kj::mv(node);
}

void guarded_rust_promise_awaiter_new_in_place(PtrGuardedRustPromiseAwaiter ptr, OptionWaker* optionWaker, OwnPromiseNode node) {
  kj::ctor(*ptr, *optionWaker, kj::mv(node));
}
void guarded_rust_promise_awaiter_drop_in_place(PtrGuardedRustPromiseAwaiter ptr) {
  kj::dtor(*ptr);
}

// =======================================================================================
// CoAwaitWaker

CoAwaitWaker::CoAwaitWaker(kj::_::Event& futurePoller): futurePoller(futurePoller) {}

bool CoAwaitWaker::is_current() const {
  return &kjWaker.getExecutor() == &kj::getCurrentThreadExecutor();
}

const CxxWaker* CoAwaitWaker::clone() const {
  return kjWaker.clone();
}

void CoAwaitWaker::wake() const {
  // CoAwaitWakers are only exposed to Rust by const borrow, meaning Rust can never arrange to call
  // `wake()`, which drops `self`, on this object.
  KJ_UNIMPLEMENTED(
      "Rust user code should never have possess a consumable reference to CoAwaitWaker");
}

void CoAwaitWaker::wake_by_ref() const {
  kjWaker.wake_by_ref();
}

void CoAwaitWaker::drop() const {
  kjWaker.drop();
}

void CoAwaitWaker::onReady(kj::_::Event* event) noexcept {
  KJ_UNIMPLEMENTED("CoAwaitWaker's PromiseNode base class exists only for tracing");
}
void CoAwaitWaker::get(kj::_::ExceptionOrValue& output) noexcept {
  KJ_UNIMPLEMENTED("CoAwaitWaker's PromiseNode base class exists only for tracing");
}

void CoAwaitWaker::tracePromise(kj::_::TraceBuilder& builder, bool stopAtNextEvent) {
  // We ignore `stopAtNextEvent`, because `kj::_::Coroutine` is our only possible caller. And if
  // it's calling us, it wants us to trace our promise, not ignore the call.

  // CoAwaitWaker is inherently a "join". Even though it polls only one Future, that Future may in
  // turn poll any number of different Futures and Promises.
  //
  // When tracing, we can only pick one branch to follow. Arbitrarily, I'm following the first
  // RustPromiseAwaiter branch. In the common case, this will be whatever OwnPromiseNode our Rust
  // Future is currently `.await`ing.
  //
  // NOTE: If you change this logic, you must also change the `wouldTrace()` member function!
  auto rustPromiseAwaiters = linkedObjects();
  if (rustPromiseAwaiters.begin() != rustPromiseAwaiters.end()) {
    // Our Rust Future is awaiting an OwnPromiseNode. We'll pick the first one in our list.
    rustPromiseAwaiters.front().tracePromise(builder, stopAtNextEvent);
  } else KJ_IF_SOME(awaiter, arcWakerAwaiter) {
    // Our Rust Future is not awaiting any OwnPromiseNode, and instead cloned our Waker. We'll trace
    // our ArcWaker Promise instead.
    awaiter.tracePromise(builder, stopAtNextEvent);
  }
}

kj::_::Event& CoAwaitWaker::getFuturePollEvent() {
  return futurePoller;
}

bool CoAwaitWaker::wouldTrace(kj::Badge<ArcWakerAwaiter>, ArcWakerAwaiter& awaiter) {
  // We would only trace the ArcWakerAwaiter if we have no RustPromiseAwaiters.
  if (linkedObjects().empty()) {
    KJ_IF_SOME(awa, arcWakerAwaiter) {
      KJ_ASSERT(&awa == &awaiter,
          "Should not be possible for foreign ArcWakerAwaiter to call our wouldTrace()");
      return true;
    }
  }
  return false;
}

bool CoAwaitWaker::wouldTrace(kj::Badge<RustPromiseAwaiter>, RustPromiseAwaiter& awaiter) {
  // We prefer to trace the first RustPromiseAwaiter in our list, if there is one.
  auto objects = linkedObjects();
  if (objects.begin() != objects.end()) {
    return &awaiter == &objects.front();
  }
  return false;
}

void CoAwaitWaker::suspend() {
  auto state = kjWaker.reset();

  if (state.wakeCount > 0) {
    // The future returned Pending, but synchronously called `wake_by_ref()` on the KjWaker,
    // indicating it wants to immediately be polled again. We should arm our event right now,
    // which will call `await_ready()` again on the event loop.
    futurePoller.armDepthFirst();
  } else KJ_IF_SOME(promise, state.cloned) {
    // The future returned Pending and cloned an ArcWaker to notify us later. We'll arrange for
    // the ArcWaker's promise to arm our event once it's fulfilled.
    arcWakerAwaiter.emplace(*this, kj::_::PromiseNode::from(kj::mv(promise)));
  } else {
    // The future returned Pending, did not call `wake_by_ref()` on the KjWaker, and did not
    // clone an ArcWaker. Rust is either awaiting a KJ promise, or the Rust equivalent of
    // kj::NEVER_DONE.
  }
}

}  // namespace workerd::rust::async
