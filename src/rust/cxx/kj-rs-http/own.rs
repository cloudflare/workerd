//! Helpers for Rust wrappers around C++ objects passed through the CXX bridge.
//!
//! Rust-visible wrappers in this crate hold an [`OwnOrMut`]: the underlying C++ API hands Rust
//! either `kj::Own<T>` or `T&`.

use std::ops::Deref;
use std::pin::Pin;

use kj_rs::KjOwn;

/// Wrapper for C++ objects that are always owned or mutably borrowed.
///
/// `OwnOrMut` represents the two ways a mutable C++ object can be handed to Rust through this
/// crate's wrappers: owned as `kj::Own<T>` or borrowed as `T&`.
///
/// Instances of this type are not usually exposed directly. Instead, wrapper structs store an
/// `OwnOrMut<T>` internally and expose methods that operate on the underlying mutable C++ object.
pub enum OwnOrMut<'a, T> {
    Own(KjOwn<T>),
    MutRef(Pin<&'a mut T>),
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

impl<T> Deref for OwnOrMut<'_, T> {
    type Target = T;

    fn deref(&self) -> &Self::Target {
        self.as_ref()
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
