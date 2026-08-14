#include "kj-rs-demo/lib.rs.h"
#include "kj-rs-demo/test-promises.h"
#include "kj-rs/awaiter.h"
#include "kj-rs/future.h"
#include "kj-rs/waker.h"

#include <sys/types.h>

#include <kj/test.h>

// Raw-RustFuture test helpers, defined in tests/lib.rs and tests/test_futures.rs. The
// bridge's generated `async fn` shims always apply RustFuture's eager-by-default
// kj::Promise conversion, so tests that need a *cold* promise receive the not-yet-converted
// RustFuture through these and call `.lazily()` (kj-rs/future.h) themselves.
extern "C" {
void kj_rs_demo_lazy_side_effect_future(::kj_rs::repr::RustFuture* out);
void kj_rs_demo_lazy_future_awaiting_cancellable_promise(::kj_rs::repr::RustFuture* out);
void kj_rs_demo_work_before_poll(uint64_t* target, ::kj_rs::repr::RustFuture* out);
}

namespace kj_rs_demo {
namespace {

KJ_TEST("polling pending future") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  kj::Promise<void> promise = new_pending_future_void();
  KJ_EXPECT(!promise.poll(waitScope));
}

KJ_TEST("C++ KJ coroutine can co_await rust ready void future") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  []() -> kj::Promise<void> { co_await new_ready_future_void(); }().wait(waitScope);
}

KJ_TEST("C++ KJ coroutines can co_await Rust Futures") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  []() -> kj::Promise<void> {
    co_await new_ready_future_void();
    co_await new_waking_future_void(CloningAction::None, WakingAction::WakeByRefSameThread);
  }().wait(waitScope);
}

KJ_TEST("c++ can receive synchronous wakes during poll()") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  struct Actions {
    CloningAction cloningAction;
    WakingAction wakingAction;
  };

  for (auto testCase: std::initializer_list<Actions>{
         {CloningAction::None, WakingAction::WakeByRefSameThread},
         {CloningAction::None, WakingAction::WakeByRefBackgroundThread},
         {CloningAction::CloneSameThread, WakingAction::WakeByRefSameThread},
         {CloningAction::CloneSameThread, WakingAction::WakeByRefBackgroundThread},
         {CloningAction::CloneBackgroundThread, WakingAction::WakeByRefSameThread},
         {CloningAction::CloneBackgroundThread, WakingAction::WakeByRefBackgroundThread},
         {CloningAction::CloneSameThread, WakingAction::WakeSameThread},
         {CloningAction::CloneSameThread, WakingAction::WakeBackgroundThread},
         {CloningAction::CloneBackgroundThread, WakingAction::WakeSameThread},
         {CloningAction::CloneBackgroundThread, WakingAction::WakeBackgroundThread},
         {CloningAction::WakeByRefThenCloneSameThread, WakingAction::WakeSameThread},
       }) {
    auto waking = new_waking_future_void(testCase.cloningAction, testCase.wakingAction);
    KJ_EXPECT(waking.poll(waitScope));
    waking.wait(waitScope);
  }
}

KJ_TEST("RustPromiseAwaiter: Rust can .await KJ promises under a co_await") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  []() -> kj::Promise<void> { co_await new_layered_ready_future_void(); }().wait(waitScope);
}

KJ_TEST("RustPromiseAwaiter: Rust can poll() multiple promises under a single "
        "co_await") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  []() -> kj::Promise<void> { co_await new_naive_select_future_void(); }().wait(waitScope);
}

// TODO(someday): Similar to "Rust can poll() multiple promises ...", but poll() until all are ready.

KJ_TEST("RustPromiseAwaiter: PromiseFuture survives coroutine death and re-links") {
  // A PromiseFuture (containing a RustPromiseAwaiter) is polled under coroutine A, linking the
  // RustPromiseAwaiter to A's FuturePollEvent. Coroutine A completes and is destroyed, severing
  // the link. The promise is fulfilled, then coroutine B polls the same PromiseFuture. The
  // RustPromiseAwaiter re-links to B's FuturePollEvent before fire() runs (because coroutine B's
  // initial poll is synchronous), so fire() arms B's FuturePollEvent normally.
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  // Phase 1: coroutine A polls the fulfillable promise once, stashes the PromiseFuture.
  []() -> kj::Promise<void> { co_await poll_and_stash_promise_future(); }().wait(waitScope);
  // Coroutine A and its FuturePollEvent are now destroyed.

  // Fulfill the promise. The RustPromiseAwaiter's fire() event is armed but hasn't run yet.
  fulfill_stored_promise();

  // Phase 2: coroutine B retrieves the stashed PromiseFuture and awaits it. B's initial poll
  // re-links the RustPromiseAwaiter to B's FuturePollEvent before the event loop turns.
  []() -> kj::Promise<void> { co_await unstash_and_await_promise_future(); }().wait(waitScope);
}

