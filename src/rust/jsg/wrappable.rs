// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Trait for converting between Rust and JavaScript values.
//!
//! # Supported Types
//!
//! | Rust Type | JavaScript Type |
//! |-----------|-----------------|
//! | `()` | `undefined` |
//! | `String` | `string` |
//! | `bool` | `boolean` |
//! | `f64` | `number` |
//! | `Option<T>` | `T` or `null` |
//! | `Result<T, E>` | `T` or throws |
//! | `NonCoercible<T>` | `T` |
//! | `T: Struct` | `object` |

use std::fmt::Display;

use crate::Lock;
use crate::NonCoercible;
use crate::Struct;
use crate::Type;
use crate::v8;
use crate::v8::ToLocalValue;

impl Type for String {
    type This = Self;

    fn class_name() -> &'static str {
        "string"
    }

    fn wrap<'a, 'b>(this: Self::This, lock: &'a mut Lock) -> v8::Local<'b, v8::Value>
    where
        'b: 'a,
    {
        this.to_local(lock)
    }

    fn is_exact(value: &v8::Local<v8::Value>) -> bool {
        value.is_string()
    }

    fn unwrap(isolate: v8::IsolatePtr, value: v8::Local<v8::Value>) -> Self {
        unsafe { v8::ffi::unwrap_string(isolate.as_ffi(), value.into_ffi()) }
    }
}

impl Type for bool {
    type This = Self;

    fn class_name() -> &'static str {
        "boolean"
    }

    fn wrap<'a, 'b>(this: Self::This, lock: &'a mut Lock) -> v8::Local<'b, v8::Value>
    where
        'b: 'a,
    {
        this.to_local(lock)
    }

    fn is_exact(value: &v8::Local<v8::Value>) -> bool {
        value.is_boolean()
    }

    fn unwrap(isolate: v8::IsolatePtr, value: v8::Local<v8::Value>) -> Self {
        unsafe { v8::ffi::unwrap_boolean(isolate.as_ffi(), value.into_ffi()) }
    }
}

impl Type for f64 {
    type This = Self;

    fn class_name() -> &'static str {
        "number"
    }

    fn wrap<'a, 'b>(this: Self::This, lock: &'a mut Lock) -> v8::Local<'b, v8::Value>
    where
        'b: 'a,
    {
        this.to_local(lock)
    }

    fn is_exact(value: &v8::Local<v8::Value>) -> bool {
        value.is_number()
    }

    fn unwrap(isolate: v8::IsolatePtr, value: v8::Local<v8::Value>) -> Self {
        unsafe { v8::ffi::unwrap_number(isolate.as_ffi(), value.into_ffi()) }
    }
}

/// Trait for converting between Rust and JavaScript values.
///
/// Provides bidirectional conversion: `wrap` converts Rust to JavaScript,
/// `unwrap` converts JavaScript to Rust. The `try_unwrap` method is used
/// by macros to unwrap parameters with proper error handling.
pub trait Wrappable: Sized {
    /// Converts this Rust value into a JavaScript value.
    fn wrap(self, lock: &mut Lock) -> v8::Local<'_, v8::Value>;

    /// Converts a JavaScript value into this Rust type.
    fn unwrap(isolate: v8::IsolatePtr, value: v8::Local<v8::Value>) -> Self;

    /// Attempts to unwrap a JavaScript value, returning None and throwing a JS error on failure.
    /// Most types simply delegate to `unwrap`, but `NonCoercible<T>` validates the type first.
    fn try_unwrap(lock: &mut Lock, value: v8::Local<v8::Value>) -> Option<Self> {
        Some(Self::unwrap(lock.isolate(), value))
    }

    /// Wraps this value and sets it as the function return value.
    fn wrap_return(self, lock: &mut Lock, args: &mut v8::FunctionCallbackInfo) {
        args.set_return_value(self.wrap(lock));
    }
}

impl Wrappable for () {
    fn wrap(self, lock: &mut Lock) -> v8::Local<'_, v8::Value> {
        v8::Local::<v8::Value>::undefined(lock)
    }

    fn unwrap(_isolate: v8::IsolatePtr, _value: v8::Local<v8::Value>) -> Self {}
}

impl<T: Wrappable, E: Display> Wrappable for Result<T, E> {
    fn wrap(self, lock: &mut Lock) -> v8::Local<'_, v8::Value> {
        match self {
            Ok(value) => value.wrap(lock),
            Err(_) => unreachable!("Cannot wrap Result::Err"),
        }
    }

    fn unwrap(_isolate: v8::IsolatePtr, _value: v8::Local<v8::Value>) -> Self {
        unreachable!("Cannot unwrap into Result")
    }

    fn wrap_return(self, lock: &mut Lock, args: &mut v8::FunctionCallbackInfo) {
        match self {
            Ok(value) => value.wrap_return(lock, args),
            Err(err) => {
                // TODO(soon): Use jsg::Error trait to dynamically call proper method to throw the error.
                let description = err.to_string();
                unsafe { v8::ffi::isolate_throw_error(lock.isolate().as_ffi(), &description) };
            }
        }
    }
}

impl<T: Wrappable> Wrappable for Option<T> {
    fn wrap(self, lock: &mut Lock) -> v8::Local<'_, v8::Value> {
        match self {
            Some(value) => value.wrap(lock),
            None => v8::Local::<v8::Value>::null(lock),
        }
    }

    fn unwrap(isolate: v8::IsolatePtr, value: v8::Local<v8::Value>) -> Self {
        if value.is_null_or_undefined() {
            None
        } else {
            Some(T::unwrap(isolate, value))
        }
    }
}

impl<T: Type<This = T>> Wrappable for NonCoercible<T> {
    fn wrap(self, lock: &mut Lock) -> v8::Local<'_, v8::Value> {
        T::wrap(self.value, lock)
    }

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

impl<T: Struct<This = T>> Wrappable for T {
    fn wrap(self, lock: &mut Lock) -> v8::Local<'_, v8::Value> {
        <T as Type>::wrap(self, lock)
    }

    fn unwrap(isolate: v8::IsolatePtr, value: v8::Local<v8::Value>) -> Self {
        <T as Type>::unwrap(isolate, value)
    }
}

/// Implements `Wrappable` for types that already implement `Type`.
macro_rules! impl_wrappable_for_type {
    ($($t:ty),*) => {
        $(
            impl Wrappable for $t {
                fn wrap(self, lock: &mut Lock) -> v8::Local<'_, v8::Value> {
                    <Self as Type>::wrap(self, lock)
                }

                fn unwrap(isolate: v8::IsolatePtr, value: v8::Local<v8::Value>) -> Self {
                    <Self as Type>::unwrap(isolate, value)
                }
            }
        )*
    };
}

// When adding new primitive types that implement `Type`, add them here.
impl_wrappable_for_type!(String, bool, f64);
