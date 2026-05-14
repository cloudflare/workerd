//! Helpers for Rust wrappers around C++ objects passed through the CXX bridge.
//!
//! Most Rust-visible wrappers in this crate hold one of two shapes:
//! - [`OwnOrRef`] when the underlying C++ API can hand Rust any of `kj::Own<T>`, `const T&`, or
//!   `T&`
//! - [`OwnOrMut`] when the underlying C++ API only ever hands Rust `kj::Own<T>` or `T&`
//!
//! Pick the narrowest holder that matches the FFI surface you are wrapping. In particular, use
//! [`OwnOrMut`] for mutable-only wrappers so the type system does not represent an impossible shared
//! borrow state.

use std::ops::Deref;
use std::pin::Pin;

use kj_rs::KjOwn;

/// Wrapper for C++ objects.
///
/// `OwnOrRef` represents the three ways a C++ object can be handed to Rust through this crate's
/// wrappers: owned as `kj::Own<T>`, borrowed as `const T&`, or borrowed as `T&`.
///
/// Use this type when the wrapped FFI surface genuinely accepts shared borrows. If the wrapper only
/// needs owned-or-mutable access, use [`OwnOrMut`] instead.
///
/// Instances of this type are not usually exposed directly. Instead, wrapper structs store an
/// `OwnOrRef<T>` internally and expose an API matching the capabilities of the underlying C++
/// object.
///
/// Typical mapping from C++ to a Rust wrapper:
///
/// - `kj::Own<T>` becomes an owned wrapper value
/// - `const T&` becomes `&Wrapper`
/// - `T&` becomes `&mut Wrapper`
///
/// The same wrapper can then expose shared methods through `AsRef` / `Deref`, and mutable methods
/// through `as_mut()` when the caller knows the value is not in the `Ref` state.
pub enum OwnOrRef<'a, T> {
    Own(KjOwn<T>),
    Ref(&'a T),
    MutRef(Pin<&'a mut T>),
}

/// Wrapper for C++ objects that are always owned or mutably borrowed.
///
/// `OwnOrMut` represents the two ways a mutable C++ object can be handed to Rust through this
/// crate's wrappers: owned as `kj::Own<T>` or borrowed as `T&`.
///
/// Use this type when the wrapped FFI surface does not allow `const T&`. That keeps the wrapper's
/// internal state aligned with reality and allows safe mutable access through `as_mut()`.
///
/// Instances of this type are not usually exposed directly. Instead, wrapper structs store an
/// `OwnOrMut<T>` internally and expose methods that operate on the underlying mutable C++ object.
pub enum OwnOrMut<'a, T> {
    Own(KjOwn<T>),
    MutRef(Pin<&'a mut T>),
}

impl<T> AsRef<T> for OwnOrRef<'_, T> {
    fn as_ref(&self) -> &T {
        match self {
            OwnOrRef::Own(own) => own.as_ref(),
            OwnOrRef::Ref(ref_) => ref_,
            OwnOrRef::MutRef(ref_) => ref_,
        }
    }
}

impl<T> OwnOrRef<'_, T> {
    /// Obtain mut reference to the underlying object.
    ///
    /// # Safety
    ///
    /// - self should not be `Ref` variant.
    ///
    /// C++ mutable references are represented by `Pin<&mut T>`, otherwise we'd implement `AsMut`.
    pub unsafe fn as_mut(&mut self) -> Pin<&mut T> {
        match self {
            OwnOrRef::Own(own) => own.as_mut(),
            OwnOrRef::Ref(_) => unreachable!("mut reference to borrowed object"),
            OwnOrRef::MutRef(ref_) => ref_.as_mut(),
        }
    }
}

impl<T> AsRef<T> for OwnOrMut<'_, T> {
    fn as_ref(&self) -> &T {
        match self {
            OwnOrMut::Own(own) => own.as_ref(),
            OwnOrMut::MutRef(ref_) => ref_,
        }
    }
}

impl<T> OwnOrMut<'_, T> {
    /// Obtain a mutable reference to the underlying object.
    pub fn as_mut(&mut self) -> Pin<&mut T> {
        match self {
            OwnOrMut::Own(own) => own.as_mut(),
            OwnOrMut::MutRef(ref_) => ref_.as_mut(),
        }
    }
}

impl<T> Deref for OwnOrRef<'_, T> {
    type Target = T;

    fn deref(&self) -> &Self::Target {
        self.as_ref()
    }
}

impl<T> Deref for OwnOrMut<'_, T> {
    type Target = T;

    fn deref(&self) -> &Self::Target {
        self.as_ref()
    }
}

impl<'a, T> From<&'a T> for OwnOrRef<'a, T> {
    fn from(value: &'a T) -> Self {
        Self::Ref(value)
    }
}

impl<T> From<KjOwn<T>> for OwnOrRef<'_, T> {
    fn from(value: KjOwn<T>) -> Self {
        Self::Own(value)
    }
}

impl<'a, T> From<Pin<&'a mut T>> for OwnOrRef<'a, T> {
    fn from(value: Pin<&'a mut T>) -> Self {
        Self::MutRef(value)
    }
}

impl<T> From<KjOwn<T>> for OwnOrMut<'_, T> {
    fn from(value: KjOwn<T>) -> Self {
        Self::Own(value)
    }
}

impl<'a, T> From<Pin<&'a mut T>> for OwnOrMut<'a, T> {
    fn from(value: Pin<&'a mut T>) -> Self {
        Self::MutRef(value)
    }
}
