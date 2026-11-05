// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The `IoContext` of the current request, for APIs implemented in Rust.
//!
//! `IoContext` (`src/workerd/io/io-context.h`) holds the state of the request that JavaScript is
//! running on behalf of. This crate is a package of its own because `//src/workerd/io` depends on
//! `jsg`.

mod ffi;
mod trace;

use std::any::Any;
use std::ffi::CStr;
use std::future::Future;
use std::marker::PhantomData;
use std::panic::AssertUnwindSafe;
use std::sync::Arc;
use std::sync::Mutex;
use std::sync::MutexGuard;
use std::sync::PoisonError;
use std::task::Poll;
use std::time::Duration;
use std::time::SystemTime;
use std::time::UNIX_EPOCH;

use ffi::bridge;
use jsg::v8;
use kj_rs::KjOwn;
pub use trace::SpanBuilder;
pub use trace::TagValue;
pub use trace::TraceContext;

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
    /// `IoContext`, with a new lock. It wraps C++ `IoContext::awaitIoAndRun()`, which shares its
    /// implementation with `IoContext::awaitIo()`, and behaves as it does:
    ///
    /// - `future` is polled by this `IoContext`'s thread's event loop, without the isolate lock
    ///   and with no `IoContext` current. Only its wake-ups may come from other threads. It must
    ///   not block, which would stall every request on the thread.
    /// - `callback` runs in the async context frame that was current when this was called, the
    ///   one the I/O was started in, rather than whichever is current when `future` completes.
    ///   In a Durable Object it waits for the input gate, or runs in the critical section that
    ///   was current when this was called.
    /// - While it waits, the request is neither finished nor hung. The wait is recorded for async
    ///   tracing.
    ///
    /// If this `IoContext` is destroyed first, both are dropped without `callback` running. In a
    /// Durable Object, the `IoContext` can outlive the request that called this.
    ///
    /// Unlike `awaitIo()`, this returns no promise: it sits a level below an `await_io` that
    /// would return one, as `awaitIo()` does. `callback` settles what it needs to, such as a
    /// promise, itself, and a failure rejects nothing. If `callback` fails, or `future` or
    /// `callback` panics, the failure is handled as for a task of C++ `IoContext::addTask()`: it
    /// is logged as an uncaught exception and recorded in the request's `waitUntil()` status. A
    /// panic in `callback` also terminates JavaScript execution, as in a `#[jsg_method]`.
    ///
    /// `future` and its output are `Send` because the future is polled without the isolate lock.
    /// That keeps values bound to the isolate, such as a `jsg::Rc`, out of it.
    ///
    /// # Errors
    /// Fails with an internal error if this `IoContext` is no longer the current one.
    pub fn await_io_and_run<T, F, C>(&self, future: F, callback: C) -> jsg::Result<()>
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
                .unwrap_or_else(|_| unreachable!("the output of await_io_and_run()'s future"));
            callback(lock, *output)
        }));
        Ok(bridge::io_context_await_io_and_run(
            self.id,
            Box::new(future),
            Box::new(callback),
        )?)
    }

    /// Waits for `future` as part of this request, then returns a promise for what `callback`
    /// returns when called with its output, as C++ `IoContext::awaitIo()` does. Waiting and
    /// running `callback` work as in [`await_io_and_run`](Self::await_io_and_run).
    ///
    /// The returned promise settles in a reaction, as in C++, which costs a microtask:
    ///
    /// - `Ok(value)` resolves it. A promise or thenable is followed.
    /// - `Err(error)` rejects it with the exception that throwing `error` would throw, created in
    ///   the reaction so that V8 records an async stack trace.
    /// - An exception that JavaScript threw in `callback` rejects it.
    /// - A panic in `future` rejects it with an internal error, as a KJ exception from the I/O
    ///   does for C++ `awaitIo()`. Execution continues.
    /// - A panic in `callback`, or execution terminating in it, settles nothing and fails the
    ///   task, as for `await_io_and_run`.
    ///
    /// If this `IoContext` is destroyed first, the promise never settles.
    ///
    /// # Errors
    /// Fails with an internal error if this `IoContext` is no longer the current one, and leaves
    /// an exception pending if V8 fails to create the promise. Nothing is waited for then.
    pub fn await_io<T, F, C>(
        &self,
        lock: &mut jsg::Lock<'s>,
        future: F,
        callback: C,
    ) -> jsg::Result<v8::Local<'s, v8::Promise>>
    where
        T: Send + 'static,
        F: Future<Output = T> + Send + 'static,
        C: for<'t> FnOnce(&mut jsg::Lock<'t>, T) -> jsg::Result<v8::Local<'t, v8::Value>> + 'static,
    {
        let resolver = v8::PromiseResolver::new(lock)?;
        let outcome = CallbackOutcome::default();
        let promise = resolver
            .promise()
            .then(lock, outcome.clone(), return_or_throw)?;
        let resolver = resolver.to_global(lock);
        // `catch_poll_panic()` turns a panic while polling into an output, so that it rejects the
        // promise instead of failing the task as `io_future_wait()` would. `io_future_wait()`
        // still catches a panic while dropping the future when the wait is cancelled.
        self.await_io_and_run(catch_poll_panic(future), move |lock, output| {
            let resolver = resolver.as_local(lock);
            let result = match output {
                Ok(output) => {
                    // Only the exceptions that JavaScript threw are caught here. An error that
                    // `callback` returned is thrown by the reaction instead.
                    let result = lock.try_catch(|lock| match callback(lock, output) {
                        Err(err) if err.is_pending() => Err(err),
                        result => Ok(result),
                    })?;
                    match result {
                        Ok(result) => result.map(|value| value.to_global(lock)),
                        Err(exception) => return resolver.reject(lock, exception),
                    }
                }
                Err(message) => Err(jsg::Error::new_internal_error(format!(
                    "the future that IoContext::await_io() waited for panicked: {message}"
                ))),
            };
            outcome.set(result);
            let undefined = lock.undefined();
            resolver.resolve(lock, undefined)
        })?;
        Ok(promise)
    }

    /// [`await_io`](Self::await_io) for a future whose output converts to the promise's value:
    /// `Ok(value)` resolves the promise with `value` converted by [`jsg::ToJS`], and `Err(error)`
    /// rejects it.
    ///
    /// # Errors
    /// As for `await_io`.
    pub fn await_io_value<T, E, F>(
        &self,
        lock: &mut jsg::Lock<'s>,
        future: F,
    ) -> jsg::Result<v8::Local<'s, v8::Promise>>
    where
        T: for<'t> jsg::ToJS<'t> + Send + 'static,
        E: Into<jsg::Error> + Send + 'static,
        F: Future<Output = Result<T, E>> + Send + 'static,
    {
        self.await_io(lock, future, |lock, output| {
            output.map_err(Into::into)?.to_js(lock)
        })
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

    /// Has the request wait for `promise` after the response, as `ctx.waitUntil()` does through
    /// C++ `IoContext::awaitJs()` and `IoContext::addWaitUntil()`: the request is not done until
    /// `promise` settles. If `promise` rejects, the rejection is logged as an uncaught exception,
    /// as for every JavaScript promise that C++ waits for, and the response is unaffected.
    ///
    /// As with `awaitJs()`, `promise` must settle in this request. Its reactions run in the
    /// request that settles it, or, under the `handle_cross_request_promise_resolution`
    /// compatibility flag, in the one it was created in. If that is another request, the
    /// reaction fails there, as an unhandled rejection, and the wait fails with "Promise will
    /// never complete.", which is logged as an uncaught exception. So does the wait for a
    /// promise that is garbage-collected without settling.
    ///
    /// To take a value of any type, as `ctx.waitUntil()` does, take a [`jsg::v8::VoidPromise`]
    /// argument, and pass it on as it is. It converts the value as C++ JSG converts the
    /// `kj::Promise<void>` argument, before the method runs and so before it looks up the
    /// `IoContext`.
    ///
    /// # Errors
    /// Fails with an internal error if this `IoContext` is no longer the current one. Fails with
    /// an error for which [`jsg::Error::is_pending`] is true if V8 does, leaving the exception
    /// pending (see [`jsg::v8::Promise`]), or if execution is terminating.
    pub fn add_wait_until(
        &self,
        lock: &mut jsg::Lock<'s>,
        promise: impl Into<jsg::v8::Local<'s, jsg::v8::Promise>>,
    ) -> jsg::Result<()> {
        // JavaScript can't call `ctx.waitUntil()` while terminating, but Rust can call this after
        // catching a termination. `awaitJs()` would then create objects, which fails a V8 debug
        // check, and the request ends anyway.
        if lock.is_execution_terminating() {
            return Err(jsg::Error::pending());
        }
        ffi::add_wait_until(lock, self.id, promise.into())
    }

    /// Has the request fail open, as `ctx.passThroughOnException()` does through C++
    /// `IoContext::setFailOpen()`: if the handler then fails before it has sent a response, the
    /// request is passed on to the origin, as if there were no Worker, rather than answered with
    /// an error.
    ///
    /// # Errors
    /// Fails with an internal error if this `IoContext` is no longer the current one.
    pub fn set_fail_open(&self) -> jsg::Result<()> {
        Ok(bridge::io_context_set_fail_open(self.id)?)
    }

    /// Aborts the request, as `ExecutionContext`'s `ctx.abort(reason)` does, then terminates
    /// JavaScript execution, and returns the error that carries the termination; return it from
    /// the method that called this (see [`jsg::Lock::terminate_execution_now`]).
    ///
    /// The `IoContext` is aborted with `reason` as C++ `jsg::Lock::exceptionToKj()` encodes it,
    /// or, without one, with an `Error` whose message is `Worker execution was aborted due to
    /// call to ctx.abort().`, through C++ `IoContext::abort()`, so the request fails with that
    /// exception.
    ///
    /// `ctx.abort()` terminates right after aborting, so this does too, and no caller can forget
    /// to. `state.abort()` in a Durable Object aborts with an exception of its own, after breaking
    /// storage, so it can't use this.
    ///
    /// # Errors
    /// Always returns an error, for which [`jsg::Error::is_pending`] is true unless this
    /// `IoContext` is no longer the current one, or the C++ shim otherwise fails with a KJ
    /// exception, when it is an internal error and nothing is terminated. Nothing is aborted
    /// or terminated if encoding `reason` runs JavaScript that throws (a getter of its
    /// `overloaded` or `retryable` property, say), whose exception the error then stands for, or
    /// if execution is already terminating.
    #[must_use = "return the error, so that the termination reaches the code that started the \
                  JavaScript"]
    pub fn abort(
        &self,
        lock: &mut jsg::Lock<'s>,
        reason: Option<jsg::v8::Local<'s, jsg::v8::Value>>,
    ) -> jsg::Error {
        // JavaScript can't call `ctx.abort()` while terminating, but Rust can call this after
        // catching a termination. Encoding a reason would then create objects, and terminating
        // again would enter V8, both of which fail V8 debug checks, and the request ends anyway.
        if lock.is_execution_terminating() {
            return jsg::Error::pending();
        }
        match ffi::abort(lock, self.id, reason) {
            Ok(()) => lock.terminate_execution_now(),
            Err(err) => err,
        }
    }

    /// Starts a span of internal tracing, as C++ `IoContext::makeTraceSpan()` does. It is a child
    /// of the span current in the async context frame, or of the request's span, and ends when
    /// the returned [`SpanBuilder`] is dropped. If the request is not traced, it records nothing.
    ///
    /// # Errors
    /// Fails with an internal error if this `IoContext` is no longer the current one.
    pub fn make_trace_span(&self, name: &'static CStr) -> jsg::Result<SpanBuilder> {
        SpanBuilder::start(self.id, name)
    }

    /// Starts a span of both internal and user tracing, as C++ `IoContext::makeUserTraceSpan()`
    /// does, which ends when the returned [`TraceContext`] is dropped. If the request has async
    /// tracing, the span is an operation, which the next [`IoContext::await_io_and_run`] in the
    /// same turn reports its wait under, unless [`TraceContext::detach_async`] is called first.
    ///
    /// # Errors
    /// Fails with an internal error if this `IoContext` is no longer the current one.
    pub fn make_user_trace_span(&self, name: &'static CStr) -> jsg::Result<TraceContext> {
        TraceContext::start(self.id, name)
    }
}

