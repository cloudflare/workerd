#include "kj-rs-demo/lib.rs.h"
#include "kj-rs-demo/test-promises.h"
#include "kj-rs/awaiter.h"
#include "kj-rs/future.h"
#include "kj-rs/waker.h"

#include <sys/types.h>

#include <kj/test.h>
#include <kj/thread.h>

#include <csignal>

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
  // runs while the RustPromiseAwaiter has no linked FuturePollEvent and no stored Waker clone.
  // fire() handles this gracefully: it finds neither wake path, sets `done`, and does nothing, so
  // coroutine B's poll() sees the promise is ready.
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

KJ_TEST("A foreign wake of a dead retained waker never reaches the loop") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  // Contrast, alive case: a foreign wake of a live retained waker travels to the loop (through
  // the loop's CrossThreadWakeSink), which is observable as at least one event turned by poll().
  {
    auto promise = new_threaded_delay_future_void();
    KJ_EXPECT(!promise.poll(waitScope));
    KJ_EXPECT(waitScope.poll() == 0);
    wake_stashed_waker_from_background_thread();
    KJ_EXPECT(waitScope.poll() > 0);
    promise.wait(waitScope);
  }

  // Dead case: the future is gone, so the retained clone's cell is neutralized. A foreign wake
  // must be filtered out on the foreign thread (the cell's `alive` flag) rather than queued and
  // replayed as a no-op: the loop sees nothing at all.
  {
    auto promise = new_threaded_delay_future_void();
    KJ_EXPECT(!promise.poll(waitScope));
    { auto dropped = kj::mv(promise); }
    KJ_EXPECT(waitScope.poll() == 0);
    wake_stashed_waker_from_background_thread();
    KJ_EXPECT(waitScope.poll() == 0);
  }
}

KJ_TEST("Cross-thread wakes survive cancelAllDetached(): queued, then replayed by the restarted "
        "drain") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  // A future parked on a retained waker; the loop's cross-thread drain is running.
  auto promise = new_threaded_delay_future_void();
  KJ_EXPECT(!promise.poll(waitScope));

  // KJ's documented hack kills every detached task on the loop, our drain included.
  waitScope.cancelAllDetached();

  // The foreign wake now has no drain to fire: it must be queued, not lost.
  wake_stashed_waker_from_background_thread();
  KJ_EXPECT(waitScope.poll() == 0);  // nothing reached the loop yet

  // The next bridged-future activity on this loop (here: creating one) restarts the drain, whose
  // first arm finds the queue non-empty and fires immediately, replaying the wake.
  new_ready_future_void().wait(waitScope);
  KJ_EXPECT(waitScope.poll() > 0);
  KJ_EXPECT(promise.poll(waitScope));
  promise.wait(waitScope);
}

KJ_TEST("Cross-thread wakes survive cancelAllDetached(): re-polling any bridged future also "
        "restarts the drain") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  // `other` is a bridged future awaiting a KJ promise the test can fulfill: fulfilling it arms
  // the future's event same-thread, so the next turn re-polls the Rust future -- without any new
  // cell being created on the loop.
  auto other = new_await_fulfillable_promise_future_void();
  KJ_EXPECT(!other.poll(waitScope));
  auto parked = new_threaded_delay_future_void();
  KJ_EXPECT(!parked.poll(waitScope));

  waitScope.cancelAllDetached();
  wake_stashed_waker_from_background_thread();  // queued: no drain to fire
  KJ_EXPECT(waitScope.poll() == 0);

  // Re-polling `other` (PollWaker construction) restarts the drain, which replays the wake.
  fulfill_stored_promise();
  KJ_EXPECT(waitScope.poll() > 0);
  other.wait(waitScope);
  parked.wait(waitScope);
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

// =======================================================================================
// Multithreaded stress tests
//
// These exist for ThreadSanitizer (`bazel test --config=tsan ...`, which instruments both the
// C++ and the Rust side of the bridge): they run the cross-thread machinery under genuine
// concurrency so TSAN can watch every racing access pair. They are also meaningful under ASAN
// and in normal runs (they must complete and not crash), just less exhaustively.

KJ_TEST("Wake storm: many threads waking one waker concurrently across many polls") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  // 4 foreign threads x 250 wakes each, racing one another, the owner's per-poll cross-thread
  // fulfiller renewal, and same-turn re-polls. The future completes only after every wake has
  // been observed; a lost wake hangs this wait().
  new_wake_storm_future_void(4, 250).wait(waitScope);
}

KJ_TEST("Clone storm: many threads cloning and dropping one waker concurrently") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  // Pure atomic-refcount churn on the waker cell from 4 foreign threads x 1000 clone/drop
  // pairs, plus the final cross-thread wakes. Any refcount race corrupts the cell and shows up
  // here (use-after-free / double-free under ASAN, racing accesses under TSAN).
  new_clone_storm_future_void(4, 1000).wait(waitScope);
}

