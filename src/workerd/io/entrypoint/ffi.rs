// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The bridge to the C++ calls in `ffi.h`, and the events that C++ hands to Rust through it.
//!
//! Each C++ call does its work when it is called, an async one included: it returns a future for
//! what remains to wait for. The Rust functions that C++ calls catch panics, which would abort
//! the process if they reached C++, and fail with a KJ exception instead.

#![allow(
    unsafe_code,
    reason = "the cxx bridge expands to unsafe FFI glue, invocation_delivered() and the span names \
              keep the names they are given, and handler_callback_run() opens a scope C++ has set \
              up"
)]

use std::ffi::CStr;
use std::future::Future;
use std::panic::AssertUnwindSafe;
use std::pin::Pin;

use cxx::KjException;
use io_context::CatchDropPanic;
use io_context::CatchPanics;
use io_context::IoCallbackOutcome;
use jsg::v8;
use kj_rs::KjDate;
use kj_rs::KjOwn;

#[cxx::bridge(namespace = "workerd::entrypoint")]
pub mod bridge {
    #[namespace = "workerd::rust::jsg"]
    unsafe extern "C++" {
        include!("workerd/rust/jsg/ffi.h");
        include!("workerd/rust/jsg/v8.rs.h");

        type Local = jsg::v8::ffi::Local;
    }

    #[namespace = "workerd::io_context"]
    unsafe extern "C++" {
        include!("workerd/io/io-context/ffi.rs.h");

        type IoCallbackOutcome = io_context::IoCallbackOutcome;
    }

    #[namespace = "workerd::rust::worker"]
    unsafe extern "C++" {
        include!("workerd/rust/worker/ffi.rs.h");

        type ScheduledResult = worker::ffi::bridge::ScheduledResult;
    }

    unsafe extern "C++" {
        include!("workerd/io/entrypoint/ffi.h");

        type Invocation;

        fn invocation_set_custom_event_info(invocation: Pin<&mut Invocation>) -> Result<()>;
        fn invocation_set_scheduled_event_info(
            invocation: Pin<&mut Invocation>,
            scheduled_time: KjDate,
            cron: &[u8],
        ) -> Result<()>;
        /// # Safety
        /// `event_type` is a static NUL-terminated string with the NUL, which C++ keeps. (C++
        /// checks that the NUL is its only one.)
        unsafe fn invocation_delivered(
            invocation: Pin<&mut Invocation>,
            event_type: &[u8],
        ) -> Result<()>;
        fn invocation_drain(invocation: KjOwn<Invocation>);
        /// # Safety
        /// `span_name` is a static NUL-terminated string with the NUL, as for
        /// `invocation_delivered()`.
        unsafe fn invocation_run_in_wait_until(
            invocation: Pin<&mut Invocation>,
            span_name: &[u8],
            callback: Box<HandlerCallback>,
            then: Box<WaitUntilContinuation>,
        ) -> Result<()>;
        fn invocation_start_scheduled_in_wait_until(
            invocation: Pin<&mut Invocation>,
            scheduled_time: KjDate,
            cron: &[u8],
        ) -> Result<()>;
        /// # Safety
        /// `span_name` is a static NUL-terminated string with the NUL, as for
        /// `invocation_delivered()`.
        async unsafe fn invocation_finish_scheduled<'a>(
            invocation: KjOwn<Invocation>,
            span_name: &'a [u8],
        ) -> Result<ScheduledResult>;
    }

    extern "Rust" {
        type HandlerCallback;

        /// # Safety
        /// C++ calls this with the isolate locked, a context entered and a `HandleScope` open, in
        /// which `handler`, `env` and `ctx` are handles, and which outlive the call.
        unsafe fn handler_callback_run(
            isolate: usize,
            callback: Box<HandlerCallback>,
            handler: Local,
            env: Local,
            ctx: Local,
        ) -> Result<IoCallbackOutcome>;

        type WaitUntilContinuation;

        async fn wait_until_continuation_wait(then: Box<WaitUntilContinuation>) -> Result<()>;

        type ScheduledEvent;

        /// Starts the scheduled event of `invocation`: see `runScheduled()` in `ffi.h`.
        fn scheduled_event_start(
            invocation: KjOwn<Invocation>,
            scheduled_time: KjDate,
            cron: &[u8],
        ) -> Result<Box<ScheduledEvent>>;
        async fn scheduled_event_finish(event: Box<ScheduledEvent>) -> Result<ScheduledResult>;

        type TestEvent;

        /// Starts the test event of `invocation`: see `runTest()` in `ffi.h`.
        fn test_event_start(invocation: KjOwn<Invocation>) -> Result<Box<TestEvent>>;
        async fn test_event_finish(event: Box<TestEvent>) -> Result<bool>;
    }
}

