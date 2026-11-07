// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Lending what lives on lol-html's stack to code that runs while lol-html waits for it.
//!
//! lol-html passes a handler a `&mut` token that is valid only while the handler runs, and the
//! handler calls JavaScript, which reaches the token through a resource it holds. A resource
//! can't hold a borrow, so it holds the token in a [`TokenSlot`], which [`TokenSlot::lend`] fills
//! for the duration of a closure and empties when the closure returns or unwinds. A slot's clones
//! share it, so that the handler can empty it while the resource lives on: when lol-html runs on
//! a fiber, JavaScript runs on the thread's main stack, and the slot stays filled until the
//! handler's promise settles. In the same way, a rewriter's handlers reach the
//! [`Dispatcher`](crate::rewriter::Dispatcher) of the call that is running through a
//! [`DispatcherSlot`].
//!
//! A slot is `Send` and `Sync`, as the resources and rewriter state that hold one must be, since
//! the isolate lock passes between threads. What a slot holds lives on the stack of the thread
//! that filled it, and lol-html's tokens aren't `Send`, so a slot lends what it holds only to
//! that thread: on any other, it is empty.

#![allow(
    unsafe_code,
    reason = "erases the lifetimes of borrows lent for the duration of a call"
)]

use std::ptr::NonNull;
use std::sync::Arc;
use std::sync::Mutex;
use std::sync::MutexGuard;
use std::sync::PoisonError;
use std::thread::ThreadId;

use lol_html::html_content::Comment;
use lol_html::html_content::Doctype;
use lol_html::html_content::DocumentEnd;
use lol_html::html_content::EndTag;
use lol_html::html_content::TextChunk;

use crate::rewriter::Dispatcher;
use crate::rewriter::Element;

/// A lol-html token type, and the same type with `'static` lifetimes, which a [`TokenSlot`]
/// holds.
///
/// # Safety
///
/// `Static` must be `Self` with every lifetime parameter replaced by `'static`.
pub unsafe trait Lendable {
    type Static: 'static;
}

// SAFETY: `Static` is `Self` with `'static` lifetimes.
unsafe impl Lendable for Element<'_, '_> {
    type Static = Element<'static, 'static>;
}
// SAFETY: as above.
unsafe impl Lendable for EndTag<'_> {
    type Static = EndTag<'static>;
}
// SAFETY: as above.
unsafe impl Lendable for TextChunk<'_> {
    type Static = TextChunk<'static>;
}
// SAFETY: as above.
unsafe impl Lendable for Comment<'_> {
    type Static = Comment<'static>;
}
// SAFETY: as above.
unsafe impl Lendable for Doctype<'_> {
    type Static = Doctype<'static>;
}
// SAFETY: as above.
unsafe impl Lendable for DocumentEnd<'_> {
    type Static = DocumentEnd<'static>;
}

/// Locks `mutex`, which is held only to read or replace what a slot holds, so is never poisoned.
fn lock_mutex<T>(mutex: &Mutex<T>) -> MutexGuard<'_, T> {
    mutex.lock().unwrap_or_else(PoisonError::into_inner)
}

/// What a slot holds: a pointer, and the thread that lent it.
struct Lent<T: ?Sized> {
    thread: ThreadId,
    pointer: NonNull<T>,
}

// SAFETY: a slot dereferences its `Lent` only on the thread that lent it, while the lending call
// runs there; other threads only compare the thread or drop the pointer, which owns nothing.
unsafe impl<T: ?Sized> Send for Lent<T> {}

impl<T: ?Sized> Lent<T> {
    fn new(pointer: NonNull<T>) -> Self {
        Self {
            thread: std::thread::current().id(),
            pointer,
        }
    }

    fn is_here(&self) -> bool {
        self.thread == std::thread::current().id()
    }
}

/// Empties a slot when dropped, including when the closure it was lent to unwinds.
struct Empty<'a, T: ?Sized>(&'a Mutex<Option<Lent<T>>>);

impl<T: ?Sized> Drop for Empty<'_, T> {
    fn drop(&mut self) {
        *lock_mutex(self.0) = None;
    }
}

/// A token that lol-html lent for the duration of a handler call, or nothing. Clones share the
/// slot.
pub struct TokenSlot<T: 'static> {
    token: Arc<Mutex<Option<Lent<T>>>>,
}

impl<T: 'static> Clone for TokenSlot<T> {
    fn clone(&self) -> Self {
        Self {
            token: Arc::clone(&self.token),
        }
    }
}

