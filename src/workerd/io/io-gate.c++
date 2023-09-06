// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/io/io-gate.h>
#include <kj/debug.h>

namespace workerd {

InputGate::Hooks InputGate::Hooks::DEFAULT;

InputGate::InputGate(Hooks& hooks): InputGate(hooks, kj::newPromiseAndFulfiller<void>()) {}
InputGate::InputGate(Hooks& hooks, kj::PromiseFulfillerPair<void> paf)
    : hooks(hooks),
      brokenPromise(paf.promise.fork()),
      brokenState(kj::mv(paf.fulfiller)) {}
InputGate::~InputGate() noexcept {
  // Intentionally `noexcept` because if this throws then there are dangling references.
  KJ_ASSERT(lockCount == 0,
      "destroying InputGate when locks are still present; they would become dangling references");

  // If the lock count is zero, then the waiters must be empty.
  KJ_ASSERT(waiters.empty());
}

InputGate::Waiter::Waiter(
    kj::PromiseFulfiller<Lock>& fulfiller, InputGate& gate, bool isChildWaiter)
    : fulfiller(fulfiller), gate(&gate), isChildWaiter(isChildWaiter) {
  gate.hooks.inputGateWaiterAdded();
  if (isChildWaiter) {
    gate.waitingChildren.add(*this);
  } else {
    gate.waiters.add(*this);
  }
}
InputGate::Waiter::~Waiter() noexcept(false) {
  gate->hooks.inputGateWaiterRemoved();
  if (link.isLinked()) {
    if (isChildWaiter) {
      gate->waitingChildren.remove(*this);
    } else {
      gate->waiters.remove(*this);
    }
  }
}

kj::Promise<InputGate::Lock> InputGate::wait() {
  KJ_IF_SOME(e, brokenState.tryGet<kj::Exception>()) {
    return kj::cp(e);
  } else if (lockCount == 0) {
    return Lock(*this);
  } else {
    return kj::newAdaptedPromise<Lock, Waiter>(*this, false);
  }
}

kj::Promise<void> InputGate::onBroken() {
  KJ_IF_SOME(e, brokenState.tryGet<kj::Exception>()) {
    return kj::cp(e);
  } else {
    return brokenPromise.addBranch();
  }
}

InputGate::Lock::Lock(InputGate& gate)
    : gate(&gate),
      cs(gate.isCriticalSection
          ? kj::Maybe(kj::addRef(static_cast<CriticalSection&>(gate)))
          : kj::none) {
  InputGate* gateToLock = &gate;

  KJ_IF_SOME(c, cs) {
    if (c.get()->state == CriticalSection::REPARENTED) {
      gateToLock = &c.get()->parentAsInputGate();
    }
  }

  if (++gateToLock->lockCount == 1) {
    gateToLock->hooks.inputGateLocked();
  }
}

void InputGate::releaseLock() {
  if (isCriticalSection) {
    auto& self = static_cast<CriticalSection&>(*this);
    if (self.state == CriticalSection::REPARENTED) {
      // This lock was for a critical section that has already completed, therefore the lock
      // should be considered "reparented", and we should forward the release to the parent.

      // Ensure any waiters on us have already been reparented.
      KJ_DASSERT(self.waitingChildren.size() == 0);
      KJ_DASSERT(self.waiters.size() == 0);
      KJ_DASSERT(lockCount == 0);

      self.parentAsInputGate().releaseLock();
      return;
    }
  }

  KJ_ASSERT(lockCount-- > 0);

  // Check if any waiters can be released.
  if (lockCount == 0) {
    hooks.inputGateReleased();
    if (!waitingChildren.empty()) {
      auto& waiter = waitingChildren.front();
      waitingChildren.remove(waiter);
      waiter.fulfiller.fulfill(Lock(*this));
    } else if (!waiters.empty()) {
      auto& waiter = waiters.front();
      waiters.remove(waiter);
      waiter.fulfiller.fulfill(Lock(*this));
    }
  }
}

kj::Own<InputGate::CriticalSection> InputGate::Lock::startCriticalSection() {
  return kj::refcounted<CriticalSection>(*gate);
}

kj::Maybe<InputGate::CriticalSection&> InputGate::Lock::getCriticalSection() {
  if (gate->isCriticalSection) {
    return static_cast<CriticalSection&>(*gate);
  } else {
    return kj::none;
  }
}

bool InputGate::Lock::isFor(const InputGate& otherGate) const {
  KJ_ASSERT(!otherGate.isCriticalSection);

  InputGate* ptr = gate;
  while (ptr->isCriticalSection) {
    ptr = &static_cast<CriticalSection&>(*ptr).parentAsInputGate();
  }
  return ptr == &otherGate;
}

InputGate::CriticalSection::CriticalSection(InputGate& parent) {
  isCriticalSection = true;
  if (parent.isCriticalSection) {
    this->parent = kj::addRef(static_cast<CriticalSection&>(parent));
  } else {
    this->parent = &parent;
  }
}
InputGate::CriticalSection::~CriticalSection() noexcept(false) {
  switch (state) {
    case NOT_STARTED:
      // Oh well.
      break;
    case INITIAL_WAIT:
      // The initial wait() had better have been canceled... but we have no way to tell here.
      break;
    case RUNNING:
      failed(JSG_KJ_EXCEPTION(FAILED, Error,
          "A critical section within this Durable Object awaited a Promise that "
          "apparently will never complete. This could happen in particular if a critical section "
          "awaits a task that was initiated outside of the critical section. Since a critical "
          "section blocks all other tasks from completing, this leads to deadlock."));
      break;
    case REPARENTED:
      // Common case.
      break;
  }
}

kj::Promise<InputGate::Lock> InputGate::CriticalSection::wait() {
  switch (state) {
    case NOT_STARTED: {
      state = INITIAL_WAIT;

      auto& target = parentAsInputGate();
      KJ_IF_SOME(e, target.brokenState.tryGet<kj::Exception>()) {
        // Oops, we're broken.
        setBroken(e);
        return kj::cp(e);
      }

      // Add ourselves to this parent's child waiter list.
      if (target.lockCount == 0) {
        state = RUNNING;
        parentLock = Lock(target);
        return wait();
      } else {
        return kj::newAdaptedPromise<Lock, Waiter>(target, true)
            .then([this](Lock lock) {
          state = RUNNING;
          parentLock = kj::mv(lock);
          return wait();
        }, [this](kj::Exception&& e) -> kj::Promise<InputGate::Lock> {
          state = RUNNING;
          setBroken(e);
          return kj::mv(e);
        });
      }
    }
    case INITIAL_WAIT:
      // To avoid the need for a ForkedPromise, we assume wait() is called once initially to
      // get things started. This is the case in practice because any further tasks would be
      // started only after some code runs under the initial lock.
      KJ_FAIL_REQUIRE("CriticalSection::wait() should be called once initially");
    case RUNNING:
      // CriticalSection is active, so defer to InputGate implementation.
      return InputGate::wait();
    case REPARENTED:
      // Once the CriticalSection has declared itself done, then any straggler tasks it initiated
      // are adopted by the parent.
      // WARNING: Don't use parentAsInputGate() here as that'll bypass the override of wait() if
      //   the parent is a CriticalSection itself.
      KJ_SWITCH_ONEOF(parent) {
        KJ_CASE_ONEOF(p, InputGate*) {
          return p->wait();
        }
        KJ_CASE_ONEOF(c, kj::Own<CriticalSection>) {
          return c->wait();
        }
      }
      KJ_UNREACHABLE;
  }
  KJ_UNREACHABLE;
}

InputGate::Lock InputGate::CriticalSection::succeeded() {
  KJ_REQUIRE(state == RUNNING);

  // Once the CriticalSection has declared itself done, then any straggler tasks it initiated are
  // adopted by the parent.
  auto& parentGate = parentAsInputGate();
  for (auto& waiter: waitingChildren) {
    waitingChildren.remove(waiter);
    parentGate.waitingChildren.add(waiter);
    waiter.gate = &parentGate;
  }
  for (auto& waiter: waiters) {
    waiters.remove(waiter);
    parentGate.waiters.add(waiter);
    waiter.gate = &parentGate;
  }
  parentGate.lockCount += lockCount;
  lockCount = 0;

  state = REPARENTED;
  auto result = KJ_ASSERT_NONNULL(kj::mv(parentLock));
  parentLock = kj::none;
  return result;
}

void InputGate::CriticalSection::failed(const kj::Exception& e) {
  if (brokenState.is<kj::Exception>()) {
    // Already failed I guess.
    return;
  }

  setBroken(e);
  KJ_SWITCH_ONEOF(parent) {
    KJ_CASE_ONEOF(p, InputGate*) {
      p->setBroken(e);
    }
    KJ_CASE_ONEOF(c, kj::Own<CriticalSection>) {
      c->failed(e);
    }
  }
}

void InputGate::setBroken(const kj::Exception& e) {
  for (auto& waiter: waitingChildren) {
    waiter.fulfiller.reject(kj::cp(e));
    waitingChildren.remove(waiter);
  }
  for (auto& waiter: waiters) {
    waiter.fulfiller.reject(kj::cp(e));
    waiters.remove(waiter);
  }
  KJ_IF_SOME(f, brokenState.tryGet<kj::Own<kj::PromiseFulfiller<void>>>()) {
    f.get()->reject(kj::cp(e));
  }
  brokenState = kj::cp(e);
}

InputGate& InputGate::CriticalSection::parentAsInputGate() {
  CriticalSection* ptr = this;
  for(;;) {
    KJ_SWITCH_ONEOF(ptr->parent) {
      KJ_CASE_ONEOF(p, InputGate*) {
        return *p;
      }
      KJ_CASE_ONEOF(c, kj::Own<CriticalSection>) {
        if (c.get()->state == REPARENTED) {
          // Keep looping...
          ptr = c;
        } else {
          return *c.get();
        }
      }
    }
  }
}

// =======================================================================================

OutputGate::OutputGate(Hooks& hooks)
    : hooks(hooks), pastLocksPromise(kj::Promise<void>(kj::READY_NOW).fork()) {}
OutputGate::~OutputGate() noexcept(false) {}

OutputGate::Hooks OutputGate::Hooks::DEFAULT;

kj::Own<kj::PromiseFulfiller<void>> OutputGate::lock() {
  auto paf = kj::newPromiseAndFulfiller<void>();
  auto joined = kj::joinPromises(kj::arr(pastLocksPromise.addBranch(), kj::mv(paf.promise)));
  pastLocksPromise = joined.fork();
  return kj::mv(paf.fulfiller);
}

kj::Promise<void> OutputGate::wait() {
  hooks.outputGateWaiterAdded();
  return pastLocksPromise.addBranch().attach(kj::defer([this]() {
    hooks.outputGateWaiterRemoved();
  }));
}

kj::Promise<void> OutputGate::onBroken() {
  KJ_REQUIRE(!brokenState.is<kj::Own<kj::PromiseFulfiller<void>>>(),
      "onBroken() can only be called once");

  KJ_IF_SOME(e, brokenState.tryGet<kj::Exception>()) {
    return kj::cp(e);
  } else {
    auto paf = kj::newPromiseAndFulfiller<void>();
    brokenState = kj::mv(paf.fulfiller);
    return kj::mv(paf.promise);
  }
}

bool OutputGate::isBroken() {
  return brokenState.is<kj::Exception>();
}

namespace {

void END_OUTPUT_LOCK_CANCELATION_STACK_START_WAITER_STACK() {}

} // namespace

kj::Exception OutputGate::makeUnfulfilledException() {
  return kj::getDestructionReason(
      reinterpret_cast<void*>(&END_OUTPUT_LOCK_CANCELATION_STACK_START_WAITER_STACK),
      kj::Exception::Type::FAILED, __FILE__, __LINE__,
      "output lock was canceled before completion"_kj);
}

void OutputGate::setBroken(const kj::Exception& e) {
  // We assume the exception is already propagated into `pastLocksPromise`, so all we need to do
  // is handle onBroken().
  KJ_IF_SOME(f, brokenState.tryGet<kj::Own<kj::PromiseFulfiller<void>>>()) {
    f.get()->reject(kj::cp(e));
  }
  brokenState = kj::cp(e);
}

}  // namespace workerd