KJ_TEST("Wake storm racing future destruction") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  {
    // The future stashes 4 waker clones into a global and stays Pending forever.
    auto promise = new_stash_wakers_future_void(4);
    KJ_EXPECT(!promise.poll(waitScope));

    // Foreign threads hammer those clones with wakes...
    start_wake_storm(4, 2000);

    // ...give the storm real overlap with a live event (some wakes arm it; we keep polling)...
    for (int i = 0; i < 10; i++) {
      KJ_EXPECT(!promise.poll(waitScope));
    }

    // ...then destroy the future MID-STORM. ~FutureAwaiter runs its neutralize path on this
    // thread while foreign threads are inside wakeByRef() on the same cell. This is the exact
    // owner-destruction-vs-foreign-wake race the cell's design must absorb.
  }

  join_wake_storm();

  // The stashed clones now hold the last references to the neutralized cell; drop them on a
  // foreign thread, so cell destruction itself happens off the owning thread.
  clear_stashed_wakers_on_background_thread();
}

KJ_TEST("Waker woken from a thread running a DIFFERENT KJ event loop") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  auto promise = new_shared_stash_waker_future_void();
  KJ_EXPECT(!promise.poll(waitScope));

  // Wake from a thread that HAS a live KJ event loop — just not ours. The waker must recognize
  // that thread's executor is foreign (executor identity, not merely has-a-loop) and take the
  // cross-thread delivery path; arming the event directly from there would corrupt our loop.
  {
    // Scoped so the thread is joined (and the wake has happened) before we wait.
    kj::Thread otherLoopThread([]() noexcept {
      kj::EventLoop otherLoop;
      kj::WaitScope otherWaitScope(otherLoop);
      wake_shared_stashed_waker();
    });
  }

  promise.wait(waitScope);
}

// =======================================================================================
// Teardown-ordering tests (ASAN targets)

KJ_TEST("Retained waker wakes and drops quietly after the event loop itself is destroyed") {
  {
    kj::EventLoop loop;
    kj::WaitScope waitScope(loop);

    auto promise = new_shared_stash_waker_future_void();
    KJ_EXPECT(!promise.poll(waitScope));

    // `promise` (and its FuturePollEvent) dies here, then the loop itself.
  }

  // No KJ event loop exists on this thread anymore, but a waker clone is still stashed. Waking
  // it must be a quiet no-op — the cell's owned Executor reports not-current, and fulfilling the
  // long-dead cross-thread fulfiller is a no-op — and consuming it drops the last cell reference
  // with no loop anywhere in sight. Under ASAN this is the canonical
  // wake-after-everything-died regression test.
  wake_shared_stashed_waker();
}

KJ_TEST("Stashed PromiseFuture dropped after event loop destruction") {
  {
    kj::EventLoop loop;
    kj::WaitScope waitScope(loop);

    // Poll once and stash: the RustPromiseAwaiter is linked to this coroutine's FuturePollEvent
    // and holds a live OwnPromiseNode.
    []() -> kj::Promise<void> { co_await poll_and_stash_promise_future(); }().wait(waitScope);

    // The coroutine's FuturePollEvent died with the coroutine; the loop dies at scope end.
  }

  // Drop the stashed future with NO event loop on this thread: ~GuardedRustPromiseAwaiter's
  // teardown-tolerant destructor must let destruction (including cancelling the inner promise
  // node) proceed quietly. This is the same shape as tokio drop glue running after ~EventLoop.
  drop_stashed_future();
}

// =======================================================================================
// Stored-Waker semantics (the generic, non-KJ-waker poll path)

KJ_TEST("RustPromiseAwaiter stores, reuses, replaces, and drops Waker clones exactly") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  // The Rust side asserts Arc strong counts around every transition: store on first poll,
  // will_wake-based reuse on re-poll with the same waker, drop-and-replace on a new waker, and
  // drop-on-destruction. See new_waker_reuse_and_replace_future_void in test_futures.rs.
  []() -> kj::Promise<void> { co_await new_waker_reuse_and_replace_future_void(); }().wait(
           waitScope);
}

KJ_TEST("Cloned waker dropped without ever waking") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  // Clone the waker (same-thread and via a foreign thread) and then never wake: the clone is
  // simply dropped. The future stays Pending forever, so cancel it by dropping the promise —
  // the retained-then-dropped clone must not keep anything alive or dangle.
  {
    auto promise = new_waking_future_void(CloningAction::CloneSameThread, WakingAction::None);
    KJ_EXPECT(!promise.poll(waitScope));
  }
  {
    auto promise = new_waking_future_void(CloningAction::CloneBackgroundThread, WakingAction::None);
    KJ_EXPECT(!promise.poll(waitScope));
  }
}

// =======================================================================================
// Coverage-gap tests: bridge branches not reached by anything above.

KJ_TEST("Retained waker woken on the owning thread after future destruction") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  // The same-thread counterpart of the cross-thread "after destruction" case: the future stashes
  // a waker clone, dies, and the clone is woken from the loop's own thread. The cell's
  // neutralized event link must make this a no-op (ASAN: never arms the freed event).
  {
    auto promise = new_threaded_delay_future_void();
    KJ_EXPECT(!promise.poll(waitScope));
  }
  wake_delayed_future();
  // A quiet loop turn proves nothing got armed behind our back.
  kj::evalLater([]() {}).wait(waitScope);
}

