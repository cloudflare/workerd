// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! [`LockGuarded`], a value that only its isolate's lock guards.

#![allow(
    unsafe_code,
    reason = "a `LockGuarded` is `Send` though its value isn't, and takes the value out to drop it"
)]

use std::mem::ManuallyDrop;
use std::sync::Arc;

use crate::Lock;
use crate::MemoryTracker;
use crate::isolate_state::IsolateState;
use crate::isolate_state::Releasable;
use crate::isolate_state::ReleaseContext;
use crate::v8::GcVisitor;

/// A value that only the lock of its isolate guards, such as a C++ object with a non-atomic
/// reference count.
///
/// It is `Send` though the value isn't, as a [`jsg::Rc`](crate::Rc) is, since it lends the value
/// only under that lock and drops it as a `jsg::Rc` drops its parts (see `README.md` §Safety
/// rules, rule 5):
///
/// - [`get`](Self::get) lends the value under a lock of its isolate, for no longer than the lock's
///   scope.
/// - [`get_for_visit`](Self::get_for_visit) lends it to a GC or detaching visit under that lock.
/// - A drop under the lock drops the value at once, even while the isolate is torn down, until it
///   is about to be disposed, so that what teardown drops is destroyed under the lock, as C++ JSG
///   objects are. A drop without it queues the value, untouched, for the next lock; once the
///   isolate is disposed, one thread at a time drops what is queued.
pub struct LockGuarded<T: 'static> {
    value: ManuallyDrop<T>,
    state: Arc<IsolateState>,
}

impl<T: 'static> LockGuarded<T> {
    /// Guards `value` with the lock of `lock`'s isolate.
    ///
    /// # Safety
    /// `value` belongs to `lock`'s isolate, and any thread that holds that isolate's lock may use
    /// and drop it: it is bound to no thread but through the lock, as a C++ JSG object is. Once the
    /// isolate is disposed, any one thread at a time may drop it without the lock.
    pub unsafe fn new(lock: &mut Lock<'_>, value: T) -> Self {
        Self {
            value: ManuallyDrop::new(value),
            state: Arc::clone(lock.isolate_state()),
        }
    }

    /// Lends the value, for no longer than `lock`'s scope.
    ///
    /// # Panics
    /// Panics if `lock` belongs to an isolate other than the value's, or once the value's
    /// isolate is closed.
    pub fn get<'a, 's: 'a>(&'a self, lock: &Lock<'s>) -> &'a T {
        assert!(
            !self.state.is_closed() && self.state.belongs_to(lock.isolate().as_ffi()),
            "attempt to use a jsg::LockGuarded in an isolate other than the one it was created in"
        );
        &self.value
    }

    /// Lends the value to a visit that may change the references in it: a GC visit on behalf of
    /// the value's isolate, a detaching visit under its lock, or the visit for a resource's first
    /// strong reference on a thread that holds its lock. Returns `None` for other visits, which
    /// leave the value as it is, as [`GcVisitor::visit_rc`] does for a `jsg::Rc`.
    ///
    /// # Panics
    /// Panics in a detaching visit under the lock of an isolate other than the value's.
    pub fn get_for_visit<'a>(&'a self, visitor: &mut GcVisitor) -> Option<&'a T> {
        visitor
            .may_change_in(&self.state, "jsg::LockGuarded")
            .then_some(&*self.value)
    }

    /// Lends the value to a heap snapshot of its isolate, which runs under that isolate's lock.
    /// Returns `None` for a snapshot of another isolate, or once the isolate is being torn down,
    /// as a `jsg::Rc` reports nothing then.
    pub fn get_for_memory_info<'a>(&'a self, tracker: &MemoryTracker<'_>) -> Option<&'a T> {
        tracker
            .can_track_object_of(&self.state)
            .then_some(&*self.value)
    }
}

impl<T: 'static> Drop for LockGuarded<T> {
    fn drop(&mut self) {
        // SAFETY: `drop` runs once, and the value is not used after it is taken.
        let value = unsafe { ManuallyDrop::take(&mut self.value) };
        self.state.release(ReleasedValue(value));
    }
}

/// The value of a dropped [`LockGuarded`], which `new`'s contract lets any of the threads that
/// release it drop.
struct ReleasedValue<T>(T);

impl<T: 'static> Releasable for ReleasedValue<T> {
    fn release(self, _context: ReleaseContext) {}
}

// SAFETY: the value is lent only under the lock of its isolate (`get` and `get_for_visit` check,
// and `Lock` is neither `Send` nor `Sync`), for no longer than the lock's scope. It is dropped
// under that lock, or queued untouched and dropped by the next thread to lock the isolate, or once
// the isolate is disposed by one thread at a time. `new`'s contract lets any of those threads use
// or drop it.
unsafe impl<T: 'static> Send for LockGuarded<T> {}
