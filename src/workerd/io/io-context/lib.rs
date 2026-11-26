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
use std::cell::Cell;
use std::ffi::CStr;
use std::future::Future;
use std::marker::PhantomData;
use std::panic::AssertUnwindSafe;
use std::pin::Pin;
use std::rc::Rc;
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

    /// The id that identifies this `IoContext` to C++, for the C++ shims of other crates: they
    /// find the C++ `IoContext` with `io_context::currentWithId()` (`current-with-id.h`), which
    /// fails unless it is still the current one, as every method here does.
    #[must_use]
    pub const fn as_ffi(&self) -> u64 {
        self.id
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
    /// - `future` is first polled in this call, under the isolate lock and with this `IoContext`
    ///   current, as the KJ promise passed to C++ `awaitIo()` has already run up to its first
    ///   wait. Later polls run on this `IoContext`'s thread's event loop, without the isolate
    ///   lock and with no `IoContext` current. Only its wake-ups may come from other threads. It
    ///   must not block, which would stall every request on the thread.
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
    /// `future` and its output are `Send` because the future's later polls run without the
    /// isolate lock.
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

    /// Waits for `promise` as part of this request, as C++ `IoContext::awaitJs(js, promise, func)`
    /// does, and returns a future for the outcome, which can be waited for without the isolate
    /// lock. Dropping the future cancels the wait: `map` then does not run.
    ///
    /// Once `promise` is fulfilled, `map` runs with its value, in the reaction to the promise and
    /// under the isolate lock, and the future resolves with what `map` returns. `map` is `Copy`,
    /// so that what it captures holds no GC references, and `Send`, as the reaction's data must
    /// be. What it returns holds no GC references either ([`jsg::HoldsNoGcReferences`]), since
    /// nothing traces it while the future holds it. For a conversion that needs the `IoContext`, or
    /// whose output is not `Send`, use [`await_js_local`](Self::await_js_local).
    ///
    /// The future fails with a [`cxx::KjException`], so that an exception C++ created reaches C++
    /// as it is:
    ///
    /// - If `promise` rejects or `map` fails, with that exception, tunneled as
    ///   [`jsg::Lock::exception_to_kj`] does, after it is logged as an uncaught exception from
    ///   [`UncaughtExceptionSource::InternalAsync`].
    /// - If this `IoContext` is aborted before the wait settles, with the exception it was
    ///   aborted with. If it was already aborted when the wait started, the abort fails the wait
    ///   in a later turn, unless the promise settles first.
    /// - If the wait never settles, because `promise` is garbage-collected first, this
    ///   `IoContext` is destroyed first, or logging or tunneling the exception throws, with an
    ///   `Error` whose message is `Promise will never complete.`, or, if the isolate has
    ///   excessively exceeded its heap limit by then, with C++
    ///   `IoContext::memoryLimitException()`.
    ///
    /// The promise must settle in this request: a reaction that runs in another one rejects the
    /// promise it returns, and settles nothing.
    ///
    /// If the request has async tracing, the wait is recorded as a `JS_TO_KJ` resource named
    /// `awaitJs`, as C++ records it.
    ///
    /// # Errors
    /// Fails with an internal error if this `IoContext` is no longer the current one, and with an
    /// error for which [`jsg::Error::is_pending`] is true if V8 fails to register the reactions
    /// or execution is terminating.
    //
    // TODO(someday): The join with the request's abort relies on C++ `IoContext::onAbort()`,
    // through an `AbortWatcher`; a Rust IoContext will need its own abort signal.
    pub fn await_js<T, M>(
        &self,
        lock: &mut jsg::Lock<'s>,
        promise: impl Into<v8::Local<'s, v8::Promise>>,
        map: M,
    ) -> jsg::Result<AwaitJs<T>>
    where
        T: jsg::HoldsNoGcReferences + 'static,
        M: Copy
            + Send
            + 'static
            + for<'a> Fn(&mut jsg::Lock<'a>, v8::Local<'a, v8::Value>) -> jsg::Result<T>,
    {
        if lock.is_execution_terminating() {
            return Err(jsg::Error::pending());
        }
        let shared = Arc::new(AwaitJsShared(Mutex::new(AwaitJsState::Waiting(None))));
        // Weak, so that the watcher, which the sender holds, doesn't keep the wait alive.
        let weak = Arc::downgrade(&shared);
        let on_abort = ffi::AbortCallback(Box::new(move |err| {
            if let Some(shared) = weak.upgrade() {
                shared.settle(AwaitJsState::Rejected(err));
            }
        }));
        let sender = self.add_object(AwaitJsSender {
            shared: Arc::clone(&shared),
            isolate: bridge::io_context_isolate_weak_ref(self.id)?,
            async_trace: bridge::io_context_make_await_js_resource(self.id)?.into(),
            abort_watcher: ThreadBoundAbortWatcher(bridge::io_context_watch_abort(
                self.id,
                Box::new(on_abort),
            )?),
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
        T: for<'a> jsg::FromJS<'a, ResultType = T> + jsg::HoldsNoGcReferences + 'static,
    {
        self.await_js(lock, promise, |lock, value| T::from_js(lock, value))
    }

    /// [`await_js`](Self::await_js) for a conversion that runs with this `IoContext` and may
    /// produce a value local to its thread, as the continuation that C++ `awaitJs()` waits for
    /// does when it is wrapped in `IoContext::addFunctor()`: for example a `KjOwn<Frankenvalue>`,
    /// a `KjRc<SubrequestChannel>`, or a C++ struct that holds them, which are neither `Send` nor
    /// `Sync`. Nothing moves to another thread: the conversion, the future and its output all stay
    /// on this `IoContext`'s.
    ///
    /// The wait is that of `await_js`, with the same failures and cancellation, and the
    /// conversion runs as `await_js`'s `map` does, with this `IoContext` as well. A reaction that
    /// runs while this `IoContext` is not the current one, as for a promise that another request
    /// resolves, fails before the conversion runs, and the wait never settles; it fails once GC
    /// collects the reactions or this `IoContext` is destroyed, as when C++ `awaitJs()` waits for
    /// such a promise.
    ///
    /// The returned future is neither `Send` nor `Sync`, so it is polled and dropped on this
    /// `IoContext`'s thread, as the future a reentry callback's function returns is (see
    /// [`make_reentry_callback_weak`](Self::make_reentry_callback_weak)). The output need only be
    /// `'static`, as a reentry callback's is. Nothing traces it, so a `jsg::Rc` or `v8::Global` in
    /// it is a strong reference, held untraced until the future's caller drops it.
    ///
    /// # Errors
    /// As for `await_js`.
    pub fn await_js_local<T, M>(
        &self,
        lock: &mut jsg::Lock<'s>,
        promise: impl Into<v8::Local<'s, v8::Promise>>,
        map: M,
    ) -> jsg::Result<AwaitJsLocal<T>>
    where
        T: 'static,
        M: Copy
            + Send
            + 'static
            + for<'a> Fn(
                &mut jsg::Lock<'a>,
                &IoContext<'a>,
                v8::Local<'a, v8::Value>,
            ) -> jsg::Result<T>,
    {
        let wait = self.await_js(lock, promise, move |lock, value| {
            // The reaction has taken its sender, which succeeds only in this IoContext.
            let context = IoContext::current(lock)?;
            Ok(ThreadBound::new(map(lock, &context, value)?))
        })?;
        Ok(AwaitJsLocal {
            wait,
            _thread: PhantomData,
        })
    }

    /// Registers `on_fulfilled` to run in this `IoContext` when `promise` is fulfilled, and
    /// returns the promise for its result, as C++ `promise.then(js, ioctx.addFunctor(func))`
    /// does. A rejection passes through without running it, in whatever `IoContext` is current.
    ///
    /// `on_fulfilled` gets this `IoContext`, as `addFunctor()` passes the current `IoContext` to
    /// a functor that takes one, then `data`, then the value. It must capture nothing, as for
    /// [`v8::Local::<v8::Promise>::then`].
    ///
    /// As with `addFunctor()`, the reaction fails without running `on_fulfilled` if it runs while
    /// another `IoContext`, or none, is current, and rejects the returned promise. That happens,
    /// without the `handle_cross_request_promise_resolution` compatibility flag, when another
    /// request settles the promise. With the flag, V8 runs the reaction in this `IoContext`
    /// whichever request created or settles the promise, or never if this `IoContext` is gone.
    /// The error is the one C++ throws for an I/O object of another request, whether another
    /// `IoContext` or none is current, without the " (I/O type: ...)" suffix the C++ one has.
    ///
    /// Unlike `addFunctor()`, which gives the `IoContext` ownership of the continuation, `data` is
    /// held by the reaction, which the garbage collector traces, so it may hold `jsg::Rc`s and
    /// `TracedGlobal`s, and a cycle through them can be collected. It is dropped once a reaction
    /// has run, in whatever `IoContext` is current then: by a rejection that passes through, or a
    /// reaction that fails, it may be dropped in another request's `IoContext`, or in none.
    /// Otherwise it is dropped when the garbage collector collects the reactions, rather than when
    /// the `IoContext` is destroyed. So an I/O object, which a functor of `addFunctor()` would
    /// capture, goes in `data` inside an [`IoOwned`] (see [`IoContext::add_object`]), which drops
    /// it in its `IoContext` wherever the handle is dropped.
    ///
    /// # Errors
    /// Fails unless this `IoContext` is the current one, as `addFunctor()` does, or, leaving an
    /// exception pending, if V8 does; see [`v8::Local::<v8::Promise>::then`].
    pub fn then<D, F>(
        &self,
        lock: &mut jsg::Lock<'s>,
        promise: impl Into<v8::Local<'s, v8::Promise>>,
        data: D,
        on_fulfilled: F,
    ) -> jsg::Result<v8::Local<'s, v8::Promise>>
    where
        D: jsg::Traced + 'static,
        F: Copy
            + 'static
            + for<'a> Fn(
                &mut jsg::Lock<'a>,
                &IoContext<'a>,
                &D,
                v8::Local<'a, v8::Value>,
            ) -> jsg::Result<v8::Local<'a, v8::Value>>,
    {
        const {
            assert!(
                size_of::<F>() == 0,
                "on_fulfilled must not capture anything"
            );
        };
        let data = self.in_context(data)?;
        promise
            .into()
            .then(lock, data, move |lock, data: &InContext<D>, value| {
                let context = data.require_current(lock)?;
                on_fulfilled(lock, &context, &data.data, value)
            })
    }

    /// Registers `on_rejected` to run in this `IoContext` when `promise` is rejected, and returns
    /// the promise for its result, as C++ `promise.catch_(js, ioctx.addFunctor(func))` does. A
    /// fulfillment passes through without running it, in whatever `IoContext` is current.
    ///
    /// Like [`IoContext::then`] otherwise, with the rejection reason in place of the value.
    ///
    /// # Errors
    /// Fails unless this `IoContext` is the current one, as `addFunctor()` does, or, leaving an
    /// exception pending, if V8 does; see [`v8::Local::<v8::Promise>::catch`].
    pub fn catch<D, R>(
        &self,
        lock: &mut jsg::Lock<'s>,
        promise: impl Into<v8::Local<'s, v8::Promise>>,
        data: D,
        on_rejected: R,
    ) -> jsg::Result<v8::Local<'s, v8::Promise>>
    where
        D: jsg::Traced + 'static,
        R: Copy
            + 'static
            + for<'a> Fn(
                &mut jsg::Lock<'a>,
                &IoContext<'a>,
                &D,
                v8::Local<'a, v8::Value>,
            ) -> jsg::Result<v8::Local<'a, v8::Value>>,
    {
        const { assert!(size_of::<R>() == 0, "on_rejected must not capture anything") };
        let data = self.in_context(data)?;
        promise
            .into()
            .catch(lock, data, move |lock, data: &InContext<D>, exception| {
                let context = data.require_current(lock)?;
                on_rejected(lock, &context, &data.data, exception)
            })
    }

    /// Registers `on_fulfilled` and `on_rejected` to run in this `IoContext` when `promise`
    /// settles, and returns the promise for the result of whichever runs, as C++
    /// `promise.then(js, ioctx.addFunctor(func), ioctx.addFunctor(errorFunc))` does.
    ///
    /// Like [`IoContext::then`] otherwise: either callback gets this `IoContext`, then `data`,
    /// which they share, then the value or the rejection reason, and neither runs while another
    /// `IoContext`, or none, is current.
    ///
    /// # Errors
    /// Fails unless this `IoContext` is the current one, as `addFunctor()` does, or, leaving an
    /// exception pending, if V8 does; see [`v8::Local::<v8::Promise>::then_or_catch`].
    pub fn then_or_catch<D, F, R>(
        &self,
        lock: &mut jsg::Lock<'s>,
        promise: impl Into<v8::Local<'s, v8::Promise>>,
        data: D,
        on_fulfilled: F,
        on_rejected: R,
    ) -> jsg::Result<v8::Local<'s, v8::Promise>>
    where
        D: jsg::Traced + 'static,
        F: Copy
            + 'static
            + for<'a> Fn(
                &mut jsg::Lock<'a>,
                &IoContext<'a>,
                &D,
                v8::Local<'a, v8::Value>,
            ) -> jsg::Result<v8::Local<'a, v8::Value>>,
        R: Copy
            + 'static
            + for<'a> Fn(
                &mut jsg::Lock<'a>,
                &IoContext<'a>,
                &D,
                v8::Local<'a, v8::Value>,
            ) -> jsg::Result<v8::Local<'a, v8::Value>>,
    {
        const {
            assert!(
                size_of::<F>() == 0,
                "on_fulfilled must not capture anything"
            );
        };
        const { assert!(size_of::<R>() == 0, "on_rejected must not capture anything") };
        let data = self.in_context(data)?;
        promise.into().then_or_catch(
            lock,
            data,
            move |lock, data: &InContext<D>, value| {
                let context = data.require_current(lock)?;
                on_fulfilled(lock, &context, &data.data, value)
            },
            move |lock, data: &InContext<D>, exception| {
                let context = data.require_current(lock)?;
                on_rejected(lock, &context, &data.data, exception)
            },
        )
    }

    /// The data of reactions that run in this `IoContext`, which must be the current one, as
    /// `addFunctor()` requires.
    fn in_context<D>(&self, data: D) -> jsg::Result<InContext<D>> {
        bridge::io_context_require_current(self.id)?;
        Ok(InContext {
            data,
            owner: self.id,
        })
    }

    /// Gives this `IoContext` ownership of `value`, as C++ `IoContext::addObject()` does: it is
    /// dropped when the `IoContext` is destroyed, or when the returned handle is dropped, unless
    /// [`IoOwned::take`] has taken it. How the value is reached differs from C++; see
    /// [`IoOwned`].
    ///
    /// `value` must hold no JavaScript handles ([`jsg::HoldsNoGcReferences`]): nothing traces it.
    ///
    /// # Errors
    /// Fails with an internal error if this `IoContext` is no longer the current one.
    pub fn add_object<T: jsg::HoldsNoGcReferences + 'static>(
        &self,
        value: T,
    ) -> jsg::Result<IoOwned<T>> {
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
    /// `promise` settles. The wait is an [`await_js`](Self::await_js), whose future C++
    /// `addWaitUntil()` waits for. If `promise` rejects, the rejection is logged as an uncaught
    /// exception, as for every JavaScript promise that C++ waits for, and the response is
    /// unaffected.
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
        // catching a termination. `await_js()` fails then, before creating anything, which would
        // fail a V8 debug check, and the request ends anyway.
        //
        // As C++ `PromiseWrapper` converts the promise to a `kj::Promise<void>`, the value is
        // discarded.
        let wait = self.await_js(lock, promise, |_lock, _value| Ok(()))?;
        let future = ffi::WaitUntilFuture(Box::pin(wait));
        Ok(bridge::io_context_add_wait_until(
            self.id,
            Box::new(future),
        )?)
    }

    /// Returns a callback that re-enters this `IoContext` each time it is called, from KJ rather
    /// than from JavaScript, and runs `function` there, as the callback of C++
    /// `IoContext::makeReentryCallbackWeak()` does: it is one, around `function`. Use it to hand a
    /// JavaScript callback to code that calls it later, such as a C++ `kj::Function` given to
    /// I/O; see [`ReentryCallback`].
    ///
    /// The callback holds this `IoContext` weakly. Unlike C++ `makeReentryCallback()`'s, it adds
    /// no task, so holding it neither keeps the request alive nor, in a Durable Object, blocks
    /// hibernation. It does hold a pending event, as C++'s does, so the request is not treated as
    /// hung while the callback exists. `function` is dropped when the callback is and no call of
    /// it is still running in `IoContext::run()`, which keeps it alive until it returns, or when
    /// this `IoContext` is destroyed, whichever is first.
    ///
    /// Each call runs `function` in C++ `IoContext::run()`, in the critical section that was
    /// current when this was called, if any, with a new lock and the `IoContext`. What `function`
    /// returns is waited for as part of the request, and the call's future completes with its
    /// output. To wait for a JavaScript promise, return the future of an
    /// [`await_js_local`](Self::await_js_local), whose conversion runs in this
    /// `IoContext` or not at all, and can build an output that belongs to its thread, as the
    /// continuation C++ wraps in `addFunctor()` does; the future can be returned as it is.
    ///
    /// If `function` fails, the call fails with a KJ exception, as a C++ function that `run()`
    /// runs does:
    ///
    /// - With an exception that JavaScript threw, logged as uncaught from
    ///   [`UncaughtExceptionSource::Internal`], then tunneled as [`jsg::Lock::exception_to_kj`]
    ///   does.
    /// - With an error it returned, tunneled as the exception throwing it would throw, and not
    ///   logged, as a KJ exception from C++ is not.
    /// - If execution is terminating, with the exception C++ `run()` fails with then: the
    ///   exceeded limit, or the exception the request was aborted with.
    /// - If it panics, with an internal error, and execution is terminated, as for
    ///   [`await_io_and_run`](Self::await_io_and_run).
    ///
    /// Neither `function`, nor the futures it returns, nor their output need be `Send`, so the
    /// output can hold C++ objects that belong to this `IoContext`'s thread. C++ holds `function`
    /// in this `IoContext` and runs and drops it only on its thread, KJ polls the futures there,
    /// and the output reaches only the call's future, which is not `Send` either. The futures are
    /// polled and dropped without the isolate lock, and are `'static`, as `function` is, so they
    /// can't hold a `v8::Local` of a lock; a `v8::Global` or `jsg::Rc` they hold can only be
    /// dropped, which needs no lock.
    ///
    /// # Example
    ///
    /// An API that keeps a JavaScript callback returning a Worker's code, for C++ to call later,
    /// as `WorkerLoader.get()` does. The conversion runs in this `IoContext` when the code's
    /// promise is fulfilled, and builds an output that is not `Send`, here an `Rc`; for
    /// `WorkerLoader.get()`, the `DynamicWorkerSource` with the Worker's `env` as a
    /// `KjOwn<Frankenvalue>` and its outbound channels. The `ok-reentry-callback` compile test
    /// builds this example.
    ///
    /// ```ignore
    /// #[jsg_struct]
    /// pub struct WorkerCode {
    ///     pub main_module: String,
    ///     pub compatibility_date: String,
    /// }
    ///
    /// pub fn keep<'s>(
    ///     lock: &mut jsg::Lock<'s>,
    ///     get_code: v8::Local<'s, v8::Function>,
    /// ) -> jsg::Result<ReentryCallback<Rc<WorkerCode>>> {
    ///     let get_code = get_code.to_global(lock);
    ///     IoContext::current(lock)?.make_reentry_callback_weak(move |lock, context| {
    ///         let code = get_code.as_local(lock).call(lock, None, &[])?;
    ///         let code = v8::Promise::resolved(lock, code)?;
    ///         context.await_js_local(lock, code, |lock, _context, value| {
    ///             Ok(Rc::new(WorkerCode::from_js(lock, value)?))
    ///         })
    ///     })
    /// }
    /// ```
    ///
    /// # Errors
    /// Fails with an internal error if this `IoContext` is no longer the current one.
    pub fn make_reentry_callback_weak<T, Fut, F>(
        &self,
        function: F,
    ) -> jsg::Result<ReentryCallback<T>>
    where
        T: 'static,
        Fut: Future<Output = Result<T, cxx::KjException>> + 'static,
        F: for<'t> FnMut(&mut jsg::Lock<'t>, &IoContext<'t>) -> jsg::Result<Fut> + 'static,
    {
        let mut function = function;
        let function = ffi::ReentryFn(Box::new(move |lock| {
            let context = IoContext::current(lock)?;
            let future = function(lock, &context)?;
            let future: ffi::ReentryFuture = Box::pin(async move {
                let output: Box<dyn Any> = Box::new(future.await?);
                Ok(output)
            });
            Ok(future)
        }));
        let callback = bridge::io_context_make_reentry_callback_weak(self.id, Box::new(function))?;
        Ok(ReentryCallback {
            callback,
            _output: PhantomData,
        })
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
jsg::jsg_traced_unchecked!(CallbackOutcome);

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
    /// The promise was rejected, converting its value failed, or the request was aborted first.
    Rejected(cxx::KjException),
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
    /// The watcher of the request's abort, which settles the wait with the exception the request
    /// is aborted with, as the `onAbort()` branch of C++'s `exclusiveJoin()` does. It holds the
    /// wait weakly, and is dropped with the sender, on the request's thread: in the reaction that
    /// settles the wait, when GC collects the reactions, or when the request is destroyed. So it
    /// is not dropped when the wait is cancelled, unlike C++'s branch, but it then settles nothing.
    /// It is a plain `KjOwn` rather than an [`IoOwned`]: the request may drop the sender while it
    /// unlinks the sender from its owned objects, and the sender must not then reentrantly unlink
    /// another owned object from that list.
    abort_watcher: ThreadBoundAbortWatcher,
}

