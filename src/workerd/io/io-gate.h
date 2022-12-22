// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// An I/O gate allows someone to "lock" a type of I/O so that other concurrent tasks trying to
// perform that type of I/O are blocked until the lock is released.
//
// I/O gates are used in actors to implement consistency guarantees, allowing in-memory state and
// storage to be synchronized.
//
// Each Actor has two main gates:
// - Input gate: While locked, blocks all incoming I/O events of any type from being delivered to
//   the actor, other than the specific event or events that hold the lock. This includes
//   blocking responses to subrequests, timer events, input streams, etc. Used when storage
//   operations are outstanding, so that awaiting a storage operation does not risk allowing
//   concurrent events that render the state inconsistent.
// - Output gate: While locked, blocks all outgoing messages from an actor that would allow the
//   rest of the world to observe the actor's state. Held while writes that have been confirmed
//   to the application are still being flushed to disk. If the flush fails, these messages will
//   never be sent, so that the rest of the world cannot observe a prematurely-confirmed write.

#include <kj/async.h>
#include <kj/one-of.h>
#include <kj/list.h>
#include <workerd/jsg/jsg.h>

#include <type_traits>

namespace workerd {

using kj::uint;

class InputGate {
  // An InputGate blocks incoming events from being delivered to an actor while the lock is held.

public:
  class Hooks {
    // Hooks that can be used to customize InputGate behavior.
    //
    // Technically, everything implemented here could be accomplished by a class that wraps
    // InputGate, but the part of the code that wants to implement these hooks (Worker::Actor)
    // is far away from the part of the code that calls into the InputGate (ActorCache), and so
    // it was more convenient to give Worker::Actor a way to inject behavior into InputGate which
    // would kick in when ActorCache tried to use it.

  public:
    virtual void inputGateLocked() {}
    virtual void inputGateReleased() {}
    virtual void inputGateWaiterAdded() {}
    virtual void inputGateWaiterRemoved() {}
    // Optionally track metrics. In practice these are implemented by MetricsCollector::Actor, but
    // we don't want to depend on that class from here.

    static Hooks DEFAULT;
  };

  InputGate(Hooks& hooks = Hooks::DEFAULT);
  ~InputGate() noexcept;

  class CriticalSection;

  class Lock {
    // A lock that blocks all new events from being delivered while it exists.

  public:
    KJ_DISALLOW_ONLY_COPY(Lock);
    Lock(Lock&& other): gate(other.gate), cs(kj::mv(other.cs)) { other.gate = nullptr; }
    ~Lock() noexcept(false) { if (gate != nullptr) gate->releaseLock(); }

    Lock addRef() { return Lock(*gate); }
    // Increments the lock's refcount, returning a duplicate `Lock`. All `Lock`s must be dropped
    // before the gate is unlocked.

    kj::Own<CriticalSection> startCriticalSection();
    // Start a new critical section from this lock. After `wait()` has been called on the returned
    // critical section for the first time, no further Locks will be handed out by
    // InputGate::wait() until the CriticalSection has been dropped.
    //
    // CriticalSections can be nested. If this Lock is itself part of a CriticalSection, the new
    // CriticalSection will be nested within it and the outer CriticalSection's wait() won't
    // produce a Lock again until the inner CriticalSection is dropped.

    kj::Maybe<CriticalSection&> getCriticalSection();
    // If this lock was taken in a CriticalSection, return it.

    bool isFor(const InputGate& gate) const;

    inline bool operator==(const Lock& other) const { return gate == other.gate; }

  private:
    InputGate* gate;
    // Becomes null on move.

    kj::Maybe<kj::Own<CriticalSection>> cs;

    Lock(InputGate& gate);
    friend class InputGate;
  };

  kj::Promise<Lock> wait();
  // Wait until there are no `Lock`s, then create a new one and return it.

  kj::Promise<void> onBroken();
  // Rejects if and when calls to `wait()` become broken due to a failed critical section. The
  // actor should be shut down in this case. This promise never resolves, only rejects.

private:
  Hooks& hooks;

