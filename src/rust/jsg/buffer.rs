// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#![allow(unsafe_code, reason = "creates typed arrays through the V8 FFI")]

//! Typed arrays and `ArrayBuffer`s from Rust sequences.
//!
//! A `Vec` converts to a JavaScript `Array` through [`ToJS`](crate::ToJS). A typed array or an
//! `ArrayBuffer` is an explicit conversion of a slice:
//! [`to_js_typed_array`](ToJsTypedArray::to_js_typed_array) makes the typed array of the element
//! type, and [`to_js_array_buffer`](ToJsArrayBuffer::to_js_array_buffer) makes an `ArrayBuffer` of
//! bytes, which is what C++ JSG makes of a `kj::Array<kj::byte>`.
//!
//! A typed array that comes from JavaScript is a handle, such as `v8::Local<'s, v8::Uint8Array>`,
//! whose `as_slice(lock)` reads its memory in place. For an owned copy, call `to_vec()` on the
//! slice.

use crate::Lock;
use crate::v8;

/// Converts a slice of numbers to the typed array of its element type: `[u8]` to a
/// `Uint8Array`, `[f64]` to a `Float64Array`, `[i64]` to a `BigInt64Array`, and so on.
///
/// The typed array views a new `ArrayBuffer` that holds the elements. The slice is not borrowed
/// afterwards.
///
/// A `Vec<T>` or an array `[T; N]` calls these through auto-deref:
///
/// ```ignore
/// let bytes: Vec<u8> = encode(input);
/// let typed: v8::Local<v8::Uint8Array> = bytes.to_js_typed_array(lock);
/// ```
pub trait ToJsTypedArray {
    /// The typed array type of the element type, such as [`v8::Uint8Array`] for `u8`.
    type TypedArray;

    /// Creates the typed array holding the elements of `self`.
    #[must_use]
    fn to_js_typed_array<'s>(&self, lock: &mut Lock<'s>) -> v8::Local<'s, Self::TypedArray>;
}

macro_rules! impl_to_js_typed_array {
    ($($elem:ty => $marker:ident, $new_fn:ident;)*) => {
        $(
            impl ToJsTypedArray for [$elem] {
                type TypedArray = v8::$marker;

                fn to_js_typed_array<'s>(&self, lock: &mut Lock<'s>) -> v8::Local<'s, v8::$marker> {
                    let isolate = lock.isolate();
                    // SAFETY: Lock guarantees the isolate is locked and a HandleScope is active.
                    // The C++ side reads `self.len()` elements from `self.as_ptr()` before it
                    // returns, keeps no pointer into `self`, and returns a handle to a new
                    // typed array of the marker's type.
                    unsafe {
                        v8::Local::from_ffi(v8::ffi::$new_fn(
                            isolate.as_ffi(),
                            self.as_ptr(),
                            self.len(),
                        ))
                    }
                }
            }
        )*
    };
}

impl_to_js_typed_array! {
    u8 => Uint8Array, local_new_uint8_array;
    u16 => Uint16Array, local_new_uint16_array;
    u32 => Uint32Array, local_new_uint32_array;
    i8 => Int8Array, local_new_int8_array;
    i16 => Int16Array, local_new_int16_array;
    i32 => Int32Array, local_new_int32_array;
    f32 => Float32Array, local_new_float32_array;
    f64 => Float64Array, local_new_float64_array;
    i64 => BigInt64Array, local_new_bigint64_array;
    u64 => BigUint64Array, local_new_biguint64_array;
}

/// Converts a byte slice to a JavaScript `ArrayBuffer` holding its bytes.
///
/// This is the shape C++ JSG gives a returned `kj::Array<kj::byte>`. For a `Uint8Array`, use
/// [`to_js_typed_array`](ToJsTypedArray::to_js_typed_array). Only `[u8]` implements this, as
/// only `kj::byte` does in C++ JSG: the bytes of wider numbers are in the platform's byte order,
/// and a caller that wants them can take the `buffer` of their typed array.
///
/// The slice is not borrowed afterwards. A `Vec<u8>` or an array `[u8; N]` calls this through
/// auto-deref:
///
/// ```ignore
/// let bytes: Vec<u8> = encode(input);
/// let buffer: v8::Local<v8::ArrayBuffer> = bytes.to_js_array_buffer(lock);
/// ```
pub trait ToJsArrayBuffer {
    /// Creates an `ArrayBuffer` holding the bytes of `self`.
    #[must_use]
    fn to_js_array_buffer<'s>(&self, lock: &mut Lock<'s>) -> v8::Local<'s, v8::ArrayBuffer>;
}

