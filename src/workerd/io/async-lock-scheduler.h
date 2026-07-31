// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// Fair asynchronous lock queue, used to serialize access to a resource which only one thread may
// use at a time -- in practice, a JavaScript isolate.
//
// Threads queue up for a resource by calling `AsyncLockQueue::lock()`, and each gets its turn in
// the order it joined. Taking this lock before taking the resource's real (synchronous) lock means
// threads don't fight over the synchronous lock, and every interested thread gets a fair turn.
//
// This header deliberately depends on nothing but KJ, so that the scheduling behaviour can be
// tested directly without standing up a JavaScript isolate.

#include <kj/async.h>
#include <kj/debug.h>
#include <kj/mutex.h>
#include <kj/refcount.h>
#include <kj/string.h>

namespace workerd {

using kj::uint;

template <typename Resource>
class AsyncLockQueue;

namespace _ {  // private

// One thread's attempt to take the lock on one resource. Refcounted, because repeat requests from
// the same thread for the same resource share a single turn in the queue.
//
// Internal to AsyncLockQueue; members are public only to avoid a mutual-friendship dance.
template <typename Resource>
class AsyncLockWaiter: public kj::Refcounted {
 public:
  AsyncLockWaiter(const AsyncLockQueue<Resource>& queue, kj::Own<const Resource> resourceParam);
  ~AsyncLockWaiter() noexcept;
  KJ_DISALLOW_COPY_AND_MOVE(AsyncLockWaiter);

  // Executor for this waiter's thread. Never read, but constructing it forces the thread's
  // executor to exist, which `newPromiseAndCrossThreadFulfiller()` would otherwise only do on the
  // contended path.
  // TODO(cleanup): Nothing depends on this; dropping it would save an allocation per event loop.
  const kj::Executor& executor;

  const AsyncLockQueue<Resource>& queue;

  // Keeps the resource -- and therefore `queue`, which the resource owns -- alive for as long as
  // we are queued on it. This is what makes it impossible for a queue to outlive its waiters.
  kj::Own<const Resource> resource;

  // Fires when this waiter reaches the front of the queue. Fulfilled by the departing front
  // waiter, which may be on another thread.
  kj::ForkedPromise<void> readyPromise = nullptr;
  kj::Own<kj::CrossThreadPromiseFulfiller<void>> readyFulfiller;

  // Fires when this waiter is destroyed, i.e. the lock is released. Used to serialize one thread's
  // attempts on different resources so it only ever chases one at a time. Deliberately NOT a
  // cross-thread fulfiller: only the owning thread may fulfill it.
  kj::ForkedPromise<void> releasePromise = nullptr;
  kj::Own<kj::PromiseFulfiller<void>> releaseFulfiller;

  // Guarded by the queue's mutex, along with the list itself.
  kj::Maybe<AsyncLockWaiter&> next;
  kj::Maybe<AsyncLockWaiter&>* prev;
};

}  // namespace _

// Lock queue for a single resource.
//
// `Resource` must be atomically refcounted, must expose `kj::StringPtr getId() const` for
// observability, and must own its `AsyncLockQueue` as a member.
template <typename Resource>
class AsyncLockQueue {
 public:
  // Observability callbacks. Invoked on the thread requesting the lock, before it starts waiting.
  class Hooks {
   public:
    // Called when this thread is blocked by a lock it holds (or awaits) on a *different* resource,
    // and so cannot yet queue for this one. `id` identifies the resource blocking us.
    virtual void waitingForOtherResource(kj::StringPtr id) {}

    // Called exactly once per `lock()` call, before waiting begins.
    //
    // `currentLoad` is the number of threads queued for this resource at the time of the call.
    // `coalesced` is true if this request joined a turn this thread had already taken in this same
    // queue. `blockedByOtherResourceCount` counts the distinct other resources which blocked this
    // request before it could queue here.
    virtual void reportAsyncInfo(
        uint currentLoad, bool coalesced, uint blockedByOtherResourceCount) {}
  };

  using Waiter = _::AsyncLockWaiter<Resource>;

  // The holder of a granted turn. Hand this to the resource's synchronous lock to prove the async
  // lock was taken first. Never store one long-term: use it in a continuation and discard it.
  class Lock {
   public:
    Lock(Lock&&) = default;
    Lock& operator=(Lock&&) = default;

    const Resource& getResource() const {
      return *waiter->resource;
    }

   private:
    explicit Lock(kj::Own<Waiter> waiter): waiter(kj::mv(waiter)) {}

    kj::Own<Waiter> waiter;

    friend class AsyncLockQueue;
  };

  AsyncLockQueue() = default;
  KJ_DISALLOW_COPY_AND_MOVE(AsyncLockQueue);

  // Queue this thread for the resource, resolving when it is this thread's turn.
  //
  // `resource` is a strong reference to the resource owning this queue, which the returned lock
  // keeps alive. `hooks`, if provided, must outlive the returned promise.
  kj::Promise<Lock> lock(kj::Own<const Resource> resource, kj::Maybe<Hooks&> hooks) const;

  // Number of threads currently queued for (or holding) this resource.
  uint getCurrentLoad() const {
    return __atomic_load_n(&loadGauge, __ATOMIC_RELAXED);
  }

  // Resolves once this thread holds no lock, is waiting for none, and has drained its pending
  // events (a la `kj::evalLast()`).
  static kj::Promise<void> whenThreadIdle();

 private:
  struct WaiterList {
    kj::Maybe<Waiter&> head = kj::none;
    kj::Maybe<Waiter&>* tail = &head;