  uint lockCount = 0;
  // How many instances of `Lock` currently exist? When this reaches zero, we'll release some
  // waiters.

  bool isCriticalSection = false;
  // CriticalSection inherits InputGate for implementation convenience (since much implementation
  // is shared).

  struct Waiter {
    Waiter(kj::PromiseFulfiller<Lock>& fulfiller, InputGate& gate, bool isChildWaiter);
    ~Waiter() noexcept(false);

    kj::PromiseFulfiller<Lock>& fulfiller;
    InputGate* gate;
    bool isChildWaiter;
    kj::ListLink<Waiter> link;
  };

  kj::List<Waiter, &Waiter::link> waiters;

  kj::List<Waiter, &Waiter::link> waitingChildren;
  // Waiters representing CriticalSections that are ready to start. These take priority over other
  // waiters.

  kj::ForkedPromise<void> brokenPromise;
  kj::OneOf<kj::Own<kj::PromiseFulfiller<void>>, kj::Exception> brokenState;
  // A fulfiller for onBroken(), or an exception if already broken.

  void releaseLock();

  void setBroken(const kj::Exception& e);
  // Called when a critical section fails. All future waiters will throw this exception.

  InputGate(Hooks& hooks, kj::PromiseFulfillerPair<void> paf);
};

class InputGate::CriticalSection: private InputGate, public kj::Refcounted {
  // A CriticalSection is a procedure that must not be interrupted by anything "external".
  // While a CriticalSection is running, all events that were not initiated by the
  // CriticalSection itself will be blocked from being delivered.
  //
  // The difference between a Lock and a CriticalSection is that a critical section may succeed
  // or fail. A failed critical section permanently breaks the input gate. Locks, on the other
  // hand, are simply released when dropped.
  //
  // A CriticalSection itself holds a Lock, which blocks the "parent scope" from continuing
  // execution until the critical section is done. Meanwhile, the code running inside the critical
  // section obtains nested Locks. These nested locks control concurrency of the operations
  // initiated within the critical section in the same way that input locks normally do at the
  // top-level scope. E.g., if a critical section initiates a storage read and a fetch() at the
  // same time, the fetch() is prevented from returning until after the storage read has returned.

public:
  CriticalSection(InputGate& parent);
  ~CriticalSection() noexcept(false);

  kj::Promise<Lock> wait();
  // Wait for a nested lock in order to continue this CriticalSection.
  //
  // The first call to wait() begins the CriticalSection. After that wait completes, until the
  // CriticalSection is done and dropped, no other locks will be allowed on this InputGate, except
  // locks requested by calling wait() on this CriticalSection -- or one of its children.

  Lock succeeded();
  // Call when the critical section has completed successfully. If this is not called before the
  // CriticalSection is dropped, then failed() is called implicitly.
  //
  // Returns the input lock that was held on the parent critical section. This can be used to
  // continue execution in the parent before any other input arrives.

  void failed(const kj::Exception& e);
  // Call to indicate the CriticalSection has failed with the given exception. This immediately
  // breaks the InputGate.

private:
  enum State {
    NOT_STARTED,
    // wait() hasn't been called.

    INITIAL_WAIT,
    // wait() has been called once, and that wait hasn't finished yet.

    RUNNING,
    // First lock has been obtained, waiting for success() or failed().

    REPARENTED
    // success() or failed() has been called.
  };

  State state = NOT_STARTED;

  kj::OneOf<InputGate*, kj::Own<CriticalSection>> parent;
  // Points to the parent scope, which may be another CriticalSection in the case of nesting.

  kj::Maybe<Lock> parentLock;
  // A lock in the parent scope. `parentLock` becomes non-null after the first lock is obtained,
  // and becomes null again when succeeded() is called.

  friend class InputGate;

