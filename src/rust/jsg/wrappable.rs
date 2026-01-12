// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#![allow(clippy::allow_attributes)]

//! Traits for converting between Rust and JavaScript values.
//!
//! # Supported Types
//!
//! ## Primitive Types
//!
//! | Rust Type | JavaScript Type |
//! |-----------|-----------------|
//! | `()` | `undefined` |
//! | `String` | `string` |
//! | `&str` | `string` |
//! | `bool` | `boolean` |
//! | `f64` | `number` |
//! | `Option<T>` | `T` or `undefined` |
//! | `Nullable<T>` | `T`, `null`, or `undefined` |
//! | `Result<T, E>` | `T` or throws |
//! | `NonCoercible<T>` | `T` (strict type checking) |
//! | `T: Struct` | `object` |
//! | `Vec<T>` | `Array<T>` |
//! | `&[T]` | `Array<T>` (parameter only) |
//!
//! ## `TypedArray` Types
//!
//! These specialized `Vec` types map directly to JavaScript `TypedArray`s for
//! efficient binary data transfer:
//!
//! | Rust Type | JavaScript Type |
//! |-----------|-----------------|
//! | `Vec<u8>` | `Uint8Array` |
//! | `Vec<u16>` | `Uint16Array` |
//! | `Vec<u32>` | `Uint32Array` |
//! | `Vec<i8>` | `Int8Array` |
//! | `Vec<i16>` | `Int16Array` |
//! | `Vec<i32>` | `Int32Array` |
//!
//! ## Integer Parameter Types
//!
//! Integer types (`u8`, `u16`, `u32`, `i8`, `i16`, `i32`) can be used as method
//! parameters. JavaScript numbers are converted via truncation:
//!
//! - Values are truncated toward zero (e.g., `3.7` → `3`, `-2.9` → `-2`)
//! - Out-of-range values wrap (e.g., `256.0` → `0` for `u8`)
//! - `NaN` becomes `0`
//!
//! For strict validation, wrap parameters in `NonCoercible<T>` or validate manually.

use crate::Error;
use crate::Lock;
use crate::NonCoercible;
use crate::Nullable;
use crate::Type;
use crate::v8;
use crate::v8::ToLocalValue;

// =============================================================================
// ToJS trait (Rust → JavaScript)
// =============================================================================

/// Trait for converting Rust values to JavaScript.
///
/// Provides Rust → JavaScript conversion.
pub trait ToJS: Sized {
    /// Converts this Rust value into a JavaScript value.
    fn to_js<'a, 'b>(self, lock: &'a mut Lock) -> v8::Local<'b, v8::Value>
    where
        'b: 'a;
}

// =============================================================================
// FromJS trait (JavaScript → Rust)
// =============================================================================

/// Trait for converting JavaScript values to Rust.
///
/// Provides JS → Rust conversion. The `try_unwrap` method is used by macros
/// to unwrap function parameters with proper error handling.
pub trait FromJS: Sized {
    type ResultType;

    /// Converts a JavaScript value into this Rust type.
    fn from_js(lock: &mut Lock, value: v8::Local<v8::Value>) -> Result<Self::ResultType, Error>;

