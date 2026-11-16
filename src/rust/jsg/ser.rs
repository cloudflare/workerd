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
//! [`serialize_with_options`] and [`deserialize_with`] take the options of
//! `jsg::Serializer::Options` and `jsg::Deserializer::Options` that don't concern externals.
//!
//! The data refers to a `SharedArrayBuffer`, and to an `ArrayBuffer` in
//! [`SerializeOptions::transfer`], by number. Their backing stores stay in the [`Serialized`]
//! that [`serialize_with_options`] returns, and only [`Serialized::deserialize`] hands them to a
//! reader, so a reader always gets the buffers of the data it reads, as `jsg::Deserializer` does
//! when given a serializer's `Released`.
//!
//! Externals are not supported: there is no `jsg::Serializer::ExternalHandler` to write an object
//! as a reference to something outside the data, so serializing a function, a `Proxy` or an API
//! object that needs one (an RPC stub, a `ReadableStream`) throws a `DataCloneError`, as
//! `jsg::Serializer` does without a handler.

use std::marker::PhantomData;

use crate::Error;
use crate::Lock;
use crate::v8;

/// What [`serialize_with_options`] does with a class instance.
///
/// A class instance is an object whose prototype is not `Object.prototype`, and that is not a
/// workerd API object, a native error, a `DOMException` or an instance of one of the runtime's
/// classes implemented in JavaScript.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum ClassInstances {
    /// Writes its own properties, as for a plain object, and drops its prototype, as the
    /// structured clone algorithm does.
    #[default]
    AsPlainObjects,
    /// Throws a `DataCloneError`, as RPC does, so that a class instance is not read back as a
    /// plain object without its methods. V8 still writes an instance of a subclass of a built-in
    /// type, such as `Map`, as the built-in type.
    Reject,
}

/// Options of [`serialize_with_options`], as those of `jsg::Serializer::Options` it supports. The
/// default is `jsg::Serializer`'s.
#[derive(Clone, Copy, Debug, Default)]
pub struct SerializeOptions<'a, 's> {
    /// The version of V8's format to write, rather than the latest. V8 writes versions 13 and
    /// later, up to its latest.
    pub version: Option<u32>,
    /// Whether to leave out the header, which holds the version. Data without one is read with
    /// [`DeserializeOptions::read_header`] false and the version it was written in.
    pub omit_header: bool,
    /// What to do with a class instance, as
    /// `jsg::Serializer::Options::treatClassInstancesAsPlainObjects`.
    pub class_instances: ClassInstances,
    /// `ArrayBuffer`s, or views of them, whose contents move to the reader rather than being
    /// copied, as with `structuredClone`'s `transfer` option and `jsg::Serializer::transfer()`.
    /// Each buffer is transferred once, however many times it or views of it are listed, and is
    /// detached once the value is written.
    pub transfer: &'a [v8::Local<'s, v8::Value>],
}

/// What [`serialize_with_options`] writes, as `jsg::Serializer::Released`: the bytes, and the
/// backing stores of the buffers they refer to by number.
///
/// `Serialized` is neither `Send` nor `Sync`: its buffers are read in the isolate that wrote them,
/// as with `Released`, so that memory that isn't shared is never reached from two threads.
pub struct Serialized {
    data: Vec<u8>,
    shared_array_buffers: Vec<v8::BackingStore>,
    transferred_array_buffers: Vec<v8::BackingStore>,
    _not_send: PhantomData<*mut ()>,
}

impl std::fmt::Debug for Serialized {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Serialized")
            .field("data_len", &self.data.len())
            .field("shared_array_buffers", &self.shared_array_buffers)
            .field("transferred_array_buffers", &self.transferred_array_buffers)
            .finish_non_exhaustive()
    }
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
    /// transferred `ArrayBuffer` as a new one that holds its contents.
    ///
    /// # Errors
    ///
    /// As for [`deserialize_with`].
    pub fn deserialize<'s>(
        self,
        lock: &mut Lock<'s>,
        options: DeserializeOptions,
    ) -> Result<v8::Local<'s, v8::Value>, Error> {
        let (transferred, shared) = self.backing_store_addresses();
        deserialize_impl(lock, &self.data, options, &transferred, &shared)
    }

    /// Takes ownership of what C++ wrote, normally what one `jsg::Serializer` released.
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
            _not_send: PhantomData,
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

