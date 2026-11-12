// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The bridge to the C++ `IoContext` calls in `ffi.h`.

#![allow(
    unsafe_code,
    reason = "the cxx bridge expands to unsafe FFI glue, io_callback_run() opens a scope C++ has \
              set up, the inspector call takes V8 handles, the trace span calls \
              take literals that C++ does not copy, and IsolateWeakRef, AsyncResource and \
              AbortWatcher are declared Send"
)]

use std::any::Any;
use std::future::Future;
use std::panic::AssertUnwindSafe;
use std::pin::Pin;
use std::task::Poll;

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
        fn io_context_await_io_and_run(
            id: u64,
            future: Box<IoFuture>,
            callback: Box<IoCallback>,
        ) -> Result<()>;
        fn io_context_add_object(id: u64, slot: Box<IoOwnedSlot>) -> Result<KjOwn<IoOwnedObject>>;
        fn io_context_set_fail_open(id: u64) -> Result<()>;

        fn io_context_add_wait_until(id: u64, future: Box<WaitUntilFuture>) -> Result<()>;

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

    /// How `io_callback_run()` ended.
    enum IoCallbackOutcome {
        Succeeded,
        /// The callback failed, leaving an exception pending, or a termination.
        Failed,
        /// The callback panicked. `jsg::catch_panic()` logged the panic, threw an internal error
        /// and requested termination.
        Panicked,
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

        async fn io_future_wait(future: Box<IoFuture>) -> Result<Box<IoOutput>>;
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

// SAFETY: an `AbortWatcher` must be used and destroyed on its `IoContext`'s thread, while the
// `IoContext`'s abort exception is alive. Only an await_js() sender holds one, and the sender is
// `Send` only because `IoContext::add_object` requires it: it is dropped either by its
// `IoContext`, on the `IoContext`'s thread, before the abort exception, or by a reaction that takes
// it, which requires the `IoContext` to be current.
unsafe impl Send for bridge::AbortWatcher {}

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

/// Waits for `future`. A panic while polling it fails the wait, so that it becomes a KJ exception
/// rather than unwinding into the KJ event loop, which would abort the process. A panic while
/// dropping it, when the wait is cancelled, is logged.
async fn io_future_wait(future: Box<IoFuture>) -> Result<Box<IoOutput>, cxx::KjError> {
    let mut future = CatchDropPanic(Some(future.0));
    std::future::poll_fn(move |cx| {
        let Some(inner) = future.0.as_mut() else {
            unreachable!("dropped only with the wait")
        };
        match std::panic::catch_unwind(AssertUnwindSafe(|| inner.as_mut().poll(cx))) {
            Ok(poll) => poll.map(|output| Ok(Box::new(IoOutput(output)))),
            Err(payload) => Poll::Ready(Err(cxx::KjError::new(
                cxx::KjExceptionType::Failed,
                format!(
                    "the future that IoContext::await_io_and_run() waited for panicked: {}",
                    panic_message(&*payload)
                ),
            ))),
        }
    })
    .await
}

/// Waits for `future` as a waitUntil task. A panic while polling it fails the task, and a panic
/// while dropping it is logged, as in `io_future_wait()`. The task fails with the future's
/// exception as it is.
async fn wait_until_future_wait(future: Box<WaitUntilFuture>) -> Result<(), cxx::KjException> {
    let mut future = CatchDropPanic(Some(future.0));
    std::future::poll_fn(move |cx| {
        let Some(inner) = future.0.as_mut() else {
            unreachable!("dropped only with the wait")
        };
        match std::panic::catch_unwind(AssertUnwindSafe(|| inner.as_mut().poll(cx))) {
            Ok(poll) => poll,
            Err(payload) => Poll::Ready(Err(cxx::KjError::new(
                cxx::KjExceptionType::Failed,
                format!(
                    "the future of a waitUntil task panicked: {}",
                    panic_message(&*payload)
                ),
            )
            .into())),
        }
    })
    .await
}

/// Holds the future of an `io_future_wait()` or `wait_until_future_wait()`, which kj-rs drops
/// through an `extern "C"` function, where a panic would abort the process.
struct CatchDropPanic<F>(Option<F>);

impl<F> Drop for CatchDropPanic<F> {
    fn drop(&mut self) {
        let future = self.0.take();
        if let Err(payload) = std::panic::catch_unwind(AssertUnwindSafe(|| drop(future))) {
            bridge::io_context_log_error(&format!(
                "dropping a future that the IoContext waited for panicked: {}",
                panic_message(&*payload)
            ));
        }
    }
}

pub fn panic_message(payload: &(dyn Any + Send)) -> &str {
    if let Some(message) = payload.downcast_ref::<&str>() {
        message
    } else if let Some(message) = payload.downcast_ref::<String>() {
        message
    } else {
        "<non-string panic payload>"
    }
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
