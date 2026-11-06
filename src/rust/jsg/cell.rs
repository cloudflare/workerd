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
//! out of the field, as `std::cell::Cell::take` or `std::cell::RefCell::borrow_mut` would do,
//! stays weak, and the owner no longer traces it: the next GC can collect the wrapper of a
//! `jsg::Rc`'s resource, or the value of a `TracedGlobal`. C++ `jsg::Ref` makes a moved
//! reference strong in its move constructor; these cells do it as a value leaves them instead,
//! with a detaching visit (see [`GcVisitor::is_detaching`]). The std cells are `Traced`
//! only for types that hold no GC references ([`HoldsNoGcReferences`]), so these cells are the
//! only way out of a field of a live resource.

use std::cell::BorrowError;
use std::cell::BorrowMutError;
use std::cell::Ref;
use std::cell::RefMut;
use std::ffi::CStr;

use crate::HoldsNoGcReferences;
use crate::Lock;
use crate::MemoryInfo;
use crate::MemoryTracker;
use crate::Traced;
use crate::v8::GcVisitor;

/// A cell for traced state in a resource, such as an `Option<jsg::Rc<T>>`, whose value is
/// moved in and out whole.
///
/// Like [`std::cell::Cell`], it gives its value up only by moving it out, with [`take`](Self::take)
/// or [`replace`](Self::replace). Those detach the value they return from the cell's owner (see
/// [`GcVisitor::is_detaching`]), which makes every `jsg::Rc` and `TracedGlobal` in it strong, so
/// one that GC has visited in the cell stays valid outside it. They take the lock, since making a
/// reference strong touches V8 handles.
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
    /// Dropping a weak reference needs no fix-up, so this takes no lock.
    pub fn set(&self, value: T) {
        self.inner.set(value);
    }

    /// Replaces the value with `value` and returns the old one, made strong.
    ///
    /// # Panics
    /// Panics if the value holds a `TracedGlobal` of an isolate other than the lock's.
    pub fn replace(&self, lock: &mut Lock<'_>, value: T) -> T {
        // Detached once out of the cell: making a `jsg::Rc` to a resource with no wrapper strong
        // visits that resource, which can lead back to this cell, and that visit must not find
        // the old value here still weak.
        let old = self.inner.replace(value);
        GcVisitor::detach(lock, &old);
        old
    }

    /// Takes the value, made strong, and leaves `T::default()` behind.
    ///
    /// # Panics
    /// Panics if the value holds a `TracedGlobal` of an isolate other than the lock's.
    pub fn take(&self, lock: &mut Lock<'_>) -> T
    where
        T: Default,
    {
        self.replace(lock, T::default())
    }
}

impl<T: Traced> Traced for Cell<T> {
    fn trace(&self, visitor: &mut GcVisitor) {
        // SAFETY: visits are single-threaded within an isolate, create only shared references, and
        // write only through the interior mutability of the GC references they reach (`trace`
        // changes nothing other than through the visitor). The cell hands out no reference to its
        // value, and `replace` moves the value out before it runs a visit, so no visit overlaps a
        // write to the cell.
        unsafe { (*self.inner.as_ptr()).trace(visitor) }
    }
}

impl<T: HoldsNoGcReferences> HoldsNoGcReferences for Cell<T> {}

/// Reads the value in place, as [`std::cell::Cell`]'s impl does, under the same rule: reporting
/// code writes into no cell.
impl<T: MemoryInfo> MemoryInfo for Cell<T> {
    const PLAIN_DATA: bool = T::PLAIN_DATA;

    fn self_bytes_reported(&self) -> usize {
        self.inner.self_bytes_reported()
    }

    fn track_as_field(&self, tracker: &mut MemoryTracker<'_>, edge_name: Option<&'static CStr>) {
        self.inner.track_as_field(tracker, edge_name);
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
/// `std::cell::RefCell`, [`borrow_mut`](Self::borrow_mut) first detaches the value from the
/// cell's owner (see [`GcVisitor::is_detaching`]), which makes everything in it strong, so that
/// whatever the borrower moves out, for example by popping a `Vec`, is strong. It takes the lock
/// for that. The borrow itself does not hold the lock.
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

    /// Mutably borrows the value, after making everything in it strong. That visits every
    /// element of a collection, so make a batch of changes under one borrow.
    ///
    /// # Panics
    /// Panics if the value is borrowed, as [`std::cell::RefCell::borrow_mut`] does, or if it holds
    /// a `TracedGlobal` of an isolate other than the lock's.
    pub fn borrow_mut(&self, lock: &mut Lock<'_>) -> RefMut<'_, T> {
        let guard = self.inner.borrow_mut();
        GcVisitor::detach(lock, &*guard);
        guard
    }

    /// Mutably borrows the value, after making everything in it strong, or fails if it is
    /// borrowed.
    ///
    /// # Panics
    /// Panics if the value holds a `TracedGlobal` of an isolate other than the lock's.
    pub fn try_borrow_mut(&self, lock: &mut Lock<'_>) -> Result<RefMut<'_, T>, BorrowMutError> {
        let guard = self.inner.try_borrow_mut()?;
        GcVisitor::detach(lock, &*guard);
        Ok(guard)
    }
}

impl<T: Traced> Traced for RefCell<T> {
    fn trace(&self, visitor: &mut GcVisitor) {
        // A mutably borrowed cell is skipped, by a detaching visit too: `borrow_mut` made
        // everything in it strong, and anything the borrower puts in is strong too, so the visit
        // has nothing to keep alive or to make strong. The next visit after the borrow ends makes
        // the contents weak again where they should be.
        if let Ok(inner) = self.inner.try_borrow() {
            inner.trace(visitor);
        }
    }
}

impl<T: HoldsNoGcReferences> HoldsNoGcReferences for RefCell<T> {}

/// Reports nothing while the value is mutably borrowed, as [`std::cell::RefCell`]'s impl does.
impl<T: MemoryInfo> MemoryInfo for RefCell<T> {
    const PLAIN_DATA: bool = T::PLAIN_DATA;

    fn self_bytes_reported(&self) -> usize {
        self.inner.self_bytes_reported()
    }

    fn track_as_field(&self, tracker: &mut MemoryTracker<'_>, edge_name: Option<&'static CStr>) {
        self.inner.track_as_field(tracker, edge_name);
    }
}

impl<T: Traced + Default> Default for RefCell<T> {
    fn default() -> Self {
        Self::new(T::default())
    }
}
