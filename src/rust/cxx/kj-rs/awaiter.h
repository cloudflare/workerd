#pragma once

#include "kj-rs/executor-guarded.h"
#include "kj-rs/promise.h"
#include "kj-rs/waker.h"

#include <kj/debug.h>
#include <kj/list.h>

namespace kj_rs {

// =======================================================================================
// Opaque Rust types
//
// The following types are defined in the cxx bridge (ffi.rs), and thus in ffi.rs.h. ffi.rs.h
// depends on our C++ headers, including awaiter.h (the file you're currently reading), so we
// forward-declare some types here for use in the C++ headers.

// Wrapper around an `&std::task::Waker`, passed to `RustPromiseAwaiter::poll()`. This indirection
// is required because cxx-rs does not permit us to expose opaque Rust types to C++ defined outside
// of our own crate, like `std::task::Waker`.
struct WakerRef;

// Wrapper around an `Option<std::task::Waker>`. RustPromiseAwaiter calls `set()` with the WakerRef
// passed to `poll()` if RustPromiseAwaiter is unable to find an optimized path for awaiting its
// Promise. Later on, when its Promise becomes ready, RustPromiseAwaiter will use OptionWaker to
// call wake the wrapped Waker.
//
// Otherwise, if RustPromiseAwaiter finds an optimized path for awaiting its Promise, it calls
// `set_none()` on the OptionWaker to ensure it's empty.
struct OptionWaker;

// =======================================================================================
// RustPromiseAwaiter

// RustPromiseAwaiter allows Rust `async` blocks to `.await` KJ promises. Rust code creates one in
// the block's storage at the point where the `.await` expression is evaluated, similar to how
// `kj::_::PromiseAwaiter` is created in the KJ coroutine frame when C++ `co_await`s a promise.
//
// To elaborate, RustPromiseAwaiter is part of the IntoFuture trait implementation for the
// OwnPromiseNode class, and `.await` expressions implicitly call `.into_future()`. So,
// RustPromiseAwaiter can be thought of a "Promise-to-Future" adapter. This also means that
// RustPromiseAwaiter can be constructed outside of `.await` expressions, and potentially _not_
// driven to complete readiness. Our implementation must be able to handle this case.
//
// Rust knows how big RustPromiseAwaiter is because we generate a Rust type of equal size and
// alignment using bindgen. See inside awaiter.c++ for a static_assert to remind us to re-run
// bindgen.
//
// RustPromiseAwaiter has one base class: KJ Event. We use the Event, via the native
// `node->onReady(this)` mechanism, to discover when our wrapped Promise is ready (exactly as a KJ
// coroutine registers its own Event on the promise it `co_await`s). Our Event fire() implementation
// records the fact that we are done, then wakes our Waker or arms the FuturePollEvent, if we have
// one. We hold a weak reference to the FuturePollEvent that last polled us (see maybePollEvent
// below), so a fired Promise can arm that poll event directly.
//
// Cancellation: Dropping the RustPromiseAwaiter destroys its OwnPromiseNode, cancelling the
// wrapped KJ promise. If the RustPromiseAwaiter was never constructed, Rust's OwnPromiseNode::drop()
// cancels the promise directly.
class RustPromiseAwaiter final: public kj::_::Event {
 public:
  // The Rust code which constructs RustPromiseAwaiter passes us a pointer to a OptionWaker, which can
  // be thought of as a Rust-native component RustPromiseAwaiter. Its job is to hold a clone of
  // of any non-KJ Waker that we are polled with, and forward calls to `wake()`. Ideally, we could
  // store the clone of the Waker ourselves (it's just two pointers) on the C++ side, so the
  // lifetime safety is more obvious. But, storing a reference works for now.
  RustPromiseAwaiter(
      OptionWaker& optionWaker, OwnPromiseNode node, kj::SourceLocation location = {});
  ~RustPromiseAwaiter() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(RustPromiseAwaiter);

  // -------------------------------------------------------
  // kj::_::Event API

  void fire() override;
  void traceEvent(kj::_::TraceBuilder& builder) override;

  // Helper for FuturePollEvent to report what promise it's waiting on.
  void tracePromise(kj::_::TraceBuilder& builder, bool stopAtNextEvent);

  // -------------------------------------------------------
  // API exposed to Rust code
  //
  // Additionally, see GuardedRustPromiseAwaiter below, which mediates access to this API.

  // Poll this Promise for readiness.
  //
  // The two-argument overload is for polls driven by a PollWaker (i.e. a `co_await`ed Future's
  // poll): it may optimize the wake by arming a KJ Event directly when the wrapped Promise
  // becomes ready. Polls driven by any other Waker use the one-argument overload.
  bool poll(const WakerRef& waker);
  bool poll(const WakerRef& waker, const PollWaker& pollWaker);

