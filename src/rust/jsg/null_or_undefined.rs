// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use crate::Nullable;

/// A value of type `T`, `null`, or `undefined`, preserving all three states.
///
/// Prefer this to `Option<Nullable<T>>` for optional nullable values. Unlike
/// [`Nullable<T>`], input conversion keeps `undefined` distinct from `null`.
/// Method arguments may be omitted, producing [`NullOrUndefined::Undefined`].
/// Non-nullish values use `T`'s normal conversion rules; use [`crate::Lenient`]
/// when shallow type mismatches should also become `undefined`.
///
/// The default is [`NullOrUndefined::Undefined`], without requiring `T: Default`.
/// Conversions to and from `Option<Nullable<T>>` preserve all three states.
/// [`Nullable<T>`] also converts into this type, preserving `Some` and `Null`.
///
/// In a `#[jsg_struct]`, missing fields become `Undefined`. On output, `Null`
/// produces a `null` property. `Undefined` is omitted unless `fast_jsg_struct`
/// is enabled, in which case it produces an own `undefined` property.
///
/// ```ignore
/// #[jsg_method]
/// pub fn process(&self, value: jsg::NullOrUndefined<String>) {
///     match value {
///         jsg::NullOrUndefined::Some(s) => println!("Got value: {s}"),
///         jsg::NullOrUndefined::Null => println!("Got null"),
///         jsg::NullOrUndefined::Undefined => println!("Got undefined"),
///     }
/// }
/// ```
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Hash)]
pub enum NullOrUndefined<T> {
    /// A value of type `T`.
    Some(T),
    /// JavaScript `null`.
    Null,
    /// JavaScript `undefined`, including an omitted argument or missing field.
    #[default]
    Undefined,
}

impl<T> NullOrUndefined<T> {
    /// Returns `true` if this contains a value.
    pub fn is_some(&self) -> bool {
        matches!(self, Self::Some(_))
    }

    /// Returns `true` if this is `Null`.
    pub fn is_null(&self) -> bool {
        matches!(self, Self::Null)
    }

    /// Returns `true` if this is `Undefined`.
    pub fn is_undefined(&self) -> bool {
        matches!(self, Self::Undefined)
    }

    /// Borrows the contained value, preserving `Null` and `Undefined`.
    pub fn as_ref(&self) -> NullOrUndefined<&T> {
        match self {
            Self::Some(v) => NullOrUndefined::Some(v),
            Self::Null => NullOrUndefined::Null,
            Self::Undefined => NullOrUndefined::Undefined,
        }
    }

    /// Mutably borrows the contained value, preserving `Null` and `Undefined`.
    pub fn as_mut(&mut self) -> NullOrUndefined<&mut T> {
        match self {
            Self::Some(v) => NullOrUndefined::Some(v),
            Self::Null => NullOrUndefined::Null,
            Self::Undefined => NullOrUndefined::Undefined,
        }
    }

    /// Transforms the contained value, preserving `Null` and `Undefined`.
    pub fn map<U>(self, f: impl FnOnce(T) -> U) -> NullOrUndefined<U> {
        match self {
            Self::Some(v) => NullOrUndefined::Some(f(v)),
            Self::Null => NullOrUndefined::Null,
            Self::Undefined => NullOrUndefined::Undefined,
        }
    }
}

impl<T> From<Nullable<T>> for NullOrUndefined<T> {
    fn from(value: Nullable<T>) -> Self {
        match value {
            Nullable::Some(v) => Self::Some(v),
            Nullable::Null => Self::Null,
        }
    }
}

impl<T> From<Option<Nullable<T>>> for NullOrUndefined<T> {
    fn from(value: Option<Nullable<T>>) -> Self {
        value.map_or(Self::Undefined, Self::from)
    }
}

impl<T> From<NullOrUndefined<T>> for Option<Nullable<T>> {
    fn from(value: NullOrUndefined<T>) -> Self {
        match value {
            NullOrUndefined::Some(v) => Some(Nullable::Some(v)),
            NullOrUndefined::Null => Some(Nullable::Null),
            NullOrUndefined::Undefined => None,
        }
    }
}

#[cfg(test)]
#[path = "null_or_undefined-test.rs"]
mod tests;
