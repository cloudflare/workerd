// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Rust interface to the C++ `workerd::util::Autogate` system.
//!
//! Autogates are temporary runtime feature gates for gradual rollout of risky
//! changes. They can be toggled independently of binary releases.
//!
//! # Adding a gate
//!
//! 1. Add a `WD_AUTOGATE_KEYS` entry in `src/workerd/util/autogate.h` (single source of truth)
//! 2. Add a matching entry to `WD_AUTOGATE_FFI_KEYS` in `src/rust/autogate/ffi.h`
//! 3. Add the variant to [`AutogateKey`] below
//! 4. Update the `static_assert` count in `ffi.h`
//!
//! # Usage
//!
//! ```ignore
//! use autogate::{Autogate, AutogateKey};
//!
//! if Autogate::is_enabled(AutogateKey::RustBackedNodeDns) {
//!     // fast path
//! }
//! ```

/// Mirrors `workerd::util::AutogateKey`.
///
/// `#[repr(C)]` with discriminants matching the C++ enum ordinals — ABI-compatible
/// so values pass through `extern "C"` without any cast. The `static_assert` in
/// `ffi.h` verifies at compile time that neither side has drifted.
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

/// Thin wrapper around `workerd::util::Autogate`.
///
/// [`Autogate::is_enabled`] passes [`AutogateKey`] directly through `extern "C"`
/// — one array lookup in C++, no strings, no allocation.
pub struct Autogate;

impl Autogate {
    #[inline]
    pub fn is_enabled(key: AutogateKey) -> bool {
        // SAFETY: reads from a process-global bool array written once at startup.
        unsafe { autogate_is_enabled(key) }
    }
}