  // Release ownership of the OwnPromiseNode. Asserts if called before the Promise is ready; that
  // is, `poll()` must have returned true prior to calling `take_own_promise_node()`.
  OwnPromiseNode take_own_promise_node();

 private:
  // Purpose-built one-to-many weak link to the FuturePollEvent that last polled us. The link is
  // weak in both directions: destroying either side severs it (see
  // clearPollEvent() and ~FuturePollEvent()), so a fired Promise never arms, and tracing never
  // touches, a destroyed FuturePollEvent. A RustPromiseAwaiter may outlive the FuturePollEvent that
  // first polled it (e.g. a stashed PromiseFuture) and later re-link to a different one.
  friend class FuturePollEvent;
  void setPollEvent(FuturePollEvent& futurePollEvent);
  void clearPollEvent();

  kj::Maybe<FuturePollEvent&> maybePollEvent;
  kj::ListLink<RustPromiseAwaiter> link;

  // The Rust code which instantiates RustPromiseAwaiter does so with a OptionWaker object right
  // next to the RustPromiseAwaiter, such that it is dropped after RustPromiseAwaiter. Thus, our
  // reference to our OptionWaker is stable. We use the OptionWaker to (optionally) store a clone of
  // the Waker with which we were last polled.
  //
  // When we wake our enclosing Future, either with the FuturePollEvent or with OptionWaker, we
  // nullify this Maybe. Therefore, this Maybe being kj::none means our OwnPromiseNode is ready, and
  // it is safe to call `node->get()` on it.
  kj::Maybe<OptionWaker&> maybeOptionWaker;

  kj::UnwindDetector unwindDetector;
  OwnPromiseNode node;
};

// We force Rust to call our `poll()` overloads using this ExecutorGuarded wrapper around the actual
// RustPromiseAwaiter class. This allows us to assume all calls that reach RustPromiseAwaiter itself
// are on the correct thread.
struct GuardedRustPromiseAwaiter: ExecutorGuarded<RustPromiseAwaiter> {
  // We need to inherit constructors or else placement-new will try to aggregate-initialize us.
  using ExecutorGuarded<RustPromiseAwaiter>::ExecutorGuarded;

  bool poll(const WakerRef& waker) {
    return get().poll(waker);
  }
  bool pollWithPollWaker(const WakerRef& waker, const PollWaker& pollWaker) {
    return get().poll(waker, pollWaker);
  }
  OwnPromiseNode take_own_promise_node() {
    return get().take_own_promise_node();
  }
};

void guarded_rust_promise_awaiter_new_in_place(
    GuardedRustPromiseAwaiter*, OptionWaker*, OwnPromiseNode);
void guarded_rust_promise_awaiter_drop_in_place(GuardedRustPromiseAwaiter*);

// =======================================================================================
// FuturePollEvent

// Base class for `FutureAwaiter<F>`. `FutureAwaiter<F>` implements the type-specific
// `Event::fire()` override which actually polls the Future; this class implements all other base
// class virtual functions.
//
// A FuturePollEvent owns an optional FutureWakerCell (handed out to Rust by PollWaker::cloneCell())
// and a list of zero or more RustPromiseAwaiters. These "sub-Promise awaiters" all wrap a KJ
// Promise of some sort, and arrange to arm the FuturePollEvent when their Promises become ready; a
// woken FutureWakerCell arms it the same way.
//
// The PromiseNode base class is a hack to implement async tracing. That is, we only implement the
// `tracePromise()` function, and decide which Promise to trace into if/when the coroutine calls our
// `tracePromise()` implementation. This primarily makes the lifetimes easier to manage: our
// weakly-linked RustPromiseAwaiter leaves have independent lifetimes from the FuturePollEvent, so
// we mustn't leave references to them, or their members, lying around in the Coroutine class.
class FuturePollEvent: public kj::_::PromiseNode, public kj::_::Event {
 public:
  FuturePollEvent(kj::SourceLocation location = {});
  ~FuturePollEvent() noexcept(false);

  // -------------------------------------------------------
  // PromiseNode API
  //
  // HACK: We only implement this interface for `tracePromise()`, which is the only function
  // CoroutineBase uses on its `promiseNodeForTrace` reference.

  void tracePromise(kj::_::TraceBuilder& builder, bool stopAtNextEvent) override;

