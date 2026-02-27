// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "wasm-shutdown-signal.h"

#include <kj/test.h>

#include <cstring>

namespace workerd {
namespace {

// ---------------------------------------------------------------------------
// SignalSafeList tests
// ---------------------------------------------------------------------------

KJ_TEST("SignalSafeList pushFront and iterate") {
  SignalSafeList<int> list;

  KJ_EXPECT(list.isEmpty());

  list.pushFront(3);
  list.pushFront(2);
  list.pushFront(1);

  KJ_EXPECT(!list.isEmpty());

  // Iterate should visit 1, 2, 3 (pushFront prepends).
  int expected = 1;
  list.iterate([&](int value) {
    KJ_EXPECT(value == expected, value, expected);
    ++expected;
  });
  KJ_EXPECT(expected == 4);
}

KJ_TEST("SignalSafeList filter removes matching nodes") {
  SignalSafeList<int> list;

  list.pushFront(5);
  list.pushFront(4);
  list.pushFront(3);
  list.pushFront(2);
  list.pushFront(1);

  // Keep only odd numbers.
  list.filter([](int value) { return value % 2 != 0; });

  kj::Vector<int> remaining;
  list.iterate([&](int value) { remaining.add(value); });

  KJ_EXPECT(remaining.size() == 3);
  KJ_EXPECT(remaining[0] == 1);
  KJ_EXPECT(remaining[1] == 3);
  KJ_EXPECT(remaining[2] == 5);
}

KJ_TEST("SignalSafeList filter removes all nodes") {
  SignalSafeList<int> list;

  list.pushFront(2);
  list.pushFront(4);
  list.pushFront(6);

  list.filter([](int) { return false; });

  KJ_EXPECT(list.isEmpty());
}

KJ_TEST("SignalSafeList filter keeps all nodes") {
  SignalSafeList<int> list;

  list.pushFront(1);
  list.pushFront(2);
  list.pushFront(3);

  list.filter([](int) { return true; });

  int count = 0;
  list.iterate([&](int) { ++count; });
  KJ_EXPECT(count == 3);
}

KJ_TEST("SignalSafeList single element filter remove") {
  SignalSafeList<int> list;

  list.pushFront(42);

  list.filter([](int) { return false; });

  KJ_EXPECT(list.isEmpty());
}

KJ_TEST("SignalSafeList clear removes all nodes") {
  SignalSafeList<int> list;

  list.pushFront(3);
  list.pushFront(2);
  list.pushFront(1);

  KJ_EXPECT(!list.isEmpty());

  list.clear();

  KJ_EXPECT(list.isEmpty());

  // List should be reusable after clear.
  list.pushFront(42);
  KJ_EXPECT(!list.isEmpty());
  int value = 0;
  list.iterate([&](int v) { value = v; });
  KJ_EXPECT(value == 42);
}

KJ_TEST("SignalSafeList clear on empty list is a no-op") {
  SignalSafeList<int> list;

  KJ_EXPECT(list.isEmpty());
  list.clear();
  KJ_EXPECT(list.isEmpty());
}

KJ_TEST("SignalSafeList filter removes head only") {
  SignalSafeList<int> list;

  list.pushFront(3);
  list.pushFront(2);
  list.pushFront(1);

  // Remove head (value 1).
  list.filter([](int value) { return value != 1; });

  kj::Vector<int> remaining;
  list.iterate([&](int value) { remaining.add(value); });

  KJ_EXPECT(remaining.size() == 2);
  KJ_EXPECT(remaining[0] == 2);
  KJ_EXPECT(remaining[1] == 3);
}

KJ_TEST("SignalSafeList filter removes tail only") {
  SignalSafeList<int> list;

  list.pushFront(3);
  list.pushFront(2);
  list.pushFront(1);

  // Remove tail (value 3).
  list.filter([](int value) { return value != 3; });

  kj::Vector<int> remaining;
  list.iterate([&](int value) { remaining.add(value); });

  KJ_EXPECT(remaining.size() == 2);
  KJ_EXPECT(remaining[0] == 1);
  KJ_EXPECT(remaining[1] == 2);
}

// ---------------------------------------------------------------------------
// WasmShutdownSignal memory-lifetime tests
// ---------------------------------------------------------------------------

// Simulates a WASM module's backing store (e.g. a v8::BackingStore). Sets a flag on destruction so
// tests can observe exactly when the memory is reclaimed.
struct FakeBackingStore {
  FakeBackingStore(bool& destroyed, size_t size)
      : destroyed(destroyed),
        data(kj::heapArray<kj::byte>(size)) {
    memset(data.begin(), 0, data.size());
  }
  ~FakeBackingStore() noexcept(false) {
    destroyed = true;
  }