    /// Tries to convert only if the JavaScript type matches exactly.
    /// Returns `None` if the type doesn't match, `Some(result)` if conversion was attempted.
    /// Used by `#[jsg_oneof]` macro to try each variant without coercion.
    fn try_from_js_exact(
        lock: &mut Lock,
        value: &v8::Local<v8::Value>,
    ) -> Option<Result<Self::ResultType, Error>>
    where
        Self: Type,
    {
        if Self::is_exact(value) {
            Some(Self::from_js(lock, value.clone()))
        } else {
            None
        }
    }
}

// =============================================================================
// Primitive type implementations
// =============================================================================

/// Implements `Type`, `ToJS`, and `FromJS` for primitive types.
macro_rules! impl_primitive {
    { $type:ty, $class_name:literal, $is_exact:ident, $unwrap_fn:ident } => {
        impl Type for $type {
            fn class_name() -> &'static str {
                $class_name
            }

            fn is_exact(value: &v8::Local<v8::Value>) -> bool {
                value.$is_exact()
            }
        }

        impl ToJS for $type {
            fn to_js<'a, 'b>(self, lock: &'a mut Lock) -> v8::Local<'b, v8::Value>
            where
                'b: 'a,
            {
                self.to_local(lock)
            }
        }

        impl FromJS for $type {
            type ResultType = Self;

            fn from_js(lock: &mut Lock, value: v8::Local<v8::Value>) -> Result<Self::ResultType, Error> {
                Ok(unsafe { v8::ffi::$unwrap_fn(lock.isolate().as_ffi(), value.into_ffi()) })
            }
        }
    };
}

impl_primitive!(String, "string", is_string, unwrap_string);
impl_primitive!(bool, "boolean", is_boolean, unwrap_boolean);
impl_primitive!(f64, "number", is_number, unwrap_number);

// Special implementation for &str - allows functions to accept &str parameters
// by converting JavaScript strings to owned Strings, then borrowing.
// The macro handles passing &arg instead of arg for reference types.zs
impl Type for &str {
    fn class_name() -> &'static str {
        "string"
    }

    fn is_exact(value: &v8::Local<v8::Value>) -> bool {
        value.is_string()
    }
}

impl FromJS for &str {
    type ResultType = String;

    fn from_js(lock: &mut Lock, value: v8::Local<v8::Value>) -> Result<Self::ResultType, Error> {
        Ok(unsafe { v8::ffi::unwrap_string(lock.isolate().as_ffi(), value.into_ffi()) })
    }
}

impl<T: FromJS<ResultType = T>> FromJS for &T {
    type ResultType = T;

    fn from_js(lock: &mut Lock, value: v8::Local<v8::Value>) -> Result<Self::ResultType, Error> {
        T::from_js(lock, value)
    }
}

// Slice type - allows functions to accept &[T] parameters.
// JavaScript arrays are converted to Vec<T>, then borrowed as &[T] by the macro.
impl<T: Type> Type for &[T] {
    fn class_name() -> &'static str {
        "Array"
    }

    fn is_exact(value: &v8::Local<v8::Value>) -> bool {
        value.is_array()
    }
}

impl<T: Type + FromJS<ResultType = T>> FromJS for &[T] {
    type ResultType = Vec<T>;

    fn from_js(lock: &mut Lock, value: v8::Local<v8::Value>) -> Result<Self::ResultType, Error> {
        Vec::<T>::from_js(lock, value)
    }
}

// Integer types - JavaScript numbers are IEEE 754 doubles (f64)
//
// Conversion behavior:
// - Values are truncated toward zero (e.g., 3.7 → 3, -2.9 → -2)
// - Values outside the target type's range wrap around (e.g., 256.0 → 0 for u8)
// - NaN becomes 0
// - Infinity wraps to 0 for unsigned types, or type MIN/MAX for signed types
//
// This matches JavaScript's behavior for TypedArray element assignment.
// For strict validation, use `NonCoercible<T>` or validate in your method.
macro_rules! impl_integer_from_js {
    ($($type:ty),*) => {
        $(
            impl FromJS for $type {
                type ResultType = Self;

                #[allow(clippy::cast_possible_truncation, clippy::cast_sign_loss)]
                fn from_js(lock: &mut Lock, value: v8::Local<v8::Value>) -> Result<Self::ResultType, Error> {
                    let num = unsafe { v8::ffi::unwrap_number(lock.isolate().as_ffi(), value.into_ffi()) };
                    Ok(num as $type)
                }
            }
        )*
    };
}

impl_integer_from_js!(u8, u16, u32, i8, i16, i32);

// =============================================================================
// Wrapper type implementations
// =============================================================================

