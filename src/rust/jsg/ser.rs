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
//! - Workerd API objects whose C++ type is serializable, such as `Blob` and `Headers`, through
//!   their own serializers, and a `DOMException`, whichever implementation made it, as C++ JSG
//!   writes it. Rust resources are not serializable.
//! - Instances of the runtime's classes implemented in JavaScript that opt into the per-isolate
//!   bootstrap's protocol for them, such as `QuotaExceededError`. [`serialize`] writes the data
//!   that the class's `clone` function returns for an instance, and [`deserialize`] makes the
//!   copy from the class's prototype, without calling its constructor, and passes it the data
//!   with the class's `deserialize` function once it has read the whole value.
//!
//! A `SharedArrayBuffer` is written as a reference that [`deserialize`] cannot resolve. Transfer
//! lists, externals and the `jsg::Serializer` options other than `version` are not supported.

use crate::Error;
use crate::Lock;
use crate::v8;

/// Options of [`serialize_with_options`], as those of `jsg::Serializer::Options` it supports.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SerializeOptions {
    /// The version of V8's format to write, rather than the latest. V8 writes versions 13 and
    /// later, up to its latest.
    pub version: Option<u32>,
}

/// Serializes `value`, with a header, as `jsg::Serializer` does.
///
/// # Errors
///
/// Serialization runs JavaScript (getters, Proxy traps, and the `clone` functions of classes
/// implemented in JavaScript). If it throws, or `value` can't be
/// serialized (a `DataCloneError`), or an exception or a termination is already pending, the
/// exception stays pending and the error is one for which [`Error::is_pending`] is true. Return
/// it, as for [`v8::Local::<v8::Function>::call`].
pub fn serialize<'s>(
    lock: &mut Lock<'s>,
    value: v8::Local<'s, v8::Value>,
) -> Result<Vec<u8>, Error> {
    serialize_with_options(lock, value, SerializeOptions::default())
}

/// [`serialize`] with `options`, as `jsg::Serializer` with those options.
///
/// # Errors
///
/// As for [`serialize`]. A version V8 can't write fails with a pending `DataCloneError`.
pub fn serialize_with_options<'s>(
    lock: &mut Lock<'s>,
    value: v8::Local<'s, v8::Value>,
    options: SerializeOptions,
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
                options.version.into(),
                &mut data,
            )
        }?;
        Ok(ok.then_some(data))
    })
}

/// Deserializes a value that [`serialize`] or `jsg::Serializer` wrote, as `jsg::Deserializer`
/// does with its default options.
///
/// Runs no JavaScript but the `deserialize` functions of the classes implemented in JavaScript
/// whose instances the value holds, once it has read the whole value.
///
/// # Errors
///
/// If `data` is malformed or names an unknown API type or class, or a type's deserializer or a
/// class's `deserialize` function throws, or an exception or a termination is already pending,
/// the exception stays pending and the error is one for which [`Error::is_pending`] is true. So
/// it does, as a `DataCloneError`, if the value holds an instance of a class implemented in
/// JavaScript while C++ code has disallowed JavaScript execution (`jsg::DisallowJavaScriptScope`).
/// If the header is missing or of an unsupported version, the error is an internal one.
pub fn deserialize<'s>(
    lock: &mut Lock<'s>,
    data: &[u8],
) -> Result<v8::Local<'s, v8::Value>, Error> {
    deserialize_with(lock, data, DeserializeOptions::default())
}

/// The options of [`deserialize_with`], those of `jsg::Deserializer::Options` that it supports.
#[derive(Debug, Clone, Copy)]
pub struct DeserializeOptions {
    /// Whether an error read from the data gets the `stack` it was written with. If not, it
    /// keeps the stack it gets when created. `true` by default, as in C++ JSG.
    pub preserve_stack_in_errors: bool,
}

impl Default for DeserializeOptions {
    fn default() -> Self {
        Self {
            preserve_stack_in_errors: true,
        }
    }
}

/// Deserializes as [`deserialize`] does, as `jsg::Deserializer` does with `options`.
///
/// # Errors
///
/// As for [`deserialize`].
pub fn deserialize_with<'s>(
    lock: &mut Lock<'s>,
    data: &[u8],
    options: DeserializeOptions,
) -> Result<v8::Local<'s, v8::Value>, Error> {
    v8::call_into_js(lock, |lock| {
        // SAFETY: the lock guarantees the isolate is locked and a HandleScope is active. The
        // result belongs to the lock's scope.
        let value = unsafe {
            v8::ffi::value_deserialize(
                lock.isolate().as_ffi(),
                data,
                options.preserve_stack_in_errors,
            )
        }?;
        // SAFETY: as above.
        Ok(unsafe { v8::Local::from_ffi_maybe(value) })
    })
}
