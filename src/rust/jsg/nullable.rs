// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::ops::Deref;
use std::ops::DerefMut;

use crate::Error;
use crate::Result;

/// A wrapper type that accepts `null`, `undefined`, or a value of type `T`.
///
/// `Nullable<T>` is similar to `Option<T>` but also accepts `undefined` as a null-ish value.
/// This is useful for writing flexible JavaScript APIs where we have full control over
/// behavior depending on the passed value, including easily throwing errors to make an API
/// conform to Web IDL nullable behavior.
///
/// # Behavior
///
/// - `null` → `Nullable::Null`
/// - `undefined` → `Nullable::Undefined`
/// - `T` → `Nullable::Some(T)`
///
/// # Examples
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
///
/// Error Handling:
///
///
/// ```ignore
/// use jsg::Nullable;
///
/// #[jsg_method]
/// pub fn process_some(&self, value: Nullable<Number>) -> Result<(), Error> {
///     let Nullable::Some(v) = value {
///         // Process...
///     } else {
///         return Err(Error::new_type_error("Must be some"))
///     }
/// }
/// ```
///
/// ```ignore
/// use jsg::Nullable;
///
/// #[jsg_method]
/// pub fn process_some_or_none(&self, value: Nullable<Number>) -> Result<(), Error> {
///     // Returns an Error
///     let value = value.not_null()?;
///     // Process...
/// }
/// ```
///
/// Returning:
///
/// ```ignore
/// use jsg::Nullable;
///
/// #[jsg_method]
/// pub fn return_nullable(&self) -> Nullable<Number> {
///     if self.condition {
///         Nullable::Some(self.number)
///     } else {
///         Nullable::Undefined
///     }
/// }
/// ```
///
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

    /// Returns `Ok(self)` if `!self.is_null()`. Otherwise returns `TypeError` if
    /// `self` is null.
    pub fn not_null(self) -> Result<Self> {
        if self.is_null() {
            Err(Error::new_type_error("Value cannot be null"))
        } else {
            Ok(self)
        }
    }

    /// Returns `Some(self)` if `!self.is_undefined()`. Otherwise returns `None` if
    /// `self` is undefined.
    pub fn not_undefined(self) -> Result<Self> {
        if self.is_undefined() {
            Err(Error::new_type_error("Value cannot be undefined"))
        } else {
            Ok(self)
        }
    }

    /// Coerces `undefined` to `null`, matching the Web IDL specified behavior for T?
    /// where T does not support `undefined`.
    #[must_use]
    pub fn coerce_null(self) -> Self {
        if self.is_undefined() {
            Self::Null
        } else {
            self
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

    /// Returns the contained [`Some`] value, consuming `self`.
    ///
    /// This function panics on `!self.is_some()`, making it unsuitable
    /// for general use. Outside of testing, prefer pattern matching, or a non-panicking
    /// variant.
    ///
    /// # Panics
    ///
    /// Panics if the `self` value is [`Nullable::Null`] or [`Nullable::Undefined`], using a custom
    /// panic message passed to `msg`.
    ///
    /// [`Some`]: Nullable::Some
    #[track_caller]
    pub fn expect(self, msg: &str) -> T {
        match self {
            Self::Some(v) => v,
            Self::Null | Self::Undefined => panic!("{}", msg),
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

    /// Transforms the `Nullable` into a `Result`, mapping `Some(v)` to `Ok(v)`
    /// and `None` to `Err(err)`.
    ///
    /// This function is eagerly evaluated. Use [`Nullable::ok_or_else`] for
    /// a lazyily evaluated alternative.
    pub fn ok_or(self, err: Error) -> Result<T> {
        match self {
            Self::Some(v) => Ok(v),
            Self::Null | Self::Undefined => Err(err),
        }
    }

    /// Transforms the `Nullable` into a `Result`, mapping `Some(v)` to `Ok(v)`
    /// and `None` to `Err(err())`.
    ///
    /// This function is lazily evaluated.
    pub fn ok_or_else<F>(self, err: F) -> Result<T>
    where
        F: FnOnce() -> Error,
    {
        match self {
            Self::Some(v) => Ok(v),
            Self::Null | Self::Undefined => Err(err()),
        }
    }

    /// Converts from `Nullable<T>` to `Nullable<&T::Target>`. Equivalent
    /// to [`Option::as_deref`] for nullable.
    pub fn as_deref(&self) -> Nullable<&T::Target>
    where
        T: Deref,
    {
        self.as_ref().map(Deref::deref)
    }

    /// Converts from `Nullable<T>` to `Nullable<&mut T::Target>`. Equivalent
    /// to [`Option::as_deref`] for nullable.
    pub fn as_deref_mut(&mut self) -> Nullable<&mut T::Target>
    where
        T: DerefMut,
    {
        self.as_mut().map(DerefMut::deref_mut)
    }

    /// Applies the passed function if `Some`, and flattens the result.
    /// Otherwise, returns `self`.
    pub fn and_then<U, F>(self, f: F) -> Nullable<U>
    where
        F: FnOnce(T) -> Nullable<U>,
    {
        match self {
            Self::Some(v) => f(v),
            Self::Null => Nullable::Null,
            Self::Undefined => Nullable::Undefined,
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
