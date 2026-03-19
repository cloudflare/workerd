// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// When adding a gate:
//   1. Add WD_AUTOGATE_KEYS entry in src/workerd/util/autogate.h  (single source of truth)
//   2. Add matching entry to WD_AUTOGATE_FFI_KEYS in src/rust/autogate/ffi.h
//   3. Add the variant here (value from the C++ enum via ffi.h)
//   4. Update the static_assert count in ffi.h

/// Mirrors `workerd::util::AutogateKey`.
///
/// `#[repr(C)]` with discriminants that match the C++ enum ordinals.
/// Defined once here — `ffi.h` builds its `AutogateKey` enum from this same
/// list (via `WD_AUTOGATE_FFI_KEYS`) so both sides always agree.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AutogateKey {
    TestWorkerd = 0,
    V8FastApi = 1,
    StreamingTailWorker = 2,
    TailStreamRefactor = 3,
    RustBackedNodeDns = 4,
    RpcUseExternalPusher = 5,
    WasmShutdownSignalShim = 6,
    EnableFastTextencoder = 7,
    EnableDrainingReadOnStandardStreams = 8,
}

unsafe extern "C" {
    // Implemented in src/workerd/util/autogate.c++ — one array lookup.
    #[link_name = "workerd_autogate_is_enabled"]
    fn autogate_is_enabled(key: AutogateKey) -> bool;
}

pub fn is_enabled(key: AutogateKey) -> bool {
    // SAFETY: reads from a process-global bool array written once at startup.
    unsafe { autogate_is_enabled(key) }
}
