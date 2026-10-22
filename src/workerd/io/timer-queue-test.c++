// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "io-channels.h"
#include "timer-queue.h"

#include <kj/test.h>
#include <kj/timer.h>

namespace workerd {
namespace {

class QueueTimer final: public TimerChannel {
 public:
  void syncTime() override {}

  kj::Date now(kj::Maybe<kj::Date> clamp) override {
    auto time = kj::UNIX_EPOCH + (timer.now() - kj::origin<kj::TimePoint>());
    return clamp.map([&](kj::Date deadline) { return kj::min(time, deadline); }).orDefault(time);
  }

  kj::Promise<void> atTime(kj::Date when) override {
    if (failNext) {
      failNext = false;
      KJ_FAIL_REQUIRE("injected arming failure");
    }
    if (when <= now(kj::none)) return kj::READY_NOW;
    return timer.atTime(kj::origin<kj::TimePoint>() + (when - kj::UNIX_EPOCH));
  }

  kj::Promise<void> afterLimitTimeout(kj::Duration) override {
    return kj::NEVER_DONE;
  }

  void advanceTo(uint milliseconds) {
    timer.advanceTo(kj::origin<kj::TimePoint>() + milliseconds * kj::MILLISECONDS);
  }

  void failNextWait() {
    failNext = true;
  }

 private:
  kj::TimerImpl timer{kj::origin<kj::TimePoint>()};
  bool failNext = false;
};

TimerQueue::Key key(uint milliseconds, uint tiebreaker = 0) {
  return {kj::UNIX_EPOCH + milliseconds * kj::MILLISECONDS, tiebreaker};
}

KJ_TEST("TimerQueue retains fired deadlines and orders overdue insertions and ties") {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);
  QueueTimer timer;
  TimerQueue queue;
  auto first = kj::newPromiseAndFulfiller<void>();
  auto tied = kj::newPromiseAndFulfiller<void>();
  auto last = kj::newPromiseAndFulfiller<void>();
  queue.insert(key(10, 1), kj::mv(tied.fulfiller), timer);
  queue.insert(key(20), kj::mv(last.fulfiller), timer);
  queue.insert(key(10), kj::mv(first.fulfiller), timer);

  timer.advanceTo(30);
  KJ_ASSERT(first.promise.poll(ws));
  first.promise.wait(ws);
  KJ_EXPECT(queue.getClockClamp() == key(10).when);
  KJ_EXPECT(!tied.promise.poll(ws));
  KJ_EXPECT(!last.promise.poll(ws));

  auto inserted = kj::newPromiseAndFulfiller<void>();
  queue.insert(key(15), kj::mv(inserted.fulfiller), timer);
  queue.erase(key(10), timer);
  KJ_ASSERT(tied.promise.poll(ws));
  tied.promise.wait(ws);
  KJ_EXPECT(!inserted.promise.poll(ws));

  queue.erase(key(10, 1), timer);
  KJ_ASSERT(inserted.promise.poll(ws));
  inserted.promise.wait(ws);
  KJ_EXPECT(!last.promise.poll(ws));

  queue.erase(key(15), timer);
  KJ_ASSERT(last.promise.poll(ws));
  last.promise.wait(ws);
  queue.erase(key(20), timer);
  KJ_EXPECT(queue.empty());
  KJ_EXPECT(queue.getClockClamp() == kj::none);
}

KJ_TEST("TimerQueue rearms after canceling armed and fired entries") {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);
  QueueTimer timer;
  TimerQueue queue;
  auto canceled = kj::newPromiseAndFulfiller<void>();
  auto next = kj::newPromiseAndFulfiller<void>();
  queue.insert(key(10), kj::mv(canceled.fulfiller), timer);
  queue.insert(key(20), kj::mv(next.fulfiller), timer);
  queue.erase(key(10), timer);
  KJ_EXPECT_THROW_MESSAGE("PromiseFulfiller", canceled.promise.wait(ws));

  timer.advanceTo(10);
  KJ_EXPECT(!next.promise.poll(ws));
  auto earlier = kj::newPromiseAndFulfiller<void>();
  queue.insert(key(5), kj::mv(earlier.fulfiller), timer);
  KJ_ASSERT(earlier.promise.poll(ws));
  earlier.promise.wait(ws);
  queue.erase(key(5), timer);

