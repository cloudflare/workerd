// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Traits for converting between Rust and JavaScript values.
//!
//! # Supported Types
//!
//! | Rust Type | JavaScript Type |
//! |-----------|-----------------|
//! | `()` | `undefined` |
//! | `String` | `string` |
//! | `bool` | `boolean` |
//! | `f64` | `number` |
//! | `Option<T>` | `T` or `undefined` |
//! | `Nullable<T>` | `T`, `null`, or `undefined` |
//! | `Result<T, E>` | `T` or throws |
//! | `NonCoercible<T>` | `T` (strict type checking) |
//! | `T: Struct` | `object` |

use std::fmt::Display;

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

impl<T: ToJS, E: Display> ToJS for Result<T, E> {
    fn to_js<'a, 'b>(self, lock: &'a mut Lock) -> v8::Local<'b, v8::Value>
    where
        'b: 'a,
    {
        match self {
            Ok(value) => value.to_js(lock),
            Err(err) => {
                // TODO(soon): Use Error trait to dynamically call proper method to throw the error.
                let description = err.to_string();
                unsafe { v8::ffi::isolate_throw_error(lock.isolate().as_ffi(), &description) };
                v8::Local::<v8::Value>::undefined(lock)
            }
        }
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
            let error_msg = format!("Expected {} or undefined but got null", T::class_name());
            // TODO(soon): Throw errors in the correct place. This function should return
            // a result and the caller should handle the error.
            let err = Error::new("TypeError", error_msg);
            unsafe {
                v8::ffi::isolate_throw_exception(
                    lock.isolate().as_ffi(),
                    err.to_local(lock.isolate()).into_ffi(),
                );
            }
            Err(err)
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
            // TODO(soon): Throw errors in the correct place. This function should return
            // a result and the caller should handle the error.
            let err = Error::new("TypeError", error_msg);
            unsafe {
                v8::ffi::isolate_throw_exception(
                    lock.isolate().as_ffi(),
                    err.to_local(lock.isolate()).into_ffi(),
                );
            }
            return Err(err);
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