  InputGate& parentAsInputGate();
  // Return a reference for the parent scope, skipping any reparented CriticalSections
};

class OutputGate {
  // An OutputGate blocks outgoing messages from an Actor until writes which they might depend on
  // are confirmed.

public:
  class Hooks {
    // Hooks that can be used to customize OutputGate behavior.
    //
    // Technically, everything implemented here could be accomplished by a class that wraps
    // OutputGate, but the part of the code that wants to implement these hooks (Worker::Actor)
    // is far away from the part of the code that calls into the OutputGate (ActorCache), and so
    // it was more convenient to give Worker::Actor a way to inject behavior into OutputGate which
    // would kick in when ActorCache tried to use it.

  public:
    virtual kj::Promise<void> makeTimeoutPromise() { return kj::NEVER_DONE; }
    // Optionally make a promise which should be exclusiveJoin()ed with the lock promise to
    // implement a timeout. The returned promise should be something that throws an exception
    // after some timeout has expired.

    virtual void outputGateLocked() {}
    virtual void outputGateReleased() {}
    virtual void outputGateWaiterAdded() {}
    virtual void outputGateWaiterRemoved() {}
    // Optionally track metrics. In practice these are implemented by MetricsCollector::Actor, but
    // we don't want to depend on that class from here.

    static Hooks DEFAULT;
  };

  OutputGate(Hooks& hooks = Hooks::DEFAULT);
  ~OutputGate() noexcept(false);

  template <typename T>
  kj::Promise<T> lockWhile(kj::Promise<T> promise);
  // Block all future `wait()` calls until `promise` completes. Returns a wrapper around `promise`.
  // If `promise` rejects, the exception will propagate to all future `wait()`s. If the returned
  // promise is canceled before completion, all future `wait()`s will also throw.

  kj::Promise<void> wait();
  // Wait until all preceding locks are released. The wait will not be affected by any future
  // call to `lockWhile()`.

  kj::Promise<void> onBroken();
  // Rejects if and when calls to `wait()` become broken due to a failed lockWhile(). The actor
  // should be shut down in this case. This promise never resolves, only rejects.
  //
  // This method can only be called once.

  bool isBroken();

private:
  Hooks& hooks;

  kj::ForkedPromise<void> pastLocksPromise;

  kj::OneOf<kj::Own<kj::PromiseFulfiller<void>>, kj::Exception> brokenState;
  // A fulfiller for onBroken(), or an exception if already broken.

  void setBroken(const kj::Exception& e);

  kj::Own<kj::PromiseFulfiller<void>> lock();
  static kj::Exception makeUnfulfilledException();
};

// =======================================================================================
// inline implementation details

inline InputGate::Lock::Lock(InputGate& gate)
    : gate(&gate),
      cs(gate.isCriticalSection
          ? kj::Maybe(kj::addRef(static_cast<CriticalSection&>(gate)))
          : nullptr) {
  if (++gate.lockCount == 1) {
    gate.hooks.inputGateLocked();
  }
}

template <typename T>
kj::Promise<T> OutputGate::lockWhile(kj::Promise<T> promise) {
  auto fulfiller = lock();

  if constexpr (std::is_void_v<T>) {
    promise = promise.exclusiveJoin(hooks.makeTimeoutPromise());
  } else {
    promise = promise.exclusiveJoin(hooks.makeTimeoutPromise()
        .then([]() -> T { KJ_UNREACHABLE; }));
  }

  hooks.outputGateLocked();
  auto rejectIfCanceled = kj::defer([this, &fulfiller](){
    hooks.outputGateReleased();
    if (fulfiller->isWaiting()) {
      auto e = makeUnfulfilledException();
      setBroken(e);
      fulfiller->reject(kj::mv(e));
    }
  });

  try {
    if constexpr (std::is_void_v<T>) {
      co_await promise;
      fulfiller->fulfill();
    } else {
      auto v = co_await promise;
      fulfiller->fulfill();
      co_return v;
    }
  } catch (kj::Exception e) {
    setBroken(e);
    fulfiller->reject(kj::cp(e));
    kj::throwFatalException(kj::mv(e));
  }
}

}  // namespace workerd
