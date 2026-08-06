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
