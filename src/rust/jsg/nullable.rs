// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use crate::Error;
use crate::Lenient;
use crate::Traced;

/// A wrapper type that accepts `null`, `undefined`, or a value of type `T`.
///
/// `Nullable<T>` is similar to `Option<T>` but also accepts `undefined` as a null-ish value.
/// This is useful for JavaScript APIs where both `null` and `undefined` represent
/// the absence of a value.
///
/// # Behavior
///
/// - `null` → `Nullable::Null`
/// - `undefined` → `Nullable::Undefined`
/// - `T` → `Nullable::Some(T)`
///
/// # Example
///
/// ```ignore
/// use jsg::Nullable;
///
/// #[jsg_method]
/// pub fn process(&self, value: Nullable<String>) -> Result<(), Error> {
///     match value {
///         Nullable::Some(s) => println!("Got value: {}", s),
///         Nullable::Null => println!("Got null"),
///         Nullable::Undefined => println!("Got undefined"),
///     }
///     Ok(())
/// }
/// ```
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub enum Nullable<T> {
    Some(T),
    Null,
    Undefined,
}

impl<T> Nullable<T> {
    /// Returns `true` if the nullable contains a value.
    pub fn is_some(&self) -> bool {
        matches!(self, Self::Some(_))
    }

    /// Returns `true` if the nullable is `Null`.
    pub fn is_null(&self) -> bool {
        matches!(self, Self::Null)
    }

    /// Returns `true` if the nullable is `Undefined`.
    pub fn is_undefined(&self) -> bool {
        matches!(self, Self::Undefined)
    }

    /// Returns `true` if the nullable is `Null` or `Undefined`.
    pub fn is_null_or_undefined(&self) -> bool {
        !self.is_some()
    }

    /// Returns a reference to the contained value in `Some` if `Some`, and returns
    /// the respective empty value if empty.
    pub fn as_ref(&self) -> Nullable<&T> {
        match self {
            Self::Some(v) => Nullable::Some(v),
            Self::Null => Nullable::Null,
            Self::Undefined => Nullable::Undefined,
        }
    }

    /// Returns an exclusive reference to the contained value in `Some` if `Some`, and
    /// returns the respective empty value if empty.
    pub fn as_mut(&mut self) -> Nullable<&mut T> {
        match self {
            Self::Some(v) => Nullable::Some(v),
            Self::Null => Nullable::Null,
            Self::Undefined => Nullable::Undefined,
        }
    }

    /// Returns the contained [`Some`] value, consuming `self`.
    ///
    /// This function panics on `!self.is_some()`, making it unsuitable
    /// for general use. Outside of testing, prefer pattern matching, or a non-panicking
    /// variant.
    ///
    /// # Panics
    ///
    /// Panics if the `self` value is [`Nullable::Null`] or [`Nullable::Undefined`]
    ///
    /// [`Some`]: Nullable::Some
    #[track_caller]
    pub fn unwrap(self) -> T {
        match self {
            Self::Some(v) => v,
            Self::Null => panic!("called `Nullable::unwrap()` on a `Null` value"),
            Self::Undefined => panic!("called `Nullable::unwrap()` on an `Undefined` value"),
        }
    }

    /// Returns the contained [`Some`] value or the provided default.
    ///
    /// Arguments passed to `unwrap_or` are eagerly evaluated; if you are passing the result of
    /// a function call, it is recommended to use [`unwrap_or_else`], which is lazily evaluated.
    ///
    /// [`Some`]: Nullable::Some
    /// [`unwrap_or_else`]: Nullable::unwrap_or_else
    pub fn unwrap_or(self, default: T) -> T {
        match self {
            Self::Some(v) => v,
            Self::Null | Self::Undefined => default,
        }
    }

    /// Returns the contained [`Some`] value or computes it from the closure.
    ///
    /// [`Some`]: Nullable::Some
    pub fn unwrap_or_else<F>(self, f: F) -> T
    where
        F: FnOnce() -> T,
    {
        match self {
            Self::Some(v) => v,
            Self::Null | Self::Undefined => f(),
        }
    }

    /// Maps a [`Nullable<T>`] to a [`Nullable<U>`] by applying the passed function if [`Some`], or
    /// the respective empty type otherwise.
    ///
    /// [`Some`]: Nullable::Some
    pub fn map<U, F>(self, f: F) -> Nullable<U>
    where
        F: FnOnce(T) -> U,
    {
        match self {
            Self::Some(t) => Nullable::Some(f(t)),
            Self::Null => Nullable::Null,
            Self::Undefined => Nullable::Undefined,
        }
    }
}

impl<T> From<Option<T>> for Nullable<T> {
    fn from(opt: Option<T>) -> Self {
        match opt {
            Some(v) => Self::Some(v),
            None => Self::Null,
        }
    }
}

impl<T> From<Nullable<T>> for Option<T> {
    fn from(nullable: Nullable<T>) -> Self {
        match nullable {
            Nullable::Some(v) => Some(v),
            Nullable::Null | Nullable::Undefined => None,
        }
    }
}

/// A version wrapper around [`Nullable<T>`] that ensures it cannot be [`Nullable::Null`].
/// Compatiable with [`Lenient<T>`] as well.
///
/// The internal value is private, and a [`NonNull`] can only be created
/// from `FromJS` and the the `TryFrom` implementations on `Nullable` and `Lenient`.
///
/// Attempts to pass `null` to a rust `NonNull` will result in a type mismatch error,
/// and passing `Null` to [`NonNull::try_from`] results in a `None`.
pub struct NonNull<T>(T);

impl<T> TryFrom<Nullable<T>> for NonNull<Nullable<T>> {
    type Error = Error;

    fn try_from(value: Nullable<T>) -> Result<Self, Self::Error> {
        match value {
            Nullable::Null => Err(Error::new_type_mismatch_error(
                "Expected T or undefined, got null",
            )),
            other => Ok(Self(other)),
        }
    }
}

impl<T> NonNull<Nullable<T>> {
    pub fn inner(self) -> Nullable<T> {
        // Sanity check, type system should guarantee this is impossible
        debug_assert!(!self.0.is_null());
        self.0
    }
}

impl<T> TryFrom<Lenient<T>> for NonNull<Lenient<T>> {
    type Error = Error;

    fn try_from(value: Lenient<T>) -> Result<Self, Self::Error> {
        match value {
            Lenient::Null => Err(Error::new_type_mismatch_error(
                "Expected T or undefined, got null",
            )),
            other => Ok(Self(other)),
        }
    }
}

impl<T> NonNull<Lenient<T>> {
    pub fn inner(self) -> Lenient<T> {
        // Sanity check, type system should guarantee this is impossible
        debug_assert!(!self.0.is_null());
        self.0
    }
}

// Implemented here because the internal field is private
impl<T: Traced> Traced for NonNull<T> {
    fn trace(&self, visitor: &mut crate::v8::GcVisitor) {
        self.0.trace(visitor);
    }
}
