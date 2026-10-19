// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "worker-generation.h"

#include <kj/debug.h>

namespace workerd {

WorkerGeneration::WorkerGeneration(kj::Array<Memory> memories): memories(kj::mv(memories)) {}

kj::Exception WorkerGeneration::makeStoppedException() {
  return JSG_KJ_EXCEPTION(FAILED, Error, "Worker generation stopped.");
}

void WorkerGeneration::pruneCancelledWaiters(
    kj::Vector<kj::Own<kj::CrossThreadPromiseFulfiller<void>>>& waiters) {
  kj::Vector<kj::Own<kj::CrossThreadPromiseFulfiller<void>>> liveWaiters;
  for (auto& waiter: waiters) {
    if (waiter->isWaiting()) {
      liveWaiters.add(kj::mv(waiter));
    }
  }
  waiters = kj::mv(liveWaiters);
}

bool WorkerGeneration::canAcknowledgeTermination(const State& state) {
  return state.stopRequested && !state.startupPending &&
      (!state.isolateEverAttached || state.isolateDisposed);
}

void WorkerGeneration::acknowledgeTermination(State& state) {
  KJ_ASSERT(canAcknowledgeTermination(state));
  if (state.terminationAcknowledged) return;

  state.terminationAcknowledged = true;
  for (auto& waiter: state.terminationWaiters) {
    waiter->fulfill();
  }
  state.terminationWaiters.clear();
}

void WorkerGeneration::claim() const {
  auto state = stateMutex.lockExclusive();
  JSG_REQUIRE(!state->stopRequested, Error, "Worker generation stopped.");
  JSG_REQUIRE(!state->claimed, Error, "Worker generation can only be claimed once.");

  state->claimed = true;
  state->startupPending = true;
}

void WorkerGeneration::beginStartup() const {
  auto state = stateMutex.lockExclusive();
  KJ_REQUIRE(
      state->claimed && state->startupPending && !state->startupEntered, "startup is not pending");
  state->startupEntered = true;
}

void WorkerGeneration::attachIsolate(v8::Isolate* isolate) const {
  KJ_REQUIRE(isolate != nullptr, "isolate must not be null");

  auto state = stateMutex.lockExclusive();
  JSG_REQUIRE(!state->stopRequested, Error, "Worker generation stopped.");
  JSG_REQUIRE(state->claimed && state->startupPending, Error,
      "Worker generation isolate must be attached during startup.");
  JSG_REQUIRE(
      !state->isolateEverAttached, Error, "Worker generation isolate can only be attached once.");

  state->isolateEverAttached = true;
  state->isolate = isolate;
}

void WorkerGeneration::detachIsolate() const {
  auto state = stateMutex.lockExclusive();
  KJ_REQUIRE(state->isolateEverAttached && state->isolate != nullptr && !state->isolateDisposed,
      "isolate is not attached");
  state->isolate = nullptr;
}

void WorkerGeneration::finishStartup() const {
  auto state = stateMutex.lockExclusive();
  KJ_REQUIRE(state->claimed && state->startupPending, "startup is not pending");
  state->startupPending = false;
  if (canAcknowledgeTermination(*state)) {
    acknowledgeTermination(*state);
  }
}

void WorkerGeneration::isolateDisposed() const {
  auto state = stateMutex.lockExclusive();
  KJ_REQUIRE(state->isolateEverAttached && state->isolate == nullptr && !state->isolateDisposed,
      "isolate must be detached and disposed exactly once");
  state->isolateDisposed = true;
  if (canAcknowledgeTermination(*state)) {
    acknowledgeTermination(*state);
  }
}

void WorkerGeneration::requestStop() const {
  auto state = stateMutex.lockExclusive();
  if (!state->stopRequested) {
    state->stopRequested = true;

    // detachIsolate() takes this same lock before V8 teardown, so a non-null pointer is still
    // registered and safe to terminate here.
    if (state->isolate != nullptr) {
      state->isolate->TerminateExecution();
    }

    auto exception = makeStoppedException();
    for (auto& waiter: state->stopWaiters) {
      waiter->reject(exception.clone());
    }
    state->stopWaiters.clear();
  }

  if (canAcknowledgeTermination(*state)) {
    acknowledgeTermination(*state);
  }
}

bool WorkerGeneration::isStopRequested() const {
  return stateMutex.lockExclusive()->stopRequested;
}

kj::Promise<void> WorkerGeneration::onStop() const {
  auto state = stateMutex.lockExclusive();
  if (state->stopRequested) {
    return makeStoppedException();
  }

  pruneCancelledWaiters(state->stopWaiters);
  auto paf = kj::newPromiseAndCrossThreadFulfiller<void>();
  state->stopWaiters.add(kj::mv(paf.fulfiller));
  return kj::mv(paf.promise);
}

kj::Promise<void> WorkerGeneration::whenTerminated() const {
  auto state = stateMutex.lockExclusive();
  if (state->terminationAcknowledged) {
    return kj::READY_NOW;
  }

  pruneCancelledWaiters(state->terminationWaiters);
  auto paf = kj::newPromiseAndCrossThreadFulfiller<void>();
  state->terminationWaiters.add(kj::mv(paf.fulfiller));
  return kj::mv(paf.promise);
}

void WorkerGeneration::requireRunning() const {
  auto state = stateMutex.lockExclusive();
  JSG_REQUIRE(!state->stopRequested, Error, "Worker generation stopped.");
}

}  // namespace workerd
