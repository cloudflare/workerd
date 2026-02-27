// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/array.h>
#include <kj/common.h>

#include <cstdint>

namespace workerd {

namespace jsg {
class Lock;
}  // namespace jsg

// Byte size of each signal field in WASM linear memory (a single uint32).
constexpr size_t WASM_SIGNAL_FIELD_BYTES = sizeof(uint32_t);

// Represents a single WASM module that has opted into receiving the "shut down" signal when CPU
// time is nearly exhausted. The module must export at least "__instance_terminated"; the
// "__instance_signal" export is optional:
//
//   "__instance_signal"     — (optional) address of a uint32 in linear memory. When present,
//                            the runtime writes SIGXCPU (24) here when CPU time is nearly
//                            exhausted.
//   "__instance_terminated" �� address of a uint32 in linear memory. The WASM module writes a
//                            non-zero value here when it has exited and is no longer listening.
//                            The runtime checks this in a GC prologue hook and removes entries
//                            where terminated is non-zero, allowing the linear memory to be
//                            reclaimed. The runtime also writes 1 here when the isolate is
//                            killed after exceeding its CPU limit.
struct WasmShutdownSignal {
  // Owns a reference to the WASM module's linear memory. The underlying v8::BackingStore is kept
  // alive via kj::Array's attach() mechanism, preventing V8 from garbage-collecting the memory
  // while we still need to read/write signal addresses. This gets cleaned up in a V8 GC prologue
  // hook where we atomically remove the entry from the signal list before releasing the memory.
  //
  // TODO: If a user were to grow a 64 bit linear memory >16GB, relocation will happen and this
  // array will point to stale (but not free'd) memory. The impact is that the user will see a
  // spike in memory usage and no longer receive the signal in that module instance. In practice this
  // should almost never happen since they would hit a memory limit well before 16GB,
  // and 64 bit WASM is currently used very infrequently anyways. Regardless, we should address this
  // soon.
  kj::Array<kj::byte> memory;

  // Offset into `memory` of the uint32 the runtime writes SIGXCPU (24) to (__instance_signal).
  // When kj::none, the module did not export __instance_signal and will not receive the
  // SIGXCPU shutdown warning, but will still receive the terminated flag.
  kj::Maybe<uint32_t> signalByteOffset;

  // Offset into `memory` of the uint32 the module writes to (__instance_terminated).
  uint32_t terminatedByteOffset;

  // Returns true if the module is still listening for signals (terminated == 0).
  // Returns false if the module has exited and this entry should be removed.
  bool isModuleListening() const {
    uint32_t terminated = 0;
    for (auto& b:
        memory.slice(terminatedByteOffset, terminatedByteOffset + WASM_SIGNAL_FIELD_BYTES)) {
      terminated |= b;
    }
    return terminated == 0;
  }
};

// A linked list type which is signal-safe (for reading), but not thread safe - it can handle
// same-thread concurrency and pre-emptive reads ONLY.
// SAFETY: All mutations are must happen with the isolate lock held!
template <typename T>
class SignalSafeList {
 public:
  struct Node {
    T value;
    Node* next;
    template <typename... Args>
    explicit Node(Args&&... args): value(kj::fwd<Args>(args)...),
                                   next(nullptr) {}
  };

  SignalSafeList() {}

  ~SignalSafeList() noexcept(false) {
    Node* node = __atomic_load_n(&head, __ATOMIC_RELAXED);
    while (node != nullptr) {
      Node* doomed = node;
      node = __atomic_load_n(&doomed->next, __ATOMIC_RELAXED);
      delete doomed;
    }
  }

  // Prepends a new node constructed from `args` at the front of the list
  template <typename... Args>
  void pushFront(Args&&... args) {
    Node* node = new Node(kj::fwd<Args>(args)...);
    __atomic_store_n(&node->next, __atomic_load_n(&head, __ATOMIC_RELAXED), __ATOMIC_RELAXED);
    __atomic_store_n(&head, node, __ATOMIC_RELEASE);
  }

  // Removes all nodes for which `predicate(node.value)` returns false
  template <typename Predicate>
  void filter(Predicate&& predicate) noexcept {
    Node** prev = &head;
    Node* current = __atomic_load_n(prev, __ATOMIC_RELAXED);

    while (current != nullptr) {
      Node* next = __atomic_load_n(&current->next, __ATOMIC_RELAXED);

      if (predicate(current->value)) {
        prev = &current->next;
      } else {
        // Splice out `current` by pointing its predecessor at `next`. Release ordering ensures a
        // signal handler that loads *prev with acquire sees a fully consistent successor chain.
        __atomic_store_n(prev, next, __ATOMIC_RELEASE);
        delete current;
      }

      current = next;
    }
  }

  // Removes all nodes from the list, destroying each one.
  void clear() noexcept {
    Node* current = __atomic_load_n(&head, __ATOMIC_RELAXED);
    __atomic_store_n(&head, static_cast<Node*>(nullptr), __ATOMIC_RELEASE);
    while (current != nullptr) {
      Node* next = __atomic_load_n(&current->next, __ATOMIC_RELAXED);
      delete current;
      current = next;
    }
  }

