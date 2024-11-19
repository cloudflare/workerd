// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/async.h>
#include <kj/list.h>
#include <kj/map.h>
#include <kj/mutex.h>

namespace workerd {

using kj::uint;

// A class that allows multiple threads to wait for an event, and for any thread to later trigger
// that event. This is like using kj::newPromiseAndCrossThreadFulfiller<void>() and forking the
// promise, except:
// * Normally, a ForkedPromise's addBranch() can only be called in the thread that created the
//   fork. `CrossThreadWaitList` can be awaited from any thread.
// * CrossThreadWaitList is one object, not a promise/fulfiller pair. In many use cases, this
//   turns out to be most convenient. But if you want a separate fulfiller, you can call the
//   `makeSeparateFulfiller()` method.
class CrossThreadWaitList {
 public:
  struct Options {
    // Enable this if it is common for there to be multiple waiters in the same thread. This avoids
    // sending multiple cross-thread signals in this case, instead sending one signal that all
    // waiters in the thread wait on.
    bool useThreadLocalOptimization = false;
  };

  CrossThreadWaitList(): CrossThreadWaitList(Options()) {}
  CrossThreadWaitList(Options options);
  CrossThreadWaitList(CrossThreadWaitList&& other) = default;
  ~CrossThreadWaitList() noexcept(false) {
    // Check if moved away.
    if (state.get() != nullptr) destroyed();
  }

  kj::Promise<void> addWaiter() const;

  // Wake all current *and future* waiters.
  void fulfill() const {
    KJ_IREQUIRE(!createdFulfiller);
    state->fulfill();
  }

  // Causes all past and future `addWaiter()` calls to reject with the given exception.
  void reject(kj::Exception&& e) const {
    KJ_IREQUIRE(!createdFulfiller);
    state->reject(kj::mv(e));
  }

  // Has `fulfill()` or `reject()` been called? Of course, the caller should consider if
  // `fulfill()` might be called in another thread concurrently.
  bool isDone() const {
    return __atomic_load_n(&state->done, __ATOMIC_ACQUIRE);
  }

  // Creates a PromiseFulfiller that will fulfill this wait list. Once this is called, it is no
  // longer the CrossThreadWaitList's responsibility to fulfill the waiters.
  //
  // Arguably, we should always make people create a PromiseFulfiller-CrossThreadWaitList pair,
  // like kj::newPromiseAndFulfiller, instead of having methods directly on CrossThreadWaitList
  // to fulfill/reject. However, in practice, in many use cases the fulfiller would be stored
  // right next to the wait list, so it's convenient to let people opt into having two parts
  // explicitly.
  kj::Own<kj::CrossThreadPromiseFulfiller<void>> makeSeparateFulfiller();

 private:
  // Forward declare our private structs so we can name the Map for public use in the source file.
  struct State;
  struct Waiter;

 public:
  using WaiterMap = kj::HashMap<const CrossThreadWaitList::State*, Waiter*>;

 private:
  struct Waiter: public kj::Refcounted {
    Waiter(const State& state, kj::Own<kj::CrossThreadPromiseFulfiller<void>> fulfiller);
    ~Waiter() noexcept(false);

    kj::Own<const State> state;
    kj::Own<kj::CrossThreadPromiseFulfiller<void>> fulfiller;

    // Protected by list mutex.
    kj::ListLink<Waiter> link;

    // Optimization: This is atomically set true when the waiter is removed from the list so that
    // we don't have to redundantly take the lock.
    bool unlinked = false;

    // Only initialized if useThreadLocalOptimization is enabled.
    kj::ForkedPromise<void> forkedPromise = nullptr;
  };

  struct State: public kj::AtomicRefcounted {
    kj::MutexGuarded<kj::List<Waiter, &Waiter::link>> waiters;

    const bool useThreadLocalOptimization = false;

    // Atomically set true at the start of fulfill() or reject(). This can be checked before taking
    // the lock, but if false, it must be checked again after taking the lock, to avoid a race.
    mutable bool done = false;

    // If `done` is true due to `reject()` being called, this is the exception. This field
    // does not change after `done` is set true.
    mutable kj::Maybe<kj::Exception> exception;

    bool wakeNext() const;
    void fulfill() const;
    void reject(kj::Exception&& e) const;
    void lostFulfiller() const;

    explicit State(const Options& options)
        : useThreadLocalOptimization(options.useThreadLocalOptimization) {}
  };

  kj::Own<const State> state;
  bool createdFulfiller = false;

  void destroyed();
};

}  // namespace workerd
