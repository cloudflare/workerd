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
use std::pin::Pin;
use std::sync::Arc;
use std::sync::Mutex;
use std::sync::MutexGuard;
use std::sync::PoisonError;
use std::task::Poll;
use std::task::Waker;
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
        Ok(time_from_unix_nanos(bridge::io_context_now(self.id)?))
    }

    /// The current request's clock, as [`IoContext::now`] reads it, or `None` outside a request,
    /// as C++ `dateNow()` reads it.
    ///
    /// It needs no lock, for code that holds none, such as a `Drop` impl: the current request
    /// belongs to the thread, as for C++ `IoContext::tryCurrent()`. Where a lock is at hand, use
    /// [`IoContext::now`], which reports the errors that this turns into `None`.
    #[must_use]
    pub fn now_in_current_request() -> Option<SystemTime> {
        let id = Option::from(bridge::io_context_try_current_id())?;
        bridge::io_context_now(id).ok().map(time_from_unix_nanos)
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
    /// A `jsg::Rc` or `jsg::Weak` it holds can only be dropped, since every other use takes a
    /// [`jsg::Lock`], which the future doesn't have.
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

    /// Waits for `promise` as part of this request, then resolves the returned future with what
    /// `map` returns when called with the value it is fulfilled with, as C++
    /// `IoContext::awaitJs(js, promise, func)` does. `map` runs in the reaction to the promise,
    /// under the isolate lock. It is `Copy`, so that what it captures holds no GC references, and
    /// `Send`, as the reactions' data must be.
    ///
    /// The future fails with the promise's rejection, or with what `map` throws, as C++ fails
    /// with what `func` throws: what JavaScript threw in `map`, or the exception that throwing
    /// `map`'s own error would throw ([`jsg::Lock::throw_exception`]), as C++ converts a KJ
    /// exception from `func` to JavaScript. An internal error is redacted and logged as
    /// [`jsg::Lock::throw_internal_error`] does, rather than as C++ `jsg::exceptionToJs()` logs a
    /// KJ exception, with its origin and for Sentry. The reaction first logs that value as an
    /// uncaught exception from [`UncaughtExceptionSource::InternalAsync`], as
    /// [`log_uncaught_exception`](Self::log_uncaught_exception) does, then tunnels it as
    /// [`jsg::Lock::exception_to_kj`] does. If either runs JavaScript that throws (a getter of the
    /// rejection's `stack`, say), the reaction rejects with that exception, and the future fails
    /// as for a promise that never settles, as in C++. As in C++, the promise must settle in
    /// this request: its reactions are not run in another, and if the promise is
    /// garbage-collected without settling, or this `IoContext` is destroyed first, the future
    /// fails with an `Error` whose message is `Promise will never complete.`. If the isolate has
    /// excessively exceeded its heap limit by then, it fails instead with C++
    /// `IoContext::memoryLimitException()`, an overloaded `Error` whose message is `Worker has
    /// exceeded memory limit.`, with the detail that marks an out-of-memory outcome.
    ///
    /// If the request has async tracing, the wait is recorded as C++ records it: as a `JS_TO_KJ`
    /// resource named `awaitJs`, from C++ `IoContext::makeAwaitJsResource()`, which the reaction
    /// settles as `OK` once the promise is fulfilled and `map` succeeds, or the promise is
    /// fulfilled after the wait was cancelled, and as `ERROR` once the promise rejects or `map`
    /// fails. A wait that never settles reports a `destroy` instead, once the half that settles it
    /// is dropped.
    ///
    /// The reactions hold the half of the wait that settles it through an [`IoOwned`], which
    /// gives it the lifetime C++ gives `AwaitJsFulfiller` through `IoOwn`:
    ///
    /// - A reaction takes it only in the request that started the wait. In any other, taking it
    ///   fails with the error C++ throws for an I/O object of another request, which rejects the
    ///   promise the reaction returns, as in C++.
    /// - Dropping the reactions' data drops it through C++ `IoOwn`, so at once if the request
    ///   that owns it is current, and otherwise through the request's delete queue, the next time
    ///   it runs. The request drops it when it is destroyed.
    ///
    /// # Errors
    /// Fails with an internal error if this `IoContext` is no longer the current one, and with an
    /// error for which [`jsg::Error::is_pending`] is true if V8 fails to register the reactions
    /// or execution is terminating.
    //
    // TODO(soon): Join the wait with the request's abort.
    pub fn await_js<T, M>(
        &self,
        lock: &mut jsg::Lock<'s>,
        promise: impl Into<v8::Local<'s, v8::Promise>>,
        map: M,
    ) -> jsg::Result<AwaitJs<T>>
    where
        T: Send + 'static,
        M: Copy
            + Send
            + 'static
            + for<'a> Fn(&mut jsg::Lock<'a>, v8::Local<'a, v8::Value>) -> jsg::Result<T>,
    {
        if lock.is_execution_terminating() {
            return Err(jsg::Error::pending());
        }
        let shared = Arc::new(AwaitJsShared(Mutex::new(AwaitJsState::Waiting(None))));
        let sender = self.add_object(AwaitJsSender {
            shared: Arc::clone(&shared),
            isolate: bridge::io_context_isolate_weak_ref(self.id)?,
            async_trace: bridge::io_context_make_await_js_resource(self.id)?.into(),
        })?;
        promise.into().then_or_catch(
            lock,
            AwaitJsReaction { sender, map },
            await_js_on_fulfilled::<T, M>,
            await_js_on_rejected::<T, M>,
        )?;
        Ok(AwaitJs { shared })
    }

    /// [`await_js`](Self::await_js) for a value that converts with [`jsg::FromJS`].
    ///
    /// # Errors
    /// As for `await_js`.
    pub fn await_js_value<T>(
        &self,
        lock: &mut jsg::Lock<'s>,
        promise: impl Into<v8::Local<'s, v8::Promise>>,
    ) -> jsg::Result<AwaitJs<T>>
    where
        T: for<'a> jsg::FromJS<'a, ResultType = T> + Send + 'static,
    {
        self.await_js(lock, promise, |lock, value| T::from_js(lock, value))
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

    /// Gives this `IoContext` ownership of `span`, a span of its request, as
    /// [`add_object`](Self::add_object) does for a value that is `Send`. The span then ends no
    /// later than the `IoContext` does, and on its thread.
    ///
    /// Hold a span this way when something whose lifetime the garbage collector decides, such
    /// as the reaction to a promise, keeps it. Such a holder can be dropped on whichever thread
    /// next locks the isolate, and a span reports to its request's observer, whose state belongs
    /// to the request's thread. The returned handle is `Send` and `Sync`, so that it can be
    /// held there, and dropped anywhere, as an [`IoOwned`] can: the span stays behind, for the
    /// `IoContext` to end.
    ///
    /// # Errors
    /// Fails with an internal error if this `IoContext` is no longer the current one.
    pub fn add_span(&self, span: SpanBuilder) -> jsg::Result<IoOwnedSpan> {
        Ok(IoOwnedSpan(self.add_object(ThreadBoundSpan(span))?))
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
    /// The `IoContext` is aborted with `reason` as [`jsg::Lock::exception_to_kj`] encodes it,
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

    /// Logs `exception`, a value JavaScript threw or rejected a promise with, as an uncaught
    /// exception from `source`, as C++ `IoContext::logUncaughtException(source, exception)` does
    /// through `Worker::Lock::logUncaughtException()`:
    ///
    /// - If the isolate has an inspector, reports it to the inspector as thrown, with `source`'s
    ///   text and a message that V8 creates for the value, so that it shows in the inspector's
    ///   console.
    /// - If KJ logs at INFO level (`workerd --verbose`), logs `uncaught exception` with `source`
    ///   and the value's `stack`, if the value is an object whose `stack` is not `undefined`, or
    ///   else the value itself, each as [`v8::Local::<v8::Value>::to_detail_string`] describes
    ///   it, up to its first NUL.
    ///
    /// C++ also adds an uncaught exception to the request's tail-worker trace, but not one from
    /// either [`UncaughtExceptionSource`]; see there.
    ///
    /// # Errors
    /// Fails with an internal error if this `IoContext` is no longer the current one. Fails with
    /// an error for which [`jsg::Error::is_pending`] is true:
    ///
    /// - if an exception or a termination is already pending, and the isolate has an inspector or
    ///   KJ logs at INFO level;
    /// - if reading `stack` runs JavaScript that throws (a getter, say), leaving that exception
    ///   pending, as C++ throws `JsExceptionThrown` then. The inspector has been told of
    ///   `exception` by then.
    pub fn log_uncaught_exception(
        &self,
        lock: &mut jsg::Lock<'s>,
        source: UncaughtExceptionSource,
        exception: v8::Local<'s, v8::Value>,
    ) -> jsg::Result<()> {
        let source = source.into_ffi();
        ffi::send_exception_to_inspector(lock, self.id, source, exception)?;
        if !bridge::io_context_should_log_uncaught_exceptions() {
            return Ok(());
        }
        if let Ok(object) = v8::Local::<v8::Object>::try_from(exception) {
            let stack = object.get(lock, "stack")?;
            if !stack.is_undefined() {
                let stack = describe_for_log(lock, stack)?;
                bridge::io_context_log_uncaught_exception_stack(source, &stack);
                return Ok(());
            }
        }
        let exception = describe_for_log(lock, exception)?;
        bridge::io_context_log_uncaught_exception_value(source, &exception);
        Ok(())
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

/// Where an uncaught exception comes from, as C++ `UncaughtExceptionSource` says.
///
/// [`IoContext::log_uncaught_exception`] gives the inspector and the log the source's text, such
/// as `Uncaught (in promise)`.
///
/// C++ has more sources, for the exceptions of handlers and tasks, whose uncaught exceptions it
/// also adds to the request's tail-worker trace (`addExceptionToTrace()`). It adds none from these
/// two, and `log_uncaught_exception` adds none at all, so only these two are offered.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum UncaughtExceptionSource {
    /// `INTERNAL`, `Uncaught`.
    Internal,
    /// `INTERNAL_ASYNC`, `Uncaught (in promise)`: a JavaScript promise that the runtime waits
    /// for rejected, as in [`IoContext::await_js`].
    InternalAsync,
}

impl UncaughtExceptionSource {
    const fn into_ffi(self) -> bridge::UncaughtExceptionSource {
        match self {
            Self::Internal => bridge::UncaughtExceptionSource::INTERNAL,
            Self::InternalAsync => bridge::UncaughtExceptionSource::INTERNAL_ASYNC,
        }
    }
}

/// `value` as C++ JSG describes a handle in a log (`kj::str(handle)`): its detail string, up to
/// its first NUL, since C++ copies it out of V8 as a C string.
fn describe_for_log<'s>(
    lock: &mut jsg::Lock<'s>,
    value: v8::Local<'s, v8::Value>,
) -> jsg::Result<String> {
    let mut detail = value.to_detail_string(lock)?;
    if let Some(nul) = detail.find('\0') {
        detail.truncate(nul);
    }
    Ok(detail)
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

/// What a wait of [`IoContext::await_js`] has come to, shared by its [`AwaitJs`] and its
/// [`AwaitJsSender`].
enum AwaitJsState<T> {
    /// Not settled. Holds the waker of the last poll, if any.
    Waiting(Option<Waker>),
    /// The promise was fulfilled with this value, converted.
    Fulfilled(T),
    /// The promise was rejected, or converting its value failed.
    Rejected(cxx::KjError),
    /// The sender was dropped without settling the wait.
    NeverSettled,
    /// The sender was dropped without settling the wait while the isolate had excessively
    /// exceeded its heap limit.
    MemoryLimitExceeded,
    /// The [`AwaitJs`] was dropped before the wait settled.
    Cancelled,
    /// The [`AwaitJs`] returned the outcome.
    Done,
}

struct AwaitJsShared<T>(Mutex<AwaitJsState<T>>);

impl<T> AwaitJsShared<T> {
    // Nothing that holds the lock calls into V8 or panics, so it is never poisoned, and a drop
    // in a GC finalizer cannot find it held by the code the finalizer interrupted.
    fn lock(&self) -> MutexGuard<'_, AwaitJsState<T>> {
        self.0.lock().unwrap_or_else(PoisonError::into_inner)
    }

    /// Settles the wait with `state` if it is waiting, then wakes its future.
    fn settle(&self, state: AwaitJsState<T>) {
        let waker = {
            let mut current = self.lock();
            let AwaitJsState::Waiting(waker) = &mut *current else {
                return;
            };
            let waker = waker.take();
            *current = state;
            waker
        };
        if let Some(waker) = waker {
            waker.wake();
        }
    }

    fn is_cancelled(&self) -> bool {
        matches!(*self.lock(), AwaitJsState::Cancelled)
    }

    fn is_waiting(&self) -> bool {
        matches!(*self.lock(), AwaitJsState::Waiting(_))
    }
}

/// The half of a wait of [`IoContext::await_js`] that settles it, which the request owns through
/// an [`IoOwned`].
struct AwaitJsSender<T> {
    shared: Arc<AwaitJsShared<T>>,
    /// The isolate of the request the wait was started in, weakly, as C++ `awaitJsImpl()` takes
    /// it, so that the wait never keeps the isolate alive. It is `Send`, as the sender must be.
    isolate: KjOwn<bridge::IsolateWeakRef>,
    /// The wait's async trace resource, or `None` if the request is not traced. Settled with the
    /// wait, as `AwaitJsFulfiller::settle()` does, and dropped with the sender, which reports a
    /// `destroy` if it never settled. It is `Send`, as the sender must be.
    ///
    /// A reaction takes the sender, so a settled resource is released from the tracker, which
    /// tells its sinks (`onRelease`), at the end of the reaction. C++ keeps its `AsyncResource`
    /// until both reactions are dropped, and releases it then, or not at all if the tracker has
    /// closed by then. Likewise, if the reaction fails before settling (logging the rejection
    /// throws, or execution terminates), the sender is dropped with the reaction, which reports
    /// the `destroy` then, where C++ reports it once the reactions are collected. No other event
    /// differs.
    async_trace: Option<KjOwn<bridge::AsyncResource>>,
}

impl<T> AwaitJsSender<T> {
    /// Settles the wait with `state`, then reports `outcome` for async tracing, in that order, as
    /// `AwaitJsFulfiller` does.
    fn settle(&self, state: AwaitJsState<T>, outcome: bridge::AsyncOutcome) {
        self.shared.settle(state);
        self.settle_trace(outcome);
    }

    /// Reports `outcome` for async tracing, if the request is traced. Only the first is reported.
    fn settle_trace(&self, outcome: bridge::AsyncOutcome) {
        if let Some(resource) = &self.async_trace {
            resource.settle(outcome);
        }
    }
}

impl<T> Drop for AwaitJsSender<T> {
    /// Settles the wait as never settling, as `~AwaitJsFulfiller()` does, unless it has settled
    /// or was cancelled. As there, whether the isolate has excessively exceeded its heap limit
    /// is decided now, and if it has, the wait fails with the memory-limit error instead. An
    /// isolate that is gone has not.
    ///
    /// The reactions' data is dropped by the garbage collector once the promise is unreachable,
    /// in a cppgc finalizer or the GC epilogue, so this must not enter V8, allocate on its heap,
    /// or panic. It only reads the isolate's limit enforcer, which does none of these (see
    /// `isolate_weak_ref_has_excessively_exceeded_heap_limit()`), sets the state and wakes the
    /// future, which builds the error once it is polled. Then the async trace resource is
    /// dropped, which reports to the tracker's sinks, as `~AwaitJsFulfiller()` destroys its
    /// `AsyncResource` in the same finalizers. Of workerd's sinks, only the inspector's calls V8,
    /// and it ignores `JS_TO_KJ` resources (`async-trace-inspector.c++`).
    fn drop(&mut self) {
        if !self.shared.is_waiting() {
            return;
        }
        let exceeded =
            match bridge::isolate_weak_ref_has_excessively_exceeded_heap_limit(&self.isolate) {
                Ok(exceeded) => exceeded,
                // The check is not expected to throw. If it does, the wait still fails, as never
                // settled. Logging enters no V8.
                Err(err) => {
                    bridge::io_context_log_error(&format!(
                        "checking the heap limit for an unsettled IoContext::await_js() failed: \
                         {err}"
                    ));
                    false
                }
            };
        self.shared.settle(if exceeded {
            AwaitJsState::MemoryLimitExceeded
        } else {
            AwaitJsState::NeverSettled
        });
    }
}

/// A future for the outcome of a JavaScript promise, from [`IoContext::await_js`].
///
/// It is `Send`, and is polled without the isolate lock, like the futures that
/// [`IoContext::await_io_and_run`] waits for. Dropping it before the promise settles cancels the
/// wait: the conversion then does not run.
pub struct AwaitJs<T> {
    shared: Arc<AwaitJsShared<T>>,
}

impl<T> Future for AwaitJs<T> {
    type Output = Result<T, cxx::KjError>;

    fn poll(self: Pin<&mut Self>, cx: &mut std::task::Context<'_>) -> Poll<Self::Output> {
        let state = {
            let mut state = self.shared.lock();
            if let AwaitJsState::Waiting(waker) = &mut *state {
                if waker.as_ref().is_some_and(|w| w.will_wake(cx.waker())) {
                    return Poll::Pending;
                }
                let replaced = waker.replace(cx.waker().clone());
                // Dropping a waker runs code that is not ours, so not under the lock.
                drop(state);
                drop(replaced);
                return Poll::Pending;
            }
            std::mem::replace(&mut *state, AwaitJsState::Done)
        };
        match state {
            AwaitJsState::Waiting(_) => unreachable!("returned above"),
            AwaitJsState::Fulfilled(value) => Poll::Ready(Ok(value)),
            AwaitJsState::Rejected(err) => Poll::Ready(Err(err)),
            AwaitJsState::NeverSettled => Poll::Ready(Err(cxx::KjError::new(
                cxx::KjExceptionType::Failed,
                format!("jsg.Error: {}", bridge::io_context_never_settled_message()),
            ))),
            AwaitJsState::MemoryLimitExceeded => Poll::Ready(Err(ffi::memory_limit_exception())),
            AwaitJsState::Cancelled | AwaitJsState::Done => {
                unreachable!("an AwaitJs polled after it returned its outcome")
            }
        }
    }
}

impl<T> Drop for AwaitJs<T> {
    fn drop(&mut self) {
        let mut state = self.shared.lock();
        if matches!(*state, AwaitJsState::Waiting(_)) {
            let waiting = std::mem::replace(&mut *state, AwaitJsState::Cancelled);
            // Its waker is dropped after the lock is released, as in `poll`.
            drop(state);
            drop(waiting);
        }
    }
}

/// The data of the reactions [`IoContext::await_js`] registers.
struct AwaitJsReaction<T, M> {
    sender: IoOwned<AwaitJsSender<T>>,
    map: M,
}

// A `Copy` `map` holds no `jsg::Rc` or `TracedGlobal`, and an `IoOwned` holds no JavaScript
// handles.
jsg::jsg_traced!(<T, M: Copy> AwaitJsReaction<T, M>);

/// The reaction to the fulfillment of the promise that [`IoContext::await_js`] waits for.
fn await_js_on_fulfilled<'a, T, M>(
    lock: &mut jsg::Lock<'a>,
    reaction: &AwaitJsReaction<T, M>,
    value: v8::Local<'a, v8::Value>,
) -> jsg::Result<v8::Local<'a, v8::Value>>
where
    T: Send + 'static,
    M: for<'b> Fn(&mut jsg::Lock<'b>, v8::Local<'b, v8::Value>) -> jsg::Result<T>,
{
    let Some(sender) = reaction.sender.take(lock)? else {
        unreachable!("only one reaction to a promise runs, once");
    };
    // As C++ does once the KJ promise is gone, `map` does not run for a cancelled wait, and the
    // trace still reports how the JavaScript promise settled.
    if sender.shared.is_cancelled() {
        sender.settle_trace(bridge::AsyncOutcome::OK);
        return Ok(lock.undefined());
    }
    // `map`'s own error is caught as the exception that throwing it throws, as C++
    // `jsg::Lock::tryCatch()` converts a KJ exception from `func`.
    match lock.try_catch(|lock| (reaction.map)(lock, value))? {
        Ok(value) => sender.settle(AwaitJsState::Fulfilled(value), bridge::AsyncOutcome::OK),
        Err(exception) => {
            let err = reject_with_js_exception(lock, exception)?;
            sender.settle(AwaitJsState::Rejected(err), bridge::AsyncOutcome::ERROR);
        }
    }
    Ok(lock.undefined())
}

/// The reaction to the rejection of the promise that [`IoContext::await_js`] waits for.
fn await_js_on_rejected<'a, T, M>(
    lock: &mut jsg::Lock<'a>,
    reaction: &AwaitJsReaction<T, M>,
    reason: v8::Local<'a, v8::Value>,
) -> jsg::Result<v8::Local<'a, v8::Value>> {
    let Some(sender) = reaction.sender.take(lock)? else {
        unreachable!("only one reaction to a promise runs, once");
    };
    // As in C++, the trace reports the rejection even if the wait was cancelled.
    let err = reject_with_js_exception(lock, reason)?;
    sender.settle(AwaitJsState::Rejected(err), bridge::AsyncOutcome::ERROR);
    Ok(lock.undefined())
}