impl ToJsArrayBuffer for [u8] {
    fn to_js_array_buffer<'s>(&self, lock: &mut Lock<'s>) -> v8::Local<'s, v8::ArrayBuffer> {
        v8::ArrayBuffer::new(lock, self)
    }
}

/// The bytes of an `ArrayBuffer`, an `ArrayBufferView` or a `SharedArrayBuffer`, as a method
/// parameter or struct field of C++ type `kj::Array<kj::byte>` takes them: anything else is a
/// type error naming `ArrayBuffer or ArrayBufferView`, as in C++. Unlike C++, which views the
/// buffer's memory, this copies the bytes, so that what Rust holds can't change or be detached
/// while it holds it.
///
/// RTTI describes it as C++ describes `kj::Array<kj::byte>`, so TypeScript declares it as
/// `ArrayBuffer | ArrayBufferView` for a parameter.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Bytes(pub Vec<u8>);

crate::jsg_traced_unchecked!(Bytes);

impl Bytes {
    const TYPE_NAME: &'static str = "ArrayBuffer or ArrayBufferView";
}

impl std::ops::Deref for Bytes {
    type Target = [u8];

    fn deref(&self) -> &[u8] {
        &self.0
    }
}

impl crate::TypeInfo for Bytes {
    fn describe(_cx: &mut crate::rtti::Context) -> crate::rtti::Type {
        crate::rtti::Type::Bytes
    }
}

impl<'s> crate::FromJS<'s> for Bytes {
    type ResultType = Self;

    // Reading a buffer's bytes runs no script.
    const OBJECT_CONVERSION_MAY_RUN_SCRIPT: bool = false;

    fn type_name() -> String {
        Self::TYPE_NAME.to_owned()
    }

    fn from_js(lock: &mut Lock<'s>, value: v8::Local<'s, v8::Value>) -> crate::Result<Self> {
        Self::try_from_js(lock, value)?.ok_or_else(|| {
            crate::Error::new_type_error(format!(
                "expected {}, got {}",
                Self::TYPE_NAME,
                value.type_of(lock)
            ))
        })
    }

    #[expect(
        unsafe_code,
        reason = "copies the bytes of a SharedArrayBuffer from V8"
    )]
    fn try_from_js(
        _lock: &mut Lock<'s>,
        value: v8::Local<'s, v8::Value>,
    ) -> crate::Result<Option<Self>> {
        // In C++'s order: a view, then a buffer.
        if let Ok(view) = v8::Local::<v8::ArrayBufferView>::try_from(value) {
            return Ok(Some(Self(view.to_vec())));
        }
        if let Ok(buffer) = v8::Local::<v8::ArrayBuffer>::try_from(value) {
            return Ok(Some(Self(buffer.to_vec())));
        }
        if value.is_shared_array_buffer() {
            let mut length = 0;
            // SAFETY: `value` is a `SharedArrayBuffer` valid in the current HandleScope.
            let data =
                unsafe { v8::ffi::local_shared_array_buffer_contents(value.raw(), &mut length) };
            if length == 0 {
                return Ok(Some(Self(Vec::new())));
            }
            // SAFETY: V8 returned the address of the buffer's `length` bytes, which stay in place
            // while no JavaScript runs, as none does before the copy completes. The isolate's
            // JavaScript can't touch them meanwhile, and nothing in workerd shares a
            // SharedArrayBuffer with another thread.
            let bytes = unsafe { std::slice::from_raw_parts(data as *const u8, length) };
            return Ok(Some(Self(bytes.to_vec())));
        }
        Ok(None)
    }
}
