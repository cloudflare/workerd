// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#![allow(
    unsafe_code,
    reason = "registers module callbacks with the C++ registry"
)]

use std::pin::Pin;

use crate::CallbackScope;
use crate::Lock;
use crate::UnitType;
use crate::v8;
use crate::v8::ffi;
pub use crate::v8::ffi::ModuleType;

/// Registers a builtin module with the given specifier and module type.
///
/// `callback` produces the module's exports when JavaScript imports it. It runs in the module
/// registry's current scope, so it receives a [`Lock`] for that scope and returns a handle
/// branded with the same scope. It must capture nothing: C++ calls it through a plain function
/// pointer, so there is nowhere to keep captured state (see [`UnitType`]).
pub fn add_builtin<F>(
    registry: Pin<&mut ffi::ModuleRegistry>,
    specifier: &str,
    callback: F,
    module_type: ModuleType,
) -> Result<(), cxx::KjError>
where
    F: UnitType + for<'s> Fn(&mut Lock<'s>) -> v8::Local<'s, v8::Value>,
{
    // Called only by the C++ module registry, while `isolate` is locked, a context is entered
    // and a `HandleScope` is open that outlives the call. It is not an `unsafe fn` because cxx
    // passes function pointers as safe `fn`s, and no other code can name it. Returns an empty
    // handle if `F` panicked, with the exception pending.
    fn trampoline<F>(isolate: *mut ffi::Isolate) -> ffi::MaybeLocal
    where
        F: UnitType + for<'s> Fn(&mut Lock<'s>) -> v8::Local<'s, v8::Value>,
    {
        // SAFETY: see above.
        let scope = std::pin::pin!(unsafe { CallbackScope::new(isolate) });
        crate::catch_panic(scope, |lock| {
            // SAFETY: `add_builtin` was handed a value of `F`.
            let callback = unsafe { crate::scope::unit_value::<F>() };
            // SAFETY: the handle lives in the registry's HandleScope, which outlives this call.
            unsafe { v8::Local::into_ffi_maybe(Some(callback(lock))) }
        })
        .unwrap_or_default()
    }

    let _ = callback;
    // SAFETY: the registry calls `trampoline` as its contract requires.
    unsafe {
        ffi::register_add_builtin_module(registry, specifier, trampoline::<F>, module_type)?;
    }
    Ok(())
}