/// The exception a wait of [`IoContext::await_js`] fails with for `exception`, which the promise
/// rejected with or `map` threw, as C++ `AwaitJsFulfiller::rejectWithJsException()` creates it:
/// logs `exception` as uncaught, then tunnels it. Fails with what JavaScript that either runs
/// throws, leaving it pending, before anything is tunneled.
fn reject_with_js_exception<'a>(
    lock: &mut jsg::Lock<'a>,
    exception: v8::Local<'a, v8::Value>,
) -> jsg::Result<cxx::KjError> {
    IoContext::current(lock)?.log_uncaught_exception(
        lock,
        UncaughtExceptionSource::InternalAsync,
        exception,
    )?;
    lock.exception_to_kj(exception)
}

/// A value an `IoContext` owns, from [`IoContext::add_object`].
///
/// Like C++ `IoOwn<T>`, which `IoContext::addObject()` returns, it ties the value to its
/// `IoContext`. Compared with `IoOwn`:
///
/// - **When the value is dropped:** the same, when the handle is dropped or the `IoContext` is
///   destroyed, unless [`take`](Self::take) took it first. As with `IoOwn`, dropping the handle
///   while its `IoContext` is not current leaves the value to be dropped later, by the
///   `IoContext`.
/// - **Access:** `IoOwn` is dereferenced; here the value is used in place with
///   [`with`](Self::with), or moved out with [`take`](Self::take). Both work only while the
///   `IoContext` is current, and fail otherwise, including once it has been destroyed.
/// - **Threads:** the value is shared with the `IoContext`, which drops it, unless it was taken,
///   on its own thread and without the isolate lock. The handle, held by a resource, may be
///   dropped wherever the resource is, such as by GC on another thread that has locked the
///   isolate. So a mutex guards the value, which must be `Send`. A span, which is not, has
///   [`IoOwnedSpan`].
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

    /// Calls `f` with the value, in place, and returns what it returns, or `None` if the value
    /// was taken. While `f` runs, the value is out of the handle: `f` sees `None` if it reaches
    /// the value through the handle again. If `f` panics, the value is dropped.
    ///
    /// # Errors
    /// Fails as [`take`](Self::take) does, unless the `IoContext` that owns the value is the
    /// current one.
    pub fn with<R>(
        &self,
        _lock: &mut jsg::Lock<'_>,
        f: impl FnOnce(&mut T) -> R,
    ) -> jsg::Result<Option<R>> {
        bridge::io_context_require_current(self.owner)?;
        // Out of the mutex while `f` runs, so that nothing panics while holding it.
        let Some(mut value) = lock_slot(&self.slot).take() else {
            return Ok(None);
        };
        let result = f(&mut value);
        // The IoContext drops the slot's value only when it is destroyed, or when this handle's
        // object is, neither of which can happen while it is current and this is borrowed.
        *lock_slot(&self.slot) = Some(value);
        Ok(Some(result))
    }
}

