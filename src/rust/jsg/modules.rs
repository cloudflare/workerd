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
use crate::Rc;
use crate::Resource;
use crate::ToJS;
use crate::TypeInfo;
use crate::UnitType;
use crate::rtti::Context;
use crate::v8;
use crate::v8::ffi;
pub use crate::v8::ffi::ModuleType;

/// Registers a builtin module with the given specifier and module type.
///
/// `callback` produces the module's exports resource when JavaScript imports it, and
/// registration wraps it. The callback runs in the module registry's current scope, so it
/// receives a [`Lock`] for that scope; an error it returns is thrown to the importer. It sees no
/// request, even when a request imports the module: the C++ registration runs it with no current
/// `IoContext`. It must capture nothing: C++ calls it through a plain function pointer, so there is nowhere to keep
/// captured state (see [`UnitType`]).
///
/// The exports type `T` also determines the module's RTTI, which the RTTI export collects
/// without invoking the callback.
pub fn add_builtin<T, F>(
    registry: Pin<&mut ffi::ModuleRegistry>,
    specifier: &str,
    callback: F,
    module_type: ModuleType,
) where
    T: Resource + TypeInfo,
    F: UnitType + for<'s> Fn(&mut Lock<'s>) -> crate::Result<Rc<T>>,
{
    // Called only by the C++ module registry, while `isolate` is locked, a context is entered
    // and a `HandleScope` is open that outlives the call. It is not an `unsafe fn` because cxx
    // passes function pointers as safe `fn`s, and no other code can name it. Returns an empty
    // handle if `F` or the conversion of its result failed, or `F` panicked, with the exception
    // pending.
    fn trampoline<T, F>(isolate: *mut ffi::Isolate) -> ffi::MaybeLocal
    where
        T: Resource,
        F: UnitType + for<'s> Fn(&mut Lock<'s>) -> crate::Result<Rc<T>>,
    {
        // SAFETY: see above.
        let scope = std::pin::pin!(unsafe { CallbackScope::new(isolate) });
        crate::catch_panic(scope, |lock| {
            // SAFETY: `add_builtin` was handed a value of `F`.
            let callback = unsafe { crate::scope::unit_value::<F>() };
            let exports = match callback(lock).and_then(|exports| exports.to_js(lock)) {
                Ok(exports) => Some(exports),
                Err(err) => {
                    lock.throw_exception(&err);
                    None
                }
            };
            // SAFETY: the handle lives in the registry's HandleScope, which outlives this call.
            unsafe { v8::Local::into_ffi_maybe(exports) }
        })
        .unwrap_or_default()
    }

    let _ = callback;
    // SAFETY: the registry calls `trampoline` as its contract requires.
    unsafe {
        ffi::register_add_builtin_module(
            registry,
            specifier,
            trampoline::<T, F>,
            Context::add_module::<T>,
            module_type,
        );
    }
}