/// Serializes `value`, with a header, as `jsg::Serializer` does, and returns the bytes.
///
/// Data that refers to a `SharedArrayBuffer` can't be read back from them;
/// [`serialize_with_options`] keeps the buffers.
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
    Ok(serialize_with_options(lock, value, SerializeOptions::default())?.into_data())
}

/// Serializes `value` as `jsg::Serializer` does with `options`, transferring `options.transfer`
/// first, and returns the bytes with the buffers they refer to.
///
/// # Errors
///
/// As for [`serialize`]. A version V8 can't write fails with a pending `DataCloneError`, and so
/// does a class instance with [`ClassInstances::Reject`]. A value in `options.transfer` that is
/// not an `ArrayBuffer` or a view of one fails with a pending `TypeError`, and a view of a
/// `SharedArrayBuffer` with a pending `DataCloneError`; nothing is detached then.
pub fn serialize_with_options<'s>(
    lock: &mut Lock<'s>,
    value: v8::Local<'s, v8::Value>,
    options: SerializeOptions<'_, 's>,
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
        // SAFETY: on success the shim hands over the data and the backing stores it wrote, of
        // the kinds V8 gave them.
        Ok(ok.then(|| unsafe { Serialized::from_ffi(result) }))
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
/// The default is `jsg::Deserializer`'s.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct DeserializeOptions {
    /// The version of V8's format to read the data in, rather than the one its header holds. Set
    /// it for data written without a header. V8 reads versions 13 and later, up to its latest.
    pub version: Option<u32>,
    /// Whether the data begins with a header. `true` by default.
    pub read_header: bool,
    /// Whether an error read from the data gets the `stack` it was written with. If not, it
    /// keeps the stack it gets when created. `true` by default, as in C++ JSG.
    pub preserve_stack_in_errors: bool,
    /// What the data is, such as `"RPC result"`, to tell apart the internal errors for a missing
    /// or unsupported header, which are logged. A fixed label, so never user data.
    pub diagnostic_context: Option<&'static str>,
}

impl Default for DeserializeOptions {
    fn default() -> Self {
        Self {
            version: None,
            read_header: true,
            preserve_stack_in_errors: true,
            diagnostic_context: None,
        }
    }
}

/// Deserializes as [`deserialize`] does, as `jsg::Deserializer` does with `options` and no
/// buffers. Data that refers to a buffer is malformed without it; read it with
/// [`Serialized::deserialize`].
///
/// # Errors
///
/// As for [`deserialize`]. If the header is read, the internal error for a missing or unsupported
/// one names `options.diagnostic_context`. A version V8 can't read fails with a pending
/// `DataCloneError`, and so does a buffer the data refers to and isn't given.
pub fn deserialize_with<'s>(
    lock: &mut Lock<'s>,
    data: &[u8],
    options: DeserializeOptions,
) -> Result<v8::Local<'s, v8::Value>, Error> {
    deserialize_impl(lock, data, options, &[], &[])
}

/// Deserializes `data` with the backing stores at `transferred_array_buffers` and
/// `shared_array_buffers`, which outlive the call.
fn deserialize_impl<'s>(
    lock: &mut Lock<'s>,
    data: &[u8],
    options: DeserializeOptions,
    transferred_array_buffers: &[usize],
    shared_array_buffers: &[usize],
) -> Result<v8::Local<'s, v8::Value>, Error> {
    let ffi_options = v8::ffi::DeserializeOptions {
        has_version: options.version.is_some(),
        version: options.version.unwrap_or_default(),
        read_header: options.read_header,
        preserve_stack_in_errors: options.preserve_stack_in_errors,
    };
    let diagnostic_context = options.diagnostic_context.unwrap_or("unknown");
    v8::call_into_js(lock, |lock| {
        // SAFETY: the lock guarantees the isolate is locked and a HandleScope is active, and the
        // backing stores, of the kinds V8 gave them, outlive the call. The result belongs to the
        // lock's scope.
        let value = unsafe {
            v8::ffi::value_deserialize(
                lock.isolate().as_ffi(),
                data,
                &ffi_options,
                diagnostic_context,
                transferred_array_buffers,
                shared_array_buffers,
            )
        }?;
        // SAFETY: as above.
        Ok(unsafe { v8::Local::from_ffi_maybe(value) })
    })
}
