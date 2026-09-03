// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "wait-list.h"

#include <kj/debug.h>

namespace workerd {

namespace {
void END_WAIT_LIST_CANCELER_STACK_START_CANCELEE_STACK() {}

kj::Exception makeNeverFulfilledException() {
  return kj::getDestructionReason(
      reinterpret_cast<void*>(&END_WAIT_LIST_CANCELER_STACK_START_CANCELEE_STACK),
      kj::Exception::Type::FAILED, __FILE__, __LINE__, "wait list was never fulfilled"_kj);
}
}  // namespace

CrossThreadWaitList::CrossThreadWaitList(Options options)
    : state(kj::atomicRefcounted<State>(options)) {}

void CrossThreadWaitList::destroyed() {
  if (!createdFulfiller) state->lostFulfiller();
}

CrossThreadWaitList::Waiter::Waiter(
    const State& state, kj::Own<kj::CrossThreadPromiseFulfiller<void>> fulfillerArg)
    : state(kj::atomicAddRef(state)),
      fulfiller(kj::mv(fulfillerArg)) {
  state.list.add(*this);
}
CrossThreadWaitList::Waiter::~Waiter() noexcept(false) {
  if (!__atomic_load_n(&unlinked, __ATOMIC_ACQUIRE)) {
    state->list.remove(*this);
  }

  if (state->useThreadLocalOptimization) {
    unwindDetector.catchExceptionsIfUnwinding([&]() {
      auto& entry = KJ_ASSERT_NONNULL(threadLocalWaiters->findEntry(state.get()));
      // Our refcount has reached zero, so the weak reference to us has already expired. Since the
      // map holds exactly one entry per State (in this thread) and we own a reference to that
      // State, this entry must be ours.
      KJ_ASSERT(entry.value == nullptr);
      threadLocalWaiters->erase(entry);
    });
  }
}

void CrossThreadWaitList::Waiter::ready(const Outcome& outcome) noexcept {
  KJ_IF_SOME(exception, outcome.exception) {
    fulfiller->reject(exception.clone());
  } else {
    fulfiller->fulfill();
  }
  __atomic_store_n(&unlinked, true, __ATOMIC_RELEASE);
}

void CrossThreadWaitList::Waiter::removed() noexcept {
  fulfiller->reject(makeNeverFulfilledException());
  __atomic_store_n(&unlinked, true, __ATOMIC_RELEASE);
}

kj::Promise<void> CrossThreadWaitList::addWaiter() const {
  KJ_IF_SOME(outcome, state->list.isReady()) {
    KJ_IF_SOME(exception, outcome.exception) {
      return exception.clone();
    } else {
      return kj::READY_NOW;
    }
  }

  if (state->useThreadLocalOptimization) {
    // The map holds only a weak reference to the shared Waiter; the strong reference lives on the
    // returned promise (via attach), and ~Waiter is what removes the entry. Because destruction is
    // synchronous and single-threaded, a present entry is always still alive when observed here, so
    // findOrCreate() only ever runs its lambda when there is genuinely no waiter yet.
    kj::Rc<Waiter> ownWaiter;

    auto& weak = threadLocalWaiters->findOrCreate(
        state.get(), [&]() -> CrossThreadWaitList::WaiterMap::Entry {
      auto paf = kj::newPromiseAndCrossThreadFulfiller<void>();
      ownWaiter = kj::rc<Waiter>(*state, kj::mv(paf.fulfiller));
      ownWaiter->forkedPromise = paf.promise.fork();
      return {state.get(), ownWaiter.addWeakRef()};
    });

    if (ownWaiter.get() == nullptr) {
      // Reusing a waiter created earlier by another waiter in this thread.
      ownWaiter = KJ_ASSERT_NONNULL(weak.upgrade());
    }

    return ownWaiter->forkedPromise.addBranch().attach(kj::mv(ownWaiter));
  } else {
    // No refcounting, no forked promise.
    auto paf = kj::newPromiseAndCrossThreadFulfiller<void>();
    auto waiter = kj::rc<Waiter>(*state, kj::mv(paf.fulfiller));
    return paf.promise.attach(kj::mv(waiter));
  }
}

kj::Own<kj::CrossThreadPromiseFulfiller<void>> CrossThreadWaitList::makeSeparateFulfiller() {
  class FulfillerImpl final: public kj::CrossThreadPromiseFulfiller<void> {
   public:
    FulfillerImpl(kj::Own<const State> state): state(kj::mv(state)) {}
    ~FulfillerImpl() noexcept(false) {
      state->lostFulfiller();
    }
    void fulfill(kj::_::Void&&) const override {
      state->tryFulfill();
    }
    void reject(kj::Exception&& exception) const override {
      state->reject(kj::mv(exception));
    }
    bool isWaiting() const override {
      // Note that it would be incorrect for isWaiting() to return false when the list is not ready
      // even if the waiter list is empty, because the waiter list could become non-empty later.
      // In theory if we could determine that there will never be a waiter, then isWaiting()
      // could return false.
      return state->list.isReady() == kj::none;
    }

   private:
    kj::Own<const State> state;
  };

  KJ_REQUIRE(!createdFulfiller, "makeSeparateFulfiller() can only be called once");
  createdFulfiller = true;
  return kj::heap<FulfillerImpl>(kj::atomicAddRef(*state));
}

bool CrossThreadWaitList::State::tryFulfill() const {
  return list.tryReady(Outcome{kj::none});
}

void CrossThreadWaitList::State::reject(kj::Exception&& e) const {
  list.tryReady(Outcome{kj::mv(e)});
}

void CrossThreadWaitList::State::lostFulfiller() const {
  if (list.isReady() != kj::none) return;
  list.tryReady(Outcome{makeNeverFulfilledException()});
}

}  // namespace workerd
