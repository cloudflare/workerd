// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "wait-list.h"

#include <kj/test.h>
#include <kj/thread.h>

namespace workerd {
namespace {

KJ_TEST("CrossThreadWaitList") {
  auto doTest = [](const CrossThreadWaitList& list) {
    kj::MutexGuarded<uint> ready;

    auto threadFunc = [&]() noexcept {
      kj::EventLoop loop;
      kj::WaitScope ws(loop);

      auto promise1 = list.addWaiter();
      auto promise2 = list.addWaiter();

      KJ_ASSERT(!promise1.poll(ws));
      KJ_ASSERT(!promise2.poll(ws));
      KJ_ASSERT(!list.isDone());

      (*ready.lockExclusive())++;

      promise1.wait(ws);
      promise2.wait(ws);

      KJ_ASSERT(list.isDone());
    };

    kj::Thread waiter1(threadFunc);
    kj::Thread waiter2(threadFunc);
    kj::Thread waiter3(threadFunc);

    kj::Thread sender([&]() {
      ready.when([](uint val) { return val == 3; }, [&](uint) {});
      list.fulfill();
    });
  };

  {
    CrossThreadWaitList list;
    doTest(list);
  }
  {
    CrossThreadWaitList list({.useThreadLocalOptimization = true});
    doTest(list);
  }
}

KJ_TEST("CrossThreadWaitList exceptions") {
  auto doTest = [](const CrossThreadWaitList& list) {
    kj::MutexGuarded<uint> ready;

    auto threadFunc = [&]() noexcept {
      kj::EventLoop loop;
      kj::WaitScope ws(loop);

      auto promise1 = list.addWaiter();
      auto promise2 = list.addWaiter();

      KJ_ASSERT(!promise1.poll(ws));
      KJ_ASSERT(!promise2.poll(ws));
      KJ_ASSERT(!list.isDone());

      (*ready.lockExclusive())++;

      promise1
          .then([]() { KJ_FAIL_REQUIRE("didn't throw"); }, [](kj::Exception&& e) {
        KJ_ASSERT(e.getDescription() == "foo");
      }).wait(ws);
      promise2
          .then([]() { KJ_FAIL_REQUIRE("didn't throw"); }, [](kj::Exception&& e) {
        KJ_ASSERT(e.getDescription() == "foo");
      }).wait(ws);

      KJ_ASSERT(list.isDone());
    };

    kj::Thread waiter1(threadFunc);
    kj::Thread waiter2(threadFunc);
    kj::Thread waiter3(threadFunc);

    kj::Thread sender([&]() {
      ready.when([](uint val) { return val == 3; }, [&](uint) {});
      list.reject(KJ_EXCEPTION(FAILED, "foo"));
    });
  };

  {
    CrossThreadWaitList list;
    doTest(list);
  }
  {
    CrossThreadWaitList list({.useThreadLocalOptimization = true});
    doTest(list);
  }
}

KJ_TEST("CrossThreadWaitList preserves the first result for future waiters") {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);

  {
    CrossThreadWaitList list;
    KJ_EXPECT(list.tryFulfill());
    list.reject(KJ_EXCEPTION(FAILED, "ignored"));
    KJ_EXPECT(!list.tryFulfill());

    list.addWaiter().wait(ws);
    KJ_EXPECT(list.isDone());
  }

  {
    CrossThreadWaitList list;
    list.reject(KJ_EXCEPTION(FAILED, "first error"));
    KJ_EXPECT(!list.tryFulfill());
    list.reject(KJ_EXCEPTION(FAILED, "ignored"));

    KJ_EXPECT_THROW_MESSAGE("first error", list.addWaiter().wait(ws));
    KJ_EXPECT(list.isDone());
  }
}

KJ_TEST("CrossThreadWaitList separate fulfiller settles the list") {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);

  CrossThreadWaitList list;
  auto fulfiller = list.makeSeparateFulfiller();
  auto promise = list.addWaiter();

  KJ_EXPECT(fulfiller->isWaiting());
  fulfiller->fulfill();
  KJ_EXPECT(!fulfiller->isWaiting());
  promise.wait(ws);
}

KJ_TEST("CrossThreadWaitList rejects when its separate fulfiller is dropped") {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);

  CrossThreadWaitList list;
  auto promise = list.addWaiter();
  { auto fulfiller = list.makeSeparateFulfiller(); }

  KJ_EXPECT_THROW_MESSAGE("wait list was never fulfilled", promise.wait(ws));
  KJ_EXPECT_THROW_MESSAGE("wait list was never fulfilled", list.addWaiter().wait(ws));
}

KJ_TEST("CrossThreadWaitList waiters can be canceled") {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);

  auto test = [&](CrossThreadWaitList& list) {
    {
      auto promise = list.addWaiter();
      KJ_EXPECT(!promise.poll(ws));
    }
    list.fulfill();
    KJ_EXPECT(list.isDone());
  };

  {
    CrossThreadWaitList list;
    test(list);
  }
  {
    CrossThreadWaitList list({.useThreadLocalOptimization = true});
    test(list);
  }
}

KJ_TEST("CrossThreadWaitList fulfillment can race waiter cancellation") {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);

  auto test = [&](CrossThreadWaitList::Options options) {
    for (uint i = 0; i < 100; ++i) {
      CrossThreadWaitList list(options);
      kj::Maybe<kj::Promise<void>> promise = list.addWaiter();
      kj::MutexGuarded<bool> start(false);
      kj::Thread sender([&]() {
        start.when([](bool value) { return value; }, [](bool) {});
        list.fulfill();
      });

      *start.lockExclusive() = true;
      promise = kj::none;
    }
  };

  test({});
  test({.useThreadLocalOptimization = true});
}

}  // namespace
}  // namespace workerd
