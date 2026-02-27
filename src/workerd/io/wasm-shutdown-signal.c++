// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "wasm-shutdown-signal.h"

namespace workerd {

void WasmShutdownSignalList::registerSignal(jsg::Lock&,
    kj::Array<kj::byte> memory,
    kj::Maybe<uint32_t> signalOffset,
    uint32_t terminatedOffset) const {
  // Silently skip registration if the terminated address would fall outside the module's linear
  // memory. The terminated field is always required.
  if (static_cast<size_t>(terminatedOffset) + WASM_SIGNAL_FIELD_BYTES > memory.size()) {
    return;
  }
  // If a signal offset was provided, validate it fits in memory too.
  KJ_IF_SOME(offset, signalOffset) {
    if (static_cast<size_t>(offset) + WASM_SIGNAL_FIELD_BYTES > memory.size()) {
      return;
    }
    // Zero the signal address to clear any stale signals.
    uint32_t value = 0;
    memory.asPtr().slice(offset, offset + WASM_SIGNAL_FIELD_BYTES).copyFrom(kj::asBytes(&value, 1));
  }

  // Safe to const_cast: the jsg::Lock& parameter proves we hold the isolate lock, which is the
  // synchronization required by the signal-safe list for mutations.
  const_cast<SignalSafeList<WasmShutdownSignal>&>(list).pushFront(
      WasmShutdownSignal{.memory = kj::mv(memory),
        .signalByteOffset = kj::mv(signalOffset),
        .terminatedByteOffset = terminatedOffset});
}

void WasmShutdownSignalList::filter(jsg::Lock&) const {
  // Safe to const_cast: the jsg::Lock& parameter proves we hold the isolate lock.
  const_cast<SignalSafeList<WasmShutdownSignal>&>(list).filter(
      [](const WasmShutdownSignal& signal) { return signal.isModuleListening(); });
}

void WasmShutdownSignalList::clear(jsg::Lock&) const {
  // Safe to const_cast: the jsg::Lock& parameter proves we hold the isolate lock.
  const_cast<SignalSafeList<WasmShutdownSignal>&>(list).clear();
}

}  // namespace workerd
