// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::ops::Deref;
use std::ops::DerefMut;

use crate::Error;
use crate::Result;

/// A value of type `T` or `null`: the JavaScript type `T | null`.
///
/// Like C++ `kj::Maybe<T>` and Web IDL's `T?`, input conversion coerces
/// `undefined` to `null`. A method parameter of this type is still required:
/// passing `undefined` produces `Nullable::Null`, but omitting the argument
/// is a `TypeError`.
///
/// Use `Option<Nullable<T>>` for an optional nullable that keeps `undefined`
/// distinct from `null`, since [`Option<T>`] is `T | undefined`:
///
/// | JavaScript  | `Nullable<T>`       | `Option<Nullable<T>>`     |
/// |-------------|---------------------|---------------------------|
/// | a `T`       | `Nullable::Some(v)` | `Some(Nullable::Some(v))` |
/// | `null`      | `Nullable::Null`    | `Some(Nullable::Null)`    |
/// | `undefined` | `Nullable::Null`    | `None`                    |
///
/// Output conversion produces only `T` or `null`. The TypeScript type is
/// `T | null`; `Option<Nullable<T>>` adds `undefined` and permits omission.
///
/// # Examples
///
/// ```ignore
/// use jsg::Nullable;
///
/// #[jsg_method]
/// pub fn process(&self, value: Nullable<String>) {
///     match value {
///         Nullable::Some(s) => println!("Got value: {s}"),
///         Nullable::Null => println!("Got null"),
///     }
/// }
///
/// #[jsg_method]
/// pub fn process_optional(&self, value: Option<Nullable<String>>) {
///     match value {
///         Some(Nullable::Some(s)) => println!("Got value: {s}"),
///         Some(Nullable::Null) => println!("Got null"),
///         None => println!("Got undefined"),
///     }
/// }
///
/// #[jsg_method]
/// pub fn process_some(&self, value: Nullable<Number>) -> Result<(), Error> {
///     // Throws a `TypeError` for `null`.
///     let value = value.not_null()?;
///     // Process...
///     Ok(())
/// }
///
/// #[jsg_method]
/// pub fn return_nullable(&self) -> Nullable<Number> {
///     if self.condition {
///         Nullable::Some(self.number)
///     } else {
///         Nullable::Null
///     }
/// }
/// ```
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub enum Nullable<T> {
    Some(T),
    Null,
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

    /// Converts from `&Nullable<T>` to `Nullable<&T>`.
    pub fn as_ref(&self) -> Nullable<&T> {
        match self {
            Self::Some(v) => Nullable::Some(v),
            Self::Null => Nullable::Null,
        }
    }

    /// Converts from `&mut Nullable<T>` to `Nullable<&mut T>`.
    pub fn as_mut(&mut self) -> Nullable<&mut T> {
        match self {
            Self::Some(v) => Nullable::Some(v),
            Self::Null => Nullable::Null,
        }
    }

    /// Returns the contained value, or a `TypeError` if `self` is `Null`.
    pub fn not_null(self) -> Result<T> {
        self.ok_or_else(|| Error::new_type_error("Value cannot be null"))
    }

    /// Returns the contained [`Some`] value, consuming `self`.
    ///
    /// This function panics on `Null`, making it unsuitable for general use.
    /// Outside of testing, prefer pattern matching, or a non-panicking variant.
    ///
    /// # Panics
    ///
    /// Panics if the `self` value is [`Nullable::Null`].
    ///
    /// [`Some`]: Nullable::Some
    #[track_caller]
    #[expect(
        clippy::panic,
        reason = "panicking on a missing value is this method's contract, as with `Option`"
    )]
    pub fn unwrap(self) -> T {
        match self {
            Self::Some(v) => v,
            Self::Null => panic!("called `Nullable::unwrap()` on a `Null` value"),
        }
    }

    /// Returns the contained [`Some`] value, consuming `self`.
    ///
    /// This function panics on `Null`, making it unsuitable for general use.
    /// Outside of testing, prefer pattern matching, or a non-panicking variant.
    ///
    /// # Panics
    ///
    /// Panics if the `self` value is [`Nullable::Null`], using a custom panic
    /// message passed to `msg`.
    ///
    /// [`Some`]: Nullable::Some
    #[track_caller]
    #[expect(
        clippy::panic,
        reason = "panicking on a missing value is this method's contract, as with `Option`"
    )]
    pub fn expect(self, msg: &str) -> T {
        match self {
            Self::Some(v) => v,
            Self::Null => panic!("{}", msg),
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
            Self::Null => default,
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
            Self::Null => f(),
        }
    }

    /// Transforms the `Nullable` into a `Result`, mapping `Some(v)` to `Ok(v)`
    /// and `Null` to `Err(err)`.
    ///
    /// This function is eagerly evaluated. Use [`Nullable::ok_or_else`] for
    /// a lazily evaluated alternative.
    pub fn ok_or(self, err: Error) -> Result<T> {
        match self {
            Self::Some(v) => Ok(v),
            Self::Null => Err(err),
        }
    }

    /// Transforms the `Nullable` into a `Result`, mapping `Some(v)` to `Ok(v)`
    /// and `Null` to `Err(err())`.
    ///
    /// This function is lazily evaluated.
    pub fn ok_or_else<F>(self, err: F) -> Result<T>
    where
        F: FnOnce() -> Error,
    {
        match self {
            Self::Some(v) => Ok(v),
            Self::Null => Err(err()),
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
    /// to [`Option::as_deref_mut`] for nullable.
    pub fn as_deref_mut(&mut self) -> Nullable<&mut T::Target>
    where
        T: DerefMut,
    {
        self.as_mut().map(DerefMut::deref_mut)
    }

    /// Applies the passed function if `Some`, and flattens the result.
    /// Otherwise, returns `Null`.
    pub fn and_then<U, F>(self, f: F) -> Nullable<U>
    where
        F: FnOnce(T) -> Nullable<U>,
    {
        match self {
            Self::Some(v) => f(v),
            Self::Null => Nullable::Null,
        }
    }

    /// Maps a [`Nullable<T>`] to a [`Nullable<U>`] by applying the passed function if [`Some`],
    /// or returns `Null` otherwise.
    ///
    /// [`Some`]: Nullable::Some
    pub fn map<U, F>(self, f: F) -> Nullable<U>
    where
        F: FnOnce(T) -> U,
    {
        match self {
            Self::Some(t) => Nullable::Some(f(t)),
            Self::Null => Nullable::Null,
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
            Nullable::Null => None,
        }
    }
}
