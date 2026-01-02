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
//! | `Option<T>` | `T` or `null`/`undefined` |
//! | `Result<T, E>` | `T` or throws |
//! | `NonCoercible<T>` | `T` (strict type checking) |
//! | `T: Struct` | `object` |

use std::fmt::Display;

use crate::Lock;
use crate::NonCoercible;
use crate::Type;
use crate::v8;
use crate::v8::ToLocalValue;

// =============================================================================
// Wrappable trait (Rust → JavaScript)
// =============================================================================

/// Trait for converting Rust values to JavaScript.
///
/// Provides Rust → JavaScript conversion.
pub trait Wrappable: Sized {
    /// Converts this Rust value into a JavaScript value.
    fn wrap<'a, 'b>(self, lock: &'a mut Lock) -> v8::Local<'b, v8::Value>
    where
        'b: 'a;
}

// =============================================================================
// Unwrappable trait (JavaScript → Rust)
// =============================================================================

/// Trait for converting JavaScript values to Rust.
///
/// Provides JS → Rust conversion. The `try_unwrap` method is used by macros
/// to unwrap function parameters with proper error handling.
pub trait Unwrappable: Sized {
    /// Converts a JavaScript value into this Rust type.
    fn unwrap(isolate: v8::IsolatePtr, value: v8::Local<v8::Value>) -> Self;

    /// Attempts to unwrap a JavaScript value, returning None and throwing a JS error on failure.
    /// Most types simply delegate to `unwrap`, but `NonCoercible<T>` validates the type first.
    fn try_unwrap(lock: &mut Lock, value: v8::Local<v8::Value>) -> Option<Self> {
        Some(Self::unwrap(lock.isolate(), value))
    }
}

// =============================================================================
// Primitive type implementations
// =============================================================================

/// Implements `Type`, `Wrappable`, and `Unwrappable` for primitive types.
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

        impl Wrappable for $type {
            fn wrap<'a, 'b>(self, lock: &'a mut Lock) -> v8::Local<'b, v8::Value>
            where
                'b: 'a,
            {
                self.to_local(lock)
            }
        }

        impl Unwrappable for $type {
            fn unwrap(isolate: v8::IsolatePtr, value: v8::Local<v8::Value>) -> Self {
                unsafe { v8::ffi::$unwrap_fn(isolate.as_ffi(), value.into_ffi()) }
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

impl Wrappable for () {
    fn wrap<'a, 'b>(self, lock: &'a mut Lock) -> v8::Local<'b, v8::Value>
    where
        'b: 'a,
    {
        v8::Local::<v8::Value>::undefined(lock)
    }
}

impl<T: Wrappable, E: Display> Wrappable for Result<T, E> {
    fn wrap<'a, 'b>(self, lock: &'a mut Lock) -> v8::Local<'b, v8::Value>
    where
        'b: 'a,
    {
        match self {
            Ok(value) => value.wrap(lock),
            Err(err) => {
                // TODO(soon): Use jsg::Error trait to dynamically call proper method to throw the error.
                let description = err.to_string();
                unsafe { v8::ffi::isolate_throw_error(lock.isolate().as_ffi(), &description) };
                v8::Local::<v8::Value>::undefined(lock)
            }
        }
    }
}

impl<T: Wrappable> Wrappable for Option<T> {
    fn wrap<'a, 'b>(self, lock: &'a mut Lock) -> v8::Local<'b, v8::Value>
    where
        'b: 'a,
    {
        match self {
            Some(value) => value.wrap(lock),
            None => v8::Local::<v8::Value>::null(lock),
        }
    }
}

impl<T: Type + Wrappable> Wrappable for NonCoercible<T> {
    fn wrap<'a, 'b>(self, lock: &'a mut Lock) -> v8::Local<'b, v8::Value>
    where
        'b: 'a,
    {
        self.into_inner().wrap(lock)
    }
}

impl<T: Type + Unwrappable> Unwrappable for Option<T> {
    fn unwrap(isolate: v8::IsolatePtr, value: v8::Local<v8::Value>) -> Self {
        if value.is_null() {
            None
        } else if value.is_undefined() {
            let error_msg = format!(
                "Expected a null or {} value but got undefined",
                T::class_name()
            );
            unsafe { v8::ffi::isolate_throw_error(isolate.as_ffi(), &error_msg) };
            None
        } else {
            Some(T::unwrap(isolate, value))
        }
    }
}

impl<T: Type + Unwrappable> Unwrappable for NonCoercible<T> {
    fn unwrap(isolate: v8::IsolatePtr, value: v8::Local<v8::Value>) -> Self {
        Self::new(T::unwrap(isolate, value))
    }

    fn try_unwrap(lock: &mut Lock, value: v8::Local<v8::Value>) -> Option<Self> {
        if !T::is_exact(&value) {
            let error_msg = format!(
                "Expected a {} value but got {}",
                T::class_name(),
                value.type_of()
            );
            unsafe { v8::ffi::isolate_throw_error(lock.isolate().as_ffi(), &error_msg) };
            return None;
        }
        Some(Self::new(T::unwrap(lock.isolate(), value)))
    }
}