  bool& destroyed;
  kj::Array<kj::byte> data;
};

KJ_TEST("kj::Array attach keeps memory alive after module instance is dropped") {
  // This test proves that the kj::Array<kj::byte> in WasmShutdownSignal, created via attach(),
  // keeps the underlying linear memory alive even after the original owner (simulating a WASM
  // module instance) is dropped.

  bool backingStoreDestroyed = false;
  SignalSafeList<WasmShutdownSignal> signals;

  // Allocate enough room for both signal fields (signalByteOffset=0, terminatedByteOffset=4).
  constexpr size_t kMemorySize = 64;
  constexpr uint32_t kSignalOffset = 0;
  constexpr uint32_t kTerminatedOffset = sizeof(uint32_t);

  {
    // Create the backing store — this simulates the WASM module instance owning linear memory.
    auto backingStore = kj::heap<FakeBackingStore>(backingStoreDestroyed, kMemorySize);

    // Build a kj::Array that points into the backing store's data and keeps it alive via attach().
    // This mirrors what the runtime does: take an ArrayPtr into the v8::BackingStore, then attach
    // the BackingStore so the array owns a reference.
    kj::Array<kj::byte> memory = backingStore->data.asPtr().attach(kj::mv(backingStore));

    // Register in the signal list.
    signals.pushFront(WasmShutdownSignal{
      .memory = kj::mv(memory),
      .signalByteOffset = kSignalOffset,
      .terminatedByteOffset = kTerminatedOffset,
    });

    // `backingStore` has been moved away — the only reference keeping the memory alive is the
    // kj::Array inside the SignalSafeList.
  }

  // The backing store must still be alive — the SignalSafeList's kj::Array owns it.
  KJ_EXPECT(!backingStoreDestroyed);

  // Prove the memory is accessible: write the shutdown signal through the list.
  writeWasmShutdownSignals(signals);

  // Read back the signal value to confirm the write landed in live memory.
  signals.iterate([&](WasmShutdownSignal& signal) {
    uint32_t value = 0;
    memcpy(&value, signal.memory.begin() + kSignalOffset, sizeof(value));
    KJ_EXPECT(value == WASM_SIGNAL_SIGXCPU, value);
  });

  // Clear the signals (writes zero), then verify the clear also works on the still-live memory.
  clearWasmShutdownSignals(signals);
  signals.iterate([&](WasmShutdownSignal& signal) {
    uint32_t value = 0xff;
    memcpy(&value, signal.memory.begin() + kSignalOffset, sizeof(value));
    KJ_EXPECT(value == 0, value);
  });

  // Memory is still alive after all those read/write operations.
  KJ_EXPECT(!backingStoreDestroyed);

  // Now remove the entry from the list — this destroys the kj::Array, which in turn destroys
  // the attached FakeBackingStore.
  signals.filter([](WasmShutdownSignal&) { return false; });

  KJ_EXPECT(backingStoreDestroyed);
  KJ_EXPECT(signals.isEmpty());
}

// ---------------------------------------------------------------------------
// Teardown-order test — models the real Worker::Isolate destruction sequence.
//
// In production, the member destruction order in Worker::Isolate is:
//
//   1. `api` destroyed  → V8 isolate disposed  (v8Alive becomes false)
//   2. `limitEnforcer` destroyed → ~SignalSafeList() frees remaining entries
//
// Each WasmShutdownSignal entry holds a shared_ptr<v8::BackingStore> whose
// destructor lives in libv8.so and may touch V8 isolate-internal state.
// If those shared_ptrs are destroyed in step 2 (after V8 is gone), the
// BackingStore destructor reads freed memory → use-after-free.
//
// The fix: call clear() in ~Isolate()'s destructor body (before member
// destruction begins), while V8 is still alive.
//
// This test models that sequence with a mock that records whether "V8" was
// still alive when each backing store was freed.  Without the clear() call,
// the test fails because the backing stores are freed after "V8 disposal".
// ---------------------------------------------------------------------------

// Mock backing store that records whether V8 was alive at destruction time.
struct V8LifetimeAwareStore {
  V8LifetimeAwareStore(const bool& v8Alive, bool& freedAfterV8, int& dtorCount, size_t size)
      : v8Alive(v8Alive),
        freedAfterV8(freedAfterV8),
        dtorCount(dtorCount),
        data(kj::heapArray<kj::byte>(size)) {
    memset(data.begin(), 0, data.size());
  }
  ~V8LifetimeAwareStore() noexcept(false) {
    ++dtorCount;
    if (!v8Alive) {
      freedAfterV8 = true;
    }
  }
  const bool& v8Alive;
  bool& freedAfterV8;
  int& dtorCount;
  kj::Array<kj::byte> data;
};

// Helper: push a WasmShutdownSignal backed by a V8LifetimeAwareStore.
void pushV8Signal(SignalSafeList<WasmShutdownSignal>& list,
    const bool& v8Alive,
    bool& freedAfterV8,
    int& dtorCount) {
  constexpr size_t kSize = 64;
  auto store = kj::heap<V8LifetimeAwareStore>(v8Alive, freedAfterV8, dtorCount, kSize);
  auto memory = store->data.asPtr().attach(kj::mv(store));
  list.pushFront(WasmShutdownSignal{
    .memory = kj::mv(memory),
    .signalByteOffset = 0,
    .terminatedByteOffset = sizeof(uint32_t),
  });
}

KJ_TEST("backing stores freed before V8 disposal when clear() is called") {
  // Models the FIXED teardown sequence:
  //   1. clear() called while V8 is alive  (the fix in ~Isolate)
  //   2. V8 disposed
  //   3. ~SignalSafeList runs on the now-empty list
  //
  // This must pass: no backing store is freed after V8 disposal.
  constexpr int kEntries = 5;
  bool v8Alive = true;
  bool freedAfterV8[kEntries] = {};
  int dtorCounts[kEntries] = {};

  {
    SignalSafeList<WasmShutdownSignal> list;
    for (int i = 0; i < kEntries; ++i) {
      pushV8Signal(list, v8Alive, freedAfterV8[i], dtorCounts[i]);
    }

    // ---- Simulates ~Isolate() destructor body (V8 still alive) ----
    list.clear();

    // All stores freed while V8 was alive.
    for (auto count: dtorCounts) {
      KJ_EXPECT(count == 1, "entry not freed by clear()", count);
    }
    for (auto bad: freedAfterV8) {
      KJ_EXPECT(!bad, "backing store freed after V8 disposal during clear()");
    }

    // ---- Simulates `api` member destruction (V8 disposed) ----
    v8Alive = false;

    // ---- ~SignalSafeList runs here (simulates `limitEnforcer` destruction) ----
  }

  // No store was freed after V8 disposal, and each was freed exactly once.
  for (auto bad: freedAfterV8) {
    KJ_EXPECT(!bad, "backing store freed after V8 disposal");
  }
  for (auto count: dtorCounts) {
    KJ_EXPECT(count == 1, "double-freed or leaked", count);
  }
}

KJ_TEST("backing stores freed after V8 disposal WITHOUT clear() — the bug") {
  // Models the BUGGY teardown (no clear() call):
  //   1. V8 disposed
  //   2. ~SignalSafeList frees entries → BackingStore destructors run after V8 is gone
  //
  // This test ASSERTS THAT THE BUG EXISTS without the fix: at least one
  // backing store is freed after V8 disposal.  If this test ever fails, it
  // means the destruction order changed and the fix may need revisiting.
  constexpr int kEntries = 3;
  bool v8Alive = true;
  bool freedAfterV8[kEntries] = {};
  int dtorCounts[kEntries] = {};

  {
    SignalSafeList<WasmShutdownSignal> list;
    for (int i = 0; i < kEntries; ++i) {
      pushV8Signal(list, v8Alive, freedAfterV8[i], dtorCounts[i]);
    }

    // NO clear() — this is the bug.

    // ---- Simulates `api` member destruction (V8 disposed) ----
    v8Alive = false;

    // ---- ~SignalSafeList runs here ----
  }

  // Prove the bug: all entries were freed AFTER V8 disposal.
  for (auto bad: freedAfterV8) {
    KJ_EXPECT(bad, "expected backing store to be freed after V8 disposal (bug scenario)");
  }
  // But each was still freed exactly once (no double-free in the buggy path either).
  for (auto count: dtorCounts) {
    KJ_EXPECT(count == 1, "double-freed or leaked in buggy path", count);
  }
}

// ---------------------------------------------------------------------------
// writeWasmShutdownSignals and WasmShutdownSignal tests are covered by the
// JS-level wasm-shutdown-signal-js-test.wd-test, which runs inside a real
// workerd instance with V8 initialized. Registration requires the
// WebAssembly.instantiate shim, so we test via JS rather than a plain kj_test.
// ---------------------------------------------------------------------------

}  // namespace
}  // namespace workerd