KJ_TEST("RustPromiseAwaiter: PromiseFuture survives coroutine death, fire() with no waker") {
  // Same scenario as above, but the event loop turns between fulfill and coroutine B, so fire()
  // runs while the RustPromiseAwaiter has no linked FuturePollEvent and an empty OptionWaker.
  // fire()'s wake_if_some() handles this gracefully; the KJ_DEFER sets maybeOptionWaker to
  // kj::none, so coroutine B's poll() sees the promise is ready.
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  []() -> kj::Promise<void> { co_await poll_and_stash_promise_future(); }().wait(waitScope);

  fulfill_stored_promise();

  // Force the event loop to turn, processing fire() before coroutine B polls.
  kj::evalLater([]() {}).wait(waitScope);

  []() -> kj::Promise<void> { co_await unstash_and_await_promise_future(); }().wait(waitScope);
}

KJ_TEST("RustPromiseAwaiter: Rust can poll() KJ promises with non-KJ Wakers") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  []() -> kj::Promise<void> { co_await new_wrapped_waker_future_void(); }().wait(waitScope);
}

KJ_TEST("co_awaiting a fallible future from C++ can throw") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  []() -> kj::Promise<void> {
    kj::Maybe<kj::Exception> maybeException;
    try {
      co_await new_errored_future_void();
    } catch (...) {
      maybeException = kj::getCaughtExceptionAsKj();
    }
    auto& exception = KJ_ASSERT_NONNULL(maybeException, "should have thrown");
    KJ_EXPECT(exception.getDescription() == "test error");
  }().wait(waitScope);
}

KJ_TEST("co_awaiting a KjError future from C++ can throw with proper exception type") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  []() -> kj::Promise<void> {
    kj::Maybe<kj::Exception> maybeException;
    try {
      co_await new_kj_errored_future_void();
    } catch (...) {
      maybeException = kj::getCaughtExceptionAsKj();
    }
    auto& exception = KJ_ASSERT_NONNULL(maybeException, "should have thrown");
    KJ_EXPECT(exception.getDescription() == "test error");
    KJ_EXPECT(exception.getType() == kj::Exception::Type::OVERLOADED);
  }().wait(waitScope);
}

KJ_TEST(".awaiting a Promise<T> from Rust can produce an Err Result") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  []() -> kj::Promise<void> { co_await new_error_handling_future_void_infallible(); }().wait(
           waitScope);
}

KJ_TEST("a panicking bridged async fn surfaces as a catchable kj::Exception, not an abort") {
  // Unwind protection in the RustFuture vtable (kj-rs/future.rs): panics escaping poll() are
  // converted into errored completions, mirroring the sync bridge's panic -> kj::Exception
  // conversion. Before that protection, any of these would abort the process (unwinding out
  // of an extern "C" fn).
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  {
    // Fallible future, panic on first poll.
    auto exception = KJ_ASSERT_NONNULL(
        kj::runCatchingExceptions([&]() { new_panicking_future_void().wait(waitScope); }));
    KJ_EXPECT(exception.getDescription().contains("bridged future panicked on purpose"),
        exception.getDescription());
  }

  {
    // Infallible future: the promise can still reject (kj::Promise<T> always carries an
    // exception channel even when the Rust signature is infallible).
    auto exception = KJ_ASSERT_NONNULL(kj::runCatchingExceptions(
        [&]() { new_panicking_infallible_future_void().wait(waitScope); }));
    KJ_EXPECT(exception.getDescription().contains("bridged infallible future panicked on purpose"),
        exception.getDescription());
  }

  {
    // Panic after a suspension point: exercises the event-loop-driven poll path (not the
    // eager creation-time poll).
    auto exception = KJ_ASSERT_NONNULL(kj::runCatchingExceptions(
        [&]() { new_panicking_after_await_future_void().wait(waitScope); }));
    KJ_EXPECT(exception.getDescription().contains("panicked after a suspension point"),
        exception.getDescription());
  }

  // A panicking bridged future can also be caught from a KJ coroutine.
  []() -> kj::Promise<void> {
    kj::Maybe<kj::Exception> maybeException;
    try {
      co_await new_panicking_future_void();
    } catch (...) {
      maybeException = kj::getCaughtExceptionAsKj();
    }
    auto& exception = KJ_ASSERT_NONNULL(maybeException, "should have thrown");
    KJ_EXPECT(exception.getDescription().contains("bridged future panicked on purpose"),
        exception.getDescription());
  }().wait(waitScope);

  // The loop is still healthy after the panics: run a normal future to completion.
  []() -> kj::Promise<void> { co_await new_ready_future_void(); }().wait(waitScope);
}

