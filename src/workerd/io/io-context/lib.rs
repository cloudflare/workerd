// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The `IoContext` of the current request, for APIs implemented in Rust.
//!
//! `IoContext` (`src/workerd/io/io-context.h`) holds the state of the request that JavaScript is
//! running on behalf of. This crate is a package of its own because `//src/workerd/io` depends on
//! `jsg`.

mod ffi;

use std::any::Any;
use std::future::Future;
use std::marker::PhantomData;
use std::sync::Arc;
use std::sync::Mutex;
use std::sync::MutexGuard;
use std::sync::PoisonError;
use std::time::Duration;
use std::time::SystemTime;
use std::time::UNIX_EPOCH;

use ffi::bridge;
use kj_rs::KjOwn;

/// The `IoContext` of the current request.
///
/// It holds the process-unique id of the C++ `IoContext` (`IoContext::getId()`), not a reference
/// to it. Each method uses the current `IoContext`, and fails unless it is the one this handle
/// identifies. Ids are never reused, so a handle can never reach another request's `IoContext`,
/// or one that has been destroyed. A handle can be used only with the [`jsg::Lock`] it was
/// obtained with, and is neither `Send` nor `Sync`.
pub struct IoContext<'s> {
    id: u64,
    _lock: PhantomData<(&'s (), *const ())>,
}

impl<'s> IoContext<'s> {
    /// Returns the `IoContext` of the current request.
    ///
    /// The closure that `jsg::modules::add_builtin` runs to create a builtin module's exports
    /// never sees a request, even when a request triggers it, so this fails there.
    ///
    /// # Errors
    /// Outside a request, such as in a Worker's global scope, fails with the error that C++
    /// `IoContext::current()` throws, which explains that the operation needs a request.
    pub fn current(lock: &mut jsg::Lock<'s>) -> jsg::Result<Self> {
        Self::try_current(lock)
            .ok_or_else(|| jsg::Error::new_error(bridge::io_context_no_request_message()))
    }

    /// Returns the `IoContext` of the current request, or `None` outside a request.
    #[must_use]
    pub fn try_current(_lock: &mut jsg::Lock<'s>) -> Option<Self> {
        Option::from(bridge::io_context_try_current_id()).map(|id| Self {
            id,
            _lock: PhantomData,
        })
    }

    /// Returns the current time, as the request observes it, like `Date.now()` does in
    /// JavaScript but to the nanosecond. Mitigating Spectre, the clock advances only when
    /// JavaScript is waiting for I/O, unless the `precise_timers` compatibility flag is on.
    ///
    /// # Errors
    /// Fails with an internal error if this `IoContext` is no longer the current one, or is
    /// handling no request, which only happens while it is not running JavaScript.
    pub fn now(&self) -> jsg::Result<SystemTime> {
        let nanos = bridge::io_context_now(self.id)?;
        let offset = Duration::from_nanos(nanos.unsigned_abs());
        Ok(if nanos < 0 {
            UNIX_EPOCH - offset
        } else {
            UNIX_EPOCH + offset
        })
    }

    /// Sends `message`, one serialized Cap'n Proto message (as `capnp::serialize::write_message()`
    /// writes it), to logfwdr channel `channel`. The request waits for the write, as it does for
    /// `ctx.waitUntil()`, and the write makes no delivery guarantees. In a Durable Object, the
    /// message is sent only once the output gate opens, that is, once the storage writes made
    /// before it are confirmed.
    ///
    /// # Errors
    /// Fails if `message` is not exactly one valid serialized message, and with an internal error
    /// if this `IoContext` is no longer the current one.
    pub fn write_logfwdr(&self, channel: u32, message: &[u8]) -> jsg::Result<()> {
        Ok(bridge::io_context_write_logfwdr(self.id, channel, message)?)
    }

