// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "wait-list.h"

#include <kj/test.h>
#include <kj/thread.h>

namespace workerd {
namespace {

class NonCopyable {
 public:
  NonCopyable() = default;
  KJ_DISALLOW_COPY_AND_MOVE(NonCopyable);
};

class MutableCopyOnly {
 public:
  MutableCopyOnly() = default;
  MutableCopyOnly(MutableCopyOnly&) {}
};

static_assert(XThreadWaitListValue<int>);
static_assert(XThreadWaitListValue<kj::Arc<int>>);
static_assert(!XThreadWaitListValue<kj::Own<int>>);
static_assert(!XThreadWaitListValue<NonCopyable>);
static_assert(!XThreadWaitListValue<MutableCopyOnly>);

KJ_TEST("XThreadWaitList copies values for current and future waiters") {
  auto test = [](XThreadWaitList<int>::Options options) {
    kj::EventLoop loop;
    kj::WaitScope ws(loop);
    XThreadWaitList<int> list(options);

    auto promise1 = list.addWaiter();
    auto promise2 = list.addWaiter();
    KJ_EXPECT(!promise1.poll(ws));
    KJ_EXPECT(!promise2.poll(ws));

    {
      kj::Thread sender([&list]() { list.fulfill(123); });
    }

    KJ_EXPECT(promise1.wait(ws) == 123);
    KJ_EXPECT(promise2.wait(ws) == 123);
    KJ_EXPECT(list.addWaiter().wait(ws) == 123);
    KJ_EXPECT(list.isDone());
  };

  test({});
  test({.useThreadLocalOptimization = true});
}

KJ_TEST("XThreadWaitList clones Arcs for current and future waiters") {
  auto test = [](XThreadWaitList<kj::Arc<int>>::Options options) {
    kj::EventLoop loop;
    kj::WaitScope ws(loop);
    XThreadWaitList<kj::Arc<int>> list(options);
    auto promise1 = list.addWaiter();
    auto promise2 = list.addWaiter();
    auto value = kj::arc<int>(123);
    auto ptr = value.get();

    {
      kj::Thread sender([&list, value = kj::mv(value)]() mutable { list.fulfill(kj::mv(value)); });
    }

    auto value1 = promise1.wait(ws);
    auto value2 = promise2.wait(ws);
    auto futureValue = list.addWaiter().wait(ws);
    KJ_EXPECT(value1.get() == ptr);
    KJ_EXPECT(value2.get() == ptr);
    KJ_EXPECT(futureValue.get() == ptr);
  };

  test({});
  test({.useThreadLocalOptimization = true});
}

KJ_TEST("XThreadWaitList preserves its first result") {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);
  XThreadWaitList<int> list;
  auto promise = list.addWaiter();

  list.reject(KJ_EXCEPTION(FAILED, "first error"));
  KJ_EXPECT(!list.tryFulfill(123));
  list.reject(KJ_EXCEPTION(FAILED, "ignored"));

  KJ_EXPECT_THROW_MESSAGE("first error", promise.wait(ws));
  KJ_EXPECT_THROW_MESSAGE("first error", list.addWaiter().wait(ws));
}

KJ_TEST("XThreadWaitList tryFulfill reports whether it settled the list") {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);
  XThreadWaitList<int> list;

  KJ_EXPECT(list.tryFulfill(123));
  KJ_EXPECT(!list.tryFulfill(456));
  KJ_EXPECT(list.addWaiter().wait(ws) == 123);
}

KJ_TEST("XThreadWaitList separate fulfiller settles the list") {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);
  XThreadWaitList<int> list;
  auto promise = list.addWaiter();
  auto fulfiller = list.makeSeparateFulfiller();

  KJ_EXPECT(fulfiller->isWaiting());
  fulfiller->fulfill(123);
  KJ_EXPECT(!fulfiller->isWaiting());
  KJ_EXPECT(promise.wait(ws) == 123);
}

KJ_TEST("XThreadWaitList rejects when its separate fulfiller is dropped") {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);
  XThreadWaitList<int> list;
  auto promise = list.addWaiter();
  { auto fulfiller = list.makeSeparateFulfiller(); }

  KJ_EXPECT_THROW_MESSAGE("wait list was never fulfilled", promise.wait(ws));
  KJ_EXPECT_THROW_MESSAGE("wait list was never fulfilled", list.addWaiter().wait(ws));
}

}  // namespace
}  // namespace workerd
