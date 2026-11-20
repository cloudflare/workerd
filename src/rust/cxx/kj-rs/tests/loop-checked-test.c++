#include "kj-rs-demo/lib.rs.h"

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

// The description of the exception that `func` throws on a thread that `onOtherThread` runs.
kj::String exceptionOn(void (*onOtherThread)(kj::Function<void()>), kj::Function<void()> func) {
  kj::Maybe<kj::Exception> exception;
  onOtherThread([&]() { exception = kj::runCatchingExceptions([&]() { func(); }); });
  return kj::str(KJ_ASSERT_NONNULL(exception).getDescription());
}

KJ_TEST("a LoopChecked value is used only on its event loop") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);
  auto slot = new_loop_checked_slot(1);

  for (auto onOtherThread: {onOtherEventLoop, onThreadWithoutEventLoop}) {
    KJ_EXPECT(exceptionOn(onOtherThread, [&]() {
      loop_checked_get(*slot);
    }).contains("value used off its KJ event loop"));
    KJ_EXPECT(exceptionOn(onOtherThread, [&]() {
      loop_checked_set(*slot, 2);
    }).contains("value used off its KJ event loop"));
    KJ_EXPECT(exceptionOn(onOtherThread, [&]() {
      loop_checked_take(*slot);
    }).contains("off its event loop"));
  }

  loop_checked_set(*slot, 3);
  KJ_EXPECT(loop_checked_get(*slot) == 3);
  auto drops = loop_checked_drops();
  KJ_EXPECT(loop_checked_take(*slot) == 3);
  KJ_EXPECT(loop_checked_drops() == drops + 1);
}

KJ_TEST("a LoopChecked value is dropped on its event loop") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);
  auto drops = loop_checked_drops();
  new_loop_checked_slot(1);
  KJ_EXPECT(loop_checked_drops() == drops + 1);
}

KJ_TEST("a LoopChecked value dropped where async destructors are disallowed aborts") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);
  auto slot = new_loop_checked_slot(1);
  KJ_EXPECT_SIGNAL(SIGABRT, {
    kj::DisallowAsyncDestructorsScope disallow("test");
    auto dropped = kj::mv(slot);
  });

  auto drops = loop_checked_drops();
  {
    kj::DisallowAsyncDestructorsScope disallow("test");
    kj::AllowAsyncDestructorsScope allow;
    auto dropped = kj::mv(slot);
  }
  KJ_EXPECT(loop_checked_drops() == drops + 1);
}

KJ_TEST("a LoopChecked value dropped off its event loop aborts") {
  {
    kj::EventLoop loop;
    kj::WaitScope waitScope(loop);
    for (auto onOtherThread: {onOtherEventLoop, onThreadWithoutEventLoop}) {
      auto slot = new_loop_checked_slot(1);
      KJ_EXPECT_SIGNAL(SIGABRT, { onOtherThread([&]() { auto dropped = kj::mv(slot); }); });
    }
  }

  // Even once the loop is gone.
  KJ_EXPECT_SIGNAL(SIGABRT, {
    kj::Maybe<rust::Box<LoopCheckedSlot>> slot;
    onOtherEventLoop([&]() { slot = new_loop_checked_slot(1); });
    slot = kj::none;
  });
}

KJ_TEST("a LoopChecked future is polled only on its event loop") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);
  auto future = new_loop_checked_future(42);

  for (auto onOtherThread: {onOtherEventLoop, onThreadWithoutEventLoop}) {
    KJ_EXPECT(exceptionOn(onOtherThread, [&]() {
      poll_loop_checked_future(*future);
    }).contains("value used off its KJ event loop"));
  }

  KJ_EXPECT(poll_loop_checked_future(*future) == -1);
  waitScope.poll();
  KJ_EXPECT(poll_loop_checked_future(*future) == 42);
}

}  // namespace
}  // namespace kj_rs_demo