  timer.advanceTo(19);
  KJ_EXPECT(!next.promise.poll(ws));
  timer.advanceTo(20);
  KJ_ASSERT(next.promise.poll(ws));
  next.promise.wait(ws);
  queue.clear();
  KJ_EXPECT(queue.empty());
}

KJ_TEST("TimerQueue adopts pending deadlines and releases the source timer task") {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);
  QueueTimer timer;
  TimerQueue parent;
  auto parentTimer = kj::newPromiseAndFulfiller<void>();
  auto childTimer = kj::newPromiseAndFulfiller<void>();
  parent.insert(key(20), kj::mv(parentTimer.fulfiller), timer);
  {
    TimerQueue child;
    child.insert(key(10), kj::mv(childTimer.fulfiller), timer);
    parent.adoptFrom(child, timer);
    KJ_EXPECT(child.empty());
    KJ_EXPECT(child.getClockClamp() == kj::none);
  }

  timer.advanceTo(20);
  KJ_ASSERT(childTimer.promise.poll(ws));
  childTimer.promise.wait(ws);
  KJ_EXPECT(!parentTimer.promise.poll(ws));
  parent.erase(key(10), timer);
  KJ_ASSERT(parentTimer.promise.poll(ws));
  parentTimer.promise.wait(ws);
}

KJ_TEST("TimerQueue adopts fired entries without delivering them again") {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);
  QueueTimer timer;
  TimerQueue parent;
  auto first = kj::newPromiseAndFulfiller<void>();
  auto second = kj::newPromiseAndFulfiller<void>();
  auto last = kj::newPromiseAndFulfiller<void>();
  parent.insert(key(100), kj::mv(first.fulfiller), timer);
  parent.insert(key(150), kj::mv(second.fulfiller), timer);
  parent.insert(key(250), kj::mv(last.fulfiller), timer);
  {
    TimerQueue child;
    auto adopted = kj::newPromiseAndFulfiller<void>();
    child.insert(key(200), kj::mv(adopted.fulfiller), timer);
    timer.advanceTo(300);
    KJ_ASSERT(first.promise.poll(ws));
    first.promise.wait(ws);
    KJ_ASSERT(adopted.promise.poll(ws));
    adopted.promise.wait(ws);
    parent.adoptFrom(child, timer);
  }

  KJ_EXPECT(!second.promise.poll(ws));
  parent.erase(key(100), timer);
  KJ_ASSERT(second.promise.poll(ws));
  second.promise.wait(ws);
  parent.erase(key(150), timer);
  KJ_EXPECT(parent.getClockClamp() == key(200).when);
  KJ_EXPECT(!last.promise.poll(ws));
  parent.erase(key(200), timer);
  KJ_ASSERT(last.promise.poll(ws));
  last.promise.wait(ws);
}

KJ_TEST("TimerQueue retries synchronous arming failures on later queue changes") {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);
  QueueTimer timer;
  TimerQueue queue;
  auto first = kj::newPromiseAndFulfiller<void>();
  auto second = kj::newPromiseAndFulfiller<void>();
  auto third = kj::newPromiseAndFulfiller<void>();
  queue.insert(key(10), kj::mv(first.fulfiller), timer);
  queue.insert(key(20), kj::mv(second.fulfiller), timer);
  timer.advanceTo(10);
  KJ_ASSERT(first.promise.poll(ws));
  first.promise.wait(ws);

  timer.failNextWait();
  KJ_EXPECT_THROW_MESSAGE("injected arming failure", queue.erase(key(10), timer));
  queue.insert(key(30), kj::mv(third.fulfiller), timer);
  timer.advanceTo(20);
  KJ_ASSERT(second.promise.poll(ws));
  second.promise.wait(ws);
  KJ_EXPECT(!third.promise.poll(ws));
  queue.erase(key(20), timer);
  timer.advanceTo(30);
  KJ_ASSERT(third.promise.poll(ws));
  third.promise.wait(ws);
}

}  // namespace
}  // namespace workerd
