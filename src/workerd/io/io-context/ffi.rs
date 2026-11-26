// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The bridge to the C++ `IoContext` calls in `ffi.h`.

#![allow(
    unsafe_code,
    reason = "the cxx bridge expands to unsafe FFI glue, io_callback_run(), reentry_fn_run() and \
              wait_until_run_start() open a scope C++ has set up, the inspector call takes V8 \
              handles, the trace span calls and the run in a waitUntil task take literals that \
              C++ does not copy, and IsolateWeakRef and AsyncResource are declared Send"
)]

use std::any::Any;
use std::cell::Cell;
use std::future::Future;
use std::pin::Pin;
use std::rc::Rc;

use jsg::v8;

#[cxx::bridge(namespace = "workerd::io_context")]
pub mod bridge {
    #[namespace = "workerd::rust::jsg"]
    unsafe extern "C++" {
        include!("workerd/rust/jsg/ffi.h");
        include!("workerd/rust/jsg/v8.rs.h");

        type Isolate = jsg::v8::ffi::Isolate;
        type Local = jsg::v8::ffi::Local;
    }

    /// C++ `UncaughtExceptionSource` (`workerd/util/uncaught-exception-source.h`).
    #[namespace = "workerd"]
    #[derive(Clone, Copy, Debug, PartialEq, Eq)]
    #[repr(i32)]
    enum UncaughtExceptionSource {
        INTERNAL,
        INTERNAL_ASYNC,
        ASYNC_TASK,
        REQUEST_HANDLER,
        TRACE_HANDLER,
        ALARM_HANDLER,
        ON_HIBERNATE_HANDLER,
    }

    /// C++ `AsyncOutcome` (`workerd/rust/async-trace/listener.h`).
    #[namespace = "workerd"]
    #[derive(Clone, Copy, Debug, PartialEq, Eq)]
    #[repr(u8)]
    enum AsyncOutcome {
        OK,
        ERROR,
        CANCELED,
    }

    #[namespace = "workerd"]
    unsafe extern "C++" {
        include!("workerd/io/async-trace.h");
        include!("workerd/io/io-context.h");

        type AsyncOutcome;
        type AsyncResource;
        type SpanBuilder;
        type TraceContext;
        type UncaughtExceptionSource;

        /// `AsyncResource::settle()`. Only the first settle is reported.
        fn settle(self: &AsyncResource, outcome: AsyncOutcome);
    }

