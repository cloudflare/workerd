// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Dropping persistent handles, and the liveness of their isolate.
//!
//! A [`v8::Global`](crate::v8::Global) outlives every scope, and is dropped wherever the Rust
//! value owning it goes away. Two things about its isolate decide what the drop may do:
//!
//! * **Whether this thread holds the isolate lock.** `v8::Global::Reset` edits the isolate's
//!   free list of handle nodes without synchronization, as does every handle creation on the
//!   thread holding the lock. A reset without the lock is a data race that can hand a node out
//!   twice or unlink a block of roots.
//! * **Whether the isolate is still alive.** Nothing ties a `Global` to its isolate's
//!   lifetime: one can be dropped after `v8::Isolate::Dispose`, for example when the Rust
//!   object holding it is owned by something that outlives the worker. `Dispose` frees every
//!   global and traced handle of the isolate in bulk, so a reset after it touches freed
//!   memory.
//!
//! The [`Realm`](crate::Realm) therefore owns an [`IsolateState`], shared through an `Arc`
//! with every `Global` of its isolate. [`IsolateState::drop_handle`] resets a dropped
//! `Global`'s handle at once under the lock, queues it otherwise, and leaves it to `Dispose`
//! once the state is closed. [`IsolateState::reset_awaiting_lock`] empties the queue; it runs
//! from the `IsolateBase` deferred action hook, which `IsolateBase::Lock` calls whenever it
//! locks the isolate, the same point at which C++ JSG applies its own deferred destruction
//! queue. A bare `v8::Locker`, or relocking after a `v8::Unlocker`, does not empty the queue;
//! the handles then wait for the next `IsolateBase::Lock`. The `Realm` closes the state when
//! it is dropped, under the isolate lock, before the isolate is disposed.

use std::pin::Pin;
use std::sync::Arc;
use std::sync::Mutex;
use std::sync::MutexGuard;
use std::sync::PoisonError;
use std::sync::atomic::AtomicBool;
use std::sync::atomic::Ordering;

use crate::Isolate;
use crate::scope::scope_owner;
use crate::v8;
use crate::v8::ffi;

/// What the handles of an isolate need to know about it after their scopes are gone.
pub struct IsolateState {
    /// Compared with other isolate pointers, and dereferenced only while the isolate is known
    /// to be alive: while `drop_queues` is locked and `closed` is not set, since
    /// [`close`](Self::close) sets `closed` under that same lock, before the isolate is
    /// disposed.
    isolate: *mut ffi::Isolate,
    /// Set by [`close`](Self::close), under the isolate lock, before the isolate is disposed.
    closed: AtomicBool,
    drop_queues: Mutex<DropQueues>,
}

// SAFETY: `isolate` is a raw pointer that is only compared, or dereferenced for
// `v8::Locker::IsLocked`, which any thread may call, while the isolate is alive (see the
// field). The other fields synchronize themselves.
#[expect(unsafe_code, reason = "shares the raw isolate pointer between threads")]
unsafe impl Send for IsolateState {}
// SAFETY: as for `Send`.
#[expect(unsafe_code, reason = "shares the raw isolate pointer between threads")]
unsafe impl Sync for IsolateState {}

#[derive(Default)]
struct DropQueues {
    /// Strong handles of `Global`s that were dropped without the isolate lock, waiting for the
    /// next lock.
    awaiting_lock: Vec<ffi::Global>,
}

#[expect(
    unsafe_code,
    reason = "resets V8 handles and asks V8 whether the isolate is locked"
)]
impl IsolateState {
    pub(crate) fn new(isolate: v8::IsolatePtr) -> Arc<Self> {
        Arc::new(Self {
            isolate: isolate.as_ffi(),
            closed: AtomicBool::new(false),
            drop_queues: Mutex::new(DropQueues::default()),
        })
    }