// Holds no JavaScript handles; see `IoContext::add_object`.
jsg::jsg_traced!(<T> IoOwned<T>);

/// The reference to an [`IoOwned`]'s value that its `IoContext` holds.
struct SlotRef<T>(Arc<Mutex<Option<T>>>);

/// A span an `IoContext` owns, from [`IoContext::add_span`].
///
/// It is an [`IoOwned`] for a value that is not `Send`. Unlike the span, the handle is `Send` and
/// `Sync`, since it reaches the span only on the `IoContext`'s thread.
pub struct IoOwnedSpan(IoOwned<ThreadBoundSpan>);

impl IoOwnedSpan {
    /// Takes the span, or returns `None` if it was already taken, as [`IoOwned::take`] does.
    ///
    /// # Errors
    /// As for [`IoOwned::take`].
    pub fn take(&self, lock: &mut jsg::Lock<'_>) -> jsg::Result<Option<SpanBuilder>> {
        Ok(self.0.take(lock)?.map(|span| span.0))
    }

    /// Calls `f` with the span, in place, as [`IoOwned::with`] does.
    ///
    /// # Errors
    /// As for [`IoOwned::with`].
    pub fn with<R>(
        &self,
        lock: &mut jsg::Lock<'_>,
        f: impl FnOnce(&mut SpanBuilder) -> R,
    ) -> jsg::Result<Option<R>> {
        self.0.with(lock, |span| f(&mut span.0))
    }
}

