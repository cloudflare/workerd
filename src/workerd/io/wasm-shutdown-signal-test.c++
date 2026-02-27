// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "wasm-shutdown-signal.h"

#include <kj/test.h>

#include <cstring>

namespace workerd {
namespace {

// ---------------------------------------------------------------------------
// AtomicList tests
// ---------------------------------------------------------------------------

KJ_TEST("AtomicList pushFront and iterate") {
  AtomicList<int> list;

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

KJ_TEST("AtomicList filter removes matching nodes") {
  AtomicList<int> list;

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

KJ_TEST("AtomicList filter removes all nodes") {
  AtomicList<int> list;

  list.pushFront(2);
  list.pushFront(4);
  list.pushFront(6);

  list.filter([](int) { return false; });

  KJ_EXPECT(list.isEmpty());
}

KJ_TEST("AtomicList filter keeps all nodes") {
  AtomicList<int> list;

  list.pushFront(1);
  list.pushFront(2);
  list.pushFront(3);

  list.filter([](int) { return true; });

  int count = 0;
  list.iterate([&](int) { ++count; });
  KJ_EXPECT(count == 3);
}

KJ_TEST("AtomicList single element filter remove") {
  AtomicList<int> list;

  list.pushFront(42);

  list.filter([](int) { return false; });

  KJ_EXPECT(list.isEmpty());
}

KJ_TEST("AtomicList clear removes all nodes") {
  AtomicList<int> list;

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

KJ_TEST("AtomicList clear on empty list is a no-op") {
  AtomicList<int> list;

  KJ_EXPECT(list.isEmpty());
  list.clear();
  KJ_EXPECT(list.isEmpty());
}

KJ_TEST("AtomicList filter removes head only") {
  AtomicList<int> list;

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

KJ_TEST("AtomicList filter removes tail only") {
  AtomicList<int> list;

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
  AtomicList<WasmShutdownSignal> signals;

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
    // kj::Array inside the AtomicList.
  }

  // The backing store must still be alive — the AtomicList's kj::Array owns it.
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
// writeWasmShutdownSignals and WasmShutdownSignal tests are covered by the
// JS-level wasm-shutdown-signal-js-test.wd-test, which runs inside a real
// workerd instance with V8 initialized. Registration requires the
// WebAssembly.instantiate shim, so we test via JS rather than a plain kj_test.
// ---------------------------------------------------------------------------

}  // namespace
}  // namespace workerd