/// What the callback of [`IoContext::await_io`] returned, carried to the reaction that returns or
/// throws it. As in C++, the promise the reaction is registered on resolves with no value, so that
/// only the promise the reaction returns is resolved with the callback's value: resolving both
/// would read a thenable's `then` twice. The callback can be dropped on its event loop and the
/// reaction on any thread that holds the isolate lock, so the outcome is behind an `Arc` and a
/// mutex.
#[derive(Clone, Default)]
struct CallbackOutcome(Arc<Mutex<Option<jsg::Result<v8::Global<v8::Value>>>>>);

impl CallbackOutcome {
    fn set(&self, outcome: jsg::Result<v8::Global<v8::Value>>) {
        *lock_slot(&self.0) = Some(outcome);
    }

    fn take(&self) -> Option<jsg::Result<v8::Global<v8::Value>>> {
        lock_slot(&self.0).take()
    }
}

// Holds no handle that GC traces: the value's `Global` is strong, and is taken by the reaction,
// which runs in the microtask checkpoint after the callback.
jsg::jsg_traced!(CallbackOutcome);

/// The reaction to the promise that [`IoContext::await_io`] resolves once its callback has
/// returned: returns the callback's value, or throws its error. Throwing in a reaction gives the
/// exception an async stack trace, as C++ `throwOrReturnResult()` does.
fn return_or_throw<'a>(
    lock: &mut jsg::Lock<'a>,
    outcome: &CallbackOutcome,
    _undefined: v8::Local<'a, v8::Value>,
) -> jsg::Result<v8::Local<'a, v8::Value>> {
    let Some(outcome) = outcome.take() else {
        unreachable!("the promise is resolved only once the outcome is set")
    };
    Ok(outcome?.as_local(lock))
}

/// Runs `future`, turning a panic while polling it into `Err` with the panic's message. `future`
/// is dropped before this completes, so that a panic while dropping it is caught as well; that one
/// is logged, and the output is kept.
async fn catch_poll_panic<F: Future>(future: F) -> Result<F::Output, String> {
    let mut future = std::pin::pin!(Some(future));
    let output = std::future::poll_fn(|cx| {
        let Some(inner) = future.as_mut().as_pin_mut() else {
            unreachable!("dropped only once the output is ready")
        };
        match std::panic::catch_unwind(AssertUnwindSafe(|| inner.poll(cx))) {
            Ok(poll) => poll.map(Ok),
            Err(payload) => Poll::Ready(Err(ffi::panic_message(&*payload).to_owned())),
        }
    })
    .await;
    if let Err(payload) = std::panic::catch_unwind(AssertUnwindSafe(|| future.set(None))) {
        bridge::io_context_log_error(&format!(
            "dropping the future that IoContext::await_io() waited for panicked: {}",
            ffi::panic_message(&*payload)
        ));
    }
    output
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

// Holds no JavaScript handles; see `IoContext::add_object`.
jsg::jsg_traced!(<T> IoOwned<T>);

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