// Holds no JavaScript handles.
jsg::jsg_traced!(IoOwnedSpan);

/// The span of an [`IoOwnedSpan`].
struct ThreadBoundSpan(SpanBuilder);

#[expect(
    unsafe_code,
    reason = "a span that its IoContext owns is touched only on its thread; see the SAFETY comment"
)]
// SAFETY: a span belongs to its request's thread, which is the only one that touches it here.
// `add_span()` takes it on that thread, from code that holds it there, with the IoContext
// current. `IoOwnedSpan` reaches it only through `IoOwned::take()` and `IoOwned::with()`, while
// that IoContext is current, so on its thread, and `take()` hands out the `SpanBuilder`, which is
// not `Send`. Otherwise the IoContext drops it, through the `SlotRef` it holds, on its thread:
// when it is destroyed, or when it next runs after the handle's `IoOwn` is destroyed elsewhere,
// which only queues the deletion (`DeleteQueue::scheduleDeletion()` in io-own.c++). The handle's
// own reference to the slot may be dropped on another thread, but if it is the last one, that
// `SlotRef` has emptied the slot, and dropping `None` touches no span.
unsafe impl Send for ThreadBoundSpan {}

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

/// The time `nanos` nanoseconds from the Unix epoch, before it if negative.
fn time_from_unix_nanos(nanos: i64) -> SystemTime {
    let offset = Duration::from_nanos(nanos.unsigned_abs());
    if nanos < 0 {
        UNIX_EPOCH - offset
    } else {
        UNIX_EPOCH + offset
    }
}

#[cfg(test)]
#[path = "lib-test.rs"]
mod tests;
