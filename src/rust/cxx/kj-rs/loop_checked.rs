//! [`LoopChecked`], a `Send` wrapper for a value that belongs to a KJ event loop.

#![allow(unsafe_code, reason = "makes a loop's values `Send`")]

use std::cell::Cell;
use std::future::Future;
use std::marker::PhantomData;
use std::mem::ManuallyDrop;
use std::pin::Pin;
use std::task::Context;
use std::task::Poll;

use cxx::ExternType;
use cxx::KjException;

/// A `kj::EventLoop::Id`: the id of a KJ event loop, unique for the life of the process.
#[derive(Copy, Clone, PartialEq, Eq)]
#[repr(transparent)]
pub struct EventLoopId(usize);

impl EventLoopId {
    /// The id of the current thread's event loop.
    ///
    /// # Panics
    ///
    /// Panics if the thread has no event loop.
    fn current() -> Self {
        crate::ffi::event_loop_id_current()
    }

    /// Whether the event loop is the current thread's.
    fn is_current(self) -> bool {
        crate::ffi::event_loop_id_is_current(self)
    }

    /// Fails unless the event loop is the current thread's.
    fn require_current(self) -> Result<(), KjException> {
        if self.is_current() {
            Ok(())
        } else {
            crate::ffi::event_loop_id_wrong_loop()
        }
    }
}

// SAFETY: `loop-checked.c++` asserts that `kj_rs::EventLoopId` has this type's size and
// alignment, and is trivially copyable.
unsafe impl ExternType for EventLoopId {
    type Id = cxx::type_id!("::kj_rs::EventLoopId");
    type Kind = cxx::kind::Trivial;
}

/// A value that belongs to a KJ event loop, and may move between OS threads with that loop.
///
/// It is `Send`, so a `Send` future can hold it, even across an `.await`. Every use of the value
/// checks that its event loop is current, and dropping it off that loop aborts the process, as
/// destroying a KJ object there would be unsound. It is not `Sync`. Like a KJ async object, it
/// also aborts the process if dropped in a `kj::DisallowAsyncDestructorsScope`, as C++ JSG uses
/// to catch a JavaScript object that holds a KJ I/O object other than through an `IoOwn`.
///
/// It wraps a future too: polling it polls the future, and panics off the loop. The future may
/// borrow from its caller, as one that a C++ `async` function returns does, since it is always
/// dropped at once rather than on a later turn of the loop.
pub struct LoopChecked<T> {
    value: ManuallyDrop<T>,
    loop_id: EventLoopId,
    _not_sync: PhantomData<Cell<()>>,
}

// SAFETY: `value` is used and dropped only while the event loop of `loop_id` is current, and
// `new()`'s contract makes it sound to move between OS threads with that loop, whatever value it
// holds by then. Not being `Sync` keeps a `&T` from reaching another thread.
unsafe impl<T> Send for LoopChecked<T> {}

impl<T> LoopChecked<T> {
    /// Wraps `value`, which belongs to the current thread's event loop.
    ///
    /// # Safety
    ///
    /// `value` must belong to the current event loop: state it shares that is not thread-safe,
    /// such as a non-atomic refcount, it must share only with values that are used and dropped only
    /// while this event loop is current. A `kj::Refcounted` object whose other references belong
    /// to the loop meets this, and so does a `std::rc::Rc` whose every clone is held by such
    /// values.
    ///
    /// `value` must have no affinity to the OS thread: it must be sound to use and drop on
    /// whichever OS thread runs the current event loop, since an embedder may run an event loop on
    /// different OS threads between turns. So it must keep no thread-local state across a return
    /// to the event loop. A thread-local set and cleared within a turn, as for a
    /// `kj::DisallowAsyncDestructorsScope`, is fine, so KJ objects meet this as long as they keep
    /// no other. Values tied to their OS thread do not: a `std::sync::MutexGuard`, which must be
    /// unlocked on the thread that locked it, or a value with state that a `thread_local!` keeps
    /// between turns, such as an `Rc` with a clone in one.
    ///
    /// Both must hold for every value the `LoopChecked` holds, not only `value`: safe code can
    /// replace it through `get_mut()`, or change it through interior mutability from `get()`. So
    /// unless every value of `T` meets them, keep the `LoopChecked` where only code that keeps
    /// them can reach it, such as a private field of the type that wraps the value.
    ///
    /// # Panics
    ///
    /// Panics if the thread has no event loop, dropping `value`.
    pub unsafe fn new(value: T) -> Self {
        let loop_id = EventLoopId::current();
        Self {
            value: ManuallyDrop::new(value),
            loop_id,
            _not_sync: PhantomData,
        }
    }

    /// Whether the value's event loop is the current thread's.
    #[must_use]
    pub fn is_current(&self) -> bool {
        self.loop_id.is_current()
    }

    /// The value.
    ///
    /// # Errors
    ///
    /// Fails off the value's event loop.
    pub fn get(&self) -> Result<&T, KjException> {
        self.loop_id.require_current()?;
        Ok(&self.value)
    }

    /// The value.
    ///
    /// # Errors
    ///
    /// Fails off the value's event loop.
    pub fn get_mut(&mut self) -> Result<&mut T, KjException> {
        self.loop_id.require_current()?;
        Ok(&mut self.value)
    }

    /// The pinned value.
    ///
    /// # Errors
    ///
    /// Fails off the value's event loop.
    pub fn get_pin_mut(self: Pin<&mut Self>) -> Result<Pin<&mut T>, KjException> {
        self.loop_id.require_current()?;
        // SAFETY: `value` is structurally pinned: nothing moves it out of a pinned `Self`, since
        // `into_inner()` takes `Self` by value, and `drop()` drops it in place.
        Ok(unsafe { self.map_unchecked_mut(|this| &mut *this.value) })
    }

    /// Unwraps the value, or returns `self` off the value's event loop.
    ///
    /// # Errors
    ///
    /// Returns `self` off the value's event loop.
    pub fn into_inner(self) -> Result<T, Self> {
        if !self.is_current() {
            return Err(self);
        }
        let mut this = ManuallyDrop::new(self);
        // SAFETY: `this` is never used again, nor dropped, so `value` is taken once.
        Ok(unsafe { ManuallyDrop::take(&mut this.value) })
    }
}

impl<T> Drop for LoopChecked<T> {
    fn drop(&mut self) {
        if !self.is_current() {
            crate::ffi::loop_checked_dropped_off_loop();
            std::process::abort();
        }
        crate::ffi::require_async_destructors_allowed();
        // SAFETY: `value` is not used again.
        unsafe { ManuallyDrop::drop(&mut self.value) }
    }
}

impl<F: Future> Future for LoopChecked<F> {
    type Output = F::Output;

    /// # Panics
    ///
    /// Panics off the future's event loop.
    #[expect(
        clippy::panic,
        reason = "polling off its event loop is a bug in the caller"
    )]
    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<F::Output> {
        match self.get_pin_mut() {
            Ok(future) => future.poll(cx),
            Err(err) => panic!("{}", err.what()),
        }
    }
}

#[cfg(test)]
#[path = "loop_checked-test.rs"]
mod tests;