    unsafe extern "C++" {
        include!("workerd/io/io-context/ffi.h");

        type IoOwnedObject;
        type IsolateWeakRef;
        type AbortWatcher;

        fn io_context_no_request_message() -> &'static str;
        fn io_context_never_settled_exception() -> KjException;
        fn io_context_memory_limit_exception() -> KjException;
        fn io_context_isolate_weak_ref(id: u64) -> Result<KjOwn<IsolateWeakRef>>;
        fn isolate_weak_ref_has_excessively_exceeded_heap_limit(
            isolate: &IsolateWeakRef,
        ) -> Result<bool>;
        fn io_context_make_await_js_resource(id: u64) -> Result<KjMaybe<KjOwn<AsyncResource>>>;
        fn io_context_watch_abort(
            id: u64,
            callback: Box<AbortCallback>,
        ) -> Result<KjOwn<AbortWatcher>>;
        fn abort_watcher_has_pending_abort(watcher: &AbortWatcher) -> bool;
        fn abort_watcher_pending_abort(watcher: &AbortWatcher) -> Result<KjException>;
        fn io_context_try_current_id() -> KjMaybe<u64>;
        fn io_context_require_current(id: u64) -> Result<()>;
        fn io_context_now(id: u64) -> Result<i64>;
        fn io_context_write_logfwdr(id: u64, channel: u32, message: &[u8]) -> Result<()>;
        fn io_context_write_logfwdr_with(
            id: u64,
            channel: u32,
            message: Box<LogfwdrMessage>,
        ) -> Result<()>;
        fn io_context_new_analytics_engine_request(id: u64) -> Result<()>;
        fn io_context_await_io_and_run(
            id: u64,
            future: Box<IoFuture>,
            callback: Box<IoCallback>,
        ) -> Result<()>;
        fn io_context_add_object(id: u64, slot: Box<IoOwnedSlot>) -> Result<KjOwn<IoOwnedObject>>;
        fn io_context_set_fail_open(id: u64) -> Result<()>;

        fn io_context_add_wait_until(id: u64, future: Box<WaitUntilFuture>) -> Result<()>;

        type ReentryCallback;
        type ReentryPromise;

        fn io_context_make_reentry_callback_weak(
            id: u64,
            function: Box<ReentryFn>,
        ) -> Result<KjOwn<ReentryCallback>>;
        fn reentry_callback_call(
            callback: &ReentryCallback,
            call: Box<ReentryCall>,
        ) -> Result<KjOwn<ReentryPromise>>;
        async fn reentry_promise_wait(promise: KjOwn<ReentryPromise>) -> Result<()>;

        fn io_context_check_current(id: u64) -> Result<()>;
        fn io_context_abort(id: u64, reason: KjException) -> Result<()>;
        fn io_context_abort_without_reason(id: u64) -> Result<()>;

        // The `&[u8]` names, keys and literal values below are NUL-terminated strings with the
        // NUL, which C++ requires and does not copy, so they must outlive the spans: the
        // `&'static CStr`s that `trace.rs` passes.
        unsafe fn io_context_make_trace_span(id: u64, name: &[u8]) -> Result<KjOwn<SpanBuilder>>;
        unsafe fn io_context_make_user_trace_span(
            id: u64,
            name: &[u8],
        ) -> Result<KjOwn<TraceContext>>;
        unsafe fn span_builder_new_child(
            span: Pin<&mut SpanBuilder>,
            name: &[u8],
        ) -> KjOwn<SpanBuilder>;
        unsafe fn span_builder_set_tag_bool(span: Pin<&mut SpanBuilder>, key: &[u8], value: bool);
        unsafe fn span_builder_set_tag_int(span: Pin<&mut SpanBuilder>, key: &[u8], value: i64);
        unsafe fn span_builder_set_tag_float(span: Pin<&mut SpanBuilder>, key: &[u8], value: f64);
        unsafe fn span_builder_set_tag_str(span: Pin<&mut SpanBuilder>, key: &[u8], value: &str);
        unsafe fn span_builder_set_tag_literal(
            span: Pin<&mut SpanBuilder>,
            key: &[u8],
            value: &[u8],
        );
        unsafe fn trace_context_set_tag_bool(
            context: Pin<&mut TraceContext>,
            key: &[u8],
            value: bool,
        );
        unsafe fn trace_context_set_tag_int(
            context: Pin<&mut TraceContext>,
            key: &[u8],
            value: i64,
        );
        unsafe fn trace_context_set_tag_float(
            context: Pin<&mut TraceContext>,
            key: &[u8],
            value: f64,
        );
        unsafe fn trace_context_set_tag_str(
            context: Pin<&mut TraceContext>,
            key: &[u8],
            value: &str,
        );
        unsafe fn trace_context_set_tag_literal(
            context: Pin<&mut TraceContext>,
            key: &[u8],
            value: &[u8],
        );
        fn trace_context_detach_async(context: Pin<&mut TraceContext>);

        fn io_context_log_error(message: &str);

        fn io_context_should_log_uncaught_exceptions() -> bool;
        fn io_context_log_uncaught_exception_stack(source: UncaughtExceptionSource, stack: &str);
        fn io_context_log_uncaught_exception_value(
            source: UncaughtExceptionSource,
            exception: &str,
        );
        // Returns true if the isolate has no inspector. Otherwise, returns false when an
        // exception or a termination is pending, or when V8 fails to
        // describe the message, leaving an exception pending. The isolate is the caller's lock's,
        // and the handle belongs to its scope.
        unsafe fn io_context_send_exception_to_inspector(
            id: u64,
            isolate: *mut Isolate,
            source: UncaughtExceptionSource,
            exception: Local,
        ) -> Result<bool>;
    }

    #[namespace = "workerd::rust::worker"]
    unsafe extern "C++" {
        include!("workerd/rust/worker/ffi.rs.h");

        type ScheduledResult = worker::ffi::bridge::ScheduledResult;
    }