KJ_TEST("Wake arriving after completion, before consumption, is a no-op") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  // The future stashes a waker clone and completes on its very first (eager) poll. Waking the
  // stash now arms a FuturePollEvent whose future is already done; FutureAwaiter::poll() must
  // return early via isDone() rather than poll the completed future again (which panics for
  // `async fn` futures -- and would surface here as an exception from wait()).
  auto promise = new_stash_then_ready_future_void();
  KJ_EXPECT(promise.poll(waitScope));
  wake_delayed_future();
  kj::evalLater([]() {}).wait(waitScope);
  promise.wait(waitScope);

  // Same, with the stray wake coming from a foreign thread.
  auto promise2 = new_stash_then_ready_future_void();
  KJ_EXPECT(promise2.poll(waitScope));
  wake_stashed_waker_from_background_thread();
  kj::evalLater([]() {}).wait(waitScope);
  promise2.wait(waitScope);
}

KJ_TEST("Same-thread wake storm inside a single poll coalesces into one re-poll") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  // 10,000 wake_by_ref() calls on the borrowed waker during one poll: Event::armDepthFirst() is
  // idempotent within a turn, so this must complete in one re-poll and never blow up the queue.
  auto promise = new_sync_wake_storm_future_void(10000);
  KJ_EXPECT(promise.poll(waitScope));
  promise.wait(waitScope);
}

#if !_WIN32
KJ_TEST("a panic in a bridged future's Drop aborts deterministically (no unwind into C++)") {
  // RustFuture::drop_in_place has no error channel -- it runs from C++ destructors and
  // cancellation paths -- so a panicking Drop is converted into a labeled abort via
  // cxx::private::prevent_unwind rather than unwinding across the FFI frame. Forked death test.
  KJ_EXPECT_SIGNAL(SIGABRT, {
    kj::EventLoop loop;
    kj::WaitScope waitScope(loop);
    auto promise = new_panic_on_drop_future_void();
    KJ_EXPECT(!promise.poll(waitScope));
    promise = nullptr;  // cancels: drops the Rust future, whose Drop panics
  });
}
#endif

KJ_TEST("Async tracing reaches through the bridge into the awaited KJ promise") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  // A Rust future parked on a never-resolving KJ promise. kj::Promise::trace() walks
  // FuturePollEvent::tracePromise() -> leaves.front() -> RustPromiseAwaiter::tracePromise(),
  // none of which any other test executes. We only require that it runs and produces a trace;
  // the exact addresses are meaningless.
  auto promise = new_await_pending_promise_future_void();
  KJ_EXPECT(!promise.poll(waitScope));
  kj::String trace = promise.trace();
  KJ_EXPECT(trace.size() > 0);

  // And with nothing linked (a Rust future awaiting nothing): tracePromise() takes its
  // empty-leaves branch, which contributes nothing -- the trace is legitimately empty. The point
  // is that the branch runs.
  auto pending = new_pending_future_void();
  KJ_EXPECT(!pending.poll(waitScope));
  (void)pending.trace();
}

KJ_TEST("RustPromiseAwaiter switches from the optimized to the generic path and fires it") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  // Linked under a co_await first, then re-polled with a custom (non-KJ) waker: the link must be
  // cleared and the custom waker stored, so that fulfilling the promise wakes THAT waker. The
  // Rust side asserts the custom waker was the one woken.
  auto promise = new_optimized_then_generic_future_void();
  KJ_EXPECT(!promise.poll(waitScope));
  fulfill_stored_promise();
  promise.wait(waitScope);
}

KJ_TEST("RustPromiseAwaiter switching from the generic to the optimized path drops the clone") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  // Asserted on the Rust side via the custom waker's Arc strong count.
  []() -> kj::Promise<void> {
    co_await new_generic_then_optimized_drops_clone_future_void();
  }().wait(waitScope);
}

KJ_TEST("KJ promise polled with a cloned KJ waker as Context completes via the cell") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  // The Context is built from a clone of the KJ waker rather than the borrowed PollWaker, so the
  // awaiter takes the generic path and stores a KJ cell waker; firing it must arm our own
  // event through the cell and complete the future.
  auto promise = new_cloned_kj_waker_context_future_void();
  KJ_EXPECT(!promise.poll(waitScope));
  fulfill_stored_promise();
  promise.wait(waitScope);
}

KJ_TEST("Cross-loop ping-pong: two loops on two threads wake each other for many rounds") {
  // Side 0 runs on this thread's loop, side 1 on a kj::Thread with its own loop. Each side wakes
  // the other's stashed waker on every poll, for 200 rounds: 400 cross-loop wakes taken while
  // both loops are live and racing, each of which the owning-executor check must route to the
  // right loop (a misrouted arm would corrupt a loop; a lost wake hangs this test).
  reset_ping_pong();
  kj::Thread side1([]() noexcept {
    kj::EventLoop loop;
    kj::WaitScope waitScope(loop);
    new_ping_pong_future_void(1, 200).wait(waitScope);
  });

  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);
  new_ping_pong_future_void(0, 200).wait(waitScope);
}

}  // namespace
}  // namespace kj_rs_demo