// The sender's own fields are C++ objects and an `Arc` of the wait's state, which holds GC
// references only if `T` does, which the macro bounds out.
jsg::jsg_traced_unchecked!(<T> AwaitJsSender<T>);

/// The abort watcher of an [`AwaitJsSender`].
struct ThreadBoundAbortWatcher(KjOwn<bridge::AbortWatcher>);

#[expect(
    unsafe_code,
    reason = "a sender's abort watcher is touched only on its IoContext's thread; see the SAFETY \
              comment"
)]
// SAFETY: an `AbortWatcher` must be used and destroyed on its `IoContext`'s thread, while the
// `IoContext`'s abort exception is alive. Only an `AwaitJsSender` holds one, and the sender is
// `Send` only because `IoContext::add_object` requires it. `await_js()` creates the watcher on
// the `IoContext`'s thread, with it current. The sender reaches it only from a reaction that has
// taken the sender through `IoOwned::take()`, which requires the `IoContext` to be current, or
// when it is dropped: by that reaction, or by the `IoContext`, through the `SlotRef` it holds, on
// its thread and before its abort exception. The handle's own reference to the slot may be
// dropped on another thread, but if it is the last one, that `SlotRef` has emptied the slot, and
// dropping `None` touches no watcher.
unsafe impl Send for ThreadBoundAbortWatcher {}