impl ToJS for () {
    fn to_js<'a, 'b>(self, lock: &'a mut Lock) -> v8::Local<'b, v8::Value>
    where
        'b: 'a,
    {
        v8::Local::<v8::Value>::undefined(lock)
    }
}

impl<T: ToJS> ToJS for Option<T> {
    fn to_js<'a, 'b>(self, lock: &'a mut Lock) -> v8::Local<'b, v8::Value>
    where
        'b: 'a,
    {
        match self {
            Some(value) => value.to_js(lock),
            None => v8::Local::<v8::Value>::undefined(lock),
        }
    }
}

impl<T: Type + ToJS> ToJS for NonCoercible<T> {
    fn to_js<'a, 'b>(self, lock: &'a mut Lock) -> v8::Local<'b, v8::Value>
    where
        'b: 'a,
    {
        self.into_inner().to_js(lock)
    }
}

impl<T: ToJS> ToJS for Nullable<T> {
    fn to_js<'a, 'b>(self, lock: &'a mut Lock) -> v8::Local<'b, v8::Value>
    where
        'b: 'a,
    {
        match self {
            Self::Some(value) => value.to_js(lock),
            Self::Null => v8::Local::<v8::Value>::null(lock),
            Self::Undefined => v8::Local::<v8::Value>::undefined(lock),
        }
    }
}

impl<T: Type + FromJS> FromJS for Option<T> {
    type ResultType = Option<T::ResultType>;

    fn from_js(lock: &mut Lock, value: v8::Local<v8::Value>) -> Result<Self::ResultType, Error> {
        if value.is_null() {
            let msg = format!("Expected {} or undefined but got null", T::class_name());
            Err(Error::new_type_error(msg))
        } else if value.is_undefined() {
            Ok(None)
        } else {
            Ok(Some(T::from_js(lock, value)?))
        }
    }
}

impl<T: Type + FromJS> FromJS for NonCoercible<T> {
    type ResultType = NonCoercible<T::ResultType>;

    fn from_js(lock: &mut Lock, value: v8::Local<v8::Value>) -> Result<Self::ResultType, Error> {
        if !T::is_exact(&value) {
            let error_msg = format!(
                "Expected a {} value but got {}",
                T::class_name(),
                value.type_of()
            );
            return Err(Error::new_type_error(error_msg));
        }
        Ok(<Self::ResultType>::new(T::from_js(lock, value)?))
    }
}

impl<T: Type + FromJS> FromJS for Nullable<T> {
    type ResultType = Nullable<T::ResultType>;

    fn from_js(lock: &mut Lock, value: v8::Local<v8::Value>) -> Result<Self::ResultType, Error> {
        if value.is_null() {
            Ok(Nullable::Null)
        } else if value.is_undefined() {
            Ok(Nullable::Undefined)
        } else {
            Ok(Nullable::Some(T::from_js(lock, value)?))
        }
    }
}

// =============================================================================
// Array type implementations (Vec<T>)
// =============================================================================

impl<T: Type> Type for Vec<T> {
    fn class_name() -> &'static str {
        "Array"
    }

    fn is_exact(value: &v8::Local<v8::Value>) -> bool {
        value.is_array()
    }
}

impl<T: ToJS> ToJS for Vec<T> {
    fn to_js<'a, 'b>(self, lock: &'a mut Lock) -> v8::Local<'b, v8::Value>
    where
        'b: 'a,
    {
        let mut array = v8::Local::<v8::Array>::new(lock, self.len());
        for (i, item) in self.into_iter().enumerate() {
            array.set(i, item.to_js(lock));
        }
        array.into()
    }
}

impl<T: Type + FromJS<ResultType = T>> FromJS for Vec<T> {
    type ResultType = Self;

