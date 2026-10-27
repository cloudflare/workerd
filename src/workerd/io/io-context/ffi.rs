// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The bridge to the C++ `IoContext` calls in `ffi.h`.

#![allow(
    unsafe_code,
    reason = "the cxx bridge expands to unsafe FFI glue, current_id() reads through the \
              IoContext reference it returns, and io_callback_run() opens a scope C++ has set up"
)]

use std::any::Any;
use std::future::Future;
use std::pin::Pin;

use jsg::v8;

#[cxx::bridge(namespace = "workerd::io_context")]
pub mod bridge {
    #[namespace = "workerd"]
    unsafe extern "C++" {
        include!("workerd/io/io-context.h");

        type IoContext;
    }

    unsafe extern "C++" {
        include!("workerd/io/io-context/ffi.h");

        type IoOwnedObject;

        /// The caller chooses the lifetime of the result; see `current_id()`.
        unsafe fn io_context_try_current<'a>() -> KjMaybe<Pin<&'a mut IoContext>>;
        fn io_context_id(context: &IoContext) -> u64;
        fn io_context_now(id: u64) -> Result<i64>;
        fn io_context_write_logfwdr(id: u64, channel: u32, message: &[u8]) -> Result<()>;
        fn io_context_await_io(
            id: u64,
            future: Box<IoFuture>,
            callback: Box<IoCallback>,
        ) -> Result<()>;
        fn io_context_add_object(id: u64, slot: Box<IoOwnedSlot>) -> Result<KjOwn<IoOwnedObject>>;
    }

    extern "Rust" {
        type IoFuture;
        type IoOutput;
        type IoCallback;
        type IoOwnedSlot;

        async fn io_future_wait(future: Box<IoFuture>) -> Box<IoOutput>;
        unsafe fn io_callback_run(isolate: usize, callback: Box<IoCallback>, output: Box<IoOutput>);
    }
}

/// The id of the current `IoContext`, or `None` outside a request.
pub fn current_id() -> Option<u64> {
    // SAFETY: the reference is used only to read the id, before this function returns, and the
    // current IoContext stays alive while code runs in it.
    let context: Option<Pin<&mut bridge::IoContext>> =
        unsafe { bridge::io_context_try_current() }.into();
    context.map(|context| bridge::io_context_id(&context))
}

/// The future that `IoContext::await_io()` waits for, with its output type erased.
pub struct IoFuture(pub Pin<Box<dyn Future<Output = Box<dyn Any + Send>> + Send>>);

/// The output of an [`IoFuture`].
pub struct IoOutput(Box<dyn Any + Send>);

/// What [`IoCallback`] calls.
pub type IoCallbackFn = Box<dyn for<'s> FnOnce(&mut jsg::Lock<'s>, Box<dyn Any + Send>)>;

/// The callback that `IoContext::await_io()` runs with the output of its future.
pub struct IoCallback(pub IoCallbackFn);

/// A value that `IoContext::add_object()` hands to C++, which drops it with the `IoContext`.
/// Dropping it drops the value it holds, unless Rust took the value first.
pub struct IoOwnedSlot(#[expect(dead_code, reason = "only dropped")] pub Box<dyn Send>);

async fn io_future_wait(future: Box<IoFuture>) -> Box<IoOutput> {
    Box::new(IoOutput(future.0.await))
}

/// Runs `callback` with `output`, from `io_context_await_io()` in the `IoContext`, with the
/// isolate locked, a context entered and a `HandleScope` open.
unsafe fn io_callback_run(isolate: usize, callback: Box<IoCallback>, output: Box<IoOutput>) {
    let scope = std::pin::pin!(
        // SAFETY: C++ calls this with the isolate locked, a context entered and a HandleScope
        // open, which outlive the scope.
        unsafe { jsg::CallbackScope::new(isolate as *mut v8::ffi::Isolate) }
    );
    // A panic throws an internal error and terminates execution, as in a `#[jsg_method]`.
    let _ = jsg::catch_panic(scope, |lock| (callback.0)(lock, output.0));
}
