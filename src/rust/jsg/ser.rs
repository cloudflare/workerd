// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#![allow(unsafe_code, reason = "serializes values through the V8 FFI")]

//! Structured serialization of JavaScript values, in the format of C++ JSG's `jsg::Serializer`
//! and `jsg::Deserializer`.
//!
//! Each reads what the other writes, and [`serialize_with`] writes the same bytes as
//! `jsg::Serializer` with the same options. That covers what V8's serializer supports, plus:
//!
//! - Native errors, as host objects with their own properties (including `stack`), when the
//!   `enhanced_error_serialization` compatibility flag is on.
//! - Workerd API objects whose C++ type is serializable, such as `Blob`, `Headers` and
//!   `DOMException`, through their own serializers. Rust resources are not serializable.
//!
//! A `SharedArrayBuffer` is written as a reference that [`deserialize`] cannot resolve. Transfer
//! lists and externals are not supported.

use crate::Error;
use crate::Lock;
use crate::v8;

/// How [`serialize_with`] writes a value, as `jsg::Serializer::Options`. The default is
/// `jsg::Serializer`'s.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SerializeOptions {
    /// The format version to write, at least 13, or `None` for V8's current one. Data written in
    /// an older version can be read by an older V8.
    pub version: Option<u32>,
    /// Whether to leave out the header, which holds the version. Data without one is read with
    /// [`DeserializeOptions::read_header`] false and the version it was written in.
    pub omit_header: bool,
}

/// How [`deserialize_with`] reads data, as `jsg::Deserializer::Options`. The default is
/// `jsg::Deserializer`'s.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DeserializeOptions<'a> {
    /// The format version to read in, at least 13, or `None` for the one the header holds. Set
    /// it for data written without a header.
    pub version: Option<u32>,
    /// Whether the data begins with a header.
    pub read_header: bool,
    /// Whether a native error gets back the `stack` it was written with, or keeps the one
    /// captured as it is created. Untrusted data should not choose the stack.
    pub preserve_stack_in_errors: bool,
    /// What the data is, such as `"RPC result"`, to tell apart the internal errors reported for
    /// a missing or unsupported header. A fixed label, never user data: it is logged.
    pub diagnostic_context: Option<&'a str>,
}

impl Default for DeserializeOptions<'_> {
    fn default() -> Self {
        Self {
            version: None,
            read_header: true,
            preserve_stack_in_errors: true,
            diagnostic_context: None,
        }
    }
}

/// Serializes `value`, with a header, as `jsg::Serializer` does.
///
/// # Errors
///
/// As for [`serialize_with`].
pub fn serialize<'s>(
    lock: &mut Lock<'s>,
    value: v8::Local<'s, v8::Value>,
) -> Result<Vec<u8>, Error> {
    serialize_with(lock, value, &SerializeOptions::default())
}

/// Serializes `value` as `jsg::Serializer` does with `options`.
///
/// # Errors
///
/// Serialization runs JavaScript (getters and Proxy traps). If it throws, or `value` can't be
/// serialized (a `DataCloneError`), or `options.version` is newer than V8's (a
/// `DataCloneError`), or execution is terminating, the exception stays pending and the error is
/// one for which [`Error::is_pending`] is true. Return it, as for
/// [`v8::Local::<v8::Function>::call`]. A version older than 13 is an internal error.
pub fn serialize_with<'s>(
    lock: &mut Lock<'s>,
    value: v8::Local<'s, v8::Value>,
    options: &SerializeOptions,
) -> Result<Vec<u8>, Error> {
    value.debug_assert_isolate(lock);
    let ffi_options = v8::ffi::SerializeOptions {
        has_version: options.version.is_some(),
        version: options.version.unwrap_or_default(),
        omit_header: options.omit_header,
        errors_are_host_objects: lock.feature_flags().get_enhanced_error_serialization(),
    };
    v8::call_into_js(lock, |lock| {
        let mut data = Vec::new();
        // SAFETY: the lock guarantees the isolate is locked and a HandleScope is active, and
        // `value` belongs to its scope.
        let ok = unsafe {
            v8::ffi::local_serialize(
                lock.isolate().as_ffi(),
                value.as_ffi(),
                &ffi_options,
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
/// As for [`deserialize_with`].
pub fn deserialize<'s>(
    lock: &mut Lock<'s>,
    data: &[u8],
) -> Result<v8::Local<'s, v8::Value>, Error> {
    deserialize_with(lock, data, &DeserializeOptions::default())
}

/// Deserializes a value that [`serialize_with`] or `jsg::Serializer` wrote, as
/// `jsg::Deserializer` does with `options`. Runs no JavaScript.
///
/// # Errors
///
/// If `data` is malformed or names an unknown API type, or a type's deserializer throws, or
/// execution is terminating, the exception stays pending and the error is one for which
/// [`Error::is_pending`] is true. If the header is read and is missing or of an unsupported
/// version, or `options.version` is older than 13, the error is an internal one.
pub fn deserialize_with<'s>(
    lock: &mut Lock<'s>,
    data: &[u8],
    options: &DeserializeOptions<'_>,
) -> Result<v8::Local<'s, v8::Value>, Error> {
    let ffi_options = v8::ffi::DeserializeOptions {
        has_version: options.version.is_some(),
        version: options.version.unwrap_or_default(),
        read_header: options.read_header,
        preserve_stack_in_errors: options.preserve_stack_in_errors,
    };
    let diagnostic_context = options.diagnostic_context.unwrap_or("unknown");
    v8::call_into_js(lock, |lock| {
        // SAFETY: the lock guarantees the isolate is locked and a HandleScope is active. The
        // result belongs to the lock's scope.
        let value = unsafe {
            v8::ffi::value_deserialize(
                lock.isolate().as_ffi(),
                data,
                &ffi_options,
                diagnostic_context,
            )
        }?;
        // SAFETY: as above.
        Ok(unsafe { v8::Local::from_ffi_maybe(value) })
    })
}
