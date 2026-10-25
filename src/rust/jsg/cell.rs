// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#![allow(
    unsafe_code,
    reason = "reads the contents of a `std::cell::Cell` through its pointer"
)]

//! Cells for traced state that a resource changes after construction: a [`Cell`] and a
//! [`RefCell`] that make a reference strong before it leaves the cell.
//!
//! GC visits a `jsg::Rc` or a `TracedGlobal` where it finds it. Once the owner of the field is
//! reachable only through its JavaScript wrapper, the visit makes the reference weak and relies
//! on the owner's trace to keep its target alive. Rust moves are bitwise, so a reference moved
//! out of the field with `std::cell::Cell::take` or through `std::cell::RefCell::borrow_mut`
//! stays weak, and the owner no longer traces it: the next GC can collect the wrapper of a
//! `jsg::Rc`'s resource, or the value of a `TracedGlobal`. C++ `jsg::Ref` makes a moved
//! reference strong in its move constructor; these cells do it before a value leaves them
//! instead, through [`Traced::make_strong`].

use std::cell::BorrowError;
use std::cell::BorrowMutError;
use std::cell::Ref;
use std::cell::RefMut;

use crate::Isolate;
use crate::Traced;
use crate::v8::GcVisitor;

/// A cell for traced state in a resource, such as an `Option<jsg::Rc<T>>`, whose value is
/// moved in and out whole.
///
/// Like [`std::cell::Cell`], it gives its value up only by moving it out, with [`take`](Self::take)
/// or [`replace`](Self::replace). Those first make the value strong (see [`Traced::make_strong`]),
/// so a `jsg::Rc` or `TracedGlobal` that GC has visited in the cell stays valid outside it. They
/// take the isolate, since making a reference strong touches V8 handles.
///
/// Use a [`RefCell`] to read or change the value in place.
pub struct Cell<T> {
    inner: std::cell::Cell<T>,
}

impl<T: Traced> Cell<T> {
    /// A cell holding `value`.
    pub const fn new(value: T) -> Self {
        Self {
            inner: std::cell::Cell::new(value),
        }
    }

    /// Replaces the value with `value` and drops the old one.
    ///
    /// Dropping a weak reference needs no fix-up, so this takes no isolate.
    pub fn set(&self, value: T) {
        self.inner.set(value);
    }

    /// Replaces the value with `value` and returns the old one, made strong.
    pub fn replace(&self, isolate: &mut Isolate, value: T) -> T {
        self.make_strong(isolate);
        self.inner.replace(value)
    }

    /// Takes the value, made strong, and leaves `T::default()` behind.
    pub fn take(&self, isolate: &mut Isolate) -> T
    where
        T: Default,
    {
        self.replace(isolate, T::default())
    }
}

impl<T: Traced> Traced for Cell<T> {
    fn holds_gc_references() -> bool {
        T::holds_gc_references()
    }

    fn trace(&self, visitor: &mut GcVisitor) {
        // SAFETY: GC tracing is single-threaded within an isolate and never re-entrant on the
        // same object, and the visit only reads through the pointer; the cell hands out no
        // reference to its value otherwise.
        unsafe { (*self.inner.as_ptr()).trace(visitor) }
    }

    fn make_strong(&self, isolate: &mut Isolate) {
        // SAFETY: as in `trace`. `make_strong` writes only through the `Cell`s of the GC
        // references it reaches.
        unsafe { (*self.inner.as_ptr()).make_strong(isolate) }
    }
}

impl<T: Traced + Default> Default for Cell<T> {
    fn default() -> Self {
        Self::new(T::default())
    }
}

/// A cell for traced state in a resource that is read or changed in place, such as a
/// `Vec<jsg::Rc<T>>`.
///
/// Like [`std::cell::RefCell`], it hands out shared and exclusive borrows of its value. Unlike
/// `std::cell::RefCell`, [`borrow_mut`](Self::borrow_mut) first makes everything in the value
/// strong (see [`Traced::make_strong`]), so that whatever the borrower moves out, for example
/// by popping a `Vec`, is strong. It takes the isolate for that. The borrow itself does not
/// hold the isolate.
///
/// Everything in a mutably borrowed cell is strong, so a GC visit that finds the cell borrowed
/// safely skips it. The references left in the cell are made weak again by the next visit that
/// finds the owner reachable only through its wrapper. Holding the borrow across a call that can
/// run JavaScript is still a mistake: a method that borrows the cell again panics.
pub struct RefCell<T> {
    inner: std::cell::RefCell<T>,
}

impl<T: Traced> RefCell<T> {
    /// A cell holding `value`.
    pub const fn new(value: T) -> Self {
        Self {
            inner: std::cell::RefCell::new(value),
        }
    }

    /// Borrows the value.
    ///
    /// # Panics
    /// Panics if the value is mutably borrowed, as [`std::cell::RefCell::borrow`] does.
    pub fn borrow(&self) -> Ref<'_, T> {
        self.inner.borrow()
    }

    /// Borrows the value, or fails if it is mutably borrowed.
    pub fn try_borrow(&self) -> Result<Ref<'_, T>, BorrowError> {
        self.inner.try_borrow()
    }

    /// Mutably borrows the value, after making everything in it strong.
    ///
    /// # Panics
    /// Panics if the value is borrowed, as [`std::cell::RefCell::borrow_mut`] does.
    pub fn borrow_mut(&self, isolate: &mut Isolate) -> RefMut<'_, T> {
        let guard = self.inner.borrow_mut();
        guard.make_strong(isolate);
        guard
    }

    /// Mutably borrows the value, after making everything in it strong, or fails if it is
    /// borrowed.
    pub fn try_borrow_mut(&self, isolate: &mut Isolate) -> Result<RefMut<'_, T>, BorrowMutError> {
        let guard = self.inner.try_borrow_mut()?;
        guard.make_strong(isolate);
        Ok(guard)
    }
}

impl<T: Traced> Traced for RefCell<T> {
    fn holds_gc_references() -> bool {
        T::holds_gc_references()
    }

    fn trace(&self, visitor: &mut GcVisitor) {
        // A mutably borrowed cell is skipped: `borrow_mut` made everything in it strong, and
        // anything the borrower puts in is strong too, so the visit has nothing to keep alive.
        // The next visit after the borrow ends makes the contents weak again where they should
        // be.
        if let Ok(inner) = self.inner.try_borrow() {
            inner.trace(visitor);
        }
    }

    fn make_strong(&self, isolate: &mut Isolate) {
        // A mutably borrowed cell is already strong.
        if let Ok(inner) = self.inner.try_borrow() {
            inner.make_strong(isolate);
        }
    }
}

impl<T: Traced + Default> Default for RefCell<T> {
    fn default() -> Self {
        Self::new(T::default())
    }
}
