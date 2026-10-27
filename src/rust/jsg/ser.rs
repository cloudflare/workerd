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
//! A `SharedArrayBuffer` is written as a reference to its backing store, which [`Serialized`]
//! holds alongside the bytes, and is shared by a deserializer given it; so is an `ArrayBuffer` in
//! [`SerializeOptions::transfer`], whose contents move to the deserializer.
//!
//! Externals are not supported: there is no `jsg::Serializer::ExternalHandler` to write an object
//! as a reference to something outside the data, so serializing a function, a `Proxy` or an API
//! object that needs one (an RPC stub, a `ReadableStream`) throws a `DataCloneError`, as
//! `jsg::Serializer` does without a handler.

use crate::Error;
use crate::Lock;
use crate::v8;

/// What [`serialize_with`] does with a class instance: an object whose prototype is not
/// `Object.prototype` and that is not a workerd API object or a native error.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum ClassInstances {
    /// Writes its own properties, as for a plain object, and drops its prototype, as the
    /// structured clone algorithm does.
    #[default]
    AsPlainObjects,
    /// Throws a `DataCloneError`, as RPC does, so that a class instance is not read back as a
    /// plain object without its methods.
    Reject,
}

/// How [`serialize_with`] writes a value, as `jsg::Serializer::Options`. The default is
/// `jsg::Serializer`'s.
#[derive(Clone, Copy, Debug, Default)]
pub struct SerializeOptions<'a, 's> {
    /// The format version to write, at least 13, or `None` for V8's current one. Data written in
    /// an older version can be read by an older V8.
    pub version: Option<u32>,
    /// Whether to leave out the header, which holds the version. Data without one is read with
    /// [`DeserializeOptions::read_header`] false and the version it was written in.
    pub omit_header: bool,
    /// What to do with a class instance, as
    /// `jsg::Serializer::Options::treatClassInstancesAsPlainObjects`.
    pub class_instances: ClassInstances,
    /// `ArrayBuffer`s, or views of them, whose contents move to the deserializer rather than
    /// being copied, as `structuredClone`'s `transfer` option and `jsg::Serializer::transfer()`:
    /// the data refers to each by number, and each is detached once the value is written. Their
    /// backing stores are in [`Serialized::transferred_array_buffers`].
    pub transfer: &'a [v8::Local<'s, v8::Value>],
}

/// What [`serialize_with`] writes, as `jsg::Serializer::Released`.
#[derive(Debug, Default)]
pub struct Serialized {
    /// The bytes.
    pub data: Vec<u8>,
    /// The backing store of each `SharedArrayBuffer` in the value, in the order the data numbers
    /// them. Pass them to [`DeserializeOptions::shared_array_buffers`] to share the buffers.
    pub shared_array_buffers: Vec<v8::BackingStore>,
    /// The backing store of each value in [`SerializeOptions::transfer`], in order. Pass them to
    /// [`DeserializeOptions::transferred_array_buffers`].
    pub transferred_array_buffers: Vec<v8::BackingStore>,
}

impl Serialized {
    /// The default options with this data's buffers, to read it back as
    /// `jsg::Deserializer(js, released)` does.
    pub fn deserialize_options(&self) -> DeserializeOptions<'_> {
        DeserializeOptions {
            transferred_array_buffers: &self.transferred_array_buffers,
            shared_array_buffers: Some(&self.shared_array_buffers),
            ..Default::default()
        }
    }
}

/// How [`deserialize_with`] reads data, as `jsg::Deserializer::Options`. The default is
/// `jsg::Deserializer`'s.
#[derive(Clone, Copy, Debug)]
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
    /// The backing stores of the transferred `ArrayBuffer`s, as
    /// [`Serialized::transferred_array_buffers`]. Each becomes a new `ArrayBuffer` that shares
    /// it. A shared one is an internal error.
    pub transferred_array_buffers: &'a [v8::BackingStore],
    /// The backing stores of the `SharedArrayBuffer`s, as [`Serialized::shared_array_buffers`],
    /// or `None` to throw for data that has one. Each becomes a new `SharedArrayBuffer` that
    /// shares it. One that is not shared is an internal error.
    pub shared_array_buffers: Option<&'a [v8::BackingStore]>,
}

