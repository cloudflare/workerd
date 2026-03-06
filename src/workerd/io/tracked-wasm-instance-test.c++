// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "tracked-wasm-instance.h"

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

KJ_TEST("SignalSafeList pushFront returns reference to inserted value") {
  SignalSafeList<int> list;

  int& ref = list.pushFront(42);
  KJ_EXPECT(ref == 42);

  // Mutate through the reference.
  ref = 99;

  int value = 0;
  list.iterate([&](int v) { value = v; });
  KJ_EXPECT(value == 99);
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
// TrackedWasmInstance memory-lifetime tests
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

// Local test helpers that replicate the signal-writing logic formerly provided by the inline free
// functions writeShutdownSignals, clearShutdownSignals, and writeTerminatedFlags.
// The production code now lives in TrackedWasmInstanceList methods, but these unit tests exercise
// the raw SignalSafeList directly without requiring a jsg::Lock.

void writeShutdownSignals(SignalSafeList<TrackedWasmInstance>& signals) {
  signals.iterate([](TrackedWasmInstance& signal) {
    KJ_IF_SOME(offset, signal.signalByteOffset) {
      uint32_t value = WASM_SIGNAL_SIGXCPU;
      signal.memory.asPtr().slice(offset, offset + sizeof(value)).copyFrom(kj::asBytes(&value, 1));
    }
  });
}

void clearShutdownSignals(SignalSafeList<TrackedWasmInstance>& signals) {
  signals.iterate([](TrackedWasmInstance& signal) {
    KJ_IF_SOME(offset, signal.signalByteOffset) {
      uint32_t value = 0;
      signal.memory.asPtr().slice(offset, offset + sizeof(value)).copyFrom(kj::asBytes(&value, 1));
    }
  });
}

void writeTerminatedFlags(SignalSafeList<TrackedWasmInstance>& signals) {
  signals.iterate([](TrackedWasmInstance& signal) {
    KJ_IF_SOME(offset, signal.terminatedByteOffset) {
      uint32_t value = 1;
      signal.memory.asPtr()
          .slice(offset, offset + sizeof(value))
          .copyFrom(kj::asBytes(&value, 1));
    }
  });
}

KJ_TEST("kj::Array attach keeps memory alive after module instance is dropped") {
  // This test proves that the kj::Array<kj::byte> in TrackedWasmInstance, created via attach(),
  // keeps the underlying linear memory alive even after the original owner (simulating a WASM
  // module instance) is dropped.

  bool backingStoreDestroyed = false;
  SignalSafeList<TrackedWasmInstance> signals;

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
    signals.pushFront(TrackedWasmInstance{
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
  writeShutdownSignals(signals);

  // Read back the signal value to confirm the write landed in live memory.
  signals.iterate([&](TrackedWasmInstance& signal) {
    uint32_t value = 0;
    memcpy(&value, signal.memory.begin() + kSignalOffset, sizeof(value));
    KJ_EXPECT(value == WASM_SIGNAL_SIGXCPU, value);
  });

  // Clear the signals (writes zero), then verify the clear also works on the still-live memory.
  clearShutdownSignals(signals);
  signals.iterate([&](TrackedWasmInstance& signal) {
    uint32_t value = 0xff;
    memcpy(&value, signal.memory.begin() + kSignalOffset, sizeof(value));
    KJ_EXPECT(value == 0, value);
  });

  // Memory is still alive after all those read/write operations.
  KJ_EXPECT(!backingStoreDestroyed);

  // Now remove the entry from the list — this destroys the kj::Array, which in turn destroys
  // the attached FakeBackingStore.
  signals.filter([](TrackedWasmInstance&) { return false; });

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
// Each TrackedWasmInstance entry holds a shared_ptr<v8::BackingStore> whose
// destructor lives in libv8.so and may touch V8 isolate-internal state,
// as well as a v8::Global<v8::Object> weak handle whose destructor calls
// V8::DisposeGlobal.
// If those are destroyed in step 2 (after V8 is gone), the destructors read
// freed memory → use-after-free.
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

// Helper: push a TrackedWasmInstance backed by a V8LifetimeAwareStore.
void pushV8Signal(SignalSafeList<TrackedWasmInstance>& list,
    const bool& v8Alive,
    bool& freedAfterV8,
    int& dtorCount) {
  constexpr size_t kSize = 64;
  auto store = kj::heap<V8LifetimeAwareStore>(v8Alive, freedAfterV8, dtorCount, kSize);
  auto memory = store->data.asPtr().attach(kj::mv(store));
  list.pushFront(TrackedWasmInstance{
    .memory = kj::mv(memory),
    .signalByteOffset = static_cast<uint32_t>(0),
    .terminatedByteOffset = static_cast<uint32_t>(sizeof(uint32_t)),
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
    SignalSafeList<TrackedWasmInstance> list;
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
    SignalSafeList<TrackedWasmInstance> list;
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
// Signal offset permutation tests
//
// At least one of __instance_terminated or __instance_signal must be present. The three valid
// permutations that reach the C++ signal list are:
//   1. Both signal + terminated offsets present
//   2. Only terminated offset (signal = kj::none)
//   3. Only signal offset (terminated = kj::none) — new: relies on weak instanceRef for cleanup
//
// (The fourth permutation — neither — is rejected by the JS shim before reaching C++, so it
// is only tested in the JS test file.)
//
// For each permutation we verify the full operation set:
//   - writeShutdownSignals  (writes SIGXCPU to signal address)
//   - clearShutdownSignals  (zeros the signal address)
//   - writeTerminatedFlags  (writes 1 to terminated address)
//   - shouldRetain          (returns true when entry should be kept)
//   - filter via shouldRetain (removes entries that should not be retained)
// ---------------------------------------------------------------------------

// Helper: construct a TrackedWasmInstance backed by a FakeBackingStore.
void pushSignal(SignalSafeList<TrackedWasmInstance>& list,
    bool& destroyed,
    kj::Maybe<uint32_t> signalOffset,
    kj::Maybe<uint32_t> terminatedOffset,
    size_t memorySize = 64) {
  auto store = kj::heap<FakeBackingStore>(destroyed, memorySize);
  auto memory = store->data.asPtr().attach(kj::mv(store));
  list.pushFront(TrackedWasmInstance{
    .memory = kj::mv(memory),
    .signalByteOffset = signalOffset,
    .terminatedByteOffset = terminatedOffset,
  });
}

// Helper: read a uint32 at `offset` from the first entry in the list.
uint32_t readU32(SignalSafeList<TrackedWasmInstance>& list, uint32_t offset) {
  uint32_t value = 0xDEADBEEF;
  list.iterate([&](TrackedWasmInstance& signal) {
    memcpy(&value, signal.memory.begin() + offset, sizeof(value));
  });
  return value;
}

// Permutation 1: both signal and terminated offsets present.
KJ_TEST("permutation: both offsets — writeShutdownSignals writes SIGXCPU") {
  // `destroyed` must be declared before `signals` so that `signals` is destroyed first
  // (reverse declaration order), preventing a stack-use-after-scope when FakeBackingStore's
  // destructor writes to `destroyed`.
  bool destroyed = false;
  SignalSafeList<TrackedWasmInstance> signals;
  constexpr uint32_t kSignalOffset = 0;
  constexpr uint32_t kTerminatedOffset = sizeof(uint32_t);

  pushSignal(signals, destroyed, kSignalOffset, kTerminatedOffset);
  writeShutdownSignals(signals);
  KJ_EXPECT(readU32(signals, kSignalOffset) == WASM_SIGNAL_SIGXCPU);
}

KJ_TEST("permutation: both offsets — clearShutdownSignals zeros signal") {
  bool destroyed = false;
  SignalSafeList<TrackedWasmInstance> signals;
  constexpr uint32_t kSignalOffset = 0;
  constexpr uint32_t kTerminatedOffset = sizeof(uint32_t);

  pushSignal(signals, destroyed, kSignalOffset, kTerminatedOffset);
  writeShutdownSignals(signals);
  KJ_EXPECT(readU32(signals, kSignalOffset) == WASM_SIGNAL_SIGXCPU);
  clearShutdownSignals(signals);
  KJ_EXPECT(readU32(signals, kSignalOffset) == 0);
}

KJ_TEST("permutation: both offsets — writeTerminatedFlags writes 1") {
  bool destroyed = false;
  SignalSafeList<TrackedWasmInstance> signals;
  constexpr uint32_t kSignalOffset = 0;
  constexpr uint32_t kTerminatedOffset = sizeof(uint32_t);

  pushSignal(signals, destroyed, kSignalOffset, kTerminatedOffset);
  writeTerminatedFlags(signals);
  KJ_EXPECT(readU32(signals, kTerminatedOffset) == 1);
}

KJ_TEST("permutation: both offsets — shouldRetain and filter") {
  bool destroyed = false;
  SignalSafeList<TrackedWasmInstance> signals;
  constexpr uint32_t kSignalOffset = 0;
  constexpr uint32_t kTerminatedOffset = sizeof(uint32_t);

  pushSignal(signals, destroyed, kSignalOffset, kTerminatedOffset);

  // Without instanceRef set, shouldRetain returns false (empty instanceRef means collected).
  // In production, instanceRef is always set at registration. Here we test the terminated path
  // which takes priority regardless of instanceRef state.
  writeTerminatedFlags(signals);
  signals.iterate([](TrackedWasmInstance& s) { KJ_EXPECT(!s.shouldRetain()); });

  signals.filter([](const TrackedWasmInstance& s) { return s.shouldRetain(); });
  KJ_EXPECT(signals.isEmpty());
  KJ_EXPECT(destroyed);
}

// Permutation 2: only terminated offset (signal = kj::none).
KJ_TEST("permutation: terminated only — writeShutdownSignals is a no-op") {
  bool destroyed = false;
  SignalSafeList<TrackedWasmInstance> signals;
  constexpr size_t kMemorySize = 64;
  constexpr uint32_t kTerminatedOffset = 0;

  pushSignal(signals, destroyed, kj::none, kTerminatedOffset, kMemorySize);
  writeShutdownSignals(signals);

  // Entire memory should still be zeroed — nothing was written.
  signals.iterate([&](TrackedWasmInstance& signal) {
    for (size_t i = 0; i < kMemorySize; ++i) {
      KJ_EXPECT(signal.memory[i] == 0, "unexpected non-zero byte at offset", i);
    }
  });
}

KJ_TEST("permutation: terminated only — clearShutdownSignals is a no-op") {
  bool destroyed = false;
  SignalSafeList<TrackedWasmInstance> signals;
  constexpr size_t kMemorySize = 64;
  constexpr uint32_t kTerminatedOffset = 0;

  pushSignal(signals, destroyed, kj::none, kTerminatedOffset, kMemorySize);
  clearShutdownSignals(signals);

  signals.iterate([&](TrackedWasmInstance& signal) {
    for (size_t i = 0; i < kMemorySize; ++i) {
      KJ_EXPECT(signal.memory[i] == 0, "unexpected non-zero byte at offset", i);
    }
  });
}

KJ_TEST("permutation: terminated only — writeTerminatedFlags writes 1") {
  bool destroyed = false;
  SignalSafeList<TrackedWasmInstance> signals;
  constexpr uint32_t kTerminatedOffset = 0;

  pushSignal(signals, destroyed, kj::none, kTerminatedOffset);
  writeTerminatedFlags(signals);
  KJ_EXPECT(readU32(signals, kTerminatedOffset) == 1);
}

KJ_TEST("permutation: terminated only — shouldRetain and filter") {
  bool destroyed = false;
  SignalSafeList<TrackedWasmInstance> signals;
  constexpr uint32_t kTerminatedOffset = 0;

  pushSignal(signals, destroyed, kj::none, kTerminatedOffset);

  writeTerminatedFlags(signals);
  signals.iterate([](TrackedWasmInstance& s) { KJ_EXPECT(!s.shouldRetain()); });

  signals.filter([](const TrackedWasmInstance& s) { return s.shouldRetain(); });
  KJ_EXPECT(signals.isEmpty());
  KJ_EXPECT(destroyed);
}

// Permutation 3: only signal offset (terminated = kj::none).
// In production, cleanup is via the weak instanceRef. Without a real V8 isolate, the
// instanceRef is empty (default), so shouldRetain() returns false immediately.
KJ_TEST("permutation: signal only — writeShutdownSignals writes SIGXCPU") {
  bool destroyed = false;
  SignalSafeList<TrackedWasmInstance> signals;
  constexpr uint32_t kSignalOffset = 0;

  pushSignal(signals, destroyed, kSignalOffset, kj::none);
  writeShutdownSignals(signals);
  KJ_EXPECT(readU32(signals, kSignalOffset) == WASM_SIGNAL_SIGXCPU);
}

KJ_TEST("permutation: signal only — clearShutdownSignals zeros signal") {
  bool destroyed = false;
  SignalSafeList<TrackedWasmInstance> signals;
  constexpr uint32_t kSignalOffset = 0;

  pushSignal(signals, destroyed, kSignalOffset, kj::none);
  writeShutdownSignals(signals);
  KJ_EXPECT(readU32(signals, kSignalOffset) == WASM_SIGNAL_SIGXCPU);
  clearShutdownSignals(signals);
  KJ_EXPECT(readU32(signals, kSignalOffset) == 0);
}

KJ_TEST("permutation: signal only — writeTerminatedFlags is a no-op") {
  bool destroyed = false;
  SignalSafeList<TrackedWasmInstance> signals;
  constexpr size_t kMemorySize = 64;
  constexpr uint32_t kSignalOffset = 0;

  pushSignal(signals, destroyed, kSignalOffset, kj::none, kMemorySize);
  writeTerminatedFlags(signals);

  // Entire memory should still be zeroed — no terminated offset means nothing is written.
  signals.iterate([&](TrackedWasmInstance& signal) {
    for (size_t i = 0; i < kMemorySize; ++i) {
      KJ_EXPECT(signal.memory[i] == 0, "unexpected non-zero byte at offset", i);
    }
  });
}

KJ_TEST("permutation: signal only — shouldRetain with empty instanceRef returns false") {
  // Without a real V8 isolate, instanceRef is empty (default-constructed).
  // shouldRetain() should return false since an empty instanceRef means the instance was collected.
  bool destroyed = false;
  SignalSafeList<TrackedWasmInstance> signals;
  constexpr uint32_t kSignalOffset = 0;

  pushSignal(signals, destroyed, kSignalOffset, kj::none);

  signals.iterate([](TrackedWasmInstance& s) { KJ_EXPECT(!s.shouldRetain()); });

  signals.filter([](const TrackedWasmInstance& s) { return s.shouldRetain(); });
  KJ_EXPECT(signals.isEmpty());
  KJ_EXPECT(destroyed);
}

// Mixed list: all three permutations in a single list.
// Verifies that operations correctly target each entry based on its offsets.
KJ_TEST("permutation: mixed list — all three entry types coexist") {
  bool destroyedBoth = false;
  bool destroyedTermOnly = false;
  bool destroyedSignalOnly = false;
  SignalSafeList<TrackedWasmInstance> signals;
  constexpr size_t kMemorySize = 64;

  // Push the both-offsets entry first (signal=0, terminated=4).
  pushSignal(signals, destroyedBoth, static_cast<uint32_t>(0), static_cast<uint32_t>(sizeof(uint32_t)), kMemorySize);
  // Push the terminated-only entry second.
  pushSignal(signals, destroyedTermOnly, kj::none, static_cast<uint32_t>(0), kMemorySize);
  // Push the signal-only entry third (it becomes the head).
  pushSignal(signals, destroyedSignalOnly, static_cast<uint32_t>(0), kj::none, kMemorySize);

  // --- writeShutdownSignals: only entries with signal offset get SIGXCPU ---
  writeShutdownSignals(signals);

  int index = 0;
  signals.iterate([&](TrackedWasmInstance& signal) {
    if (index == 0) {
      // Head = signal-only entry. Signal at offset 0 should be SIGXCPU.
      uint32_t value = 0;
      memcpy(&value, signal.memory.begin(), sizeof(value));
      KJ_EXPECT(value == WASM_SIGNAL_SIGXCPU, value);
    } else if (index == 1) {
      // Terminated-only entry. Entire memory should be untouched.
      for (size_t i = 0; i < kMemorySize; ++i) {
        KJ_EXPECT(signal.memory[i] == 0, "terminated-only entry modified at offset", i);
      }
    } else {
      // Both-offsets entry. Signal at offset 0 should be SIGXCPU.
      uint32_t value = 0;
      memcpy(&value, signal.memory.begin(), sizeof(value));
      KJ_EXPECT(value == WASM_SIGNAL_SIGXCPU, value);
    }
    ++index;
  });

  // --- clearShutdownSignals: zeros signal entries ---
  clearShutdownSignals(signals);

  index = 0;
  signals.iterate([&](TrackedWasmInstance& signal) {
    if (index == 0 || index == 2) {
      uint32_t value = 0xff;
      memcpy(&value, signal.memory.begin(), sizeof(value));
      KJ_EXPECT(value == 0, "clear did not zero signal on entry", index);
    }
    ++index;
  });

  // --- writeTerminatedFlags: only entries with terminated offset ---
  writeTerminatedFlags(signals);

  index = 0;
  signals.iterate([&](TrackedWasmInstance& signal) {
    KJ_IF_SOME(offset, signal.terminatedByteOffset) {
      uint32_t terminated = 0;
      memcpy(&terminated, signal.memory.begin() + offset, sizeof(terminated));
      KJ_EXPECT(terminated == 1, "entry", index, "terminated", terminated);
    }
    ++index;
  });

  // --- shouldRetain: all should report not retained ---
  // Both-offsets and terminated-only entries: terminated flag is set.
  // Signal-only entry: instanceRef is empty (no real V8).
  signals.iterate([](TrackedWasmInstance& s) { KJ_EXPECT(!s.shouldRetain()); });

  // --- filter: all entries removed, memory reclaimed ---
  signals.filter([](const TrackedWasmInstance& s) { return s.shouldRetain(); });
  KJ_EXPECT(signals.isEmpty());
  KJ_EXPECT(destroyedBoth);
  KJ_EXPECT(destroyedTermOnly);
  KJ_EXPECT(destroyedSignalOnly);
}

// ---------------------------------------------------------------------------
// TrackedWasmInstanceList method tests
//
// The methods writeShutdownSignal(), clearShutdownSignal(), and writeTerminatedSignal() on
// TrackedWasmInstanceList are the production entry points called from signal handlers and the
// CPU time limiter. The tests above exercise the same underlying logic through test-local
// helpers on a raw SignalSafeList; these tests verify the methods themselves work correctly
// through the TrackedWasmInstanceList wrapper.
//
// Since registerSignal() requires a jsg::Lock& (not available in plain KJ tests), we
// populate the internal list directly via const_cast on signals(). This is acceptable in
// tests — it mirrors what registerSignal() does internally.
// ---------------------------------------------------------------------------

// Helper: push a TrackedWasmInstance into a TrackedWasmInstanceList's internal list, bypassing
// registerSignal() which requires jsg::Lock&.
void pushEntry(const TrackedWasmInstanceList& list,
    bool& destroyed,
    kj::Maybe<uint32_t> signalOffset,
    kj::Maybe<uint32_t> terminatedOffset,
    size_t memorySize = 64) {
  auto store = kj::heap<FakeBackingStore>(destroyed, memorySize);
  auto memory = store->data.asPtr().attach(kj::mv(store));
  const_cast<SignalSafeList<TrackedWasmInstance>&>(list.signals())
      .pushFront(TrackedWasmInstance{
        .memory = kj::mv(memory),
        .signalByteOffset = signalOffset,
        .terminatedByteOffset = terminatedOffset,
      });
}

// Helper: read a uint32 at `offset` from the first entry via the TrackedWasmInstanceList.
uint32_t readU32FromList(const TrackedWasmInstanceList& list, uint32_t offset) {
  uint32_t value = 0xDEADBEEF;
  const_cast<SignalSafeList<TrackedWasmInstance>&>(list.signals())
      .iterate([&](TrackedWasmInstance& entry) {
    memcpy(&value, entry.memory.begin() + offset, sizeof(value));
  });
  return value;
}

KJ_TEST("TrackedWasmInstanceList::writeShutdownSignal writes SIGXCPU") {
  bool destroyed = false;
  TrackedWasmInstanceList list;
  constexpr uint32_t kSignalOffset = 0;
  constexpr uint32_t kTerminatedOffset = sizeof(uint32_t);

  pushEntry(list, destroyed, kSignalOffset, kTerminatedOffset);
  list.writeShutdownSignal();
  KJ_EXPECT(readU32FromList(list, kSignalOffset) == WASM_SIGNAL_SIGXCPU);
}

KJ_TEST("TrackedWasmInstanceList::clearShutdownSignal zeros the signal") {
  bool destroyed = false;
  TrackedWasmInstanceList list;
  constexpr uint32_t kSignalOffset = 0;
  constexpr uint32_t kTerminatedOffset = sizeof(uint32_t);

  pushEntry(list, destroyed, kSignalOffset, kTerminatedOffset);
  list.writeShutdownSignal();
  KJ_EXPECT(readU32FromList(list, kSignalOffset) == WASM_SIGNAL_SIGXCPU);

  list.clearShutdownSignal();
  KJ_EXPECT(readU32FromList(list, kSignalOffset) == 0);
}

KJ_TEST("TrackedWasmInstanceList::writeTerminatedSignal writes 1") {
  bool destroyed = false;
  TrackedWasmInstanceList list;
  constexpr uint32_t kSignalOffset = 0;
  constexpr uint32_t kTerminatedOffset = sizeof(uint32_t);

  pushEntry(list, destroyed, kSignalOffset, kTerminatedOffset);
  list.writeTerminatedSignal();
  KJ_EXPECT(readU32FromList(list, kTerminatedOffset) == 1);
}

KJ_TEST("TrackedWasmInstanceList::writeShutdownSignal skips entries without signal offset") {
  bool destroyed = false;
  TrackedWasmInstanceList list;
  constexpr size_t kMemorySize = 64;
  constexpr uint32_t kTerminatedOffset = 0;

  // Entry with no signal offset (kj::none).
  pushEntry(list, destroyed, kj::none, kTerminatedOffset, kMemorySize);
  list.writeShutdownSignal();

  // Entire memory should still be zeroed — writeShutdownSignal is a no-op for this entry.
  const_cast<SignalSafeList<TrackedWasmInstance>&>(list.signals())
      .iterate([&](TrackedWasmInstance& entry) {
    for (size_t i = 0; i < kMemorySize; ++i) {
      KJ_EXPECT(entry.memory[i] == 0, "unexpected non-zero byte at offset", i);
    }
  });
}

KJ_TEST("TrackedWasmInstanceList::writeTerminatedSignal skips entries without terminated offset") {
  bool destroyed = false;
  TrackedWasmInstanceList list;
  constexpr size_t kMemorySize = 64;
  constexpr uint32_t kSignalOffset = 0;

  // Signal-only entry (no terminated offset).
  pushEntry(list, destroyed, kSignalOffset, kj::none, kMemorySize);
  list.writeTerminatedSignal();

  // Entire memory should still be zeroed — writeTerminatedSignal is a no-op for this entry.
  const_cast<SignalSafeList<TrackedWasmInstance>&>(list.signals())
      .iterate([&](TrackedWasmInstance& entry) {
    for (size_t i = 0; i < kMemorySize; ++i) {
      KJ_EXPECT(entry.memory[i] == 0, "unexpected non-zero byte at offset", i);
    }
  });
}

KJ_TEST("TrackedWasmInstanceList methods work on a mixed list") {
  bool destroyedBoth = false;
  bool destroyedTermOnly = false;
  bool destroyedSignalOnly = false;
  TrackedWasmInstanceList list;
  constexpr size_t kMemorySize = 64;

  // Push a both-offsets entry (signal=0, terminated=4).
  pushEntry(list, destroyedBoth, static_cast<uint32_t>(0), static_cast<uint32_t>(sizeof(uint32_t)), kMemorySize);
  // Push a terminated-only entry (it becomes the new head).
  pushEntry(list, destroyedTermOnly, kj::none, static_cast<uint32_t>(0), kMemorySize);
  // Push a signal-only entry (becomes the head).
  pushEntry(list, destroyedSignalOnly, static_cast<uint32_t>(0), kj::none, kMemorySize);

  // writeShutdownSignal: only entries with signal offset get SIGXCPU.
  list.writeShutdownSignal();

  int index = 0;
  const_cast<SignalSafeList<TrackedWasmInstance>&>(list.signals())
      .iterate([&](TrackedWasmInstance& entry) {
    if (index == 0) {
      // Head = signal-only entry. Signal at offset 0 should be SIGXCPU.
      uint32_t value = 0;
      memcpy(&value, entry.memory.begin(), sizeof(value));
      KJ_EXPECT(value == WASM_SIGNAL_SIGXCPU, value);
    } else if (index == 1) {
      // Terminated-only entry. Entire memory should be untouched.
      for (size_t i = 0; i < kMemorySize; ++i) {
        KJ_EXPECT(entry.memory[i] == 0, "terminated-only entry modified at offset", i);
      }
    } else {
      // Both-offsets entry. Signal at offset 0 should be SIGXCPU.
      uint32_t value = 0;
      memcpy(&value, entry.memory.begin(), sizeof(value));
      KJ_EXPECT(value == WASM_SIGNAL_SIGXCPU, value);
    }
    ++index;
  });

  // clearShutdownSignal: zeros entries with signal offset.
  list.clearShutdownSignal();

  index = 0;
  const_cast<SignalSafeList<TrackedWasmInstance>&>(list.signals())
      .iterate([&](TrackedWasmInstance& entry) {
    if (index == 0 || index == 2) {
      uint32_t value = 0xff;
      memcpy(&value, entry.memory.begin(), sizeof(value));
      KJ_EXPECT(value == 0, "clear did not zero signal on entry", index);
    }
    ++index;
  });

  // writeTerminatedSignal: only entries with terminated offset get terminated=1.
  list.writeTerminatedSignal();

  index = 0;
  const_cast<SignalSafeList<TrackedWasmInstance>&>(list.signals())
      .iterate([&](TrackedWasmInstance& entry) {
    KJ_IF_SOME(offset, entry.terminatedByteOffset) {
      uint32_t terminated = 0;
      memcpy(&terminated, entry.memory.begin() + offset, sizeof(terminated));
      KJ_EXPECT(terminated == 1, "entry", index, "terminated", terminated);
    }
    ++index;
  });
}

KJ_TEST("TrackedWasmInstanceList methods are no-ops on an empty list") {
  TrackedWasmInstanceList list;

  // These should not crash or have any observable effect.
  list.writeShutdownSignal();
  list.clearShutdownSignal();
  list.writeTerminatedSignal();

  KJ_EXPECT(list.signals().isEmpty());
}

// ---------------------------------------------------------------------------
// TrackedWasmInstance tests are also covered by the JS-level
// tracked-wasm-instance-js-test.wd-test, which runs inside a real workerd
// instance with V8 initialized. Registration requires the WebAssembly.instantiate
// shim, so we test via JS rather than a plain kj_test.
// ---------------------------------------------------------------------------

}  // namespace
}  // namespace workerd
