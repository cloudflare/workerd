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
//! The data refers to a `SharedArrayBuffer`, and to an `ArrayBuffer` in
//! [`SerializeOptions::transfer`], by number. Their backing stores stay in the [`Serialized`]
//! that [`serialize_with`] returns, and only [`Serialized::deserialize`] hands them to a reader,
//! so a reader always gets the buffers of the data it reads, as `jsg::Deserializer` does when
//! given a serializer's `Released`.
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
    /// `ArrayBuffer`s, or views of them, whose contents move to the reader rather than being
    /// copied, as with `structuredClone`'s `transfer` option and `jsg::Serializer::transfer()`.
    /// A value listed twice is transferred once. Each buffer is detached once the value is
    /// written.
    pub transfer: &'a [v8::Local<'s, v8::Value>],
}

/// What [`serialize_with`] writes, as `jsg::Serializer::Released`: the bytes, and the backing
/// stores of the buffers they refer to by number.
#[derive(Debug)]
pub struct Serialized {
    data: Vec<u8>,
    shared_array_buffers: Vec<v8::BackingStore>,
    transferred_array_buffers: Vec<v8::BackingStore>,
}

impl Serialized {
    /// The bytes.
    pub fn data(&self) -> &[u8] {
        &self.data
    }

    /// The bytes, without the buffers. Data that refers to a buffer can then not be read back.
    pub fn into_data(self) -> Vec<u8> {
        self.data
    }

    /// Deserializes the value, as `jsg::Deserializer(js, released, options)` does: each
    /// `SharedArrayBuffer` is read as a new one that shares the original's memory, and each
    /// transferred `ArrayBuffer` as a new one that holds its contents. Runs no JavaScript.
    ///
    /// # Errors
    ///
    /// As for [`deserialize_with`].
    pub fn deserialize<'s>(
        self,
        lock: &mut Lock<'s>,
        options: &DeserializeOptions<'_>,
    ) -> Result<v8::Local<'s, v8::Value>, Error> {
        let (transferred, shared) = self.backing_store_addresses();
        deserialize_impl(lock, &self.data, options, &transferred, Some(&shared))
    }

    /// Takes ownership of what C++ wrote, normally what one `jsg::Serializer` released. Data that
    /// names a `SharedArrayBuffer` beyond the list is read as corrupt.
    ///
    /// # Safety
    ///
    /// Each backing store must be the address of a heap-allocated
    /// `std::shared_ptr<v8::BackingStore>` that nothing else owns, those in
    /// `shared_array_buffers` shared and those in `transferred_array_buffers` not, as V8 requires
    /// of the buffers it creates over them.
    #[doc(hidden)]
    pub unsafe fn from_ffi(serialized: v8::ffi::Serialized) -> Self {
        let take = |addresses: Vec<usize>| {
            addresses
                .into_iter()
                // SAFETY: the caller hands over each std::shared_ptr.
                .map(|address| unsafe { v8::BackingStore::from_ffi(address) })
                .collect()
        };
        Self {
            data: serialized.data,
            shared_array_buffers: take(serialized.shared_array_buffers),
            transferred_array_buffers: take(serialized.transferred_array_buffers),
        }
    }

    /// The addresses of the transferred and the shared backing stores, for C++ to borrow, in the
    /// order the data numbers them.
    #[doc(hidden)]
    pub fn backing_store_addresses(&self) -> (Vec<usize>, Vec<usize>) {
        let addresses = |stores: &[v8::BackingStore]| {
            stores
                .iter()
                .map(v8::BackingStore::as_ffi)
                .collect::<Vec<_>>()
        };
        (
            addresses(&self.transferred_array_buffers),
            addresses(&self.shared_array_buffers),
        )
    }
}

/// How [`deserialize_with`] and [`Serialized::deserialize`] read data, as
/// `jsg::Deserializer::Options`. The default is `jsg::Deserializer`'s.
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

/// Serializes `value`, with a header, as `jsg::Serializer` does, and returns the bytes. Data that
/// refers to a `SharedArrayBuffer` can't be read back from them; [`serialize_with`] keeps the
/// buffers.
///
/// # Errors
///
/// As for [`serialize_with`].
pub fn serialize<'s>(
    lock: &mut Lock<'s>,
    value: v8::Local<'s, v8::Value>,
) -> Result<Vec<u8>, Error> {
    Ok(serialize_with(lock, value, &SerializeOptions::default())?.into_data())
}

/// Serializes `value` as `jsg::Serializer` does with `options`, transferring `options.transfer`
/// first.
///
/// # Errors
///
/// A value in `options.transfer` that is not an `ArrayBuffer` or a view of one throws a
/// `TypeError`, and a view of a `SharedArrayBuffer` a `DataCloneError`; nothing is detached.
/// Serialization runs JavaScript (getters and Proxy traps). If it throws, or `value` can't be
/// serialized (a `DataCloneError`, as for a class instance with [`ClassInstances::Reject`]), or
/// `options.version` is newer than V8's (a `DataCloneError`), or execution is terminating, the
/// exception stays pending and the error is one for which [`Error::is_pending`] is true. Return
/// it, as for [`v8::Local::<v8::Function>::call`]. A version older than 13 is an internal error.
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
        // SAFETY: on success the shim hands over the data and the backing stores it wrote.
        Ok(ok.then(|| unsafe { Serialized::from_ffi(result) }))
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
/// `jsg::Deserializer` does with `options` and no buffers. Runs no JavaScript.
///
/// Data that refers to a buffer throws; read it with [`Serialized::deserialize`].
///
/// # Errors
///
/// If `data` is malformed or names an unknown API type or buffer, or a type's deserializer
/// throws, or execution is terminating, the exception stays pending and the error is one for
/// which [`Error::is_pending`] is true. If the header is read and is missing or of an unsupported
/// version, or `options.version` is older than 13, the error is an internal one.
pub fn deserialize_with<'s>(
    lock: &mut Lock<'s>,
    data: &[u8],
    options: &DeserializeOptions<'_>,
) -> Result<v8::Local<'s, v8::Value>, Error> {
    deserialize_impl(lock, data, options, &[], None)
}

fn deserialize_impl<'s>(
    lock: &mut Lock<'s>,
    data: &[u8],
    options: &DeserializeOptions<'_>,
    transferred_array_buffers: &[usize],
    shared_array_buffers: Option<&[usize]>,
) -> Result<v8::Local<'s, v8::Value>, Error> {
    let ffi_options = v8::ffi::DeserializeOptions {
        has_version: options.version.is_some(),
        version: options.version.unwrap_or_default(),
        read_header: options.read_header,
        preserve_stack_in_errors: options.preserve_stack_in_errors,
        has_shared_array_buffers: shared_array_buffers.is_some(),
    };
    let diagnostic_context = options.diagnostic_context.unwrap_or("unknown");
    v8::call_into_js(lock, |lock| {
        // SAFETY: the lock guarantees the isolate is locked and a HandleScope is active, and the
        // backing stores outlive the call. The result belongs to the lock's scope.
        let value = unsafe {
            v8::ffi::value_deserialize(
                lock.isolate().as_ffi(),
                data,
                &ffi_options,
                diagnostic_context,
                transferred_array_buffers,
                shared_array_buffers.unwrap_or_default(),
            )
        }?;
        // SAFETY: as above.
        Ok(unsafe { v8::Local::from_ffi_maybe(value) })
    })
}