KJ_TEST("Rust can await Promise<int32_t>") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  []() -> kj::Promise<void> { co_await new_promise_i32_awaiting_future_void(); }().wait(waitScope);
}

KJ_TEST("C++ can await BoxFuture<i32>") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  []() -> kj::Promise<void> { KJ_EXPECT(co_await new_ready_future_i32(123) == 123); }().wait(
           waitScope);
}

KJ_TEST("C++ can receive asynchronous wakes after poll()") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  auto promise = new_threaded_delay_future_void();
  // It's not ready yet: the future stashed a clone of its waker and returned Pending.
  KJ_EXPECT(!promise.poll(waitScope));
  // Wake the stashed waker on the loop thread; this arms the FuturePollEvent so the next poll
  // completes. Exercises a cloned-waker wake that arrives after poll() has already returned.
  wake_delayed_future();
  promise.wait(waitScope);
}

KJ_TEST("Waker woken from another thread") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  // The future clones its waker into a spawned std::thread (one with no KJ event loop), which
  // wakes it from there ~10ms later. `std::task::Waker` is Send + Sync, so this is plain safe
  // Rust; the bridge must deliver the foreign-thread wake to this loop (waker.h's cross-thread
  // fulfiller path) rather than losing it -- this wait() hangs if it does.
  new_cross_thread_wake_future_void().wait(waitScope);

  // Again, but with the loop parked in wait() the whole time (no poll()-first warmup), so the
  // wake is guaranteed to arrive while the loop sleeps rather than racing the first poll.
  new_cross_thread_wake_future_void().wait(waitScope);
}

KJ_TEST("Waker woken from another thread across multiple polls") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  // Four rounds of pending -> foreign-thread wake -> re-poll. Each round consumes the cell's
  // cross-thread fulfiller, which the next poll must renew (waker.h); if renewal ever fails,
  // a later round's wake is lost and this wait() hangs.
  new_multi_round_cross_thread_wake_future_void().wait(waitScope);
}

KJ_TEST("Retained waker woken from another thread, before and after future destruction") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  {
    // Alive case: the future stashes a waker clone on first poll; wake it from a joined foreign
    // thread, after poll() has returned. The wake must be delivered (the wait completes).
    auto promise = new_threaded_delay_future_void();
    KJ_EXPECT(!promise.poll(waitScope));
    wake_stashed_waker_from_background_thread();
    promise.wait(waitScope);
  }

  {
    // Dead case: destroy the future (and its FuturePollEvent) while the stashed clone lives on,
    // then wake from a foreign thread. The neutralized cell must make this a safe no-op — under
    // ASan this is the use-after-free regression for waking a freed event cross-thread.
    auto promise = new_threaded_delay_future_void();
    KJ_EXPECT(!promise.poll(waitScope));
    { auto dropped = kj::mv(promise); }
    wake_stashed_waker_from_background_thread();
  }
}

KJ_TEST("Work before poll") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  uint64_t val = 0;
  // It should be possible for a Rust function to do work before returning the future
  // even if we don't poll or cancel it. The future panics if polled, so it is converted
  // with RustFuture::lazily() (the eager-by-default conversion polls at creation); this
  // also proves cold promises really are never polled unawaited.
  ::kj_rs::repr::RustFuture fut;
  kj_rs_demo_work_before_poll(&val, &fut);
  auto promise = fut.lazily<void>();
  KJ_EXPECT(val == 42);
}

// =======================================================================================
// Eager-by-default vs RustFuture::lazily(): bridged async fns surface as *eager*
// kj::Promises (polled to their first suspension at creation, like a KJ coroutine);
// `.lazily()` is the C++-side escape hatch that restores the cold future.

KJ_TEST("bridged async fns are eager by default: the body runs at promise creation") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  reset_side_effect_counter();
  {
    auto promise = new_side_effect_future_void();
    // No suspension points, so it ran to completion synchronously at creation, before any
    // await or event-loop turn.
    KJ_EXPECT(get_side_effect_counter() == 1);
  }
  KJ_EXPECT(get_side_effect_counter() == 1);
}

