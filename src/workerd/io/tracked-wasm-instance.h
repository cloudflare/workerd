// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <v8-forward.h>
#include <v8-persistent-handle.h>

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
// time is nearly exhausted, or that wants its memory reclaimed when the instance is garbage
// collected. The module must export at least one of "__instance_terminated" or "__instance_signal":
//
//   "__instance_signal"     — (optional) address of a uint32 in linear memory. When present,
//                            the runtime writes SIGXCPU (24) here when CPU time is nearly
//                            exhausted.
//   "__instance_terminated" — (optional) address of a uint32 in linear memory. The runtime
//                            writes 1 here when the isolate is killed after exceeding its CPU
//                            limit, informing the WASM module that it was forcefully terminated.
//
// Cleanup of entries relies on a weak reference to the WASM instance (instanceRef): when V8
// garbage-collects the instance, the handle becomes empty and the GC prologue filter removes
// the entry, releasing the strong reference to linear memory.
struct TrackedWasmInstance {
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
  // SIGXCPU shutdown warning.
  kj::Maybe<uint32_t> signalByteOffset;

  // Offset into `memory` of the uint32 the runtime writes to (__instance_terminated).
  // When kj::none, the module did not export __instance_terminated.
  kj::Maybe<uint32_t> terminatedByteOffset;

  // Weak handle to the WASM instance. Set to weak via SetWeak() at registration time. When V8
  // collects the instance, the handle becomes empty (IsEmpty() returns true). Checked in the GC
  // prologue filter to clean up entries whose instance has been garbage-collected. Always set at
  // registration time, so IsEmpty() unambiguously means the instance was collected.
  //
  // This field is never accessed from signal handlers — signal handlers only touch `memory` and
  // `signalByteOffset`. The Global's destructor is safe to call during filter() (under isolate
  // lock) or clear() (during isolate teardown with V8 still alive).
  v8::Global<v8::Object> instanceRef;

  // Returns true if the entry should be kept in the list, false if it should be removed.
  // An entry is removed when V8 garbage-collects the WASM instance, which resets the weak
  // handle making IsEmpty() return true.
  bool shouldRetain() const {
    return !instanceRef.IsEmpty();
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

  // Prepends a new node constructed from `args` at the front of the list.
  // Returns a reference to the inserted value. The reference is stable (heap-allocated node)
  // and valid until the node is removed from the list via filter() or clear().
  template <typename... Args>
  T& pushFront(Args&&... args) {
    Node* node = new Node(kj::fwd<Args>(args)...);
    __atomic_store_n(&node->next, __atomic_load_n(&head, __ATOMIC_RELAXED), __ATOMIC_RELAXED);
    __atomic_store_n(&head, node, __ATOMIC_RELEASE);
    return node->value;
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

// Encapsulates a SignalSafeList<TrackedWasmInstance> with operations that require the isolate
// lock for mutation, and a read-only accessor for signal-handler use.
//
// The mutation methods are const and accept a jsg::Lock& to prove the caller holds the isolate
// lock. Internally they const_cast the list, which is safe because the lock provides the
// required synchronization. This design allows IsolateLimitEnforcer (whose methods are const
// per KJ convention) to return a const reference without exposing mutable access to code that
// does not hold the lock.
class TrackedWasmInstanceList {
 public:
  // Registers a WASM module for receiving the "shut down" signal and/or terminated notification.
  // At least one of signalOffset or terminatedOffset must be provided. Both are optional
  // individually: when signalOffset is kj::none, the module will not receive SIGXCPU; when
  // terminatedOffset is kj::none, the module will not receive the terminated notification.
  // Cleanup always relies on the weak instanceRef becoming empty when V8 GCs the instance.
  //
  // Silently skips registration if any provided offset falls outside the module's linear memory.
  //
  // Returns a reference to the registered entry on success, or kj::none if registration was
  // skipped. The caller uses the returned reference to set up the weak instance handle.
  kj::Maybe<TrackedWasmInstance&> registerSignal(jsg::Lock&,
      kj::Array<kj::byte> memory,
      kj::Maybe<uint32_t> signalOffset,
      kj::Maybe<uint32_t> terminatedOffset) const;

  // Filters out entries where the instance has been garbage-collected (instanceRef is empty).
  // Call from a GC prologue hook to allow linear memory to be reclaimed.
  void filter(jsg::Lock&) const;

  // Removes all entries unconditionally. Call before the V8 isolate is disposed, since each
  // entry holds a shared_ptr<v8::BackingStore> (via the memory array) and a v8::Global whose
  // destructors may access V8 state.
  void clear(jsg::Lock&) const;

  void writeShutdownSignal() const;

  void clearShutdownSignal() const;

  void writeTerminatedSignal() const;

  // Returns the underlying signal-safe list for use by signal handlers and the CPU time limiter.
  // The returned reference is const; signal-handler free functions use const_cast internally.
  const SignalSafeList<TrackedWasmInstance>& signals() const {
    return list;
  }

 private:
  SignalSafeList<TrackedWasmInstance> list;
};

// The value written to the signal address when CPU time is nearly exhausted.
// This is the UNIX signal number for SIGXCPU (24). Technically the number itself
// is not standardized, but for most architectures it is 24 so that is what we're going with.
// We're inventing WASM signals from scratch so we can do whatever we want.
constexpr uint32_t WASM_SIGNAL_SIGXCPU = 24;

}  // namespace workerd
