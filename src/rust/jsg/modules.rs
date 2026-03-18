// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::pin::Pin;

pub use ffi::ModuleType;

use crate::v8::ffi;

/// Registers a builtin module with the given specifier and module type.
///
/// The callback is invoked when JavaScript imports the module, receiving the isolate pointer
/// and returning a V8 Local handle to the module's exports object.
pub fn add_builtin(
    registry: Pin<&mut ffi::ModuleRegistry>,
    specifier: &str,
    callback: fn(*mut ffi::Isolate) -> ffi::Local,
    module_type: ModuleType,
) {
    // SAFETY: registry is a valid pinned ModuleRegistry; specifier and callback are valid.
    unsafe {
        ffi::register_add_builtin_module(registry, specifier, callback, module_type);
    }
}
