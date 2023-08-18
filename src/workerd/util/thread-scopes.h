// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// This file contains several horrible hacks involving setting a thread-local value within some
// scope in the call stack, and then being able to check the vaule from deeper in the stack,
// without passing down an object. We use this pattern to signal hints across modules that do not
// directly call each other, where it would be excessively inconvenient to pass the value down the
// stack, perhaps because there is code in between that we do not control (e.g., V8).
//
// This is an anti-pattern and these should be considered HORRIBLE HACKS... but they get their jobs
// done for the time being.

#include <kj/common.h>
#include <inttypes.h>

namespace workerd {

// Normally, we prohibit V8 worker threads, but in some cases it's useful to temporarily allow
// them. Create this on the stack to temporarily allow V8 code running in the current thread to
// spawn worker threads.
//
// In particular this is used when loading Wasm modules, to properly enable Liftoff and Tier-up.
class AllowV8BackgroundThreadsScope {
public:
  AllowV8BackgroundThreadsScope();
  ~AllowV8BackgroundThreadsScope() noexcept(false);

  static bool isActive();

  KJ_DISALLOW_COPY_AND_MOVE(AllowV8BackgroundThreadsScope);
};

// Create this on the stack when tearing down isolates. This hints to the PageAllocator that all
// page discards should be deferred until the whole cage is destroyed.
class IsolateShutdownScope {
public:
  IsolateShutdownScope();
  ~IsolateShutdownScope() noexcept(false);

  static bool isActive();

  KJ_DISALLOW_COPY_AND_MOVE(IsolateShutdownScope);
};

// Tracks whether the process hosts isolates from multiple parties that don't know about each
// other. In such a case, we must take additional precautions against Spectre, and prohibit
// functionality which cannot be made spectre-safe.
//
// (Note that simply turning this on is NOT sufficient to enable spectre protection. Instead, this
// is mostly used as a safeguard to *disable* functionality that is known not to be spectre-safe.)
//
// This is actually a process-level flag rather than thread-level. Once a process becomes
// multi-tenant it cannot go back, since secrets could persist in memory.
bool isMultiTenantProcess();

// Tracks whether the process hosts isolates from multiple parties that don't know about each
// other. In such a case, we must take additional precautions against Spectre, and prohibit
// functionality which cannot be made spectre-safe.
//
// (Note that simply turning this on is NOT sufficient to enable spectre protection. Instead, this
// is mostly used as a safeguard to *disable* functionality that is known not to be spectre-safe.)
//
// This is actually a process-level flag rather than thread-level. Once a process becomes
// multi-tenant it cannot go back, since secrets could persist in memory.
void setMultiTenantProcess();

// Tracks whether the process should run in "predictable mode" for testing purposes. This causes
// random number generators to return static results instead, changes some timers to return zero,
// etc. This should only be used in tests.
bool isPredictableModeForTest();

// Tracks whether the process should run in "predictable mode" for testing purposes. This causes
// random number generators to return static results instead, changes some timers to return zero,
// etc. This should only be used in tests.
void setPredictableModeForTest();

// RAII class which allows the thread's active watchdog to observe forward progress through
// changes in a uint64_t. Use this in places where your code cannot call Watchdog::checkIn() and
// may block for longer than the watchdog timeout, but can still observe forward progress.
class ThreadProgressCounter {
public:
  // When a ProgressCounter is instantiated, it saves the current value of `counter`. When
  // Watchdog::tryHandleSignal() is called with an active ProgressCounter on the thread, the
  // function compares this saved value with the (possibly updated) current value. If they differ,
  // we consider the process to have exhibited forward progress. Note that we don't make any
  // assumption of a less-than relationship between consecutive counter values -- you could use
  // random values if you want.
  //
  // It is expected that all read/write operations to `counter` are atomic.
  //
  // ProgressCounters are reentrant, like v8::Lockers.
  explicit ThreadProgressCounter(uint64_t& counter);

  ~ThreadProgressCounter() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(ThreadProgressCounter);

  // Returns true if progress has been made since the last call to update().
  static bool hasProgress();

  // Updates the saved progress value so that hasProgress() now returns false until the next time
  // the counter is updated.
  static void acknowledgeProgress();

private:
  uint64_t savedValue;
  uint64_t& counter;

  friend class Watchdog;
};
}  // namespace workerd