    // `IoContext::IncomingRequest` (`io/io-context.h`), and the calls on it that Rust makes, as
    // C++ has them. The code that delivers an event holds the request and makes the calls.
    unsafe extern "C++" {
        include!("workerd/io/io-context/ffi.h");

        type IncomingRequest;

        /// `IncomingRequest::delivered(eventType, NO, location)`, for the location in `file`,
        /// `function`, `line` and `column`. C++ copies `event_type`.
        ///
        /// # Safety
        /// `file` and `function` are static NUL-terminated strings with their NUL, which C++
        /// keeps as the request's `kj::SourceLocation`.
        unsafe fn incoming_request_delivered(
            request: Pin<&mut IncomingRequest>,
            event_type: &[u8],
            file: &[u8],
            function: &[u8],
            line: u32,
            column: u32,
        ) -> Result<()>;
        /// Tells the request's tracer, if it has one, that the event is a scheduled event for
        /// `scheduled_time` and the cron pattern `cron`.
        fn incoming_request_set_scheduled_event_info(
            request: Pin<&mut IncomingRequest>,
            scheduled_time: KjDate,
            cron: &[u8],
        ) -> Result<()>;
        /// Tells the request's tracer, if it has one, that the event is a custom event.
        fn incoming_request_set_custom_event_info(request: Pin<&mut IncomingRequest>)
        -> Result<()>;
        /// Fails if the request's `IoContext` belongs to an actor:
        /// `KJ_ASSERT(request.getContext().getActor() == kj::none)`.
        fn incoming_request_assert_no_actor(request: Pin<&mut IncomingRequest>) -> Result<()>;
        /// The C++ work that a run in a waitUntil task does before it calls Rust.
        type RunPrelude;

        /// `context.addWaitUntil(context.run(...))` on the request's `IoContext`, running
        /// `prelude`, then `run` with the handles `prelude` returns, under the Perfetto span that
        /// `prelude` names, in the request's trace scope.
        fn incoming_request_run_in_wait_until(
            request: Pin<&mut IncomingRequest>,
            prelude: KjOwn<RunPrelude>,
            run: Box<WaitUntilRun>,
        ) -> Result<()>;
        /// `IncomingRequest::finishScheduled(self)`. An exception that it throws fails the future.
        async fn incoming_request_finish_scheduled(
            request: KjOwn<IncomingRequest>,
        ) -> Result<ScheduledResult>;
    }

    // Dropping a `KjOwn` of them is generated here, where they are declared.
    impl KjOwn<IncomingRequest> {}
    impl KjOwn<RunPrelude> {}

    /// How `io_callback_run()` or `wait_until_run_start()` ended.
    enum IoCallbackOutcome {
        Succeeded,
        /// The callback failed, leaving an exception pending, or a termination.
        Failed,
        /// The callback panicked. `jsg::catch_panic()` logged the panic, threw an internal error
        /// and requested termination.
        Panicked,
    }

    /// How `reentry_fn_run()` ended.
    enum ReentryOutcome {
        /// The function returned a future, or failed with an error or an exception, which the
        /// call's future then fails with. The `ReentryCall` holds that future.
        Started,
        /// Execution is terminating, and the termination is pending.
        Terminated,
        /// The function panicked. `jsg::catch_panic()` logged the panic, threw an internal error
        /// and requested termination.
        Panicked,
    }

    extern "Rust" {
        /// The function of a `ReentryCallback`.
        type ReentryFn;
        /// One call of a `ReentryCallback`: receives the future the function returns, and once
        /// that completes, its output.
        type ReentryCall;

        unsafe fn reentry_fn_run(
            isolate: usize,
            function: &mut ReentryFn,
            call: &mut ReentryCall,
        ) -> ReentryOutcome;
        async fn reentry_call_wait(call: Box<ReentryCall>) -> Result<()>;
    }

    extern "Rust" {
        /// The function that `incoming_request_run_in_wait_until()` runs, then the future it
        /// returned.
        type WaitUntilRun;

        /// # Safety
        /// C++ calls this once, from `IoContext::run()`, with the isolate locked, a context
        /// entered and a `HandleScope` open, which outlive the call and hold `handles`.
        unsafe fn wait_until_run_start(
            isolate: usize,
            run: &mut WaitUntilRun,
            handles: &[Local],
        ) -> Result<IoCallbackOutcome>;
        async fn wait_until_run_wait(run: Box<WaitUntilRun>) -> Result<()>;
    }

