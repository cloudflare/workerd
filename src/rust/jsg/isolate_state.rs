// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Dropping persistent handles and resource references, and the liveness of their isolate.
//!
//! A [`v8::Global`](crate::v8::Global) or [`v8::TracedGlobal`](crate::v8::TracedGlobal)
//! outlives every scope, and is dropped wherever the Rust value owning it goes away. Two things
//! about its isolate decide what the drop may do:
//!
//! * **Whether this thread holds the isolate lock.** `v8::Global::Reset` edits the isolate's
//!   free list of handle nodes without synchronization, as does every handle creation on the
//!   thread holding the lock. A reset without the lock is a data race that can hand a node out
//!   twice or unlink a block of roots.
//! * **Whether the isolate is still alive.** Nothing ties a persistent handle to its isolate's
//!   lifetime: one can be dropped after `v8::Isolate::Dispose`, for example when the Rust
//!   object holding it is owned by something that outlives the worker. `Dispose` frees every
//!   global and traced handle of the isolate in bulk, so a reset after it touches freed
//!   memory.
//!
//! The [`Realm`](crate::Realm) therefore owns an [`IsolateState`], shared through an `Arc`
//! with every `Global` and `TracedGlobal` of its isolate. [`IsolateState::drop_global`] and
//! [`IsolateState::drop_slot`] reset a dropped handle at once under the lock, queue it
//! otherwise, and leave it to `Dispose` once the state is closed.
//! [`IsolateState::reset_awaiting_lock`] empties the queue; it runs from the `IsolateBase`
//! deferred action hook, which `IsolateBase::Lock` calls whenever it locks the isolate, the
//! same point at which C++ JSG applies its own deferred destruction queue. A bare `v8::Locker`,
//! or relocking after a `v8::Unlocker`, does not empty the queue; the handles then wait for
//! the next `IsolateBase::Lock`. The `Realm` closes the state when it is dropped, under the
//! isolate lock, before the isolate is disposed.
//!
//! A `TracedGlobal` keeps its handles in a [`GlobalSlot`], whose address V8 may record to null the
//! strong handle when it is weak and its value dies, during any GC. GC runs on the thread
//! holding the isolate lock, so a slot is only read under the lock, and freed only once its
//! strong handle has been reset or the isolate disposed, after which V8 forgets it. A drop
//! without the lock queues the slot. After the state is closed the handles are no longer reset,
//! but GC can still run before the isolate is disposed: during `IsolateBase::dropWrappers`,
//! while `v8::Isolate::Dispose` tears the heap down, or whenever V8 flags ask for it. So the
//! slot of a `TracedGlobal` dropped then is kept until `~IsolateBase` reports, through a hook the
//! `Realm` set, that the isolate is disposed. A `Global` is never weak, so V8 never writes
//! into it: it holds its strong handle inline, and puts it in a slot of its own only to queue
//! it, while a handle dropped after the state is closed needs no slot at all.
//!
//! A [`jsg::Rc`](crate::Rc) or [`jsg::Weak`](crate::Weak) also shares its isolate's state. Its
//! reference counts are not atomic, so only the isolate lock keeps threads from racing on them.
//! [`IsolateState::release`] releases a reference at once under the lock, and otherwise queues
//! it for the next lock. After the state is closed, references are kept until the isolate is
//! disposed, since `IsolateBase::dropWrappers` and `Dispose` still touch the counts. They are
//! then released with [`ReleaseContext::Disposed`], one thread at a time.

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
use crate::v8::GlobalSlot;
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
// field). The other fields synchronize themselves; see `Released` for the references queued in
// `drop_queues`.
#[expect(unsafe_code, reason = "shares the raw isolate pointer between threads")]
unsafe impl Send for IsolateState {}
// SAFETY: as for `Send`.
#[expect(unsafe_code, reason = "shares the raw isolate pointer between threads")]
unsafe impl Sync for IsolateState {}

