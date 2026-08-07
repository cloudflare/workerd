// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use crate::Nullable;
use crate::v8::Global;
use crate::v8::Value;

/// [`Lenient<T>`] is an alternative to [`Nullable<T>`].
///
/// [`Lenient`] is to be used where [`FromJS`] conversion coerces to undefined when `T::from_js`
/// fails with a type error. It is intentionally left minimal in order to keep [`Nullable<T>`]
/// as the canonical optional type for JavaScript ffi optional type, and allows easy conversion
/// to and from [`Nullable<T>`] for crossing the ffi boundary
pub enum Lenient<T> {
    Some(T),
    Unconvertable(Global<Value>),
    Null,
    Undefined,
}

impl<T> Lenient<T> {
    /// Returns `true` if the lenient contains a value.
    pub fn is_some(&self) -> bool {
        matches!(self, Self::Some(_))
    }

    /// Returns `true` if the lenient is `Null`.
    pub fn is_null(&self) -> bool {
        matches!(self, Self::Null)
    }

    /// # Warning
    /// This functions considers [`Lenient::Unconvertable`] to be undefined
    ///
    /// Returns `true` if the lenient is `Undefined` OR failed to
    /// convert into `T` across the ffi boundary
    pub fn is_undefined(&self) -> bool {
        matches!(self, Self::Undefined | Self::Unconvertable(_))
    }

    /// Returns `true` if the lenient is `Undefined`
    pub fn is_undefined_strict(&self) -> bool {
        matches!(self, Self::Undefined)
    }

    /// Returns `true` if the lenient failed to convert into `T` across
    /// the ffi boundary
    pub fn is_unconvertable(&self) -> bool {
        matches!(self, Self::Unconvertable(_))
    }
}

impl<T> Lenient<T> {
    pub fn to_parts(self) -> (Nullable<T>, Option<Global<Value>>) {
        match self {
            Self::Some(v) => (Nullable::Some(v), None),
            Self::Unconvertable(local) => (Nullable::Undefined, Some(local)),
            Self::Null => (Nullable::Null, None),
            Self::Undefined => (Nullable::Undefined, None),
        }
    }
}

impl<T> From<Lenient<T>> for Nullable<T> {
    fn from(value: Lenient<T>) -> Self {
        match value {
            Lenient::Some(v) => Self::Some(v),
            Lenient::Null => Self::Null,
            Lenient::Unconvertable(_) | Lenient::Undefined => Self::Undefined,
        }
    }
}

impl<T> From<Nullable<T>> for Lenient<T> {
    fn from(value: Nullable<T>) -> Self {
        match value {
            Nullable::Some(v) => Self::Some(v),
            Nullable::Null => Self::Null,
            Nullable::Undefined => Self::Undefined,
        }
    }
}