    /// Whether `isolate` is the isolate of this state. Only compares the pointers.
    pub(crate) fn belongs_to(&self, isolate: *mut ffi::Isolate) -> bool {
        self.isolate == isolate
    }

    /// Whether this thread holds the isolate lock. False once the state is closed.
    fn this_thread_holds_lock(&self) -> bool {
        if self.is_closed() {
            return false;
        }
        if scope_owner::current() == self.isolate {
            // A scope of the isolate is open on this thread, so this thread holds its lock.
            return true;
        }
        let _queues = self.lock_drop_queues();
        // `close` takes the mutex before it sets `closed`, so the isolate is alive until the
        // guard is dropped.
        // SAFETY: the isolate is alive (see above), and `v8::Locker::IsLocked`, which tells
        // whether this thread holds the lock, may be called from any thread.
        !self.is_closed() && unsafe { ffi::isolate_is_locked(self.isolate) }
    }

    /// Whether the isolate is about to be disposed, or already has been. Its handles are then
    /// freed with it, and must not be touched.
    pub(crate) fn is_closed(&self) -> bool {
        self.closed.load(Ordering::Acquire)
    }

    /// Takes the strong handle of a dropped `Global`.
    ///
    /// Under the isolate lock the handle is reset at once. Without it the handle is queued,
    /// and [`reset_awaiting_lock`](Self::reset_awaiting_lock) resets it the next time the
    /// isolate is locked, so until then it keeps its value alive. Once the state is closed the
    /// handle is left to `v8::Isolate::Dispose`.
    pub(crate) fn drop_handle(&self, strong: ffi::Global) {
        if self.this_thread_holds_lock() {
            reset_strong(strong);
            return;
        }
        let mut queues = self.lock_drop_queues();
        // `close` takes the queue under the mutex after it sets `closed`, so a handle pushed
        // while the state is open is reset either by the next lock or by `close`.
        if !self.is_closed() {
            queues.awaiting_lock.push(strong);
        }
    }

    /// Resets the handles that [`drop_handle`](Self::drop_handle) queued. The deferred action
    /// hook calls it whenever `IsolateBase::Lock` locks the isolate.
    pub(crate) fn reset_awaiting_lock(&self, _isolate: &mut Isolate) {
        // The handles are reset outside the mutex, which only guards the queue itself.
        let batch = std::mem::take(&mut self.lock_drop_queues().awaiting_lock);
        for strong in batch {
            reset_strong(strong);
        }
    }

    /// The number of handles waiting for the next lock.
    pub(crate) fn awaiting_lock_count(&self) -> usize {
        self.lock_drop_queues().awaiting_lock.len()
    }

    /// Records that the isolate is about to be disposed, and resets the queued handles while
    /// it still can. Every handle of the isolate that is dropped from now on is left to
    /// `Dispose`.
    ///
    /// The `Isolate` reference proves that the caller holds the isolate lock; no handle of the
    /// isolate can be reset concurrently.
    pub(crate) fn close(&self, _isolate: &mut Isolate) {
        let batch = {
            let mut queues = self.lock_drop_queues();
            self.closed.store(true, Ordering::Release);
            std::mem::take(&mut queues.awaiting_lock)
        };
        for strong in batch {
            reset_strong(strong);
        }
    }

    fn lock_drop_queues(&self) -> MutexGuard<'_, DropQueues> {
        // The mutex is only held around `Vec` operations, so a poisoned lock still guards
        // consistent queues.
        self.drop_queues
            .lock()
            .unwrap_or_else(PoisonError::into_inner)
    }
}

/// Resets a strong handle. The isolate must be locked by this thread.
#[expect(unsafe_code, reason = "resets a V8 handle")]
fn reset_strong(mut strong: ffi::Global) {
    // SAFETY: the handle is valid and the isolate is locked (guaranteed by caller). `strong`
    // is not moved while pinned.
    unsafe { ffi::global_reset(Pin::new_unchecked(&mut strong)) };
}