/// Delivers `invocation` as an event of type `event_type`: see `invocation_delivered()` in `ffi.h`.
pub fn delivered(
    invocation: Pin<&mut bridge::Invocation>,
    event_type: &'static CStr,
) -> Result<(), KjException> {
    // SAFETY: `event_type` is a static NUL-terminated string with the NUL. A `CStr` has no other
    // NUL.
    unsafe { bridge::invocation_delivered(invocation, event_type.to_bytes_with_nul()) }
}

/// Runs `callback` in a waitUntil task of `invocation`, under the Perfetto span `span_name`, then
/// waits for `then`: see `invocation_run_in_wait_until()` in `ffi.h`.
pub fn run_in_wait_until(
    invocation: Pin<&mut bridge::Invocation>,
    span_name: &'static CStr,
    callback: HandlerCallback,
    then: WaitUntilContinuation,
) -> Result<(), KjException> {
    // SAFETY: `span_name` is a static NUL-terminated string with the NUL.
    unsafe {
        bridge::invocation_run_in_wait_until(
            invocation,
            span_name.to_bytes_with_nul(),
            Box::new(callback),
            Box::new(then),
        )
    }
}

/// Finishes `invocation` as a scheduled event, under the Perfetto span `span_name`: see
/// `invocation_finish_scheduled()` in `ffi.h`. C++ is called now; the future only waits.
pub fn finish_scheduled(
    invocation: KjOwn<bridge::Invocation>,
    span_name: &'static CStr,
) -> impl Future<Output = Result<bridge::ScheduledResult, KjException>> + 'static {
    // SAFETY: `span_name` is a static NUL-terminated string with the NUL.
    unsafe { bridge::invocation_finish_scheduled(invocation, span_name.to_bytes_with_nul()) }
}

/// A future that C++ waits for, on the thread it was made on.
type LocalFuture<T> = Pin<Box<dyn Future<Output = Result<T, KjException>>>>;

/// What [`HandlerCallback`] calls.
pub type HandlerCallbackFn =
    Box<dyn for<'s> FnOnce(&mut jsg::Lock<'s>, crate::Handler<'s>) -> Result<(), crate::RunError>>;

/// The callback that `invocation_run_in_wait_until()` runs on the handler. If C++ drops it without
/// running it, a panic while dropping it is logged.
pub struct HandlerCallback(CatchDropPanic<HandlerCallbackFn>);

impl HandlerCallback {
    pub fn new(f: HandlerCallbackFn) -> Self {
        Self(CatchDropPanic::new(
            f,
            "the callback of an entrypoint event",
        ))
    }
}

