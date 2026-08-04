// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Characterization tests for the async isolate lock scheduler.
//
// These pin down the scheduler's *current* behaviour, including the same-thread serialization
// across different resources that we intend to change. When that behaviour changes, the tests
// asserting it should flip deliberately rather than silently.

#include <workerd/io/async-lock-scheduler.h>

#include <kj/test.h>
#include <kj/thread.h>
#include <kj/vector.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace workerd {
namespace {

// Stands in for Worker::Isolate: atomically refcounted, has an id, owns its queue.
class FakeResource: public kj::AtomicRefcounted {
 public:
  explicit FakeResource(kj::StringPtr id): id(kj::str(id)) {}

  kj::StringPtr getId() const {
    return id;
  }

  kj::Promise<AsyncLockQueue<FakeResource>::Lock> lock(
      kj::Maybe<AsyncLockQueue<FakeResource>::Hooks&> hooks = kj::none) const {
    return queue.lock(kj::atomicAddRef(*this), hooks);
  }

  uint getCurrentLoad() const {
    return queue.getCurrentLoad();
  }

 private:
  kj::String id;
  AsyncLockQueue<FakeResource> queue;
};

using Queue = AsyncLockQueue<FakeResource>;

// Records the observability callbacks so we can assert we still report the same things.
class RecordingHooks final: public Queue::Hooks {
 public:
  void waitingForOtherResource(kj::StringPtr id) override {
    blockedBy.add(kj::str(id));
  }
  void reportAsyncInfo(
      uint currentLoad, bool coalesced, uint blockedByOtherResourceCount) override {
    KJ_ASSERT(!reported, "reportAsyncInfo() should be called exactly once per lock()");
    reported = true;
    this->currentLoad = currentLoad;
    this->coalesced = coalesced;
    this->blockedCount = blockedByOtherResourceCount;
  }

  kj::Vector<kj::String> blockedBy;
  bool reported = false;
  uint currentLoad = 0;
  bool coalesced = false;
  uint blockedCount = 0;
};

// Spin until the resource reports at least `target` waiters, so that cross-thread tests can
// establish a deterministic queue order. Fails rather than hanging forever.
void waitForLoad(const FakeResource& resource, uint target) {
  for (uint i = 0; i < 100000; ++i) {
    if (resource.getCurrentLoad() >= target) return;
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
  KJ_FAIL_ASSERT(
      "timed out waiting for queue to reach expected load", target, resource.getCurrentLoad());
}

// =======================================================================================
// Single-threaded behaviour

KJ_TEST("AsyncLockQueue: uncontended lock is granted immediately") {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);

  auto resource = kj::atomicRefcounted<FakeResource>("r"_kj);
  KJ_EXPECT(resource->getCurrentLoad() == 0);

  {
    RecordingHooks hooks;
    auto promise = resource->lock(hooks);
    KJ_EXPECT(promise.poll(ws), "an uncontended lock should not block");
    auto lock = promise.wait(ws);

    KJ_EXPECT(&lock.getResource() == resource.get());
    KJ_EXPECT(resource->getCurrentLoad() == 1);
    KJ_EXPECT(hooks.reported);
    KJ_EXPECT(!hooks.coalesced);
    KJ_EXPECT(hooks.blockedCount == 0);
    KJ_EXPECT(hooks.blockedBy.size() == 0);
    // Load is sampled before we enqueue ourselves.
    KJ_EXPECT(hooks.currentLoad == 0);
  }

  KJ_EXPECT(resource->getCurrentLoad() == 0, "releasing the lock should drain the queue");
}

KJ_TEST("AsyncLockQueue: same thread re-acquiring the same resource coalesces") {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);

  auto resource = kj::atomicRefcounted<FakeResource>("r"_kj);

  auto first = resource->lock().wait(ws);
  KJ_EXPECT(resource->getCurrentLoad() == 1);

  // A second acquisition on the same thread shares the first one's turn, so it is granted even
  // though the first is still held. This is what makes nested/reentrant locking safe.
  RecordingHooks hooks;
  auto promise = resource->lock(hooks);
  KJ_EXPECT(promise.poll(ws), "a coalesced lock should not block behind itself");
  auto second = promise.wait(ws);

