// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/util/sync-wait-list.h>

#include <kj/async.h>
#include <kj/exception.h>
#include <kj/map.h>
#include <kj/refcount.h>

namespace workerd {

using kj::uint;

namespace _ {

template <typename T>
T cloneXThreadWaitListValue(const T& value)
  requires requires { T(value); }
{
  return T(value);
}

template <typename T>
kj::Arc<T> cloneXThreadWaitListValue(const kj::Arc<T>& value) {
  return value.addRef();
}

}  // namespace _

template <typename T>
concept XThreadWaitListValue = kj::isSameType<T, void>() || requires(const T& value) {
  { _::cloneXThreadWaitListValue(value) } -> kj::SameAs<T>;
};

// Allows multiple threads to wait for a result, and any thread to later provide that result. For
// non-void T, each waiter receives a copy of the fulfilled value, so T must be copy-constructible
// from const T& or be a kj::Arc. XThreadWaitList<void> signals an event without carrying a value.
//
// This is like using kj::newPromiseAndCrossThreadFulfiller<T>() and forking the promise, except:
// * Normally, a ForkedPromise's addBranch() can only be called in the thread that created the
//   fork. XThreadWaitList can be awaited from any thread.
// * XThreadWaitList is one object, not a promise/fulfiller pair. In many use cases, this turns out
//   to be most convenient. But if you want a separate fulfiller, call makeSeparateFulfiller().
template <XThreadWaitListValue T>
class XThreadWaitList;

template <>
class XThreadWaitList<void> {
 public:
  struct Options {
    // Enable this if it is common for there to be multiple waiters in the same thread. This avoids
    // sending multiple cross-thread signals in this case, instead sending one signal that all
    // waiters in the thread wait on.
    bool useThreadLocalOptimization = false;
  };

  XThreadWaitList(): XThreadWaitList(Options()) {}
  XThreadWaitList(Options options);
  XThreadWaitList(XThreadWaitList&& other) = default;
  ~XThreadWaitList() noexcept(false) {
    // Check if moved away.
    if (state.get() != nullptr) destroyed();
  }

  kj::Promise<void> addWaiter() const;

  // Wake all current *and future* waiters.
  void fulfill() const {
    tryFulfill();
  }

  // Wake all current and future waiters, returning false if the list was already settled.
  bool tryFulfill() const {
    KJ_IREQUIRE(!createdFulfiller);
    return state->tryFulfill();
  }

  // Causes all past and future `addWaiter()` calls to reject with the given exception.
  void reject(kj::Exception&& e) const {
    KJ_IREQUIRE(!createdFulfiller);
    state->reject(kj::mv(e));
  }

  // Has `fulfill()` or `reject()` been called? Of course, the caller should consider if
  // `fulfill()` might be called in another thread concurrently.
  bool isDone() const {
    return state->list.isReady() != kj::none;
  }

  // Creates a PromiseFulfiller that will fulfill this wait list. Once this is called, it is no
  // longer the XThreadWaitList's responsibility to fulfill the waiters.
  //
  // Arguably, we should always make people create a PromiseFulfiller-XThreadWaitList pair,
  // like kj::newPromiseAndFulfiller, instead of having methods directly on XThreadWaitList
  // to fulfill/reject. However, in practice, in many use cases the fulfiller would be stored
  // right next to the wait list, so it's convenient to let people opt into having two parts
  // explicitly.
  kj::Own<kj::CrossThreadPromiseFulfiller<void>> makeSeparateFulfiller();

 private:
  struct Outcome;
  struct State;
  struct Waiter;

  // The map holds only a *weak* reference to each Waiter. The Waiter is kept alive by the promise
  // branches handed out by addWaiter(); the Waiter's destructor is what removes it from this map.
  // Holding a strong reference here would form a cycle (map keeps Waiter alive -> refcount never
  // reaches zero -> destructor never runs -> entry never removed), so this must stay weak.
  using WaiterMap = kj::HashMap<const XThreadWaitList::State*, kj::WeakRc<Waiter>>;

  // Optimization: If the same wait list is waited multiple times in the same thread, we want to
  // share the signal rather than send two cross-thread signals.
  inline static const kj::EventLoopLocal<WaiterMap> threadLocalWaiters;

  struct Outcome {
    kj::Maybe<kj::Exception> exception;
  };

  struct Waiter: public kj::Refcounted {
    Waiter(const State& state, kj::Own<kj::CrossThreadPromiseFulfiller<void>> fulfiller);
    ~Waiter() noexcept(false);

