// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include <workerd/util/autogate.h>

namespace workerd::rust::autogate {

// AutogateKey is defined in Rust (ffi.rs) as a #[repr(C)] enum whose variant
// values are derived from the C++ util::AutogateKey enum via this X-macro table.
// The static_assert below verifies at compile time that no new gates were added
// to autogate.h without a matching entry here and in ffi.rs.
//
// When adding a gate:
//   1. Add WD_AUTOGATE_KEYS entry in src/workerd/util/autogate.h  (single source of truth)
//   2. Add a matching entry to WD_AUTOGATE_FFI_KEYS below
//   3. Add the variant to the AutogateKey enum in ffi.rs
//   4. Update the static_assert count below

#define WD_AUTOGATE_FFI_KEYS(X)                                                                    \
  X(TestWorkerd, TEST_WORKERD)                                                                     \
  X(V8FastApi, V8_FAST_API)                                                                        \
  X(StreamingTailWorker, STREAMING_TAIL_WORKER)                                                    \
  X(TailStreamRefactor, TAIL_STREAM_REFACTOR)                                                      \
  X(RustBackedNodeDns, RUST_BACKED_NODE_DNS)                                                       \
  X(RpcUseExternalPusher, RPC_USE_EXTERNAL_PUSHER)                                                 \
  X(WasmShutdownSignalShim, WASM_SHUTDOWN_SIGNAL_SHIM)                                             \
  X(EnableFastTextencoder, ENABLE_FAST_TEXTENCODER)                                                \
  X(EnableDrainingReadOnStandardStreams, ENABLE_DRAINING_READ_ON_STANDARD_STREAMS)

// Build the AutogateKey enum with values cast directly from the C++ enum —
// C++ is the authority on ordinal values, no manual numbers here.
#define WD_AUTOGATE_FFI_ENUM(rust_name, cpp_name)                                                  \
  rust_name = static_cast<int>(util::AutogateKey::cpp_name),

enum class AutogateKey : int { WD_AUTOGATE_FFI_KEYS(WD_AUTOGATE_FFI_ENUM) };

#undef WD_AUTOGATE_FFI_ENUM

// If this fires, a gate was added to autogate.h but not to WD_AUTOGATE_FFI_KEYS
// and the Rust enum in ffi.rs. Update both.
static_assert(static_cast<int>(util::AutogateKey::NumOfKeys) == 9,
    "New gate detected in AutogateKey — add it to WD_AUTOGATE_FFI_KEYS in "
    "src/rust/autogate/ffi.h and to the AutogateKey enum in src/rust/autogate/ffi.rs");

// One array lookup — AutogateKey is ABI-compatible with util::AutogateKey
// (same int backing, same ordinals), so the cast is always valid.
inline bool is_enabled(AutogateKey key) {
  return util::Autogate::isEnabled(static_cast<util::AutogateKey>(key));
}

}  // namespace workerd::rust::autogate