  KJ_EXPECT(hooks.coalesced);
  KJ_EXPECT(hooks.blockedCount == 0);
  KJ_EXPECT(resource->getCurrentLoad() == 1, "coalesced locks share a single queue slot");
}

KJ_TEST("AsyncLockQueue: a different resource on the same thread waits for the first to release") {
  // This is the head-of-line blocking the multi-enrollment design exists to remove. Resource `b`
  // is completely uncontended, yet acquiring it blocks until `a` is released.
  kj::EventLoop loop;
  kj::WaitScope ws(loop);

  auto a = kj::atomicRefcounted<FakeResource>("a"_kj);
  auto b = kj::atomicRefcounted<FakeResource>("b"_kj);

  auto lockA = kj::heap(a->lock().wait(ws));

  RecordingHooks hooks;
  auto promiseB = b->lock(hooks);

  KJ_EXPECT(!promiseB.poll(ws), "b must not be granted while a is held");
  KJ_EXPECT(b->getCurrentLoad() == 0, "we have not even joined b's queue yet");
  KJ_ASSERT(hooks.blockedBy.size() == 1);
  KJ_EXPECT(hooks.blockedBy[0] == "a"_kj);
  KJ_EXPECT(!hooks.reported, "we only report async info once we actually queue");

  // Releasing `a` lets the attempt on `b` proceed.
  lockA = nullptr;
  KJ_EXPECT(promiseB.poll(ws));
  auto lockB = promiseB.wait(ws);

  KJ_EXPECT(hooks.reported);
  KJ_EXPECT(!hooks.coalesced);
  KJ_EXPECT(hooks.blockedCount == 1, "should record that one other resource blocked us");
  KJ_EXPECT(b->getCurrentLoad() == 1);
  KJ_EXPECT(a->getCurrentLoad() == 0);
}

KJ_TEST("AsyncLockQueue: cancelling a pending attempt leaves no trace") {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);

  auto a = kj::atomicRefcounted<FakeResource>("a"_kj);
  auto b = kj::atomicRefcounted<FakeResource>("b"_kj);

  auto lockA = kj::heap(a->lock().wait(ws));

  {
    auto promiseB = b->lock();
    KJ_EXPECT(!promiseB.poll(ws));
  }  // dropped

  KJ_EXPECT(b->getCurrentLoad() == 0);

  lockA = nullptr;
  KJ_EXPECT(a->getCurrentLoad() == 0);

  // The queue is still usable afterwards.
  auto lockB = b->lock().wait(ws);
  KJ_EXPECT(b->getCurrentLoad() == 1);
}

KJ_TEST("AsyncLockQueue: whenThreadIdle waits for the held lock") {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);

  auto resource = kj::atomicRefcounted<FakeResource>("r"_kj);

  {
    auto idle = Queue::whenThreadIdle();
    KJ_EXPECT(idle.poll(ws), "a thread holding nothing is idle");
    idle.wait(ws);
  }

  auto lock = kj::heap(resource->lock().wait(ws));
  auto idle = Queue::whenThreadIdle();
  KJ_EXPECT(!idle.poll(ws), "must not report idle while a lock is held");

  lock = nullptr;
  KJ_EXPECT(idle.poll(ws));
  idle.wait(ws);
}

// =======================================================================================
// Cross-thread behaviour