#[derive(Default)]
#[expect(
    clippy::vec_box,
    reason = "V8 may write into a slot until its handle is reset or the isolate disposed"
)]
struct DropQueues {
    /// Slots of `Global`s and `TracedGlobal`s that were dropped without the isolate lock,
    /// waiting for the next lock.
    awaiting_lock: Vec<Box<GlobalSlot>>,
    /// References dropped without the isolate lock, waiting for the next lock.
    released_awaiting_lock: Vec<Released>,
    /// Slots of `TracedGlobal`s that were dropped after the state was closed, waiting for the
    /// isolate to be disposed.
    awaiting_dispose: Vec<Box<GlobalSlot>>,
    /// References dropped after the state was closed, waiting to be drained once the isolate is
    /// disposed.
    released_awaiting_dispose: Vec<Released>,
    /// Set once the isolate is disposed, after which slots are freed at once.
    disposed: bool,
    /// Set while a thread drains `released_awaiting_dispose`.
    draining_disposed: bool,
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
    pub(crate) fn this_thread_holds_lock(&self) -> bool {
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

    /// Takes the slot of a dropped `TracedGlobal`.
    ///
    /// Under the isolate lock the strong handle is reset at once, and the slot freed. Without
    /// it the slot is queued, and [`reset_awaiting_lock`](Self::reset_awaiting_lock) resets and
    /// frees it the next time the isolate is locked, so until then the handle keeps its value
    /// alive. Once the state is closed the handle is left to `v8::Isolate::Dispose`, and the
    /// slot is kept until then. The traced reference is let go of without a reset; see
    /// [`GcVisitor::visit_traced_global`](crate::v8::GcVisitor::visit_traced_global).
    pub(crate) fn drop_slot(&self, slot: Box<GlobalSlot>) {
        if self.this_thread_holds_lock() {
            reset_and_free(slot);
            return;
        }
        self.queue(slot, AfterClose::KeepUntilDisposed);
    }

    /// Takes the strong handle of a dropped `Global`, as [`drop_slot`](Self::drop_slot) takes
    /// the slot of a `TracedGlobal`. The handle gets a slot only to be queued. Once the state is
    /// closed nothing is kept: V8 never writes into the slot of a strong handle.
    pub(crate) fn drop_global(&self, mut handle: ffi::Global) {
        if self.this_thread_holds_lock() {
            // SAFETY: this thread holds the isolate lock, and the handle is valid.
            unsafe { reset(&mut handle) };
            return;
        }
        if self.is_closed() {
            return;
        }
        self.queue(GlobalSlot::new_boxed(handle), AfterClose::Free);
    }

    /// Queues the slot of a handle dropped without the isolate lock, or, once the state is
    /// closed, does what `after_close` says.
    fn queue(&self, slot: Box<GlobalSlot>, after_close: AfterClose) {
        let mut queues = self.lock_drop_queues();
        // `close` takes the queue under the mutex after it sets `closed`, so a slot pushed
        // while the state is open is reset either by the next lock or by `close`.
        if !self.is_closed() {
            queues.awaiting_lock.push(slot);
        } else if after_close == AfterClose::KeepUntilDisposed && !queues.disposed {
            queues.awaiting_dispose.push(slot);
        }
    }

    /// Releases the parts of a dropped [`jsg::Rc`](crate::Rc) or [`jsg::Weak`](crate::Weak):
    /// at once under the lock, otherwise on the next lock, or, once the state is closed, after
    /// the isolate is disposed.
    pub(crate) fn release<T: Releasable>(&self, released: T) {
        if self.this_thread_holds_lock() {
            released.release(ReleaseContext::Locked);
            return;
        }
        let mut queues = self.lock_drop_queues();
        // `close` sets `closed` and takes the queue under this mutex, so nothing pushed while the
        // state is open is missed.
        if !self.is_closed() {
            queues.released_awaiting_lock.push(Released::new(released));
            return;
        }
        queues
            .released_awaiting_dispose
            .push(Released::new(released));
        if !queues.disposed || queues.draining_disposed {
            return;
        }
        queues.draining_disposed = true;
        drop(queues);
        self.drain_disposed();
    }

    /// Releases the references and resets the handles queued without the lock. The deferred
    /// action hook calls it whenever `IsolateBase::Lock` locks the isolate.
    pub(crate) fn drain_awaiting_lock(&self, _isolate: &mut Isolate) {
        // Outside the mutex, since releasing a reference can drop a resource and release more.
        let (released, slots) = {
            let mut queues = self.lock_drop_queues();
            (
                std::mem::take(&mut queues.released_awaiting_lock),
                std::mem::take(&mut queues.awaiting_lock),
            )
        };
        released.into_iter().for_each(Released::release_locked);
        slots.into_iter().for_each(reset_and_free);
    }

    /// The number of handles waiting for the next lock.
    pub(crate) fn awaiting_lock_count(&self) -> usize {
        self.lock_drop_queues().awaiting_lock.len()
    }

    /// The number of references to resources waiting for the next lock.
    pub(crate) fn released_awaiting_lock_count(&self) -> usize {
        self.lock_drop_queues().released_awaiting_lock.len()
    }

    /// Records that the isolate is about to be disposed, and releases the queued references and
    /// resets the queued handles while it still can. Handles dropped from now on are left to
    /// `Dispose`, and references kept until the isolate is disposed.
    ///
    /// The `Isolate` reference proves that the caller holds the isolate lock; no handle of the
    /// isolate can be reset concurrently.
    pub(crate) fn close(&self, _isolate: &mut Isolate) {
        // References first, so the handles of resources they drop are reset, not left to
        // `Dispose`.
        let released = std::mem::take(&mut self.lock_drop_queues().released_awaiting_lock);
        released.into_iter().for_each(Released::release_locked);
        let (released, slots) = {
            let mut queues = self.lock_drop_queues();
            self.closed.store(true, Ordering::Release);
            (
                std::mem::take(&mut queues.released_awaiting_lock),
                std::mem::take(&mut queues.awaiting_lock),
            )
        };
        // Queued by other threads meanwhile.
        released.into_iter().for_each(Released::release_locked);
        slots.into_iter().for_each(reset_and_free);
    }

    /// Records that the isolate has been disposed, frees the slots kept for it, which V8 can no
    /// longer write into, and releases the references kept for it.
    ///
    /// Like a C++ JSG object that outlives its isolate, a resource must from now on be used by
    /// one thread at a time: releases are serialized, but clones are not.
    pub(crate) fn isolate_disposed(&self) {
        let kept = {
            let mut queues = self.lock_drop_queues();
            queues.disposed = true;
            queues.draining_disposed = true;
            std::mem::take(&mut queues.awaiting_dispose)
        };
        drop(kept);
        self.drain_disposed();
    }

    /// Releases `released_awaiting_dispose` until it is empty. Only the thread that set
    /// `draining_disposed` calls this; other threads just add to the queue.
    fn drain_disposed(&self) {
        loop {
            let batch = {
                let mut queues = self.lock_drop_queues();
                let batch = std::mem::take(&mut queues.released_awaiting_dispose);
                queues.draining_disposed = !batch.is_empty();
                batch
            };
            if batch.is_empty() {
                return;
            }
            for released in batch {
                released.release(ReleaseContext::Disposed);
            }
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

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum ReleaseContext {
    /// This thread holds the isolate lock.
    Locked,
    /// The isolate is disposed. Only the counts may be released: an unwrapped `Wrappable` can
    /// still point to the dead isolate.
    Disposed,
}

/// The parts of a dropped [`jsg::Rc`](crate::Rc) or [`jsg::Weak`](crate::Weak).
pub trait Releasable: 'static {
    fn release(self, context: ReleaseContext);
}

struct Released(Box<dyn FnOnce(ReleaseContext)>);

impl Released {
    fn new<T: Releasable>(released: T) -> Self {
        Self(Box::new(move |context| released.release(context)))
    }

    fn release(self, context: ReleaseContext) {
        (self.0)(context);
    }

    fn release_locked(self) {
        self.release(ReleaseContext::Locked);
    }
}

#[expect(
    unsafe_code,
    reason = "queues references for the thread that next locks the isolate"
)]
// SAFETY: the parts are `!Send` only because their counts are not atomic. Queuing touches no
// count, and queued parts are released either under the isolate lock or, after disposal, by the
// one draining thread.
unsafe impl Send for Released {}

/// What a handle dropped after the state is closed needs.
#[derive(Clone, Copy, PartialEq, Eq)]
enum AfterClose {
    /// V8 may write into the slot of a weak handle until the isolate is disposed.
    KeepUntilDisposed,
    /// The handle is strong, so V8 never writes into its slot.
    Free,
}

/// Resets the strong handle of a dropped `Global` or `TracedGlobal` in its slot, then frees the
/// slot, which V8 forgets with the handle. The isolate must be locked by this thread.
#[expect(unsafe_code, reason = "resets a V8 handle")]
#[expect(
    clippy::boxed_local,
    reason = "the handle is reset in the box, whose address V8 may have recorded"
)]
fn reset_and_free(mut slot: Box<GlobalSlot>) {
    // SAFETY: the handle is valid and the isolate is locked (guaranteed by caller).
    unsafe { reset(slot.strong.get_mut()) };
}

/// Resets a persistent handle where it is.
///
/// # Safety
/// The handle must be valid or empty, and its isolate locked by this thread.
#[expect(unsafe_code, reason = "resets a V8 handle")]
unsafe fn reset(handle: &mut ffi::Global) {
    // SAFETY: guaranteed by caller. The handle is not moved while pinned.
    unsafe { ffi::global_reset(Pin::new_unchecked(handle)) };
}
