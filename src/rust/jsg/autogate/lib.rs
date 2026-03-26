// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Rust interface to the C++ `workerd::util::Autogate` system.
//!
//! Autogates are temporary runtime feature gates for gradual rollout of risky
//! changes. They can be toggled independently of binary releases and are removed
//! once the change is stable.
//!
//! ```ignore
//! if jsg::Autogate::is_enabled("rust-backed-node-buffer") {
//!     // new code path
//! }
//! ```

/// Thin wrapper around the C++ `workerd::util::Autogate` system.
///
/// Autogates are temporary runtime feature gates for gradual rollout.
/// Use [`Autogate::is_enabled`] to check if a gate is active.
pub struct Autogate;

impl Autogate {
    /// Returns `true` if the autogate identified by `key` is currently enabled.
    ///
    /// `key` is the kebab-case name string (e.g. `"rust-backed-node-buffer"`),
    /// matching the `KJ_STRINGIFY` mapping in `autogate.c++`.
    ///
    /// Returns `false` if the key does not match any known autogate.
    pub fn is_enabled(key: &str) -> bool {
        super::ffi::is_enabled(key.to_owned())
    }
}