    // Keeping the polymorphic registration in a member leaves its vtable intact while ~Waiter()
    // synchronizes with a concurrent callback by removing it from the list.
    class Registration final: public SyncWaitList<Outcome>::Waiter {
     public:
      explicit Registration(XThreadWaitList::Waiter& waiter): waiter(waiter) {}

     private:
      void ready(const Outcome& outcome) noexcept override;
      void removed() noexcept override;

      XThreadWaitList::Waiter& waiter;
    };

    kj::Own<const State> state;
    kj::Own<kj::CrossThreadPromiseFulfiller<void>> fulfiller;
    Registration registration;

    // Optimization: This is atomically set true when the waiter is removed from the list so that
    // we don't have to redundantly take the lock.
    bool unlinked = false;

    // Only initialized if useThreadLocalOptimization is enabled.
    kj::ForkedPromise<void> forkedPromise = nullptr;

    // ~Waiter() is `noexcept(false)`
    kj::UnwindDetector unwindDetector;
  };

  struct State: public kj::AtomicRefcounted {
    SyncWaitList<Outcome> list;

    const bool useThreadLocalOptimization = false;

    bool tryFulfill() const;
    void reject(kj::Exception&& e) const;
    void lostFulfiller() const;

    explicit State(const Options& options)
        : useThreadLocalOptimization(options.useThreadLocalOptimization) {}
  };

  kj::Own<const State> state;
  bool createdFulfiller = false;

  void destroyed();
};

using CrossThreadWaitList = XThreadWaitList<void>;

template <XThreadWaitListValue T>
class XThreadWaitList {
 public:
  struct Options {
    // Enable this if it is common for there to be multiple waiters in the same thread. This avoids
    // sending multiple cross-thread signals in this case, instead sending one signal that all
    // waiters in the thread wait on.
    bool useThreadLocalOptimization = false;
  };

  XThreadWaitList(): XThreadWaitList(Options()) {}
  XThreadWaitList(Options options): state(kj::atomicRefcounted<State>(options)) {}
  XThreadWaitList(XThreadWaitList&& other) = default;
  ~XThreadWaitList() noexcept(false) {
    if (state.get() != nullptr && !createdFulfiller) {
      state->lostFulfiller();
    }
  }

  kj::Promise<T> addWaiter() const {
    if (state->list.isReady() != kj::none) {
      return makeResultPromise(*state);
    }

    kj::Promise<void> notification = nullptr;
    if (state->useThreadLocalOptimization) {
      kj::Rc<Waiter> ownWaiter;

      auto& weak = threadLocalWaiters->findOrCreate(state.get(), [&]() -> WaiterMap::Entry {
        auto paf = kj::newPromiseAndCrossThreadFulfiller<void>();
        ownWaiter = kj::rc<Waiter>(*state, kj::mv(paf.fulfiller));
        ownWaiter->forkedPromise = paf.promise.fork();
        return {state.get(), ownWaiter.addWeakRef()};
      });

      if (ownWaiter.get() == nullptr) {
        ownWaiter = KJ_ASSERT_NONNULL(weak.upgrade());
      }

      notification = ownWaiter->forkedPromise.addBranch().attach(kj::mv(ownWaiter));
    } else {
      auto paf = kj::newPromiseAndCrossThreadFulfiller<void>();
      auto waiter = kj::rc<Waiter>(*state, kj::mv(paf.fulfiller));
      notification = kj::mv(paf.promise).attach(kj::mv(waiter));
    }

    return notification.then(
        [state = kj::atomicAddRef(*state)]() { return makeResultPromise(*state); });
  }

  void fulfill(T&& value) const {
    tryFulfill(kj::mv(value));
  }

  // Wake all current and future waiters, returning false if the list was already settled.
  bool tryFulfill(T&& value) const {
    KJ_IREQUIRE(!createdFulfiller);
    return state->tryFulfill(kj::mv(value));
  }

  void reject(kj::Exception&& exception) const {
    KJ_IREQUIRE(!createdFulfiller);
    state->reject(kj::mv(exception));
  }

  bool isDone() const {
    return state->list.isReady() != kj::none;
  }

