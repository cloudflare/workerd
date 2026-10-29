// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The bridge to the C++ `IoContext` calls in `ffi.h`.

#![allow(
    unsafe_code,
    reason = "the cxx bridge expands to unsafe FFI glue, and io_callback_run() opens a scope C++ \
              has set up"
)]

use std::any::Any;
use std::future::Future;
use std::panic::AssertUnwindSafe;
use std::pin::Pin;
use std::task::Poll;

use jsg::v8;

#[cxx::bridge(namespace = "workerd::io_context")]
pub mod bridge {
    unsafe extern "C++" {
        include!("workerd/io/io-context/ffi.h");

        type IoOwnedObject;

        fn io_context_no_request_message() -> &'static str;
        fn io_context_try_current_id() -> KjMaybe<u64>;
        fn io_context_require_current(id: u64) -> Result<()>;
        fn io_context_now(id: u64) -> Result<i64>;
        fn io_context_write_logfwdr(id: u64, channel: u32, message: &[u8]) -> Result<()>;
        fn io_context_await_io(
            id: u64,
            future: Box<IoFuture>,
            callback: Box<IoCallback>,
        ) -> Result<()>;
        fn io_context_add_object(id: u64, slot: Box<IoOwnedSlot>) -> Result<KjOwn<IoOwnedObject>>;
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

        async fn io_future_wait(future: Box<IoFuture>) -> Result<Box<IoOutput>>;
        unsafe fn io_callback_run(
            isolate: usize,
            callback: Box<IoCallback>,
            output: Box<IoOutput>,
        ) -> IoCallbackOutcome;
    }
}

/// The future that `IoContext::await_io()` waits for, with its output type erased.
pub struct IoFuture(pub Pin<Box<dyn Future<Output = Box<dyn Any + Send>> + Send>>);

/// The output of an [`IoFuture`].
pub struct IoOutput(Box<dyn Any + Send>);

/// What [`IoCallback`] calls.
pub type IoCallbackFn =
    Box<dyn for<'s> FnOnce(&mut jsg::Lock<'s>, Box<dyn Any + Send>) -> jsg::Result<()>>;

/// The callback that `IoContext::await_io()` runs with the output of its future.
pub struct IoCallback(pub IoCallbackFn);

/// A value that `IoContext::add_object()` hands to C++, which drops it with the `IoContext`.
/// Dropping it drops the value it holds, unless Rust took the value first.
pub struct IoOwnedSlot(#[expect(dead_code, reason = "only dropped")] pub Box<dyn Send>);

/// Waits for `future`. A panic while polling it fails the wait, so that it becomes a KJ exception
/// rather than unwinding into the KJ event loop, which would abort the process.
async fn io_future_wait(future: Box<IoFuture>) -> Result<Box<IoOutput>, cxx::KjError> {
    let mut future = future.0;
    std::future::poll_fn(move |cx| {
        match std::panic::catch_unwind(AssertUnwindSafe(|| future.as_mut().poll(cx))) {
            Ok(poll) => poll.map(|output| Ok(Box::new(IoOutput(output)))),
            Err(payload) => Poll::Ready(Err(cxx::KjError::new(
                cxx::KjExceptionType::Failed,
                format!(
                    "the future that IoContext::await_io() waited for panicked: {}",
                    panic_message(&*payload)
                ),
            ))),
        }
    })
    .await
}

fn panic_message(payload: &(dyn Any + Send)) -> &str {
    if let Some(message) = payload.downcast_ref::<&str>() {
        message
    } else if let Some(message) = payload.downcast_ref::<String>() {
        message
    } else {
        "<non-string panic payload>"
    }
}

/// Runs `callback` with `output`, from `io_context_await_io()` in the `IoContext`, with the
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