impl<T> AwaitJsSender<T> {
    /// Settles the wait with `state`, then reports `outcome` for async tracing, in that order, as
    /// `AwaitJsFulfiller` does.
    fn settle(&self, state: AwaitJsState<T>, outcome: bridge::AsyncOutcome) {
        self.shared.settle(state);
        self.settle_trace(outcome);
    }

    /// Settles the wait with the exception the request was aborted with, if it was aborted after
    /// the wait started, as C++'s `exclusiveJoin()` has such an abort win over whatever settles
    /// the wait after it, even before the abort watcher has called back. A reaction calls this
    /// before it settles the wait, and so does dropping the sender unsettled.
    fn settle_with_pending_abort(&self) {
        if let Some(err) = ffi::pending_abort(&self.abort_watcher.0) {
            self.shared.settle(AwaitJsState::Rejected(err));
        }
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
    /// and it ignores `JS_TO_KJ` resources (`async-trace-inspector.c++`). The abort watcher is
    /// dropped last, which destroys a KJ promise and drops its Rust callback, and enters no V8.
    ///
    /// If the request was aborted after the wait started, the wait fails with the abort instead,
    /// as C++'s join has the abort win over the rejection that `~AwaitJsFulfiller()` causes.
    /// Checking that only reads the exception the `IoContext` was aborted with.
    fn drop(&mut self) {
        if !self.shared.is_waiting() {
            return;
        }
        self.settle_with_pending_abort();
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
    type Output = Result<T, cxx::KjException>;

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
            AwaitJsState::NeverSettled => Poll::Ready(Err(ffi::never_settled_exception())),
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

/// A future for the outcome of a JavaScript promise, converted in its `IoContext`, from
/// [`IoContext::await_js_local`].
///
/// It is an [`AwaitJs`] whose output belongs to the `IoContext`'s thread, so it is neither `Send`
/// nor `Sync`. It is polled without the isolate lock, and dropping it cancels the wait, as for an
/// `AwaitJs`.
pub struct AwaitJsLocal<T> {
    wait: AwaitJs<ThreadBound<T>>,
    // Keeps the future, and so the output it takes, on the thread that created it.
    _thread: PhantomData<*const ()>,
}

impl<T> Future for AwaitJsLocal<T> {
    type Output = Result<T, cxx::KjException>;

    fn poll(self: Pin<&mut Self>, cx: &mut std::task::Context<'_>) -> Poll<Self::Output> {
        Pin::new(&mut self.get_mut().wait)
            .poll(cx)
            .map(|result| result.map(ThreadBound::into_inner))
    }
}

/// The output of an [`AwaitJsLocal`], while the wait's shared state holds it.
///
/// It records the thread that created it, and debug builds assert that it is taken out or
/// dropped there. The SAFETY comment below does not rely on that check; it catches a change to
/// `AwaitJs` that breaks what the comment relies on.
struct ThreadBound<T> {
    // `None` only once `into_inner()` has taken the value.
    value: Option<T>,
    thread: std::thread::ThreadId,
}

impl<T> ThreadBound<T> {
    fn new(value: T) -> Self {
        Self {
            value: Some(value),
            thread: std::thread::current().id(),
        }
    }

    fn into_inner(mut self) -> T {
        self.debug_assert_thread();
        let Some(value) = self.value.take() else {
            unreachable!("only into_inner() takes the value, and it consumes the ThreadBound")
        };
        value
    }

    fn debug_assert_thread(&self) {
        debug_assert_eq!(
            self.thread,
            std::thread::current().id(),
            "the output of an AwaitJsLocal is touched only on its IoContext's thread"
        );
    }
}

impl<T> Drop for ThreadBound<T> {
    fn drop(&mut self) {
        if self.value.is_some() {
            self.debug_assert_thread();
        }
    }
}

#[expect(
    unsafe_code,
    reason = "the output of an AwaitJsLocal is touched only on its IoContext's thread; see the \
              SAFETY comment"
)]
// SAFETY: the value belongs to its IoContext's thread, and is touched only there. The conversion
// of `await_js_local()` creates it, in a reaction that has taken its sender through
// `IoOwned::take()`, which requires the IoContext to be current, so on its thread. It goes into
// the wait's state, which only the sender, the abort watcher's callback and the `AwaitJsLocal`
// reach. The `AwaitJsLocal` takes it out or drops it, on the thread that created it, which is
// the IoContext's, since `await_js_local()` requires the IoContext to be current and the
// future is neither `Send` nor `Sync`. The other references to the state, through which the last
// one may drop the value, are those of the sender, which is dropped on the IoContext's thread (see
// `ThreadBoundAbortWatcher`), and of the abort watcher's callback, which upgrades its weak one
// only while it runs, on that thread. A value that the wait discards, because it was aborted
// first, is dropped where it was created, in the reaction. No reaction data holds one, so GC never
// drops one.
unsafe impl<T> Send for ThreadBound<T> {}

// These impls exist only so that the wait's state can sit in the data of a reaction, whose sender
// `IoOwned` holds the state outside any traced value. `ThreadBound` is private, and only the state
// of an `AwaitJsLocal` wait holds one: never a resource, a reaction's data or anything else
// that GC traces. So a `T` that holds a `jsg::Rc` or a `TracedGlobal` is never visited, and its
// references stay strong, as in any untraced Rust value, until the caller of the future drops
// them. Written out rather than with `jsg_traced_unchecked!`, which would bound `T` by
// `HoldsNoGcReferences`.
impl<T> jsg::Traced for ThreadBound<T> {
    fn trace(&self, _visitor: &mut v8::GcVisitor) {}
}

impl<T> jsg::HoldsNoGcReferences for ThreadBound<T> {}

/// The data of the reactions [`IoContext::await_js`] registers.
struct AwaitJsReaction<T, M> {
    sender: IoOwned<AwaitJsSender<T>>,
    map: M,
}

// A `Copy` `map` holds no `jsg::Rc` or `TracedGlobal`, neither of which is `Copy`, and an `IoOwned`
// holds no JavaScript handles. Written out rather than with `jsg_traced_unchecked!`, which would
// bound `M` by `HoldsNoGcReferences`, which a closure or a function item can't implement.
impl<T: jsg::HoldsNoGcReferences, M: Copy + Send> jsg::Traced for AwaitJsReaction<T, M> {
    fn trace(&self, _visitor: &mut v8::GcVisitor) {}
}

impl<T: jsg::HoldsNoGcReferences, M: Copy + Send> jsg::HoldsNoGcReferences
    for AwaitJsReaction<T, M>
{
}

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
    // As C++ does once the KJ promise is gone, `map` does not run for a wait that was cancelled or
    // failed with the request's abort, and the trace still reports how the JavaScript promise
    // settled.
    if !sender.shared.is_waiting() {
        sender.settle_trace(bridge::AsyncOutcome::OK);
        return Ok(lock.undefined());
    }
    // `map`'s own error is caught as the exception that throwing it throws, as C++
    // `jsg::Lock::tryCatch()` converts a KJ exception from `func`.
    // An abort that the watcher has not yet delivered still wins, though `map` ran, as in C++.
    match lock.try_catch(|lock| (reaction.map)(lock, value))? {
        Ok(value) => {
            sender.settle_with_pending_abort();
            sender.settle(AwaitJsState::Fulfilled(value), bridge::AsyncOutcome::OK);
        }
        Err(exception) => {
            let err = reject_with_js_exception(lock, exception)?;
            sender.settle_with_pending_abort();
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
    // As in C++, the rejection is logged and the trace reports it even if the wait was cancelled
    // or failed with the request's abort.
    let err = reject_with_js_exception(lock, reason)?;
    sender.settle_with_pending_abort();
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
) -> jsg::Result<cxx::KjException> {
    IoContext::current(lock)?.log_uncaught_exception(
        lock,
        UncaughtExceptionSource::InternalAsync,
        exception,
    )?;
    Ok(lock.exception_to_kj(exception)?.into())
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
jsg::jsg_traced_unchecked!(<T> IoOwned<T>);

/// The reference to an [`IoOwned`]'s value that its `IoContext` holds.
struct SlotRef<T>(Arc<Mutex<Option<T>>>);

/// The data of the reactions [`IoContext::then`], [`IoContext::catch`] and
/// [`IoContext::then_or_catch`] register, with the `IoContext` they run in.
struct InContext<D> {
    data: D,
    // The id of the IoContext the reactions run in.
    owner: u64,
}

impl<D> InContext<D> {
    /// Returns the `IoContext` the reactions run in, which must be the current one.
    fn require_current<'a>(&self, _lock: &mut jsg::Lock<'a>) -> jsg::Result<IoContext<'a>> {
        bridge::io_context_require_current(self.owner)?;
        Ok(IoContext {
            id: self.owner,
            _lock: PhantomData,
        })
    }
}

impl<D: jsg::Traced> jsg::Traced for InContext<D> {
    fn trace(&self, visitor: &mut v8::GcVisitor) {
        self.data.trace(visitor);
    }
}

/// A span an `IoContext` owns, from [`IoContext::add_span`].
///
/// It is an [`IoOwned`] for a value that is not `Send`. Unlike the span, the handle is `Send` and
/// `Sync`, since it reaches the span only on the `IoContext`'s thread.
#[jsg_macros::jsg_traced]
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

// A span holds no JavaScript handles.
jsg::jsg_traced_unchecked!(ThreadBoundSpan);

impl<T> Drop for SlotRef<T> {
    fn drop(&mut self) {
        let value = lock_slot(&self.0).take();
        drop(value);
    }
}

/// A callback that re-enters the `IoContext` that created it, from
/// [`IoContext::make_reentry_callback_weak`].
///
/// It can be called any number of times, and each call returns a future for the output of the
/// function the callback runs.
///
/// It is the C++ callback that `IoContext::makeReentryCallbackWeak()` returns, which belongs to
/// its `IoContext`'s thread, so it is neither `Send` nor `Sync`, nor are the futures it returns.
/// Code that hands it to C++, as a `kj::Function` that returns a `kj::Promise`, wraps it in an
/// `extern "Rust"` type of its own bridge, with an `async` function that calls it and converts
/// its output. C++ must then call and destroy that type on the `IoContext`'s thread, as it would
/// the C++ callback.
pub struct ReentryCallback<T> {
    // Neither `Send` nor `Sync`, as a C++ type the bridge declares is not, which keeps the
    // callback on its thread (the `reentry-callback-is-not-send` compile test).
    callback: KjOwn<bridge::ReentryCallback>,
    // The callback holds no `T`.
    _output: PhantomData<fn() -> T>,
}

impl<T: 'static> ReentryCallback<T> {
    /// Calls the callback, and returns a future for the output of its function.
    ///
    /// The call starts now, as the C++ callback's does: it checks the `IoContext` and schedules
    /// `IoContext::run()`, which runs the function once the isolate lock is free. Dropping the
    /// future cancels the call, as dropping the C++ promise does. Dropping the callback does not:
    /// the call keeps the function alive until it finishes.
    ///
    /// The future fails with a [`cxx::KjException`]:
    ///
    /// - If the `IoContext` that created the callback is gone, with an `Error` whose message is
    ///   `The execution context which hosts this callback is no longer running.`, before the
    ///   function runs.
    /// - In a Durable Object that is hibernating, with an `Error` whose message is
    ///   `The Durable Object is hibernating.`, before the function runs.
    /// - If the `IoContext` handles no request, with the error C++ `IoContext::run()` fails with
    ///   then, before the function runs.
    /// - If the `IoContext` is destroyed or aborted while the call waits, with the exception it
    ///   is cancelled or aborted with.
    /// - If the function fails, as [`IoContext::make_reentry_callback_weak`] says.
    #[must_use = "the call is cancelled if its future is dropped"]
    pub fn call(&self) -> ReentryCallFuture<T> {
        let output = Rc::new(Cell::new(None));
        let call = ffi::ReentryCall {
            future: None,
            output: Rc::clone(&output),
        };
        let wait: ReentryWait = match bridge::reentry_callback_call(&self.callback, Box::new(call))
        {
            Ok(promise) => Box::pin(bridge::reentry_promise_wait(promise)),
            Err(err) => Box::pin(std::future::ready(Err(err))),
        };
        ReentryCallFuture {
            wait,
            output,
            _output: PhantomData,
        }
    }
}