KJ_TEST("AsyncLockQueue: turns are granted across threads in the order they were requested") {
  struct State {
    bool firstHolds = false;
    bool releaseFirst = false;
    kj::Vector<char> granted;
  };
  kj::MutexGuarded<State> state;

  auto resource = kj::atomicRefcounted<FakeResource>("r"_kj);

  // Records its name once granted, then releases immediately.
  auto contend = [&](char name) {
    kj::EventLoop loop;
    kj::WaitScope ws(loop);
    auto lock = resource->lock().wait(ws);
    state.lockExclusive()->granted.add(name);
  };

  {
    kj::Thread first([&]() {
      kj::EventLoop loop;
      kj::WaitScope ws(loop);
      auto lock = resource->lock().wait(ws);
      state.lockExclusive()->firstHolds = true;
      state.when([](const State& s) { return s.releaseFirst; }, [](State&) {});
    });

    // Wait until the first thread definitely holds the lock, then enqueue the others one at a
    // time so their queue order is deterministic.
    state.when([](const State& s) { return s.firstHolds; }, [](State&) {});

    kj::Thread second([&]() { contend('B'); });
    waitForLoad(*resource, 2);

    kj::Thread third([&]() { contend('C'); });
    waitForLoad(*resource, 3);

    state.lockExclusive()->releaseFirst = true;
  }  // joins all three threads

  auto lockedState = state.lockExclusive();
  KJ_ASSERT(lockedState->granted.size() == 2);
  KJ_EXPECT(lockedState->granted[0] == 'B', "FIFO: the earlier waiter should win");
  KJ_EXPECT(lockedState->granted[1] == 'C');
  KJ_EXPECT(resource->getCurrentLoad() == 0);
}

KJ_TEST("AsyncLockQueue: only one thread holds a resource at a time") {
  constexpr uint THREAD_COUNT = 8;
  constexpr uint ITERATIONS = 50;

  auto resource = kj::atomicRefcounted<FakeResource>("r"_kj);
  kj::MutexGuarded<uint> holders(0);
  std::atomic<uint> maxObserved{0};
  std::atomic<uint> completed{0};

  {
    kj::Vector<kj::Own<kj::Thread>> threads;
    for (uint i = 0; i < THREAD_COUNT; ++i) {
      threads.add(kj::heap<kj::Thread>([&]() {
        kj::EventLoop loop;
        kj::WaitScope ws(loop);
        for (uint j = 0; j < ITERATIONS; ++j) {
          auto lock = resource->lock().wait(ws);
          {
            // Both reads and the write happen under `holders`, so plain loads suffice.
            auto count = holders.lockExclusive();
            ++*count;
            if (*count > maxObserved.load(std::memory_order_relaxed)) {
              maxObserved.store(*count, std::memory_order_relaxed);
            }
          }
          --*holders.lockExclusive();
          ++completed;
        }
      }));
    }
  }  // joins

  KJ_EXPECT(maxObserved.load() == 1, "mutual exclusion violated", maxObserved.load());
  KJ_EXPECT(completed.load() == THREAD_COUNT * ITERATIONS);
  KJ_EXPECT(resource->getCurrentLoad() == 0);
}

KJ_TEST("AsyncLockQueue: many threads contending for many resources all make progress") {
  // Deadlock net: every thread walks the resources in a different order, so any ordering hazard
  // between resources would show up here.
  constexpr uint THREAD_COUNT = 8;
  constexpr uint RESOURCE_COUNT = 4;
  constexpr uint ITERATIONS = 40;

  kj::Vector<kj::Own<const FakeResource>> resources;
  for (uint i = 0; i < RESOURCE_COUNT; ++i) {
    resources.add(kj::atomicRefcounted<FakeResource>(kj::str("r", i)));
  }

  std::atomic<uint> completed{0};

  {
    kj::Vector<kj::Own<kj::Thread>> threads;
    for (uint t = 0; t < THREAD_COUNT; ++t) {
      threads.add(kj::heap<kj::Thread>([&, t]() {
        kj::EventLoop loop;
        kj::WaitScope ws(loop);
        for (uint j = 0; j < ITERATIONS; ++j) {
          for (uint k = 0; k < RESOURCE_COUNT; ++k) {
            auto& resource = *resources[(t + k + j) % RESOURCE_COUNT];
            auto lock = resource.lock().wait(ws);
            ++completed;
          }
        }
      }));
    }
  }  // joins; hangs here if the scheduler deadlocks

  KJ_EXPECT(completed.load() == THREAD_COUNT * RESOURCE_COUNT * ITERATIONS);
  for (auto& resource: resources) {
    KJ_EXPECT(resource->getCurrentLoad() == 0);
  }
}

}  // namespace
}  // namespace workerd