  // Returns true if the list is empty. Signal safe.
  bool isEmpty() const {
    return __atomic_load_n(&head, __ATOMIC_ACQUIRE) == nullptr;
  }

  // Traverses the list, calling `func(node.value)` for each node. Signal-safe (same-thread
  // only), but not thread-safe — callers from a signal handler context should const_cast.
  template <typename Func>
  void iterate(Func&& func) {
    Node* current = __atomic_load_n(&head, __ATOMIC_ACQUIRE);
    while (current != nullptr) {
      func(current->value);
      current = __atomic_load_n(&current->next, __ATOMIC_ACQUIRE);
    }
  }

 private:
  Node* head = nullptr;

  KJ_DISALLOW_COPY_AND_MOVE(SignalSafeList);
};

// Encapsulates a SignalSafeList<WasmShutdownSignal> with operations that require the isolate
// lock for mutation, and a read-only accessor for signal-handler use.
//
// The mutation methods are const and accept a jsg::Lock& to prove the caller holds the isolate
// lock. Internally they const_cast the list, which is safe because the lock provides the
// required synchronization. This design allows IsolateLimitEnforcer (whose methods are const
// per KJ convention) to return a const reference without exposing mutable access to code that
// does not hold the lock.
class WasmShutdownSignalList {
 public:
  // Registers a WASM module for receiving the "shut down" signal. The signal offset is optional:
  // when kj::none, the module will still receive the terminated flag but will not get SIGXCPU.
  // Silently skips registration if any provided offset falls outside the module's linear memory.
  void registerSignal(jsg::Lock&,
      kj::Array<kj::byte> memory,
      kj::Maybe<uint32_t> signalOffset,
      uint32_t terminatedOffset) const;

  // Filters out entries where the module has exited (terminated != 0). Call from a GC prologue
  // hook to allow linear memory to be reclaimed.
  void filter(jsg::Lock&) const;

  // Removes all entries unconditionally. Call before the V8 isolate is disposed, since each
  // entry holds a shared_ptr<v8::BackingStore> whose destructor may access V8 state.
  void clear(jsg::Lock&) const;

  // Returns the underlying signal-safe list for use by signal handlers and the CPU time limiter.
  // The returned reference is const; signal-handler free functions use const_cast internally.
  const SignalSafeList<WasmShutdownSignal>& signals() const {
    return list;
  }

 private:
  SignalSafeList<WasmShutdownSignal> list;
};

// The value written to the signal address when CPU time is nearly exhausted.
// This is the UNIX signal number for SIGXCPU (24). Technically the number itself
// is not standardized, but for most architectures it is 24 so that is what we're going with.
// We're inventing WASM signals from scratch so we can do whatever we want.
constexpr uint32_t WASM_SIGNAL_SIGXCPU = 24;

// Iterates a WasmShutdownSignal list and writes SIGXCPU (24) to the signal address of each
// registered module that has a signal address. Entries without a signal address are skipped.
// This function is signal-safe.
inline void writeWasmShutdownSignals(const SignalSafeList<WasmShutdownSignal>& signals) {
  // Safe to const_cast: this is called from a signal handler on the same thread that holds the
  // isolate lock, so there is no concurrent mutation of the list structure.
  const_cast<SignalSafeList<WasmShutdownSignal>&>(signals).iterate([](WasmShutdownSignal& signal) {
    KJ_IF_SOME(offset, signal.signalByteOffset) {
      uint32_t value = WASM_SIGNAL_SIGXCPU;
      signal.memory.asPtr().slice(offset, offset + sizeof(value)).copyFrom(kj::asBytes(&value, 1));
    }
  });
}

// Iterates a WasmShutdownSignal list and zeros the signal address of each registered module
// that has a signal address. Entries without a signal address are skipped.
// Call this at the start of each request to clear stale "nearly out of time" signals from a
// previous request. This function is signal-safe.
inline void clearWasmShutdownSignals(const SignalSafeList<WasmShutdownSignal>& signals) {
  // Safe to const_cast: same-thread signal-handler context, no concurrent list mutation.
  const_cast<SignalSafeList<WasmShutdownSignal>&>(signals).iterate([](WasmShutdownSignal& signal) {
    KJ_IF_SOME(offset, signal.signalByteOffset) {
      uint32_t value = 0;
      signal.memory.asPtr().slice(offset, offset + sizeof(value)).copyFrom(kj::asBytes(&value, 1));
    }
  });
}

// Iterates a WasmShutdownSignal list and writes 1 to the terminated address of each registered
// module. Call this when the isolate is killed after exhausting its CPU limit, so that WASM
// modules can detect on the next request that they were forcefully terminated.
// This function is signal-safe.
inline void writeWasmTerminatedSignals(const SignalSafeList<WasmShutdownSignal>& signals) {
  // Safe to const_cast: same-thread signal-handler context, no concurrent list mutation.
  const_cast<SignalSafeList<WasmShutdownSignal>&>(signals).iterate([](WasmShutdownSignal& signal) {
    uint32_t value = 1;
    signal.memory.asPtr()
        .slice(signal.terminatedByteOffset, signal.terminatedByteOffset + sizeof(value))
        .copyFrom(kj::asBytes(&value, 1));
  });
}

}  // namespace workerd
