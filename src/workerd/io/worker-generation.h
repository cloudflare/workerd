// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/exception.h>

#include <v8.h>

#include <kj/array.h>
#include <kj/async.h>
#include <kj/mutex.h>
#include <kj/refcount.h>
#include <kj/string.h>
#include <kj/vector.h>

#include <memory>

namespace workerd {

// A single, non-reusable worker lifetime. The isolate pointer is protected by stateMutex: stop
// requests can terminate it from another thread, while teardown first unregisters it and only
// acknowledges disposal after V8 and its API state have actually been destroyed.
class WorkerGeneration final: public kj::AtomicRefcounted {
 public:
  struct Memory {
    kj::String name;
    std::shared_ptr<v8::BackingStore> backing;
  };

  explicit WorkerGeneration(kj::Array<Memory> memories);

  kj::ArrayPtr<const Memory> getMemories() const {
    return memories.asPtr();
  }

  // Claims this generation for one startup attempt. A generation cannot be started again, even
  // after startup has finished or a stop was requested.
  void claim() const;

  // Reserves the claimed startup for one factory invocation before it installs its cleanup defer.
  void beginStartup() const;

  // Register the isolate before startup can execute JavaScript. Once stopping begins, late
  // registration is rejected.
  void attachIsolate(v8::Isolate* isolate) const;

  // Must run before disposing the V8 isolate. This prevents a concurrent stop request from using
  // the pointer after V8 teardown begins.
  void detachIsolate() const;

  // Ends the startup reservation, including on startup failure. If stop was requested, this may
  // complete the termination acknowledgement when no isolate was attached or it was already
  // disposed.
  void finishStartup() const;

  // Call only after the attached isolate and its API teardown have completed.
  void isolateDisposed() const;

  // Irreversibly stops this generation, terminates any registered isolate, and rejects onStop()
  // waiters. Repeated calls are harmless.
  void requestStop() const;

  bool isStopRequested() const;

  // Rejects with jsg.Error once stopping starts. Cancelling this promise only drops its waiter.
  kj::Promise<void> onStop() const;

  // Fulfills only after stopping has been requested, startup is no longer pending, and any
  // attached isolate has been fully disposed. Each call returns an independent waiter.
  kj::Promise<void> whenTerminated() const;

  // Throws jsg.Error if stopping has started. Check this before startup or other guest JS work.
  void requireRunning() const;

 private:
  struct State {
    bool claimed = false;
    bool startupPending = false;
    bool startupEntered = false;
    bool stopRequested = false;
    bool isolateEverAttached = false;
    bool isolateDisposed = false;
    bool terminationAcknowledged = false;
    v8::Isolate* isolate = nullptr;
    kj::Vector<kj::Own<kj::CrossThreadPromiseFulfiller<void>>> stopWaiters;
    kj::Vector<kj::Own<kj::CrossThreadPromiseFulfiller<void>>> terminationWaiters;
  };

  static kj::Exception makeStoppedException();
  static void pruneCancelledWaiters(
      kj::Vector<kj::Own<kj::CrossThreadPromiseFulfiller<void>>>& waiters);
  static bool canAcknowledgeTermination(const State& state);
  static void acknowledgeTermination(State& state);

  mutable kj::MutexGuarded<State> stateMutex;
  const kj::Array<Memory> memories;
};

}  // namespace workerd