impl<T: 'static> TokenSlot<T> {
    pub fn empty() -> Self {
        Self {
            token: Arc::new(Mutex::new(None)),
        }
    }

    /// Lends `token` to [`Self::with`] on this thread while `f` runs.
    ///
    /// # Panics
    ///
    /// Panics if the slot holds a token already: each token gets a slot of its own.
    pub fn lend<X, R>(&self, token: &mut X, f: impl FnOnce() -> R) -> R
    where
        X: Lendable<Static = T>,
    {
        {
            let mut slot = lock_mutex(&self.token);
            assert!(slot.is_none(), "a token slot was lent a second token");
            // The pointer is derived from `token` and used only until `f` returns, while `token`
            // is not used, and the slot is emptied before `token` can be used again, including
            // when `f` unwinds. `X` and `T` differ only in lifetimes (`Lendable`'s contract), so
            // the pointer refers to a valid `T` for as long as it is in the slot.
            *slot = Some(Lent::new(NonNull::from(token).cast::<T>()));
        }
        let _empty = Empty(&*self.token);
        f()
    }

    /// Runs `f` with the lent token, or returns `None` if the slot is empty or another thread
    /// lent it.
    ///
    /// The token has `'static` lifetimes but lives only until the slot is emptied, so nothing
    /// borrowed from it may outlive `f`. The slot is empty while `f` runs, so a nested `with`
    /// gets `None`.
    pub fn with<R>(&self, f: impl FnOnce(&mut T) -> R) -> Option<R> {
        let lent = take_here(&self.token)?;
        let mut restore = Restore(&*self.token, Some(lent));
        let Some(lent) = restore.1.as_mut() else {
            unreachable!("the token was just taken");
        };
        // SAFETY: the slot held the pointer, lent on this thread, so `lend` is still running here
        // with the token unused, and the pointer was taken out of the slot, so this is its only
        // use until `restore` puts it back.
        Some(f(unsafe { lent.pointer.as_mut() }))
    }
}

/// Takes what `slot` holds if this thread lent it.
fn take_here<T: ?Sized>(slot: &Mutex<Option<Lent<T>>>) -> Option<Lent<T>> {
    let mut slot = lock_mutex(slot);
    if slot.as_ref().is_some_and(Lent::is_here) {
        slot.take()
    } else {
        None
    }
}

/// Puts back what [`take_here`] took when dropped.
struct Restore<'a, T: ?Sized>(&'a Mutex<Option<Lent<T>>>, Option<Lent<T>>);

impl<T: ?Sized> Drop for Restore<'_, T> {
    fn drop(&mut self) {
        *lock_mutex(self.0) = self.1.take();
    }
}

/// The dispatcher of the `write()` or `end()` call that is running, or nothing.
pub struct DispatcherSlot {
    dispatcher: Mutex<Option<Lent<dyn Dispatcher>>>,
}

impl DispatcherSlot {
    pub const fn empty() -> Self {
        Self {
            dispatcher: Mutex::new(None),
        }
    }

    /// Lends `dispatcher` to [`Self::with`] on this thread while `f` runs.
    ///
    /// # Panics
    ///
    /// Panics if the slot holds a dispatcher already: a rewriter runs one call at a time.
    pub fn lend<'a, R>(
        &self,
        dispatcher: &'a mut (dyn Dispatcher + 'a),
        f: impl FnOnce() -> R,
    ) -> R {
        {
            let mut slot = lock_mutex(&self.dispatcher);
            assert!(
                slot.is_none(),
                "a dispatcher slot was lent a second dispatcher"
            );
            let dispatcher = NonNull::from(dispatcher);
            // SAFETY: only the lifetime bound of the trait object changes. The pointer is used
            // only until `f` returns, while `dispatcher` is not used, and the slot is emptied
            // before then, including when `f` unwinds.
            let dispatcher = unsafe {
                std::mem::transmute::<NonNull<dyn Dispatcher + 'a>, NonNull<dyn Dispatcher>>(
                    dispatcher,
                )
            };
            *slot = Some(Lent::new(dispatcher));
        }
        let _empty = Empty(&self.dispatcher);
        f()
    }

    /// Runs `f` with the lent dispatcher, or returns `None` if the slot is empty or another thread
    /// lent it. The slot is empty while `f` runs.
    pub fn with<R>(&self, f: impl FnOnce(&mut dyn Dispatcher) -> R) -> Option<R> {
        let lent = take_here(&self.dispatcher)?;
        let mut restore = Restore(&self.dispatcher, Some(lent));
        let Some(lent) = restore.1.as_mut() else {
            unreachable!("the dispatcher was just taken");
        };
        // SAFETY: the slot held the pointer, lent on this thread, so `lend` is still running
        // here with the dispatcher unused, and the pointer was taken out of the slot, so this is
        // its only use until `restore` puts it back.
        Some(f(unsafe { lent.pointer.as_mut() }))
    }
}