    ~WaiterList() noexcept {
      // Every member of the list holds a strong reference to the resource that owns us, so it
      // should be impossible to get here with a non-empty list. Crash rather than leave dangling
      // pointers behind.
      KJ_ASSERT(head == kj::none, "destroying non-empty waiter list?");
      KJ_ASSERT(tail == &head, "tail pointer corrupted?");
    }
  };

  // Protects the list itself as well as the next/prev pointers of every waiter currently in it.
  kj::MutexGuarded<WaiterList> waiters;
  // TODO(perf): Use a lock-free list? Tricky to get right. `waiters` should only be locked
  //   briefly so there's probably not that much to gain.

  // Incremented while a waiter for this resource exists, so `getCurrentLoad()` can be read without
  // taking the mutex.
  mutable uint loadGauge = 0;

  // A thread only ever has one outstanding waiter, across all resources of this type.
  inline static const kj::EventLoopLocal<Waiter*> threadCurrentWaiter;

  friend class _::AsyncLockWaiter<Resource>;
};

// =======================================================================================
// inline implementation details

namespace _ {  // private

template <typename Resource>
AsyncLockWaiter<Resource>::AsyncLockWaiter(
    const AsyncLockQueue<Resource>& queue, kj::Own<const Resource> resourceParam)
    : executor(kj::getCurrentThreadExecutor()),
      queue(queue),
      resource(kj::mv(resourceParam)) {
  {
    auto paf = kj::newPromiseAndFulfiller<void>();
    releasePromise = paf.promise.fork();
    releaseFulfiller = kj::mv(paf.fulfiller);
  }

  auto lock = queue.waiters.lockExclusive();
  if (lock->tail == &lock->head) {
    // Queue is empty, so it's immediately our turn.
    readyPromise = kj::Promise<void>(kj::READY_NOW).fork();
    // `readyFulfiller` can stay null; nobody will ever invoke it.
  } else {
    auto paf = kj::newPromiseAndCrossThreadFulfiller<void>();
    readyPromise = paf.promise.fork();
    readyFulfiller = kj::mv(paf.fulfiller);
  }

  next = kj::none;
  prev = lock->tail;
  *lock->tail = this;
  lock->tail = &next;

  *AsyncLockQueue<Resource>::threadCurrentWaiter = this;

  __atomic_add_fetch(&queue.loadGauge, 1, __ATOMIC_RELAXED);
}

template <typename Resource>
AsyncLockWaiter<Resource>::~AsyncLockWaiter() noexcept {
  // Noexcept because an exception here probably leaves the process in a bad state.

  __atomic_sub_fetch(&queue.loadGauge, 1, __ATOMIC_RELAXED);

  auto lock = queue.waiters.lockExclusive();

  releaseFulfiller->fulfill();

  // Unlink.
  *prev = next;
  KJ_IF_SOME(n, next) {
    n.prev = prev;
  } else {
    lock->tail = prev;
  }

  if (prev == &lock->head) {
    // We were at the front, i.e. we held the lock. Tell the next waiter it's their turn.
    KJ_IF_SOME(n, next) {
      n.readyFulfiller->fulfill();
    }
  }

  auto& w = *AsyncLockQueue<Resource>::threadCurrentWaiter;
  KJ_ASSERT(w == this);
  w = nullptr;
}

}  // namespace _

template <typename Resource>
kj::Promise<typename AsyncLockQueue<Resource>::Lock> AsyncLockQueue<Resource>::lock(
    kj::Own<const Resource> resource, kj::Maybe<Hooks&> hooks) const {
  kj::Maybe<uint> currentLoad;
  if (hooks != kj::none) {
    currentLoad = getCurrentLoad();
  }

  for (uint blockedByOtherResourceCount = 0;; ++blockedByOtherResourceCount) {
    Waiter* waiter = *threadCurrentWaiter;

    if (waiter == nullptr) {
      // Thread isn't waiting on anything, so just queue up.
      KJ_IF_SOME(h, hooks) {
        h.reportAsyncInfo(
            KJ_ASSERT_NONNULL(currentLoad), false /* coalesced */, blockedByOtherResourceCount);
      }
      auto newWaiter = kj::refcounted<Waiter>(*this, kj::mv(resource));
      co_await newWaiter->readyPromise;
      co_return Lock(kj::mv(newWaiter));
    } else if (&waiter->queue == this) {
      // Thread is already queued for this same resource, so share its turn.
      KJ_IF_SOME(h, hooks) {
        h.reportAsyncInfo(
            KJ_ASSERT_NONNULL(currentLoad), true /* coalesced */, blockedByOtherResourceCount);
      }
      auto newWaiterRef = kj::addRef(*waiter);
      co_await newWaiterRef->readyPromise;
      co_return Lock(kj::mv(newWaiterRef));
    } else {
      // Thread holds, or is waiting for, a lock on a different resource. Wait for that to be
      // released before pursuing this one, so we only ever chase one at a time.
      // TODO(perf): Use of ForkedPromise leads to thundering herd here. Should be minor in
      //   practice, but we could consider creating another linked list instead...
      KJ_IF_SOME(h, hooks) {
        h.waitingForOtherResource(waiter->resource->getId());
      }
      co_await waiter->releasePromise;
    }
  }
}

template <typename Resource>
kj::Promise<void> AsyncLockQueue<Resource>::whenThreadIdle() {
  Waiter*& currentWaiter = *threadCurrentWaiter;
  for (;;) {
    if (currentWaiter != nullptr) {
      co_await currentWaiter->releasePromise;
      continue;
    }

    // yieldUntilWouldSleep() waits for both the queue and event port signals, so cross-thread
    // fulfiller wakeups are processed before we declare idle.
    co_await kj::yieldUntilWouldSleep();

    if (currentWaiter == nullptr) {
      co_return;
    }
    // Whoops, a new lock attempt appeared, loop.
  }
}

}  // namespace workerd