  kj::Own<kj::CrossThreadPromiseFulfiller<T>> makeSeparateFulfiller() {
    class FulfillerImpl final: public kj::CrossThreadPromiseFulfiller<T> {
     public:
      FulfillerImpl(kj::Own<const State> state): state(kj::mv(state)) {}
      ~FulfillerImpl() noexcept(false) {
        state->lostFulfiller();
      }

      void fulfill(T&& value) const override {
        state->tryFulfill(kj::mv(value));
      }

      void reject(kj::Exception&& exception) const override {
        state->reject(kj::mv(exception));
      }

      bool isWaiting() const override {
        return state->list.isReady() == kj::none;
      }

     private:
      kj::Own<const State> state;
    };

    KJ_REQUIRE(!createdFulfiller, "makeSeparateFulfiller() can only be called once");
    createdFulfiller = true;
    return kj::heap<FulfillerImpl>(kj::atomicAddRef(*state));
  }

 private:
  struct Outcome {
    kj::Maybe<T> value;
    kj::Maybe<kj::Exception> exception;
  };

  struct State;

  struct Waiter: public kj::Refcounted {
    // Keeping the polymorphic registration in a member leaves its vtable intact while ~Waiter()
    // synchronizes with a concurrent callback by removing it from the list.
    class Registration final: public SyncWaitList<Outcome>::Waiter {
     public:
      explicit Registration(XThreadWaitList::Waiter& waiter): waiter(waiter) {}

     private:
      void ready(const Outcome&) noexcept override {
        waiter.fulfiller->fulfill();
        __atomic_store_n(&waiter.unlinked, true, __ATOMIC_RELEASE);
      }

      void removed() noexcept override {
        waiter.fulfiller->reject(makeNeverFulfilledException());
        __atomic_store_n(&waiter.unlinked, true, __ATOMIC_RELEASE);
      }

      XThreadWaitList::Waiter& waiter;
    };

    Waiter(const State& state, kj::Own<kj::CrossThreadPromiseFulfiller<void>> fulfiller)
        : state(kj::atomicAddRef(state)),
          fulfiller(kj::mv(fulfiller)),
          registration(*this) {
      state.list.add(registration);
    }

    ~Waiter() noexcept(false) {
      if (!__atomic_load_n(&unlinked, __ATOMIC_ACQUIRE)) {
        state->list.remove(registration);
      }

      if (state->useThreadLocalOptimization) {
        unwindDetector.catchExceptionsIfUnwinding([&]() {
          auto& entry = KJ_ASSERT_NONNULL(threadLocalWaiters->findEntry(state.get()));
          KJ_ASSERT(entry.value == nullptr);
          threadLocalWaiters->erase(entry);
        });
      }
    }

    kj::Own<const State> state;
    kj::Own<kj::CrossThreadPromiseFulfiller<void>> fulfiller;
    Registration registration;
    bool unlinked = false;
    kj::ForkedPromise<void> forkedPromise = nullptr;
    kj::UnwindDetector unwindDetector;
  };

  using WaiterMap = kj::HashMap<const State*, kj::WeakRc<Waiter>>;
  inline static const kj::EventLoopLocal<WaiterMap> threadLocalWaiters;

  struct State: public kj::AtomicRefcounted {
    SyncWaitList<Outcome> list;
    const bool useThreadLocalOptimization;

    explicit State(const Options& options)
        : useThreadLocalOptimization(options.useThreadLocalOptimization) {}

    bool tryFulfill(T&& value) const {
      return list.tryReady(Outcome{kj::mv(value), kj::none});
    }

    void reject(kj::Exception&& exception) const {
      list.tryReady(Outcome{kj::none, kj::mv(exception)});
    }

    void lostFulfiller() const {
      // See XThreadWaitList<void>::State::lostFulfiller().
      if (!isShared()) return;
      if (list.isReady() != kj::none) return;
      list.tryReady(Outcome{kj::none, makeNeverFulfilledException()});
    }
  };

  static void END_XTHREAD_WAIT_LIST_CANCELER_STACK_START_CANCELEE_STACK() {}

  static kj::Exception makeNeverFulfilledException() {
    return kj::getDestructionReason(
        reinterpret_cast<void*>(&END_XTHREAD_WAIT_LIST_CANCELER_STACK_START_CANCELEE_STACK),
        kj::Exception::Type::FAILED, __FILE__, __LINE__, "wait list was never fulfilled"_kj);
  }

  static kj::Promise<T> makeResultPromise(const State& state) {
    auto& outcome = KJ_ASSERT_NONNULL(state.list.isReady());
    KJ_IF_SOME(value, outcome.value) {
      // Convert exceptions thrown while cloning into a rejected promise for this waiter.
      return kj::evalNow([&]() { return _::cloneXThreadWaitListValue(value); });
    } else {
      return KJ_ASSERT_NONNULL(outcome.exception).clone();
    }
  }

  kj::Own<const State> state;
  bool createdFulfiller = false;
};

}  // namespace workerd