/// Runs `callback`, from `invocation_run_in_wait_until()`, on the handles C++ found. A panic logs,
/// throws an internal error and requests termination (`jsg::catch_panic()`), and C++ then fails
/// the run with a KJ exception (`endRunOfIoCallback()`).
///
/// # Safety
/// C++ calls this with the isolate locked, a context entered and a `HandleScope` open, in which
/// `handler`, `env` and `ctx` are handles, and which outlive the call.
#[expect(clippy::boxed_local, reason = "C++ passes it as a rust::Box")]
unsafe fn handler_callback_run(
    isolate: usize,
    mut callback: Box<HandlerCallback>,
    handler: bridge::Local,
    env: bridge::Local,
    ctx: bridge::Local,
) -> Result<IoCallbackOutcome, KjException> {
    let f = callback.0.take();
    let scope = std::pin::pin!(
        // SAFETY: forwarded from the caller.
        unsafe { jsg::CallbackScope::new(isolate as *mut v8::ffi::Isolate) }
    );
    jsg::catch_panic(scope, |lock| {
        // SAFETY: the handles are in the caller's scope, which is the lock's.
        let handler = unsafe {
            crate::Handler {
                handler: v8::Local::from_ffi(handler),
                env: v8::Local::from_ffi(env),
                ctx: v8::Local::from_ffi(ctx),
            }
        };
        match f(lock, handler) {
            Ok(()) => Ok(IoCallbackOutcome::Succeeded),
            Err(crate::RunError::Js(err)) => {
                lock.throw_exception(&err);
                Ok(IoCallbackOutcome::Failed)
            }
            // C++ throws it on, as if it had not passed through Rust.
            Err(crate::RunError::Kj(err)) => Err(err),
        }
    })
    .unwrap_or(Ok(IoCallbackOutcome::Panicked))
}

/// What the waitUntil task that `invocation_run_in_wait_until()` adds waits for once its callback
/// has run.
pub struct WaitUntilContinuation(CatchPanics<LocalFuture<()>>);

impl WaitUntilContinuation {
    pub fn new(future: impl Future<Output = Result<(), KjException>> + 'static) -> Self {
        Self(CatchPanics::new(
            Box::pin(future),
            "the future of an entrypoint event's waitUntil task",
        ))
    }
}

async fn wait_until_continuation_wait(then: Box<WaitUntilContinuation>) -> Result<(), KjException> {
    then.0.await
}

/// What a C++ call fails with if `f`, the Rust half of the call, panics: a KJ exception that says
/// that `what` panicked.
fn catch_panic<T>(
    what: &str,
    f: impl FnOnce() -> Result<T, KjException>,
) -> Result<T, KjException> {
    std::panic::catch_unwind(AssertUnwindSafe(f)).unwrap_or_else(|payload| {
        Err(kj::failed!("{what} panicked: {}", io_context::panic_message(&*payload)).into())
    })
}

/// A scheduled event that Rust has started, until it finishes.
pub struct ScheduledEvent(CatchPanics<LocalFuture<bridge::ScheduledResult>>);

fn scheduled_event_start(
    invocation: KjOwn<bridge::Invocation>,
    scheduled_time: KjDate,
    cron: &[u8],
) -> Result<Box<ScheduledEvent>, KjException> {
    let finished = catch_panic("starting a scheduled event", || {
        crate::scheduled::start(crate::Invocation::new(invocation), scheduled_time, cron)
    })?;
    Ok(Box::new(ScheduledEvent(CatchPanics::new(
        Box::pin(async move { Ok(finished.await?.into()) }),
        "the future of a scheduled event",
    ))))
}

async fn scheduled_event_finish(
    event: Box<ScheduledEvent>,
) -> Result<bridge::ScheduledResult, KjException> {
    event.0.await
}

/// A test event that Rust has started, until it finishes.
pub struct TestEvent(CatchPanics<LocalFuture<bool>>);

fn test_event_start(invocation: KjOwn<bridge::Invocation>) -> Result<Box<TestEvent>, KjException> {
    let finished = catch_panic("starting a test event", || {
        crate::test::start(crate::Invocation::new(invocation))
    })?;
    Ok(Box::new(TestEvent(CatchPanics::new(
        Box::pin(finished),
        "the future of a test event",
    ))))
}

async fn test_event_finish(event: Box<TestEvent>) -> Result<bool, KjException> {
    event.0.await
}