/// What a [`ReentryCallFuture`] waits for: the C++ promise of the call.
type ReentryWait = Pin<Box<dyn Future<Output = Result<(), cxx::KjException>>>>;

/// The output of a [`ReentryCallFuture`], once the future of the function has completed.
type ReentryOutput = Rc<Cell<Option<Box<dyn Any>>>>;

/// The future of one call of a [`ReentryCallback`], from [`ReentryCallback::call`].
pub struct ReentryCallFuture<T> {
    wait: ReentryWait,
    output: ReentryOutput,
    _output: PhantomData<fn() -> T>,
}

impl<T: 'static> Future for ReentryCallFuture<T> {
    type Output = Result<T, cxx::KjException>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut std::task::Context<'_>) -> Poll<Self::Output> {
        match self.wait.as_mut().poll(cx) {
            Poll::Pending => Poll::Pending,
            Poll::Ready(Err(err)) => Poll::Ready(Err(err)),
            Poll::Ready(Ok(())) => {
                let Some(output) = self.output.take() else {
                    unreachable!("the call succeeds only once its output is set")
                };
                // The function of `make_reentry_callback_weak()` produced `output` from this `T`.
                let output = output
                    .downcast::<T>()
                    .unwrap_or_else(|_| unreachable!("the output of a reentry callback"));
                Poll::Ready(Ok(*output))
            }
        }
    }
}

