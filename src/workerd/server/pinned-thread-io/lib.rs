// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The `IoContext` APIs that take values tied to the OS thread, such as an `Rc` with clones
//! elsewhere on the event loop, for code of the workerd binary.
//!
//! [`IoContext::await_io_local`] and [`IoContext::add_local_object`] are `unsafe`, since a host
//! may move an event loop between OS threads while they hold such values. The workerd binary
//! never does. The event loops that run its `IoContext`s come from `kj::setupAsyncIo()`, which in
//! the binary is kj-rs-tokio's (src/workerd/util/setup-async-io.c++): a
//! `kj_rs_tokio::TokioEventPort`, whose tasks live on a `LocalSet` of the thread that made it, so
//! the loop runs on that thread until it is destroyed. This crate states that once, and its Bazel
//! visibility keeps it to the binary's packages.

use io_context::IoContext;
use io_context::IoOwnedLocal;
use jsg::v8;

/// [`IoContext::await_io_local`]: waits for `future`, which need not be `Send`, and runs
/// `callback` with its output, for the promise it returns.
///
/// # Errors
/// As for [`IoContext::await_io`].
pub fn await_io<'s, T, F, C>(
    context: &IoContext<'s>,
    lock: &mut jsg::Lock<'s>,
    future: F,
    callback: C,
) -> jsg::Result<v8::Local<'s, v8::Promise>>
where
    T: 'static,
    F: Future<Output = T> + 'static,
    C: for<'t> FnOnce(&mut jsg::Lock<'t>, T) -> jsg::Result<v8::Local<'t, v8::Value>> + 'static,
{
    #[expect(
        unsafe_code,
        reason = "calls IoContext::await_io_local; see the SAFETY comment"
    )]
    // SAFETY: the event loop is the workerd binary's, which stays on this thread (crate docs).
    unsafe {
        context.await_io_local(lock, future, callback)
    }
}

/// [`IoContext::add_local_object`]: gives `context` ownership of `value`, which need not be
/// `Send`.
///
/// # Errors
/// As for [`IoContext::add_local_object`].
pub fn add_object<T: 'static>(context: &IoContext<'_>, value: T) -> jsg::Result<IoOwnedLocal<T>> {
    #[expect(
        unsafe_code,
        reason = "calls IoContext::add_local_object; see the SAFETY comment"
    )]
    // SAFETY: the event loop is the workerd binary's, which stays on this thread (crate docs).
    unsafe {
        context.add_local_object(value)
    }
}