    extern "Rust" {
        type IoFuture;
        type IoOutput;
        type IoCallback;
        type IoOwnedSlot;

        /// What an `AbortWatcher` calls with the exception its `IoContext` was aborted with, on
        /// the `IoContext`'s thread, without the isolate lock and with no `IoContext` current.
        type AbortCallback;
        fn aborted(self: &AbortCallback, exception: KjException);

        type WaitUntilFuture;

        /// Builds a message for `io_context_write_logfwdr_with()`.
        type LogfwdrMessage;
        fn logfwdr_message_build(message: Box<LogfwdrMessage>) -> Result<Vec<u8>>;

        async fn io_future_wait(future: Box<IoFuture>) -> Box<IoOutput>;
        async fn wait_until_future_wait(future: Box<WaitUntilFuture>) -> Result<()>;
        unsafe fn io_callback_run(
            isolate: usize,
            callback: Box<IoCallback>,
            output: Box<IoOutput>,
        ) -> IoCallbackOutcome;
    }
}

// SAFETY: an `IsolateWeakRef` holds only a `kj::Own<const AtomicWeakRef<Worker::Isolate>>`, which
// may be used and released on any thread: `AtomicWeakRef` is `kj::AtomicRefcounted`, and a
// `kj::MutexGuarded` guards its pointer to the isolate, which the isolate clears when it is
// destroyed, so `tryAddStrongRef()` is safe on any thread, even once the isolate is gone. The one
// thing Rust does with it, `isolate_weak_ref_has_excessively_exceeded_heap_limit()`, takes a
// strong reference to the `kj::AtomicRefcounted` `Worker::Isolate`, and calls a `const` method of
// its `IsolateLimitEnforcer`, which needs no isolate lock (see its declaration).
unsafe impl Send for bridge::IsolateWeakRef {}

// SAFETY: an `AsyncResource` holds only a `kj::Arc<AsyncTracker>` and an id. `AsyncTracker`'s
// methods, which `settle()` and the destructor call, are `const` and may be called on any thread:
// on a thread other than the tracker's own, they do nothing but count the call
// (`AsyncTracker::onOwnerThread()`). `kj::Arc` is atomically refcounted, so the reference may be
// released on any thread. Releasing the last one there destroys only a closed tracker, whose
// sinks are gone: the IoContext holds a reference until `OwnedAsyncTracker` closes the tracker
// on its own thread.
unsafe impl Send for bridge::AsyncResource {}

/// The exception `watcher`'s request was aborted with, if it was aborted after the watcher
/// started; see `abort_watcher_has_pending_abort()`.
pub fn pending_abort(watcher: &bridge::AbortWatcher) -> Option<cxx::KjException> {
    if !bridge::abort_watcher_has_pending_abort(watcher) {
        return None;
    }
    // It was just found, so this does not fail. This runs when a sender is dropped, which must
    // not panic.
    bridge::abort_watcher_pending_abort(watcher).ok()
}

/// See `bridge::AbortCallback`. The function must not panic, since the call comes from C++, and must
/// not drop the `AbortWatcher` that runs it.
pub struct AbortCallback(pub Box<dyn Fn(cxx::KjException) + Send>);

impl AbortCallback {
    fn aborted(&self, exception: cxx::KjException) {
        (self.0)(exception);
    }
}

/// C++ `IoContext::neverSettledException()`, which C++ thus defines once, for itself and for Rust.
pub fn never_settled_exception() -> cxx::KjException {
    bridge::io_context_never_settled_exception()
}

/// C++ `IoContext::memoryLimitException()`, which C++ thus defines once, for itself and for Rust.
pub fn memory_limit_exception() -> cxx::KjException {
    bridge::io_context_memory_limit_exception()
}

/// `io_context_send_exception_to_inspector()`, failing with [`jsg::Error::pending`] if the
/// isolate has an inspector and an exception or a termination is pending.
pub fn send_exception_to_inspector<'s>(
    lock: &jsg::Lock<'s>,
    id: u64,
    source: bridge::UncaughtExceptionSource,
    exception: v8::Local<'s, v8::Value>,
) -> jsg::Result<()> {
    let isolate = lock.isolate().as_ffi();
    // SAFETY: `lock` keeps the isolate locked, with a context entered, and the handle belongs to
    // its scope, which is open throughout the call.
    let sent = unsafe {
        bridge::io_context_send_exception_to_inspector(id, isolate, source, exception.into_ffi())
    }?;
    pending_unless(sent)
}

