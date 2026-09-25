// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/debug.h>
#include <kj/list.h>
#include <kj/mutex.h>

namespace workerd {

template <typename T>
class SyncWaitList {
  static_assert(!kj::isSameType<T, void>());

  template <typename U>
  static constexpr bool isConstructibleFrom() {
    return requires(U&& value) { T(kj::fwd<U>(value)); };
  }

 public:
  class Waiter {
   protected:
    Waiter() = default;

    // These callbacks may run while the wait list mutex is locked and must not re-enter or destroy
    // the wait list.
    virtual void ready(const T& value) noexcept = 0;
    virtual void removed() noexcept = 0;

   private:
    KJ_DISALLOW_COPY_AND_MOVE(Waiter);

    kj::ListLink<Waiter> link;

    friend class SyncWaitList;
  };

  SyncWaitList() = default;
  ~SyncWaitList() noexcept(false) {
    auto lock = state.waiters.lockExclusive();
    for (auto& waiter: *lock) {
      lock->remove(waiter);
      waiter.removed();
    }
  }

  KJ_DISALLOW_COPY_AND_MOVE(SyncWaitList);

  // Add a new waiter to the wait list. If the wait list is destroyed while the Waiter is still in
  // the list, removed will be called on the waiter.
  void add(Waiter& waiter) const {
    KJ_IF_SOME(value, tryAdd(waiter)) {
      waiter.ready(value);
    }
  }

  // Add a new waiter unless the wait list is already ready. If it is ready, returns its value
  // without calling ready() on the waiter.
  kj::Maybe<const T&> tryAdd(Waiter& waiter) const KJ_LIFETIMEBOUND {
    KJ_IF_SOME(value, isReady()) {
      return value;
    }

    auto lock = state.waiters.lockExclusive();

    // Do the same check after locking in case we got fulfilled in that time.
    KJ_IF_SOME(value, isReady()) {
      return value;
    }

    lock->add(waiter);
    return kj::none;
  }

  // Remove a waiter from the wait list. This does not call removed() on the Waiter.
  void remove(Waiter& waiter) const {
    auto lock = state.waiters.lockExclusive();
    if (waiter.link.isLinked()) {
      lock->remove(waiter);
    }
  }

  // Wake up all current and future waiters.
  void ready(auto&& value) const
    requires(isConstructibleFrom<decltype(value)>())
  {
    KJ_ASSERT(tryReady(kj::fwd<decltype(value)>(value)),
        "attempted to ready a list after it was already made ready");
  }

  // Wake up all current and future waiters, returning false if the list was already ready.
  bool tryReady(auto&& value) const
    requires(isConstructibleFrom<decltype(value)>())
  {
    auto lock = state.waiters.lockExclusive();
    if (__atomic_load_n(&state.valueSet, __ATOMIC_ACQUIRE)) {
      return false;
    }

    kj::ctor(state.value, kj::fwd<decltype(value)>(value));
    __atomic_store_n(&state.valueSet, true, __ATOMIC_RELEASE);

    for (auto& waiter: *lock) {
      lock->remove(waiter);
      waiter.ready(state.value);
    }
    return true;
  }

  kj::Maybe<const T&> isReady() const KJ_LIFETIMEBOUND {
    if (!__atomic_load_n(&state.valueSet, __ATOMIC_ACQUIRE)) {
      return kj::none;
    }

    return state.value;
  }

 private:
  struct State {
    State() {}
    ~State() noexcept(false) {
      if (valueSet) {
        kj::dtor(value);
      }
    }

    kj::MutexGuarded<kj::List<Waiter, &Waiter::link>> waiters;

    // Atomically set true once the value has been written.
    mutable bool valueSet = false;

    union {
      mutable T value;
    };
  };

  State state;
};

template <>
class SyncWaitList<void> {
 public:
  class Waiter {
   protected:
    Waiter() = default;

    // These callbacks may run while the wait list mutex is locked and must not re-enter or destroy
    // the wait list.
    virtual void ready() noexcept = 0;
    virtual void removed() noexcept = 0;

   private:
    KJ_DISALLOW_COPY_AND_MOVE(Waiter);

    kj::ListLink<Waiter> link;

    friend class SyncWaitList;
  };

  SyncWaitList() = default;
  ~SyncWaitList() noexcept(false) {
    auto lock = state.waiters.lockExclusive();
    for (auto& waiter: *lock) {
      lock->remove(waiter);
      waiter.removed();
    }
  }

  KJ_DISALLOW_COPY_AND_MOVE(SyncWaitList);

  // Add a new waiter to the wait list. If the wait list is destroyed while the Waiter is still in
  // the list, removed will be called on the waiter.
  void add(Waiter& waiter) const {
    if (tryAdd(waiter)) {
      waiter.ready();
    }
  }

  // Add a new waiter unless the wait list is already ready. Returns true if the list was already
  // ready without calling ready() on the waiter.
  bool tryAdd(Waiter& waiter) const {
    if (isReady()) {
      return true;
    }

    auto lock = state.waiters.lockExclusive();

    // Do the same check after locking in case we got fulfilled in that time.
    if (isReady()) {
      return true;
    }

    lock->add(waiter);
    return false;
  }

  // Remove a waiter from the wait list. This does not call removed() on the Waiter.
  void remove(Waiter& waiter) const {
    auto lock = state.waiters.lockExclusive();
    if (waiter.link.isLinked()) {
      lock->remove(waiter);
    }
  }

  // Wake up all current and future waiters.
  void ready() const {
    KJ_ASSERT(tryReady(), "attempted to ready a list after it was already made ready");
  }

  // Wake up all current and future waiters, returning false if the list was already ready.
  bool tryReady() const {
    auto expected = false;
    auto madeReady = __atomic_compare_exchange_n(
        &state.ready, &expected, true, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
    if (!madeReady) {
      return false;
    }

    wakeUpReady();
    return true;
  }

  bool isReady() const {
    return __atomic_load_n(&state.ready, __ATOMIC_RELAXED);
  }

 private:
  void wakeUpReady() const {
    auto lock = state.waiters.lockExclusive();
    for (auto& waiter: *lock) {
      lock->remove(waiter);
      waiter.ready();
    }
  }

  struct State {
    kj::MutexGuarded<kj::List<Waiter, &Waiter::link>> waiters;

    // Atomically set true once ready() is called.
    mutable bool ready = false;
  };

  State state;
};

}  // namespace workerd
