#include <workerd/rust/async/lib.rs.h>
#include <workerd/rust/async/await.h>
#include <workerd/rust/async/future.h>
#include <workerd/rust/async/waker.h>

#include <kj/test.h>

namespace workerd::rust::async {
namespace {

class TestCoroutineEvent: public kj::_::Event {
public:
  TestCoroutineEvent(kj::SourceLocation location = {})
      : Event(location) {}
  kj::Maybe<kj::Own<kj::_::Event>> fire() override {
    KJ_UNIMPLEMENTED("nope");
  }
  void traceEvent(kj::_::TraceBuilder& builder) override {}
};

class TestFuturePoller: public FuturePollerBase {
public:
  TestFuturePoller(kj::_::Event& next, kj::SourceLocation location = {})
      : FuturePollerBase(next, result, location) {}

  kj::Maybe<kj::Own<kj::_::Event>> fire() override {
    fired = true;
    return kj::none;
  }

  bool fired = false;
  kj::_::ExceptionOr<kj::_::Void> result;
};

KJ_TEST("BoxFutureVoid: C++ can poll() Rust Futures") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  // Poll a Future which returns Pending.
  {
    TestCoroutineEvent coroutineEvent;
    TestFuturePoller futurePoller{coroutineEvent};
    RootWaker waker{futurePoller};

    auto pending = new_pending_future_void();
    KJ_EXPECT(!pending.poll(waker));

    // The pending future never calls Waker::wake() because it has no intention to ever wake us up.
    // Additionally, it never even calls `waker.clone()`, so we have no promise at all.
    auto state = waker.reset();
    KJ_EXPECT(state.wakeCount == 0);
    KJ_EXPECT(state.cloned == kj::none);
  }

  // Poll a Future which returns Ready(()).
  {
    TestCoroutineEvent coroutineEvent;
    TestFuturePoller futurePoller{coroutineEvent};
    RootWaker waker{futurePoller};

    auto ready = new_ready_future_void();
    KJ_EXPECT(ready.poll(waker));

    // The ready future never calls Waker::wake() because it instead indicates immediate
    // readiness by its return value. Additionally, it never even calls `waker.clone()`, so we have no
    // promise at all.
    auto state = waker.reset();
    KJ_EXPECT(state.wakeCount == 0);
    KJ_EXPECT(state.cloned == kj::none);
  }

  // Poll a Future which returns Pending and immediately calls the Waker.
  {
    TestCoroutineEvent coroutineEvent;
    TestFuturePoller futurePoller{coroutineEvent};
    RootWaker waker{futurePoller};

    auto waking = new_waking_future_void();
    KJ_EXPECT(!waking.poll(waker));

    // The waking future immediately called wake_by_ref() on the LazyWaker. This incremented our
    // count, but didn't populate a promise.
    auto state = waker.reset();
    KJ_EXPECT(state.wakeCount == 1);
    KJ_EXPECT(state.cloned == kj::none);
  }

  // Poll a Future which clones the Waker on a different thread, then spawns a new thread to wake
  // the waker after a delay.
  {
    TestCoroutineEvent coroutineEvent;
    TestFuturePoller futurePoller{coroutineEvent};
    RootWaker waker{futurePoller};

    auto waking = new_threaded_delay_future_void();
    KJ_EXPECT(!waking.poll(waker));

    auto state = waker.reset();
    KJ_EXPECT(state.wakeCount == 0);
    KJ_EXPECT(state.cloned != kj::none);

    KJ_EXPECT(!KJ_ASSERT_NONNULL(state.cloned).promise.poll(waitScope));
    KJ_ASSERT_NONNULL(state.cloned).promise.wait(waitScope);
  }
}

// TODO(now): Test changing which RootWaker is passed to a LazyRustPromiseAwaiter::poll().

KJ_TEST("FutureAwaiter: C++ KJ coroutines can co_await Rust Futures") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  []() -> kj::Promise<void> {
    co_await new_ready_future_void();
    co_await new_waking_future_void();
  }().wait(waitScope);
}

KJ_TEST("OwnPromiseNode: Rust can wait for KJ promises") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  []() -> kj::Promise<void> {
    co_await new_layered_ready_future_void();
  }().wait(waitScope);
}

}  // namespace
}  // namespace workerd::rust::async