/// Aborts the `IoContext` that `id` identifies with `reason`, as [`jsg::Lock::exception_to_kj`]
/// encodes it, or, without one, with `IoContext::abortWithoutReasonException()`. Fails before
/// encoding `reason` unless `id` identifies the current `IoContext`, as C++ looks for the request
/// first, and with the error encoding it fails with, which is pending if JavaScript threw.
pub fn abort<'s>(
    lock: &mut jsg::Lock<'s>,
    id: u64,
    reason: Option<v8::Local<'s, v8::Value>>,
) -> jsg::Result<()> {
    let Some(reason) = reason else {
        return Ok(bridge::io_context_abort_without_reason(id)?);
    };
    bridge::io_context_check_current(id)?;
    let reason = cxx::KjException::from(lock.exception_to_kj(reason)?);
    Ok(bridge::io_context_abort(id, reason)?)
}

/// `Ok` if a shim that returns false when it leaves an exception pending returned true.
fn pending_unless(succeeded: bool) -> jsg::Result<()> {
    if succeeded {
        Ok(())
    } else {
        Err(jsg::Error::pending())
    }
}

/// The future that `IoContext::await_io_and_run()` waits for, with its output type erased.
pub struct IoFuture(pub Pin<Box<dyn Future<Output = Box<dyn Any + Send>> + Send>>);

/// The future that `io_context_add_wait_until()` waits for as a waitUntil task: an
/// [`AwaitJs`](crate::AwaitJs) whose value is discarded.
pub struct WaitUntilFuture(pub Pin<Box<dyn Future<Output = Result<(), cxx::KjException>> + Send>>);

/// The output of an [`IoFuture`].
pub struct IoOutput(Box<dyn Any + Send>);

/// What [`IoCallback`] calls.
pub type IoCallbackFn =
    Box<dyn for<'s> FnOnce(&mut jsg::Lock<'s>, Box<dyn Any + Send>) -> jsg::Result<()>>;

/// The callback that `IoContext::await_io_and_run()` runs with the output of its future.
pub struct IoCallback(pub IoCallbackFn);

// SAFETY: it holds a C++ `IoOwn`, whose destructor deletes the object at once on the thread of
// its IoContext and otherwise queues it, through a mutex, to the IoContext's `DeleteQueue`, which a
// `kj::Arc` keeps alive. It is never dereferenced.
unsafe impl Send for bridge::IoOwnedObject {}

// SAFETY: Rust has no method on it, so a shared reference to it can do nothing; it is only
// dropped, through the `KjOwn` that holds it.
unsafe impl Sync for bridge::IoOwnedObject {}

/// A value that `IoContext::add_object()` hands to C++, which drops it with the `IoContext`.
/// Dropping it drops the value it holds, unless Rust took the value first.
pub struct IoOwnedSlot(#[expect(dead_code, reason = "only dropped")] pub Box<dyn Send>);

/// Waits for `future`. kj-rs turns a panic while polling it into a KJ exception that fails the
/// wait, and logs a panic while dropping it, when the wait is cancelled.
async fn io_future_wait(future: Box<IoFuture>) -> Box<IoOutput> {
    Box::new(IoOutput(future.0.await))
}

/// Waits for `future` as a waitUntil task, which fails with the future's exception as it is. A
/// panic is handled as in `io_future_wait()`.
async fn wait_until_future_wait(future: Box<WaitUntilFuture>) -> Result<(), cxx::KjException> {
    future.0.await
}

/// The future that the function of a `ReentryCallback` returns, with its output type erased. KJ
/// polls it on the thread of the `IoContext`, so it need not be `Send`.
pub type ReentryFuture = Pin<Box<dyn Future<Output = Result<Box<dyn Any>, cxx::KjException>>>>;

/// The function of a `ReentryCallback`, with its output type erased. It finds the `IoContext`
/// itself, which `IoContext::run()` has made current.
pub type ReentryFnBox = Box<dyn for<'s> FnMut(&mut jsg::Lock<'s>) -> jsg::Result<ReentryFuture>>;

