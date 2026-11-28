#include "kj-rs-demo/lib.rs.h"
#include "kj-rs-demo/test-promises.h"

#include <signal.h>

#include <kj/async.h>
#include <kj/function.h>
#include <kj/test.h>
#include <kj/thread.h>

namespace kj_rs_demo {
namespace {

void onOtherEventLoop(kj::Function<void()> func) {
  kj::Thread thread([&]() {
    kj::EventLoop loop;
    kj::WaitScope waitScope(loop);
    func();
  });
}

void onThreadWithoutEventLoop(kj::Function<void()> func) {
  kj::Thread thread([&]() { func(); });
}

KJ_TEST("a SendKjFuture resolves on its event loop") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  // A promise is ready only once the loop has run its first poll's event.
  auto ready = new_ready_send_kj_future();
  KJ_EXPECT(!poll_send_kj_future(*ready));
  waitScope.poll();
  KJ_EXPECT(poll_send_kj_future(*ready));

  auto fulfillable = new_fulfillable_send_kj_future();
  KJ_EXPECT(!poll_send_kj_future(*fulfillable));
  waitScope.poll();
  KJ_EXPECT(!poll_send_kj_future(*fulfillable));
  fulfill_stored_promise();
  waitScope.poll();
  KJ_EXPECT(poll_send_kj_future(*fulfillable));
}

KJ_TEST("a SendKjFuture fails to poll off its event loop, and polls on it after") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);
  auto future = new_pending_send_kj_future();

  // Before and after its first poll, which starts awaiting the promise.
  for (auto i KJ_UNUSED: kj::zeroTo(2)) {
    for (auto onOtherThread: {onOtherEventLoop, onThreadWithoutEventLoop}) {
      kj::Maybe<kj::Exception> exception;
      onOtherThread([&]() {
        exception = kj::runCatchingExceptions([&]() { poll_send_kj_future(*future); });
      });
      KJ_EXPECT(KJ_ASSERT_NONNULL(exception).getDescription().contains(
          "value used off its KJ event loop"));
    }
    KJ_EXPECT(!poll_send_kj_future(*future));
  }
}

KJ_TEST("a SendKjFuture dropped off its event loop aborts") {
  {
    kj::EventLoop loop;
    kj::WaitScope waitScope(loop);

    // Unpolled, and awaiting its promise.
    for (auto polled: {false, true}) {
      for (auto onOtherThread: {onOtherEventLoop, onThreadWithoutEventLoop}) {
        auto future = new_pending_send_kj_future();
        if (polled) {
          KJ_EXPECT(!poll_send_kj_future(*future));
        }
        KJ_EXPECT_SIGNAL(SIGABRT, { onOtherThread([&]() { auto dropped = kj::mv(future); }); });
      }
    }

    // With its promise settled, so that its event is armed on the loop.
    for (auto onOtherThread: {onOtherEventLoop, onThreadWithoutEventLoop}) {
      auto future = new_fulfillable_send_kj_future();
      KJ_EXPECT(!poll_send_kj_future(*future));
      fulfill_stored_promise();
      KJ_EXPECT_SIGNAL(SIGABRT, { onOtherThread([&]() { auto dropped = kj::mv(future); }); });
      waitScope.poll();
    }
  }

  // Even once the loop is gone.
  for (auto polled: {false, true}) {
    KJ_EXPECT_SIGNAL(SIGABRT, {
      kj::Maybe<rust::Box<SendKjFutureBox>> future;
      onOtherEventLoop([&]() {
        auto made = new_pending_send_kj_future();
        if (polled) {
          KJ_ASSERT(!poll_send_kj_future(*made));
        }
        future = kj::mv(made);
      });
      future = kj::none;
    });
  }
}

KJ_TEST("a SendKjFuture dropped on its event loop that disallows async destructors aborts") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);
  auto future = new_fulfillable_send_kj_future();
  KJ_EXPECT(!poll_send_kj_future(*future));
  KJ_EXPECT_SIGNAL(SIGABRT, {
    kj::DisallowAsyncDestructorsScope scope("test");
    auto dropped = kj::mv(future);
  });
  fulfill_stored_promise();
}

}  // namespace
}  // namespace kj_rs_demo