KJ_TEST("RustFuture::lazily() opts out: nothing runs until the promise is first awaited") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  reset_side_effect_counter();
  ::kj_rs::repr::RustFuture fut;
  kj_rs_demo_lazy_side_effect_future(&fut);
  auto promise = fut.lazily<void>();
  KJ_EXPECT(get_side_effect_counter() == 0);

  // Turning the event loop without awaiting the promise doesn't run it either.
  kj::evalLater([]() {}).wait(waitScope);
  KJ_EXPECT(get_side_effect_counter() == 0);

  promise.wait(waitScope);
  KJ_EXPECT(get_side_effect_counter() == 1);
}

KJ_TEST("eager promises still cancel on drop (never explicitly polled by the caller)") {
  // Cancellation semantics are unchanged by eager evaluation: dropping the promise
  // synchronously cancels the Rust future and the KJ promise it is awaiting. Here the
  // caller never polls or awaits — creation alone started the future (suspending it at its
  // .await), and destruction alone cancels it.
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  reset_cancellation_counter();
  {
    auto promise = new_future_awaiting_cancellable_promise();
    KJ_EXPECT(get_cancellation_counter() == 0);
  }
  KJ_EXPECT(get_cancellation_counter() == 1);
}

// TODO(someday): More test cases.
//   - Standalone FutureWakerCell tests. Ensure Rust drops cloned waker cells when we expect.
//   - Throwing an exception from PromiseNode functions, including destructor.

// =======================================================================================
// Cancellation tests
//
// In both KJ and Rust, dropping a promise/future synchronously cancels the underlying work. These
// tests verify that cancellation propagates correctly across the Rust/C++ async FFI boundary using
// a "cancellation-detecting promise" that increments a counter when destroyed.

KJ_TEST("Cancellation: drop never-polled Rust future") {
  // Dropping a kj::Promise wrapping a Rust future that was never polled should not crash. Since the
  // future was never polled, the Rust async function body was never entered, so no sub-promises
  // exist to cancel. Uses a raw RustFuture converted with `.lazily()`: eager-by-default promises
  // are always polled at least once (at creation), so only a cold promise can reach this path.
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  {
    ::kj_rs::repr::RustFuture fut;
    kj_rs_demo_lazy_future_awaiting_cancellable_promise(&fut);
    auto promise = fut.lazily<void>();
  }
}

KJ_TEST("Cancellation: C++ dropping promise cancels Rust future's awaited KJ promise") {
  // When C++ drops a kj::Promise wrapping a Rust future that is currently .awaiting a KJ promise,
  // the cancellation should propagate through the Rust future to the inner KJ promise.
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  reset_cancellation_counter();

  {
    auto promise = new_future_awaiting_cancellable_promise();
    // Poll once to enter the Rust async function and suspend at the .await.
    KJ_EXPECT(!promise.poll(waitScope));
    KJ_EXPECT(get_cancellation_counter() == 0);
  }

  KJ_EXPECT(get_cancellation_counter() == 1);
}

KJ_TEST("Cancellation: propagates to current .await point in multi-step Rust future") {
  // A Rust future that has completed one .await and is suspended at a second should only cancel the
  // second sub-promise. The first has already completed and been consumed.
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  reset_cancellation_counter();

  {
    auto promise = new_two_step_cancellable_future();
    // Poll until the first step (coroutine promise) completes and the future suspends at the
    // second step (cancellation-detecting promise).
    KJ_EXPECT(!promise.poll(waitScope));
    KJ_EXPECT(get_cancellation_counter() == 0);
  }

  KJ_EXPECT(get_cancellation_counter() == 1);
}

KJ_TEST("Cancellation: Rust select cancels losing branch's KJ promise") {
  // When a Rust select() resolves one branch, the other branch is dropped, which should cancel its
  // KJ promise. This tests Rust-internal cancellation propagating to KJ promises.
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  reset_cancellation_counter();

  []() -> kj::Promise<void> { co_await new_select_with_cancellation(); }().wait(waitScope);

  // The coroutine promise won the select, so the cancellation-detecting promise was dropped.
  KJ_EXPECT(get_cancellation_counter() == 1);
}

KJ_TEST("Cancellation: Rust dropping never-polled KJ promise future") {
  // When Rust creates a PromiseFuture (by calling a C++ async fn) but drops it without ever
  // polling, the OwnPromiseNode is dropped directly by Rust, cancelling the KJ promise.
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  reset_cancellation_counter();

  []() -> kj::Promise<void> { co_await new_drop_cancellable_promise_without_polling(); }().wait(
           waitScope);

  KJ_EXPECT(get_cancellation_counter() == 1);
}

}  // namespace
}  // namespace kj_rs_demo