/// See `bridge::ReentryFn`. C++ holds it in the `IoContext`, as a `ReverseIoOwn`, and runs and drops
/// it only on the thread of the `IoContext`, so it need not be `Send`.
pub struct ReentryFn(pub ReentryFnBox);

/// See `bridge::ReentryCall`.
pub struct ReentryCall {
    /// The future of the function, once `reentry_fn_run()` has started it.
    pub future: Option<ReentryFuture>,
    /// Where `reentry_call_wait()` puts the future's output, for the `ReentryCallFuture` of the
    /// call. Both run on the thread of the `IoContext`.
    pub output: Rc<Cell<Option<Box<dyn Any>>>>,
}

/// Runs `function` for `call`, from a `ReentryCallback` in `IoContext::run()`, with the isolate
/// locked, a context entered and a `HandleScope` open.
unsafe fn reentry_fn_run(
    isolate: usize,
    function: &mut ReentryFn,
    call: &mut ReentryCall,
) -> bridge::ReentryOutcome {
    let scope = std::pin::pin!(
        // SAFETY: C++ calls this with the isolate locked, a context entered and a HandleScope
        // open, which outlive the scope.
        unsafe { jsg::CallbackScope::new(isolate as *mut v8::ffi::Isolate) }
    );
    jsg::catch_panic(scope, |lock| {
        match crate::start_reentry(lock, &mut function.0) {
            Ok(future) => {
                call.future = Some(future);
                bridge::ReentryOutcome::Started
            }
            Err(_) => bridge::ReentryOutcome::Terminated,
        }
    })
    .unwrap_or(bridge::ReentryOutcome::Panicked)
}

/// Waits for the future that `reentry_fn_run()` started for `call`, then puts its output where the
/// call's `ReentryCallFuture` takes it. A panic is handled as in `io_future_wait()`.
async fn reentry_call_wait(call: Box<ReentryCall>) -> Result<(), cxx::KjException> {
    let ReentryCall { future, output } = *call;
    let Some(future) = future else {
        unreachable!("C++ waits for a call only once reentry_fn_run() started it")
    };
    output.set(Some(future.await?));
    Ok(())
}

/// What `IoContext::write_logfwdr_with()` builds its message with. C++ drops it, unbuilt, if the
/// `IoContext` is destroyed before the output gate opens.
pub struct LogfwdrMessage(pub Box<dyn FnOnce() -> Vec<u8>>);

/// Builds `message`. A panic fails the write, rather than unwinding into C++.
#[expect(
    clippy::boxed_local,
    reason = "C++ owns a Rust type only through a Box"
)]
fn logfwdr_message_build(message: Box<LogfwdrMessage>) -> Result<Vec<u8>, cxx::KjError> {
    std::panic::catch_unwind(std::panic::AssertUnwindSafe(message.0)).map_err(|payload| {
        cxx::KjError::new(
            cxx::KjExceptionType::Failed,
            format!(
                "building a logfwdr message panicked: {}",
                panic_message(&*payload)
            ),
        )
    })
}

/// The message of a panic with `payload`, as `cxx::panic_message()` reads it.
pub fn panic_message(payload: &(dyn Any + Send)) -> &str {
    cxx::panic_message(payload).unwrap_or("<non-string panic payload>")
}

/// Runs `callback` with `output`, from `io_context_await_io_and_run()` in the `IoContext`, with the
/// isolate locked, a context entered and a `HandleScope` open.
unsafe fn io_callback_run(
    isolate: usize,
    callback: Box<IoCallback>,
    output: Box<IoOutput>,
) -> bridge::IoCallbackOutcome {
    let scope = std::pin::pin!(
        // SAFETY: C++ calls this with the isolate locked, a context entered and a HandleScope
        // open, which outlive the scope.
        unsafe { jsg::CallbackScope::new(isolate as *mut v8::ffi::Isolate) }
    );
    jsg::catch_panic(scope, |lock| match (callback.0)(lock, output.0) {
        Ok(()) => bridge::IoCallbackOutcome::Succeeded,
        Err(err) => {
            lock.throw_exception(&err);
            bridge::IoCallbackOutcome::Failed
        }
    })
    .unwrap_or(bridge::IoCallbackOutcome::Panicked)
}

