// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "thread-scopes.h"
#include <atomic>
#include <kj/debug.h>

namespace workerd {

using kj::uint;

namespace {

thread_local uint allowV8BackgroundThreadScopeCount = 0;
thread_local uint isolateShutdownThreadScopeCount = 0;

bool multiTenantProcess = false;
bool predictableMode = false;

// This variable is read in signal handlers, so use atomic stores and compiler barriers as
// needed in regular code. Atomic loads are unnecessary, because we're not synchronizing with
// other threads.
thread_local ThreadProgressCounter* activeProgressCounter = nullptr;

}  // namespace

AllowV8BackgroundThreadsScope::AllowV8BackgroundThreadsScope() {
  ++allowV8BackgroundThreadScopeCount;
}

AllowV8BackgroundThreadsScope::~AllowV8BackgroundThreadsScope() noexcept(false) {
  --allowV8BackgroundThreadScopeCount;
}

bool AllowV8BackgroundThreadsScope::isActive() {
  return allowV8BackgroundThreadScopeCount > 0;
}

IsolateShutdownScope::IsolateShutdownScope() {
  ++isolateShutdownThreadScopeCount;
}

IsolateShutdownScope::~IsolateShutdownScope() noexcept(false) {
  --isolateShutdownThreadScopeCount;
}

bool IsolateShutdownScope::isActive() {
  return isolateShutdownThreadScopeCount > 0;
}

bool isMultiTenantProcess() {
  return multiTenantProcess;
}

void setMultiTenantProcess() {
  multiTenantProcess = true;
}

bool isPredictableModeForTest() {
  return predictableMode;
}

void setPredictableModeForTest() {
  predictableMode = true;
}

ThreadProgressCounter::ThreadProgressCounter(uint64_t& counter)
    : savedValue(__atomic_load_n(&counter, __ATOMIC_RELAXED)),
      counter(counter) {
  if (activeProgressCounter == nullptr) {
    // Release compiler barrier guarantees we're initialized before signal handlers can see us.
    std::atomic_signal_fence(std::memory_order_release);
    __atomic_store_n(&activeProgressCounter, this, __ATOMIC_RELAXED);
  } else {
    // Another progress counter is active on this thread, likely meaning we reentered.
  }
}

ThreadProgressCounter::~ThreadProgressCounter() noexcept(false) {
  auto& self = KJ_ASSERT_NONNULL(activeProgressCounter,
      "~ProgressCounter() with no active progress counter.");
  if (&self == this) {
    // Acquire compiler barrier to prevent any teardown from leaking above this nullification.
    KJ_DEFER({
      __atomic_store_n(&activeProgressCounter, nullptr, __ATOMIC_RELAXED);
      std::atomic_signal_fence(std::memory_order_acquire);
    });
  } else {
    // Nothing to do, tearing down reentered progress counter.
  }
}

bool ThreadProgressCounter::hasProgress() {
  KJ_IF_SOME(progressCounter, activeProgressCounter) {
    // The counter itself may be incremented by any thread, but there's no real synchronization
    // concern, so we can use relaxed memory ordering. If the machine is so bogged down that a
    // stale value causes a false positive, then crashing seems reasonable.
    auto currentValue = __atomic_load_n(&progressCounter.counter, __ATOMIC_RELAXED);

    // `savedValue` is only ever accessed by our own thread, so no need for atomics here.
    if (progressCounter.savedValue != currentValue) {
      return true;
    }
  }

  return false;
}

void ThreadProgressCounter::acknowledgeProgress() {
  KJ_IF_SOME(progressCounter, activeProgressCounter) {
    progressCounter.savedValue = __atomic_load_n(&progressCounter.counter, __ATOMIC_RELAXED);
  }
}

// ======================================================================================

namespace {
thread_local uint warnAboutIsolateLockScopeCount = 0;
}  // namespace

WarnAboutIsolateLockScope::WarnAboutIsolateLockScope() {
  ++warnAboutIsolateLockScopeCount;
}

WarnAboutIsolateLockScope::~WarnAboutIsolateLockScope() noexcept(false) {
  if (!released) release();
}

WarnAboutIsolateLockScope::WarnAboutIsolateLockScope(WarnAboutIsolateLockScope&& other)
    : released(other.released) {
  other.released = true;
}

void WarnAboutIsolateLockScope::release() {
  if (!released) {
    --warnAboutIsolateLockScopeCount;
    released = true;
  }
}

void WarnAboutIsolateLockScope::maybeWarn() {
  if (warnAboutIsolateLockScopeCount > 0) {
    KJ_LOG(WARNING, "taking isolate lock at a bad time", kj::getStackTrace());
  }
}

}  // namespace workerd