 private:
  // Hand out a new strong reference to this event's FutureWakerCell. Used by
  // PollWaker::cloneCell(). The cell is created eagerly with this event (so even the borrowed
  // per-poll waker can be cloned or woken from any thread) and lives until both this event and
  // every Rust reference are gone; ~FuturePollEvent neutralizes it so late wakes no-op.
  friend class PollWaker;
  kj::Arc<FutureWakerCell> cloneWakerCell();
  const FutureWakerCell& wakerCellRef() {
    return *wakerCell.cell;
  }

  // (Re-)install a fresh cross-thread fulfiller in the cell if the previous one was consumed by
  // a foreign-thread wake (or never existed). The promise side is chained to arm this event.
  // Owning thread only; called at construction and at the top of each poll (PollWaker's
  // constructor) — never from within the consumed promise's own continuation, which would
  // destroy the very chain the continuation is running from.
  void ensureCrossThreadWakeArmed();

  // Weakly-linked list of the RustPromiseAwaiters ("leaves") this Future is currently `.await`ing
  // and which may arm this poll event when their Promises become ready. Severed on destruction so
  // a leaf that outlives us never arms a freed event.
  friend class RustPromiseAwaiter;
  kj::List<RustPromiseAwaiter, &RustPromiseAwaiter::link> leaves;

  // The FutureWakerCell handed out by cloneWakerCell(). We hold one strong reference through
  // this guard, whose destructor neutralizes the cell (nulling its weak Event link so retained
  // Rust references become safe no-ops) before releasing it — the invalidation is tied to this
  // event's destruction structurally, not by a destructor body remembering to call it.
  struct NeutralizeGuard {
    kj::Arc<FutureWakerCell> cell;
    ~NeutralizeGuard() noexcept(false) {
      if (cell.get() != nullptr) {
        cell->neutralize();
      }
    }
  };
  NeutralizeGuard wakerCell;

  // The promise side of the cell's cross-thread fulfiller: resolves when a foreign-thread wake
  // fulfills it, and its continuation arms this event from the owning thread. Renewed by
  // ensureCrossThreadWakeArmed(). Declared after `wakerCell` so it is destroyed first (its
  // continuation captures `this`); once it dies with this event, fulfilling the cell's retained
  // fulfiller becomes a safe no-op — the cross-thread flavor of neutralization.
  kj::Promise<void> crossThreadWakePromise = nullptr;
};

// =======================================================================================
// FutureAwaiter

template <typename F>
concept Future = requires(F f) {
  typename F::Output;
  {
    f.poll(kj::instance<const PollWaker&>(),
        kj::instance<typename ::kj::_::ExceptionOr<typename F::Output>&>())
  } -> std::same_as<void>;
};

// FutureAwaiter<T> is a Future poll() Event, and is the inner implementation of our co_await
// syntax. It wraps a Future and captures a reference to its enclosing KJ coroutine, arranging
// to continuously call `Future::poll()` on the KJ event loop until the Future produces a
// result, after which it arms the enclosing KJ coroutine's Event.
//
// Cancellation: Destroying the FutureAwaiter drops the Rust Future, which transitively drops
// any sub-Futures and their OwnPromiseNodes, cancelling the corresponding KJ sub-promises.
template <Future F>
class FutureAwaiter final: public FuturePollEvent {
 public:
  FutureAwaiter(F future, kj::SourceLocation location = {})
      : FuturePollEvent(location),
        future(kj::mv(future)) {}
  ~FutureAwaiter() noexcept(false) {}
  KJ_DISALLOW_COPY_AND_MOVE(FutureAwaiter);

  // -------------------------------------------------------
  // Event API

  void traceEvent(kj::_::TraceBuilder& builder) override {
    // Just defer to our enclosing Coroutine. It will immediately call our CoAwaitWaker's
    // `tracePromise()` implementation.
    onReadyEvent.traceEvent(builder);
  }

  void get(kj::_::ExceptionOrValue& output) noexcept override {
    output.as<typename F::Output>() = kj::mv(result);
  }

  void destroy() override {
    freePromise(this);
  }

  void onReady(kj::_::Event* event) noexcept override {
    onReadyEvent.init(event);
    poll();
  }

 private:
  void fire() override {
    poll();
  }

  // Poll the wrapped Future and arm the event if future is ready.
  void poll() {
    if (isDone()) return;

    {
      PollWaker pollWaker(*this);

      future.poll(pollWaker, result);
      if (isDone()) {
        onReadyEvent.arm();
      }
    }
  }

  bool isDone() const {
    return result.value != kj::none || result.exception != kj::none;
  }

  F::ExceptionOrValue result;
  F future;
  OnReadyEvent onReadyEvent;
};

}  // namespace kj_rs