/// Where Rust code delivered a request.
///
/// It is the `kj::SourceLocation` that `IoContext::IncomingRequest::delivered()` records, which
/// the "no current `IncomingRequest`" warnings report. Made by
/// [`delivered_location!`](crate::delivered_location).
pub struct DeliveredLocation {
    pub file: &'static std::ffi::CStr,
    pub function: &'static std::ffi::CStr,
    pub line: u32,
    pub column: u32,
}

/// The [`DeliveredLocation`] of the macro's caller, as a `&'static DeliveredLocation`.
///
/// Its function is the caller's module path, since Rust has no name for the enclosing function.
#[macro_export]
macro_rules! delivered_location {
    () => {
        const {
            &$crate::DeliveredLocation {
                file: match ::std::ffi::CStr::from_bytes_with_nul(concat!(file!(), "\0").as_bytes())
                {
                    Ok(file) => file,
                    Err(_) => panic!("a file name contains a NUL"),
                },
                function: match ::std::ffi::CStr::from_bytes_with_nul(
                    concat!(module_path!(), "\0").as_bytes(),
                ) {
                    Ok(function) => function,
                    Err(_) => panic!("a module path contains a NUL"),
                },
                line: line!(),
                column: column!(),
            }
        }
    };
}

/// `IoContext::IncomingRequest::delivered(eventType)`, for `event_type`, such as `"scheduled"`.
///
/// The event type names the request's async trace resource, and `location` is recorded as where
/// the request was delivered.
///
/// # Errors
/// Fails with the exception that `delivered()` throws.
pub fn incoming_request_delivered(
    request: Pin<&mut bridge::IncomingRequest>,
    event_type: &str,
    location: &'static DeliveredLocation,
) -> Result<(), cxx::KjException> {
    // SAFETY: a `DeliveredLocation`'s strings are static `CStr`s, passed with their NUL.
    unsafe {
        bridge::incoming_request_delivered(
            request,
            event_type.as_bytes(),
            location.file.to_bytes_with_nul(),
            location.function.to_bytes_with_nul(),
            location.line,
            location.column,
        )
    }
}

/// How the function that [`incoming_request_run_in_wait_until`] runs fails.
#[derive(Debug)]
pub enum RunError {
    /// A JavaScript error, which the run throws, unless it stands for an exception that is
    /// pending (see [`jsg::Error::is_pending`]). `IoContext::run()` then fails with the exception,
    /// which it logs as uncaught.
    Js(jsg::Error),
    /// A KJ exception, as a C++ call fails with, which `IoContext::run()` fails with unchanged,
    /// unless a JavaScript exception or a termination is pending. That one must keep propagating
    /// (see AGENTS.md), so the run fails with it, as for [`RunError::Js`], and the KJ exception
    /// is dropped.
    Kj(cxx::KjException),
}

impl From<jsg::Error> for RunError {
    fn from(err: jsg::Error) -> Self {
        Self::Js(err)
    }
}

impl From<cxx::KjException> for RunError {
    fn from(err: cxx::KjException) -> Self {
        Self::Kj(err)
    }
}

/// The future that the function of a [`WaitUntilRun`] returns. KJ polls it on the thread of the
/// `IoContext`, so it need not be `Send`.
type WaitUntilRunFuture = Pin<Box<dyn Future<Output = Result<(), cxx::KjException>>>>;

/// The function of a [`WaitUntilRun`], which receives the handles of its prelude.
type WaitUntilRunFn = Box<
    dyn for<'s> FnOnce(
        &mut jsg::Lock<'s>,
        &[v8::Local<'s, v8::Value>],
    ) -> Result<WaitUntilRunFuture, RunError>,
>;

/// See `bridge::WaitUntilRun`. C++ runs and drops it only on the thread of the `IoContext`, so it
/// need not be `Send`.
pub struct WaitUntilRun {
    /// The function, until `wait_until_run_start()` runs it.
    function: Option<WaitUntilRunFn>,
    /// The future of the function, once `wait_until_run_start()` has started it.
    future: Option<WaitUntilRunFuture>,
}