/// Runs the function of a [`ReentryCallback`], for `reentry_fn_run()`, and returns the future the
/// call waits for. Fails only while execution is terminating, with an error for which
/// [`jsg::Error::is_pending`] is true. Otherwise, a failure of `function` becomes the exception
/// the future fails with; see [`IoContext::make_reentry_callback_weak`].
fn start_reentry(
    lock: &mut jsg::Lock<'_>,
    function: &mut ffi::ReentryFnBox,
) -> jsg::Result<ffi::ReentryFuture> {
    // Only the exceptions that JavaScript threw are caught here. An error that `function`
    // returned is converted without being thrown.
    let result = lock.try_catch(|lock| match function(lock) {
        Err(err) if !err.is_pending() => Ok(Err(err)),
        result => result.map(Ok),
    })?;
    let result = match result {
        Ok(Ok(future)) => return Ok(future),
        Ok(Err(err)) => lock.try_catch(|lock| {
            let exception = lock.exception(&err)?;
            lock.exception_to_kj(exception)
        })?,
        // As C++ `IoContext::run()` does for an exception that JavaScript threw in the function
        // it runs, before it tunnels it.
        Err(exception) => lock.try_catch(|lock| {
            IoContext::current(lock)?.log_uncaught_exception(
                lock,
                UncaughtExceptionSource::Internal,
                exception,
            )?;
            lock.exception_to_kj(exception)
        })?,
    };
    // JavaScript that converting the failure runs can throw, as a getter of the exception's
    // `stack`, `overloaded` or `retryable` property: then the call fails with an internal error.
    let exception = result.unwrap_or_else(|_| {
        cxx::KjError::new(
            cxx::KjExceptionType::Failed,
            "converting what the function of a reentry callback failed with threw".to_owned(),
        )
    });
    Ok(Box::pin(std::future::ready(Err(cxx::KjException::from(
        exception,
    )))))
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
