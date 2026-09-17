// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "sync-wait-list.h"

#include <kj/test.h>
#include <kj/thread.h>

namespace workerd {
namespace {

class IntWaiter final: public SyncWaitList<int>::Waiter {
 public:
  kj::uint readyCount = 0;
  kj::uint removedCount = 0;
  int value = 0;

 private:
  void ready(const int& valueArg) noexcept override {
    ++readyCount;
    value = valueArg;
  }

  void removed() noexcept override {
    ++removedCount;
  }
};

class VoidWaiter final: public SyncWaitList<void>::Waiter {
 public:
  kj::uint readyCount = 0;
  kj::uint removedCount = 0;

 private:
  void ready() noexcept override {
    ++readyCount;
  }

  void removed() noexcept override {
    ++removedCount;
  }
};

KJ_TEST("SyncWaitList with value wakes current and future waiters") {
  SyncWaitList<int> list;
  IntWaiter current;
  IntWaiter removed;

  KJ_EXPECT(list.isReady() == kj::none);
  KJ_EXPECT(list.tryAdd(current) == kj::none);
  list.add(removed);
  list.remove(removed);
  list.remove(removed);

  {
    kj::Thread thread([&list]() { list.ready(123); });
  }
  KJ_EXPECT(!list.tryReady(456));

  KJ_EXPECT(current.readyCount == 1, current.readyCount);
  KJ_EXPECT(current.value == 123, current.value);
  KJ_EXPECT(current.removedCount == 0, current.removedCount);
  KJ_EXPECT(removed.readyCount == 0, removed.readyCount);
  KJ_EXPECT(removed.removedCount == 0, removed.removedCount);

  KJ_IF_SOME(value, list.isReady()) {
    KJ_EXPECT(value == 123, value);
  } else {
    KJ_FAIL_EXPECT("list should be ready");
  }

  IntWaiter future;
  KJ_IF_SOME(value, list.tryAdd(future)) {
    KJ_EXPECT(value == 123, value);
  } else {
    KJ_FAIL_EXPECT("tryAdd() should return the ready value");
  }
  KJ_EXPECT(future.readyCount == 0, future.readyCount);
  list.add(future);
  KJ_EXPECT(future.readyCount == 1, future.readyCount);
  KJ_EXPECT(future.value == 123, future.value);
  KJ_EXPECT(future.removedCount == 0, future.removedCount);
}

KJ_TEST("SyncWaitList with value removes pending waiters when destroyed") {
  IntWaiter waiter;
  {
    SyncWaitList<int> list;
    list.add(waiter);
  }

  KJ_EXPECT(waiter.readyCount == 0, waiter.readyCount);
  KJ_EXPECT(waiter.removedCount == 1, waiter.removedCount);
}

KJ_TEST("SyncWaitList constructs and destroys its value") {
  struct Value {
    explicit Value(kj::uint& destructionCount): destructionCount(destructionCount) {}
    ~Value() noexcept {
      ++destructionCount;
    }
    KJ_DISALLOW_COPY_AND_MOVE(Value);

    kj::uint& destructionCount;
  };

  class ValueWaiter final: public SyncWaitList<Value>::Waiter {
   public:
    bool wasReadied = false;

   private:
    void ready(const Value&) noexcept override {
      wasReadied = true;
    }

    void removed() noexcept override {}
  };

  kj::uint destructionCount = 0;
  ValueWaiter waiter;
  {
    SyncWaitList<Value> list;
    list.add(waiter);
    list.ready(destructionCount);
    KJ_EXPECT(waiter.wasReadied);
    KJ_EXPECT(destructionCount == 0, destructionCount);
  }
  KJ_EXPECT(destructionCount == 1, destructionCount);
}

KJ_TEST("SyncWaitList can retry after value construction throws") {
  struct Value {
    explicit Value(bool& shouldThrow) {
      if (shouldThrow) {
        shouldThrow = false;
        KJ_FAIL_REQUIRE("construction failed");
      }
    }
  };

  SyncWaitList<Value> list;
  bool shouldThrow = true;
  KJ_EXPECT_THROW_MESSAGE("construction failed", list.tryReady(shouldThrow));
  KJ_EXPECT(list.isReady() == kj::none);
  KJ_EXPECT(list.tryReady(shouldThrow));
  KJ_EXPECT(list.isReady() != kj::none);
}

KJ_TEST("SyncWaitList serializes competing value construction") {
  struct BlockingFailure {
    kj::MutexGuarded<bool>& entered;
    kj::MutexGuarded<bool>& mayThrow;
  };
  struct Value {
    explicit Value(BlockingFailure state) {
      *state.entered.lockExclusive() = true;
      state.mayThrow.when([](bool value) { return value; }, [](bool) {});
      KJ_FAIL_REQUIRE("construction failed");
    }
    explicit Value(int value): value(value) {}

    int value;
  };

  SyncWaitList<Value> list;
  kj::MutexGuarded<bool> constructorEntered(false);
  kj::MutexGuarded<bool> secondStarted(false);
  kj::MutexGuarded<bool> mayThrow(false);
  bool firstFailed = false;
  bool secondSucceeded = false;

  {
    kj::Thread first([&]() {
      firstFailed = kj::runCatchingExceptions([&]() {
        list.tryReady(BlockingFailure{constructorEntered, mayThrow});
      }) != kj::none;
    });

    constructorEntered.when([](bool value) { return value; }, [](bool) {});
    {
      kj::Thread second([&]() {
        *secondStarted.lockExclusive() = true;
        secondSucceeded = list.tryReady(123);
      });

      secondStarted.when([](bool value) { return value; }, [](bool) {});
      *mayThrow.lockExclusive() = true;
    }
  }

  KJ_EXPECT(firstFailed);
  KJ_EXPECT(secondSucceeded);
  KJ_IF_SOME(value, list.isReady()) {
    KJ_EXPECT(value.value == 123, value.value);
  } else {
    KJ_FAIL_EXPECT("list should be ready");
  }
}

KJ_TEST("SyncWaitList<void> wakes current and future waiters") {
  SyncWaitList<void> list;
  VoidWaiter current;
  VoidWaiter removed;

  KJ_EXPECT(!list.isReady());
  KJ_EXPECT(!list.tryAdd(current));
  list.add(removed);
  list.remove(removed);
  list.remove(removed);

  {
    kj::Thread thread([&list]() { list.ready(); });
  }
  KJ_EXPECT(!list.tryReady());

  KJ_EXPECT(list.isReady());
  KJ_EXPECT(current.readyCount == 1, current.readyCount);
  KJ_EXPECT(current.removedCount == 0, current.removedCount);
  KJ_EXPECT(removed.readyCount == 0, removed.readyCount);
  KJ_EXPECT(removed.removedCount == 0, removed.removedCount);

  VoidWaiter future;
  KJ_EXPECT(list.tryAdd(future));
  KJ_EXPECT(future.readyCount == 0, future.readyCount);
  list.add(future);
  KJ_EXPECT(future.readyCount == 1, future.readyCount);
  KJ_EXPECT(future.removedCount == 0, future.removedCount);
}

KJ_TEST("SyncWaitList<void> tryAdd races readiness without missing wakeup") {
  for (auto i: kj::zeroTo(100u)) {
    SyncWaitList<void> list;
    VoidWaiter waiter;
    bool wasAlreadyReady = false;

    {
      kj::Thread addThread([&]() { wasAlreadyReady = list.tryAdd(waiter); });
      kj::Thread readyThread([&]() { list.ready(); });
    }

    KJ_EXPECT(wasAlreadyReady || waiter.readyCount == 1, i, waiter.readyCount);
    KJ_EXPECT(!wasAlreadyReady || waiter.readyCount == 0, i, waiter.readyCount);
  }
}

KJ_TEST("SyncWaitList<void> removes pending waiters when destroyed") {
  VoidWaiter waiter;
  {
    SyncWaitList<void> list;
    list.add(waiter);
  }

  KJ_EXPECT(waiter.readyCount == 0, waiter.readyCount);
  KJ_EXPECT(waiter.removedCount == 1, waiter.removedCount);
}

}  // namespace
}  // namespace workerd
