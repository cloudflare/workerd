#include <workerd/rust/async/lib.rs.h>
#include <workerd/rust/async/awaiter.h>
#include <workerd/rust/async/future.h>
#include <workerd/rust/async/waker.h>

#include <kj/test.h>

namespace workerd::rust::async {
namespace {

KJ_TEST("LazyArcWaker: C++ can poll() Rust Futures") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  // Poll a Future which returns Pending.
  {
    LazyArcWaker waker;

    auto pending = new_pending_future_void();
    kj::_::ExceptionOr<kj::_::FixVoid<void>> result;
    KJ_EXPECT(!pending.poll(waker, result));
    KJ_EXPECT(result.value == kj::none);
    KJ_EXPECT(result.exception == kj::none);

    // The pending future never calls Waker::wake() because it has no intention to ever wake us up.
    // Additionally, it never even calls `waker.clone()`, so we have no promise at all.
    auto promise = waker.reset();
    KJ_EXPECT(promise == kj::none);
  }

  // Poll a Future which returns Ready(()).
  {
    LazyArcWaker waker;

    auto ready = new_ready_future_void();
    kj::_::ExceptionOr<kj::_::FixVoid<void>> result;
    KJ_EXPECT(ready.poll(waker, result));
    KJ_EXPECT(result.value != kj::none);
    KJ_EXPECT(result.exception == kj::none);

    // The ready future never calls Waker::wake() because it instead indicates immediate
    // readiness by its return value. Additionally, it never even calls `waker.clone()`, so we have no
    // promise at all.
    auto promise = waker.reset();
    KJ_EXPECT(promise == kj::none);
  }
}

KJ_TEST("LazyArcWaker: C++ can receive synchronous wakes during poll()") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  struct Actions {
    CloningAction cloningAction;
    WakingAction wakingAction;
  };

  // Poll a Future which immediately wakes the Waker with `wake_by_ref()` on various threads, then
  // returns Pending.
  for (auto testCase: std::initializer_list<Actions> {
    { CloningAction::None, WakingAction::WakeByRefSameThread },
    { CloningAction::None, WakingAction::WakeByRefBackgroundThread },
  }) {
    LazyArcWaker waker;

    auto waking = new_waking_future_void(testCase.cloningAction, testCase.wakingAction);
    kj::_::ExceptionOr<kj::_::FixVoid<void>> result;
    KJ_EXPECT(!waking.poll(waker, result));
    KJ_EXPECT(result.value == kj::none);
    KJ_EXPECT(result.exception == kj::none);

    // The waking future immediately called wake_by_ref() on the LazyArcWaker. This incremented our
    // count, meaning `reset()` returns an immediately-ready promise.
    auto promise = KJ_ASSERT_NONNULL(waker.reset());
    KJ_EXPECT(promise.poll(waitScope));
    promise.wait(waitScope);
  }

  for (auto testCase: std::initializer_list<Actions> {
    { CloningAction::CloneSameThread, WakingAction::WakeByRefSameThread },
    { CloningAction::CloneSameThread, WakingAction::WakeByRefBackgroundThread },
    { CloningAction::CloneBackgroundThread, WakingAction::WakeByRefSameThread },
    { CloningAction::CloneBackgroundThread, WakingAction::WakeByRefBackgroundThread },
    { CloningAction::CloneSameThread, WakingAction::WakeSameThread },
    { CloningAction::CloneSameThread, WakingAction::WakeBackgroundThread },
    { CloningAction::CloneBackgroundThread, WakingAction::WakeSameThread },
    { CloningAction::CloneBackgroundThread, WakingAction::WakeBackgroundThread },
  }) {
    LazyArcWaker waker;

    auto waking = new_waking_future_void(testCase.cloningAction, testCase.wakingAction);
    kj::_::ExceptionOr<kj::_::FixVoid<void>> result;
    KJ_EXPECT(!waking.poll(waker, result));
    KJ_EXPECT(result.value == kj::none);
    KJ_EXPECT(result.exception == kj::none);

    auto promise = KJ_ASSERT_NONNULL(waker.reset());

    // We expect our cloned ArcWaker promise to be immediately ready.
    KJ_EXPECT(promise.poll(waitScope));
    promise.wait(waitScope);
  }

  // Test wake_by_ref()-before-clone().
  for (auto testCase: std::initializer_list<Actions> {
    { CloningAction::WakeByRefThenCloneSameThread, WakingAction::WakeSameThread },
  }) {
    LazyArcWaker waker;

    auto waking = new_waking_future_void(testCase.cloningAction, testCase.wakingAction);
    kj::_::ExceptionOr<kj::_::FixVoid<void>> result;
    KJ_EXPECT(!waking.poll(waker, result));
    KJ_EXPECT(result.value == kj::none);
    KJ_EXPECT(result.exception == kj::none);

    auto promise = KJ_ASSERT_NONNULL(waker.reset());
    KJ_EXPECT(promise.poll(waitScope));
    promise.wait(waitScope);
  }

