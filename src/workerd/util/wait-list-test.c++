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

      promise1.then([]() { KJ_FAIL_REQUIRE("didn't throw"); }, [](kj::Exception&& e) {
        KJ_ASSERT(e.getDescription() == "foo");
      }).wait(ws);
      promise2.then([]() { KJ_FAIL_REQUIRE("didn't throw"); }, [](kj::Exception&& e) {
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

}  // namespace
}  // namespace workerd