/// `context.addWaitUntil(context.run(...))` on the `IoContext` of `request`, as C++ starts an
/// event's handler.
///
/// In a waitUntil task of the request, in `IoContext::run()`, with the isolate locked, under the
/// Perfetto span that `prelude` names (`RunPrelude::spanName()`) and in the request's trace scope
/// (`IoContext::makeRequestTraceScope()`), runs the C++ `prelude`, then `f` with the handles that
/// `prelude` returns, then waits for the future that `f` returns, outside the lock. `prelude` is
/// the C++ work that a C++ event's lambda does before the call that `f` stands for, such as
/// finding the exported handler. It runs before Rust holds the lock, and an exception that it
/// throws fails the run as one that C++ code throws.
///
/// The task fails with the error that `f` fails with, as [`RunError`] describes, or with the
/// future's. If `f` panics, the panic terminates execution (`jsg::catch_panic()`) and the task
/// fails with a KJ exception.
///
/// # Errors
/// Fails with the exception that keeps the task from being added.
pub fn incoming_request_run_in_wait_until<F>(
    request: Pin<&mut bridge::IncomingRequest>,
    prelude: kj_rs::KjOwn<bridge::RunPrelude>,
    f: impl for<'s> FnOnce(&mut jsg::Lock<'s>, &[v8::Local<'s, v8::Value>]) -> Result<F, RunError>
    + 'static,
) -> Result<(), cxx::KjException>
where
    F: Future<Output = Result<(), cxx::KjException>> + 'static,
{
    let function: WaitUntilRunFn = Box::new(move |lock: &mut jsg::Lock<'_>, handles| {
        let future: WaitUntilRunFuture = Box::pin(f(lock, handles)?);
        Ok(future)
    });
    let run = Box::new(WaitUntilRun {
        function: Some(function),
        future: None,
    });
    bridge::incoming_request_run_in_wait_until(request, prelude, run)
}

/// Runs the function of `run` on `handles`, from `incoming_request_run_in_wait_until()` in
/// `IoContext::run()`, after its prelude, and keeps the future it returns for
/// `wait_until_run_wait()`. An error is thrown, so that C++ fails the run with `Failed`, unless it
/// is a KJ exception, which C++ throws on as if it had not passed through Rust. A KJ exception
/// while a JavaScript exception or a termination is pending fails the run with `Failed` too, so
/// that the pending one keeps propagating.
unsafe fn wait_until_run_start(
    isolate: usize,
    run: &mut WaitUntilRun,
    handles: &[bridge::Local],
) -> Result<bridge::IoCallbackOutcome, cxx::KjException> {
    let Some(function) = run.function.take() else {
        unreachable!("C++ runs the function once")
    };
    let scope = std::pin::pin!(
        // SAFETY: C++ calls this with the isolate locked, a context entered and a HandleScope
        // open, which outlive the scope.
        unsafe { jsg::CallbackScope::new(isolate as *mut v8::ffi::Isolate) }
    );
    jsg::catch_panic(scope, |lock| {
        // SAFETY: C++ passes handles in the HandleScope that the scope is in.
        let handles: Vec<v8::Local<'_, v8::Value>> = handles
            .iter()
            .map(|&handle| unsafe { v8::Local::from_ffi(handle) })
            .collect();
        match function(lock, &handles) {
            Ok(future) => {
                run.future = Some(future);
                Ok(bridge::IoCallbackOutcome::Succeeded)
            }
            Err(RunError::Js(err)) => {
                lock.throw_exception(&err);
                Ok(bridge::IoCallbackOutcome::Failed)
            }
            Err(RunError::Kj(exception)) => {
                if lock.is_execution_terminating() || lock.has_pending_exception() {
                    Ok(bridge::IoCallbackOutcome::Failed)
                } else {
                    Err(exception)
                }
            }
        }
    })
    .unwrap_or(Ok(bridge::IoCallbackOutcome::Panicked))
}

/// Waits for the future that `wait_until_run_start()` kept. A panic is handled as in
/// `io_future_wait()`.
async fn wait_until_run_wait(run: Box<WaitUntilRun>) -> Result<(), cxx::KjException> {
    let Some(future) = run.future else {
        unreachable!("C++ waits only once wait_until_run_start() succeeded")
    };
    future.await
}
