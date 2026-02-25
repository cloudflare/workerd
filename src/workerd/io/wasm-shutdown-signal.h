// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/array.h>
#include <kj/common.h>

#include <cstdint>

namespace workerd {

// Byte size of each signal field in WASM linear memory (a single uint32).
constexpr size_t WASM_SIGNAL_FIELD_BYTES = sizeof(uint32_t);

// Represents a single WASM module that has opted into receiving the "shut down" signal when CPU
// time is nearly exhausted. The module exports two i32 globals:
//
//   "__signal_address"     — address of a uint32 in linear memory. The runtime writes SIGXCPU
//                            (24) here when CPU time is nearly exhausted.
//   "__terminated_address" — address of a uint32 in linear memory. The WASM module writes a
//                            non-zero value here when it has exited and is no longer listening.
//                            The runtime checks this in a GC prologue hook and removes entries
//                            where terminated is non-zero, allowing the linear memory to be
//                            reclaimed.
struct WasmShutdownSignal {
  // Owns a reference to the WASM module's linear memory. The underlying v8::BackingStore is kept
  // alive via kj::Array's attach() mechanism, preventing V8 from garbage-collecting the memory
  // while we still need to read/write signal addresses. This gets cleaned up in a V8 GC prologue
  // hook where we atomically remove the entry from the signal list before releasing the memory.
  kj::Array<kj::byte> memory;

  // Offset into `memory` of the uint32 the runtime writes SIGXCPU (24) to (__signal_address).
  uint32_t signalByteOffset;

  // Offset into `memory` of the uint32 the module writes to (__terminated_address).
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
// same-thread concurrency ONLY. Mutations (pushFront, filter) are not signal safe, but are
// implemented such that they can be interrupted at any point by a signal handler, and the list will
// still be in a valid state. This means that reading the list (iterate) IS signal safe.
template <typename T>
class AtomicList {
 public:
  struct Node {
    T value;
    Node* next;
    template <typename... Args>
    explicit Node(Args&&... args): value(kj::fwd<Args>(args)...),
                                   next(nullptr) {}
  };

  AtomicList() {}

  ~AtomicList() noexcept(false) {
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
  void filter(Predicate&& predicate) {
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

  // Returns true if the list is empty. Signal safe.
  bool isEmpty() const {
    return __atomic_load_n(&head, __ATOMIC_ACQUIRE) == nullptr;
  }

  // Traverses the list, calling `func(node.value)` for each node. Signal safe.
  template <typename Func>
  void iterate(Func&& func) const {
    Node* current = __atomic_load_n(&head, __ATOMIC_ACQUIRE);
    while (current != nullptr) {
      func(current->value);
      current = __atomic_load_n(&current->next, __ATOMIC_ACQUIRE);
    }
  }

 private:
  Node* head = nullptr;

  KJ_DISALLOW_COPY_AND_MOVE(AtomicList);
};

// The value written to the signal address when CPU time is nearly exhausted.
// This is the UNIX signal number for SIGXCPU (24).
constexpr uint32_t WASM_SIGNAL_SIGXCPU = 24;

// Iterates a WasmShutdownSignal list and writes SIGXCPU (24) to the signal address of each
// registered module. This function is signal-safe.
inline void writeWasmShutdownSignals(const AtomicList<WasmShutdownSignal>& signals) {
  signals.iterate([](WasmShutdownSignal& signal) {
    // Signal-safe: kj::Array::begin() is a trivial getter; memcpy into mapped WASM memory
    // is a plain store.
    uint32_t value = WASM_SIGNAL_SIGXCPU;
    memcpy(signal.memory.begin() + signal.signalByteOffset, &value, sizeof(value));
  });
}

// Iterates a WasmShutdownSignal list and zeros the signal address of each registered module.
// Call this at the start of each request to clear stale "nearly out of time" signals from a
// previous request. This function is signal-safe.
inline void clearWasmShutdownSignals(const AtomicList<WasmShutdownSignal>& signals) {
  signals.iterate([](WasmShutdownSignal& signal) {
    uint32_t value = 0;
    memcpy(signal.memory.begin() + signal.signalByteOffset, &value, sizeof(value));
  });
}

// Iterates a WasmShutdownSignal list and writes 1 to the terminated address of each registered
// module. Call this when the isolate is killed after exhausting its CPU limit, so that WASM
// modules can detect on the next request that they were forcefully terminated.
// This function is signal-safe.
inline void writeWasmTerminatedSignals(const AtomicList<WasmShutdownSignal>& signals) {
  signals.iterate([](WasmShutdownSignal& signal) {
    uint32_t value = 1;
    memcpy(signal.memory.begin() + signal.terminatedByteOffset, &value, sizeof(value));
  });
}

}  // namespace workerd
