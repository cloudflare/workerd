// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use crate::Nullable;
use crate::v8::TracedGlobal;
use crate::v8::Value;

/// [`Lenient<T>`] is a type for lenient conversion from JavaScript, akin to the
/// C++ type `LenientOptional`.
///
/// It is an alternative to `Option<Nullable<T>>` that treats type mismatches as `undefined`
/// instead of erroring. When `T::try_from_js` reports a shallow conversion mismatch, the
/// original JS value is preserved as [`Lenient::Unconvertable`] rather than propagating the error.
/// [`Lenient::Unconvertable`] is considered `undefined` when passed to JavaScript or when
/// converted into an `Option<Nullable<T>>`.
///
/// [`Lenient`] is intentionally minimal — it is designed for crossing the Rust<->C++ FFI
/// boundary, with the possibility of inspecting the JS value. For general-purpose Rust,
/// code, prefer converting to `Option<Nullable<T>>`.
pub enum Lenient<T> {
    Some(T),
    Unconvertable(TracedGlobal<Value>),
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

    /// Converts `self` into an `Option<Nullable<T>>`, where `None` is
    /// `undefined`, while also providing the value passed to a failed
    /// conversion as an optional argument in the case that it exists.
    pub fn to_parts(self) -> (Option<Nullable<T>>, Option<TracedGlobal<Value>>) {
        match self {
            Self::Some(v) => (Some(Nullable::Some(v)), None),
            Self::Unconvertable(global) => (None, Some(global)),
            Self::Null => (Some(Nullable::Null), None),
            Self::Undefined => (None, None),
        }
    }
}

impl<T> From<Lenient<T>> for Option<Nullable<T>> {
    fn from(value: Lenient<T>) -> Self {
        match value {
            Lenient::Some(v) => Some(Nullable::Some(v)),
            Lenient::Null => Some(Nullable::Null),
            Lenient::Unconvertable(_) | Lenient::Undefined => None,
        }
    }
}

impl<T> From<Nullable<T>> for Lenient<T> {
    fn from(value: Nullable<T>) -> Self {
        match value {
            Nullable::Some(v) => Self::Some(v),
            Nullable::Null => Self::Null,
        }
    }
}

impl<T> From<Option<Nullable<T>>> for Lenient<T> {
    fn from(value: Option<Nullable<T>>) -> Self {
        value.map_or(Self::Undefined, Self::from)
    }
}