impl Default for DeserializeOptions<'_> {
    fn default() -> Self {
        Self {
            version: None,
            read_header: true,
            preserve_stack_in_errors: true,
            diagnostic_context: None,
            transferred_array_buffers: &[],
            shared_array_buffers: None,
        }
    }
}

/// Serializes `value`, with a header, as `jsg::Serializer` does.
///
/// # Errors
///
/// As for [`serialize_with`].
/// The data refers to a `SharedArrayBuffer` in `value` by number, without its backing store, so
/// it can't be read back; use [`serialize_with`] to get the backing stores.
pub fn serialize<'s>(
    lock: &mut Lock<'s>,
    value: v8::Local<'s, v8::Value>,
) -> Result<Vec<u8>, Error> {
    Ok(serialize_with(lock, value, &SerializeOptions::default())?.data)
}

/// Serializes `value` as `jsg::Serializer` does with `options`, transferring
/// `options.transfer` first.
///
/// # Errors
///
/// A value in `options.transfer` that is not an `ArrayBuffer` or a view of one throws a
/// `TypeError`, and a view of a `SharedArrayBuffer` a `DataCloneError`; nothing is detached.
/// Serialization runs JavaScript (getters and Proxy traps). If it throws, or `value` can't be
/// serialized (a `DataCloneError`, as for a class instance with [`ClassInstances::Reject`]), or `options.version` is newer than V8's (a
/// `DataCloneError`), or execution is terminating, the exception stays pending and the error is
/// one for which [`Error::is_pending`] is true. Return it, as for
/// [`v8::Local::<v8::Function>::call`]. A version older than 13 is an internal error.
pub fn serialize_with<'s>(
    lock: &mut Lock<'s>,
    value: v8::Local<'s, v8::Value>,
    options: &SerializeOptions<'_, 's>,
) -> Result<Serialized, Error> {
    value.debug_assert_isolate(lock);
    for item in options.transfer {
        item.debug_assert_isolate(lock);
    }
    let ffi_options = v8::ffi::SerializeOptions {
        has_version: options.version.is_some(),
        version: options.version.unwrap_or_default(),
        omit_header: options.omit_header,
        class_instances_as_plain_objects: options.class_instances == ClassInstances::AsPlainObjects,
        errors_are_host_objects: lock.feature_flags().get_enhanced_error_serialization(),
    };
    v8::call_into_js(lock, |lock| {
        let mut result = v8::ffi::Serialized::default();
        // SAFETY: the lock guarantees the isolate is locked and a HandleScope is active, and
        // `value` and the values to transfer belong to its scope.
        let ok = unsafe {
            v8::ffi::local_serialize(
                lock.isolate().as_ffi(),
                value.as_ffi(),
                &ffi_options,
                v8::Local::raw_slice(options.transfer),
                &mut result,
            )
        }?;
        if !ok {
            return Ok(None);
        }
        let take = |addresses: Vec<usize>| {
            addresses
                .into_iter()
                // SAFETY: each is a heap-allocated std::shared_ptr<v8::BackingStore> that C++
                // handed over and nothing else owns.
                .map(|address| unsafe { v8::BackingStore::from_ffi(address) })
                .collect()
        };
        Ok(Some(Serialized {
            data: result.data,
            shared_array_buffers: take(result.shared_array_buffers),
            transferred_array_buffers: take(result.transferred_array_buffers),
        }))
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
/// If `data` is malformed or names an unknown API type or buffer, or a type's deserializer
/// throws, or
/// execution is terminating, the exception stays pending and the error is one for which
/// [`Error::is_pending`] is true. If the header is read and is missing or of an unsupported
/// version, or `options.version` is older than 13, or a backing store is of the wrong kind, the
/// error is an internal one.
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
        has_shared_array_buffers: options.shared_array_buffers.is_some(),
    };
    let addresses = |stores: &[v8::BackingStore]| {
        stores
            .iter()
            .map(v8::BackingStore::as_ffi)
            .collect::<Vec<_>>()
    };
    let transferred_array_buffers = addresses(options.transferred_array_buffers);
    let shared_array_buffers = addresses(options.shared_array_buffers.unwrap_or_default());
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
                &transferred_array_buffers,
                &shared_array_buffers,
            )
        }?;
        // SAFETY: as above.
        Ok(unsafe { v8::Local::from_ffi_maybe(value) })
    })
}
