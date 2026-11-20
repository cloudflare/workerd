// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The parts of WebAssembly that workerd adds to V8's, which `WorkerdApi` installs in each
//! isolate:
//!
//! - [`compile`]: `WebAssembly.compile()`, `instantiate()`, `compileStreaming()` and
//!   `instantiateStreaming()` settle in the request that called them, while that request runs
//!   V8's foreground tasks for them.
//! - [`streaming`]: `compileStreaming()` and `instantiateStreaming()` under the
//!   `webassembly_streaming` compatibility flag.
//! - [`dynamic`]: compiling WebAssembly while handling a request under the
//!   `webassembly_dynamic_compilation` compatibility flag, with each module going to the embedder
//!   first.

mod compile;
mod dynamic;
mod ffi;
mod streaming;

use std::future::Future;
use std::pin::Pin;
use std::task::Context;
use std::task::Poll;

use kj_rs::KjArc;

pub use crate::dynamic::ModuleBytes;

/// The isolate's foreground task signal, which its task runners notify whenever V8 posts a
/// foreground task. Posts advance an epoch, so a waiter that read the epoch before starting the
/// work can't miss one.
#[derive(Clone)]
struct ForegroundTaskSignal(KjArc<ffi::bridge::ForegroundTaskSignal>);

jsg::jsg_traced_unchecked!(ForegroundTaskSignal);

impl ForegroundTaskSignal {
    fn of(lock: &mut jsg::Lock<'_>) -> Self {
        // SAFETY: the lock guarantees the isolate is locked.
        #[expect(unsafe_code, reason = "calls into C++ with the locked isolate")]
        Self(unsafe { ffi::bridge::foreground_task_signal(lock.isolate().as_ffi()) })
    }

    fn epoch(&self) -> u64 {
        ffi::bridge::foreground_task_signal_epoch(&self.0)
    }

    fn notify(&self) {
        ffi::bridge::foreground_task_signal_notify(&self.0);
    }

    /// Resolves once V8 has posted a foreground task since the epoch was `seen`. Must be polled
    /// on a thread with a KJ event loop.
    fn wait_after(&self, seen: u64) -> impl Future<Output = ()> + Send + 'static {
        let signal = self.clone();
        OnIoContextThread(async move {
            // It never fails: the promise is a cross-thread fulfiller's.
            let _ = ffi::bridge::foreground_task_signal_wait_after(&signal.0, seen).await;
        })
    }
}

/// A future that waits for a KJ promise, for `IoContext::await_io_and_run()`, which requires its
/// future to be `Send` since it runs without the isolate lock.
struct OnIoContextThread<F>(F);

// SAFETY: `IoContext::await_io_and_run()` polls its future, and drops it, only on the IoContext's
// thread, which is where the crate creates the KJ promises these futures wait for.
#[expect(unsafe_code, reason = "a KJ promise stays on its IoContext's thread")]
unsafe impl<F> Send for OnIoContextThread<F> {}

impl<F: Future> Future for OnIoContextThread<F> {
    type Output = F::Output;

    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<F::Output> {
        // SAFETY: the future is structurally pinned: it is never moved out of `self`.
        #[expect(unsafe_code, reason = "pin projection")]
        let future = unsafe { self.map_unchecked_mut(|this| &mut this.0) };
        future.poll(cx)
    }
}