    /// Waits for `future` as part of this request, then calls `callback` with its output in this
    /// `IoContext`, with a new lock. It shares its implementation with C++
    /// `IoContext::awaitIo()`, and behaves as it does:
    ///
    /// - `future` is polled by this `IoContext`'s thread's event loop, without the isolate lock
    ///   and with no `IoContext` current. Only its wake-ups may come from other threads. It must
    ///   not block, which would stall every request on the thread.
    /// - `callback` runs in the async context frame that was current when `await_io` was called,
    ///   the one the I/O was started in, rather than whichever is current when `future`
    ///   completes. In a Durable Object it waits for the input gate, or runs in the critical
    ///   section that was current when `await_io` was called.
    /// - While it waits, the request is neither finished nor hung. The wait is recorded for async
    ///   tracing.
    ///
    /// If this `IoContext` is destroyed first, both are dropped without `callback` running. In a
    /// Durable Object, the `IoContext` can outlive the request that called `await_io`.
    ///
    /// `callback` settles what it needs to, such as a promise, itself. If it fails, or `future`
    /// or `callback` panics, the failure is handled as for a task of C++ `IoContext::addTask()`:
    /// it is logged as an uncaught exception and recorded in the request's `waitUntil()` status.
    /// A panic in `callback` also terminates JavaScript execution, as in a `#[jsg_method]`.
    ///
    /// `future` and its output are `Send` because the future is polled without the isolate lock.
    /// That keeps values bound to the isolate, such as a `jsg::Rc`, out of it.
    ///
    /// # Errors
    /// Fails with an internal error if this `IoContext` is no longer the current one.
    pub fn await_io<T, F, C>(&self, future: F, callback: C) -> jsg::Result<()>
    where
        T: Send + 'static,
        F: Future<Output = T> + Send + 'static,
        C: for<'t> FnOnce(&mut jsg::Lock<'t>, T) -> jsg::Result<()> + 'static,
    {
        let future = ffi::IoFuture(Box::pin(async move {
            let output: Box<dyn Any + Send> = Box::new(future.await);
            output
        }));
        let callback = ffi::IoCallback(Box::new(move |lock, output| {
            // `io_future_wait()` produced `output` from this `F`.
            let output = output
                .downcast::<T>()
                .unwrap_or_else(|_| unreachable!("the output of await_io()'s future"));
            callback(lock, *output)
        }));
        Ok(bridge::io_context_await_io(
            self.id,
            Box::new(future),
            Box::new(callback),
        )?)
    }

    /// Gives this `IoContext` ownership of `value`, as C++ `IoContext::addObject()` does: it is
    /// dropped when the `IoContext` is destroyed, or when the returned handle is dropped, unless
    /// [`IoOwned::take`] has taken it. How the value is reached differs from C++; see
    /// [`IoOwned`].
    ///
    /// `value` should hold no JavaScript handles: nothing traces it.
    ///
    /// # Errors
    /// Fails with an internal error if this `IoContext` is no longer the current one.
    pub fn add_object<T: Send + 'static>(&self, value: T) -> jsg::Result<IoOwned<T>> {
        let slot = Arc::new(Mutex::new(Some(value)));
        let object = bridge::io_context_add_object(
            self.id,
            Box::new(ffi::IoOwnedSlot(Box::new(SlotRef(Arc::clone(&slot))))),
        )?;
        Ok(IoOwned {
            slot,
            owner: self.id,
            _object: object,
        })
    }
}

/// A value an `IoContext` owns, from [`IoContext::add_object`].
///
/// Like C++ `IoOwn<T>`, which `IoContext::addObject()` returns, it ties the value to its
/// `IoContext`, but the value can only be taken out, not used in place. Compared with `IoOwn`:
///
/// - **When the value is dropped:** the same, when the handle is dropped or the `IoContext` is
///   destroyed, unless [`take`](Self::take) took it first. As with `IoOwn`, dropping the handle
///   while its `IoContext` is not current leaves the value to be dropped later, by the
///   `IoContext`.
/// - **Access:** `IoOwn` is dereferenced in place; here the value is moved out with
///   [`take`](Self::take). Both work only while the `IoContext` is current, and fail otherwise,
///   including once it has been destroyed.
/// - **Threads:** the value is shared with the `IoContext`, which drops it, unless it was taken,
///   on its own thread and without the isolate lock. The handle, held by a resource, may be
///   dropped wherever the resource is, such as by GC on another thread that has locked the
///   isolate. So a mutex guards the value, which must be `Send`.
pub struct IoOwned<T> {
    slot: Arc<Mutex<Option<T>>>,
    // The id of the IoContext that owns the value.
    owner: u64,
    // Dropping it drops the value, if the IoContext has not already.
    _object: KjOwn<bridge::IoOwnedObject>,
}

impl<T> IoOwned<T> {
    /// Takes the value, or returns `None` if it was already taken.
    ///
    /// # Errors
    /// Fails unless the `IoContext` that owns the value is the current one, with the error C++
    /// `IoContext` throws for an I/O object used by another request, as dereferencing an `IoOwn`
    /// does.
    pub fn take(&self, _lock: &mut jsg::Lock<'_>) -> jsg::Result<Option<T>> {
        bridge::io_context_require_current(self.owner)?;
        Ok(lock_slot(&self.slot).take())
    }
}

/// Holds no JavaScript handles; see [`IoContext::add_object`].
impl<T> jsg::Traced for IoOwned<T> {}

/// The reference to an [`IoOwned`]'s value that its `IoContext` holds.
struct SlotRef<T>(Arc<Mutex<Option<T>>>);

impl<T> Drop for SlotRef<T> {
    fn drop(&mut self) {
        let value = lock_slot(&self.0).take();
        drop(value);
    }
}

fn lock_slot<T>(slot: &Mutex<Option<T>>) -> MutexGuard<'_, Option<T>> {
    // Nothing panics while holding the lock: the value is taken out of it and dropped after.
    slot.lock().unwrap_or_else(PoisonError::into_inner)
}

#[cfg(test)]
#[path = "lib-test.rs"]
mod tests;
