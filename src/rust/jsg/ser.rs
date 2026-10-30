// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#![allow(unsafe_code, reason = "serializes values through the V8 FFI")]

//! Structured serialization of JavaScript values, in the format of C++ JSG's `jsg::Serializer`
//! and `jsg::Deserializer`.
//!
//! Each reads what the other writes, and [`serialize`] writes the same bytes as
//! `jsg::Serializer` with its default options. That covers what V8's serializer supports, plus:
//!
//! - Native errors, as host objects with their own properties (including `stack`), when the
//!   `enhanced_error_serialization` compatibility flag is on.
//! - Workerd API objects whose C++ type is serializable, such as `Blob`, `Headers` and
//!   `DOMException`, through their own serializers. Rust resources are not serializable.
//!
//! A `SharedArrayBuffer` is written as a reference that [`deserialize`] cannot resolve. Transfer
//! lists, externals and the other `jsg::Serializer` options are not supported.

use crate::Error;
use crate::Lock;
use crate::v8;

/// Serializes `value`, with a header, as `jsg::Serializer` does.
///
/// # Errors
///
/// Serialization runs JavaScript (getters and Proxy traps). If it throws, or `value` can't be
/// serialized (a `DataCloneError`), or execution is terminating, the exception stays pending
/// and the error is one for which [`Error::is_pending`] is true. Return it, as for
/// [`v8::Local::<v8::Function>::call`].
pub fn serialize<'s>(
    lock: &mut Lock<'s>,
    value: v8::Local<'s, v8::Value>,
) -> Result<Vec<u8>, Error> {
    value.debug_assert_isolate(lock);
    let errors_are_host_objects = lock.feature_flags().get_enhanced_error_serialization();
    v8::call_into_js(lock, |lock| {
        let mut data = Vec::new();
        // SAFETY: the lock guarantees the isolate is locked and a HandleScope is active, and
        // `value` belongs to its scope.
        let ok = unsafe {
            v8::ffi::local_serialize(
                lock.isolate().as_ffi(),
                value.as_ffi(),
                errors_are_host_objects,
                &mut data,
            )
        }?;
        Ok(ok.then_some(data))
    })
}

/// Deserializes a value that [`serialize`] or `jsg::Serializer` wrote, as `jsg::Deserializer`
/// does with its default options. Runs no JavaScript.
///
/// # Errors
///
/// If `data` is malformed or names an unknown API type, or a type's deserializer throws, or
/// execution is terminating, the exception stays pending and the error is one for which
/// [`Error::is_pending`] is true. If the header is missing or of an unsupported version, the
/// error is an internal one.
pub fn deserialize<'s>(
    lock: &mut Lock<'s>,
    data: &[u8],
) -> Result<v8::Local<'s, v8::Value>, Error> {
    v8::call_into_js(lock, |lock| {
        // SAFETY: the lock guarantees the isolate is locked and a HandleScope is active. The
        // result belongs to the lock's scope.
        let value = unsafe { v8::ffi::value_deserialize(lock.isolate().as_ffi(), data) }?;
        // SAFETY: as above.
        Ok(unsafe { v8::Local::from_ffi_maybe(value) })
    })
}