    fn from_js(lock: &mut Lock, value: v8::Local<v8::Value>) -> Result<Self::ResultType, Error> {
        if !value.is_array() {
            return Err(Error::new_type_error(format!(
                "Expected Array but got {}",
                value.type_of()
            )));
        }

        let globals = unsafe { value.as_array() }.iterate();
        let mut result = Self::with_capacity(globals.len());
        for global in globals {
            let local = global.as_local(lock);
            result.push(T::from_js(lock, local)?);
        }
        Ok(result)
    }
}

// =============================================================================
// TypedArray marker type implementations
// =============================================================================
//
// These implement `Type` for TypedArray marker structs (e.g., `v8::Uint8Array`).
// Marker types are used with `Local<'a, T>` handles for type-safe references.

macro_rules! impl_typed_array_type {
    ($marker:ident, $js_name:literal, $is_check:ident) => {
        impl Type for v8::$marker {
            fn class_name() -> &'static str {
                $js_name
            }

            fn is_exact(value: &v8::Local<v8::Value>) -> bool {
                value.$is_check()
            }
        }
    };
}

impl_typed_array_type!(Uint8Array, "Uint8Array", is_uint8_array);
impl_typed_array_type!(Uint16Array, "Uint16Array", is_uint16_array);
impl_typed_array_type!(Uint32Array, "Uint32Array", is_uint32_array);
impl_typed_array_type!(Int8Array, "Int8Array", is_int8_array);
impl_typed_array_type!(Int16Array, "Int16Array", is_int16_array);
impl_typed_array_type!(Int32Array, "Int32Array", is_int32_array);

// =============================================================================
// Vec<T> specialized implementations for TypedArrays
// =============================================================================
//
// These implementations map Rust `Vec<T>` types directly to JavaScript TypedArrays:
// - `Vec<u8>`  <-> `Uint8Array`
// - `Vec<u16>` <-> `Uint16Array`
// - `Vec<u32>` <-> `Uint32Array`
// - `Vec<i8>`  <-> `Int8Array`
// - `Vec<i16>` <-> `Int16Array`
// - `Vec<i32>` <-> `Int32Array`

macro_rules! impl_vec_typed_array {
    ($elem:ty, $js_name:literal, $is_check:ident, $new_fn:ident, $unwrap_fn:ident) => {
        impl Type for Vec<$elem> {
            fn class_name() -> &'static str {
                $js_name
            }

            fn is_exact(value: &v8::Local<v8::Value>) -> bool {
                value.$is_check()
            }
        }

        impl ToJS for Vec<$elem> {
            fn to_js<'a, 'b>(self, lock: &'a mut Lock) -> v8::Local<'b, v8::Value>
            where
                'b: 'a,
            {
                let isolate = lock.isolate();
                unsafe {
                    v8::Local::from_ffi(
                        isolate,
                        v8::ffi::$new_fn(isolate.as_ffi(), self.as_ptr(), self.len()),
                    )
                }
            }
        }

        impl FromJS for Vec<$elem> {
            type ResultType = Self;

            fn from_js(
                lock: &mut Lock,
                value: v8::Local<v8::Value>,
            ) -> Result<Self::ResultType, Error> {
                if !value.$is_check() {
                    return Err(Error::new_type_error(format!(
                        "Expected {} but got {}",
                        $js_name,
                        value.type_of()
                    )));
                }

                Ok(unsafe { v8::ffi::$unwrap_fn(lock.isolate().as_ffi(), value.into_ffi()) })
            }
        }
    };
}

impl_vec_typed_array!(
    u8,
    "Uint8Array",
    is_uint8_array,
    local_new_uint8_array,
    unwrap_uint8_array
);
impl_vec_typed_array!(
    u16,
    "Uint16Array",
    is_uint16_array,
    local_new_uint16_array,
    unwrap_uint16_array
);
impl_vec_typed_array!(
    u32,
    "Uint32Array",
    is_uint32_array,
    local_new_uint32_array,
    unwrap_uint32_array
);
impl_vec_typed_array!(
    i8,
    "Int8Array",
    is_int8_array,
    local_new_int8_array,
    unwrap_int8_array
);
impl_vec_typed_array!(
    i16,
    "Int16Array",
    is_int16_array,
    local_new_int16_array,
    unwrap_int16_array
);
impl_vec_typed_array!(
    i32,
    "Int32Array",
    is_int32_array,
    local_new_int32_array,
    unwrap_int32_array
);