  // Test no calls to `wake*()`.
  for (auto testCase: std::initializer_list<Actions> {
    // Note: the None, None case is covered by `new_pending_future_void()`.
    { CloningAction::CloneSameThread, WakingAction::None },
  }) {
    LazyArcWaker waker;

    auto waking = new_waking_future_void(testCase.cloningAction, testCase.wakingAction);
    kj::_::ExceptionOr<kj::_::FixVoid<void>> result;
    KJ_EXPECT(!waking.poll(waker, result));
    KJ_EXPECT(result.value == kj::none);
    KJ_EXPECT(result.exception == kj::none);

    auto promise = KJ_ASSERT_NONNULL(waker.reset());
    KJ_EXPECT(!promise.poll(waitScope));
  }
}

KJ_TEST("LazyArcWaker: C++ can receive asynchronous wakes after poll()") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  // Poll a Future which clones the Waker on a different thread, then spawns a new thread to wake
  // the waker after a delay.
  {
    LazyArcWaker waker;

    auto waking = new_threaded_delay_future_void();
    kj::_::ExceptionOr<kj::_::FixVoid<void>> result;
    KJ_EXPECT(!waking.poll(waker, result));
    KJ_EXPECT(result.value == kj::none);
    KJ_EXPECT(result.exception == kj::none);

    auto promise = KJ_ASSERT_NONNULL(waker.reset());
    // It's not ready yet.
    KJ_EXPECT(!promise.poll(waitScope));
    // But later it is.
    promise.wait(waitScope);
  }
}

KJ_TEST("CoAwaitWaker: C++ KJ coroutines can co_await Rust Futures") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  []() -> kj::Promise<void> {
    co_await new_ready_future_void();
    co_await new_waking_future_void(CloningAction::None, WakingAction::WakeByRefSameThread);
  }().wait(waitScope);
}

KJ_TEST("RustPromiseAwaiter: Rust can .await KJ promises under a co_await") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  []() -> kj::Promise<void> {
    co_await new_layered_ready_future_void();
  }().wait(waitScope);
}

KJ_TEST("RustPromiseAwaiter: Rust can poll() multiple promises under a single co_await") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  []() -> kj::Promise<void> {
    co_await new_naive_select_future_void();
  }().wait(waitScope);
}

// TODO(now): Similar to "Rust can poll() multiple promises ...", but poll() until all are ready.

// TODO(now): Test polling a Promise from Rust with multiple LazyArcWakers.
//   Need a function which:
//   - Creates an OwnPromiseNode which is fulfilled manually.
//   - Wraps OwnPromiseNode::into_future() in BoxFuture.
//   - Passes the BoxFuture to a new KJ coroutine.
//   - The KJ coroutine passes the BoxFuture to a Rust function returning NaughtyFuture.
//   - The coroutine co_awaits the NaughtyFuture.
//   - The NaughtyFuture polls the BoxFuture once and returns Ready(BoxFuture).
//   - The coroutine co_returns the BoxFuture to the local function here.
//   - The BoxFuture has now outlived the coroutine which polled it last.
//   - Fulfill the OwnPromiseNode. Should not segfault.
//   - Pass the OwnPromiseNode to a new Rust Future somehow, .await it.

KJ_TEST("RustPromiseAwaiter: Rust can poll() KJ promises with non-KJ Wakers") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  []() -> kj::Promise<void> {
    co_await new_wrapped_waker_future_void();
  }().wait(waitScope);
}

KJ_TEST("co_awaiting a BoxFuture<Fallible<T>> from C++ can throw") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  []() -> kj::Promise<void> {
    kj::Maybe<kj::Exception> maybeException;
    try {
      co_await new_errored_future_fallible_void();
    } catch (...) {
      maybeException = kj::getCaughtExceptionAsKj();
    }
    auto& exception = KJ_ASSERT_NONNULL(maybeException, "should have thrown");
    KJ_EXPECT(exception.getDescription() == "std::exception: test error");
  }().wait(waitScope);
}

KJ_TEST(".awaiting a Promise<T> from Rust can produce an Err Result") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  []() -> kj::Promise<void> {
    co_await new_error_handling_future_void();
  }().wait(waitScope);
}

KJ_TEST("Rust can await Promise<int32_t>") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  []() -> kj::Promise<void> {
    co_await new_awaiting_future_i32();
  }().wait(waitScope);
}

KJ_TEST("C++ can await BoxFuture<i32>") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  []() -> kj::Promise<void> {
    KJ_EXPECT(co_await new_ready_future_fallible_i32(123) == 123);
  }().wait(waitScope);
}

// TODO(now): More test cases.
//   - Standalone ArcWaker tests. Ensure Rust calls ArcWaker destructor when we expect.
//   - Ensure Rust calls PromiseNode destructor from LazyRustPromiseAwaiter.
//   - Throwing an exception from PromiseNode functions, including destructor.

}  // namespace
}  // namespace workerd::rust::async
