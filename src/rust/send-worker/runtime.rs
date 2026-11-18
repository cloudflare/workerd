// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The runtime send workers run on: a multi-thread tokio runtime of its own, apart from the
//! `current_thread` runtime under the KJ event loop.
//!
//! [`Runtime`] is one such runtime. [`shared`] is the process's: it is built by the first
//! caller and handed to every later one, and [`shutdown_shared`] ends it. Whoever owns the event
//! loop calls [`shutdown_shared`] once the loop has stopped running requests and before it tears
//! the loop down, so that no worker task outlives the loop it sends its results to.
//!
//! A runtime's threads never touch KJ: what a worker task sends to the loop thread goes through
//! tokio channels, whose wakers may be invoked from any thread.

use std::io;
use std::sync::Mutex;
use std::sync::PoisonError;
use std::time::Duration;

use tokio::runtime::Builder;
use tokio::runtime::Handle;

use crate::Result;

/// The worker threads of a runtime whose size nobody chose.
pub const DEFAULT_THREADS: usize = 2;

/// A multi-thread tokio runtime for send workers.
pub struct Runtime(tokio::runtime::Runtime);

impl Runtime {
    /// Builds a runtime of `threads` worker threads (at least one), with tokio's timer and I/O
    /// drivers.
    ///
    /// # Errors
    ///
    /// The threads or the I/O driver could not be created.
    pub fn new(threads: usize) -> io::Result<Self> {
        Builder::new_multi_thread()
            .worker_threads(threads.max(1))
            .thread_name("send-worker")
            .enable_all()
            .build()
            .map(Self)
    }

    /// What tasks are spawned through. A handle that outlives the runtime spawns tasks that are
    /// cancelled at once.
    #[must_use]
    pub fn handle(&self) -> Handle {
        self.0.handle().clone()
    }

    /// Stops the runtime: every task is dropped at its next yield point, and the call waits up
    /// to `timeout` for the worker threads to end.
    ///
    /// It blocks, so it must be called from outside any tokio task: tokio panics otherwise.
    pub fn shutdown(self, timeout: Duration) {
        self.0.shutdown_timeout(timeout);
    }
}

enum Shared {
    NotStarted,
    Running(Runtime),
    ShutDown,
}

static SHARED: Mutex<Shared> = Mutex::new(Shared::NotStarted);

/// The process's runtime for send workers, built by the first call with `threads` worker
/// threads. Later calls get the same runtime, whatever `threads` they pass.
///
/// # Errors
///
/// The runtime could not be built, or [`shutdown_shared`] has ended it.
pub fn shared(threads: usize) -> Result<Handle> {
    // Held across the build, so that two first callers build one runtime.
    let mut shared = SHARED.lock().unwrap_or_else(PoisonError::into_inner);
    let handle = match &*shared {
        Shared::NotStarted => {
            let runtime = Runtime::new(threads)
                .map_err(|e| kj::failed!("the send worker runtime could not start: {e}"))?;
            let handle = runtime.handle();
            *shared = Shared::Running(runtime);
            handle
        }
        Shared::Running(runtime) => runtime.handle(),
        Shared::ShutDown => return Err(kj::failed!("the send worker runtime has shut down")),
    };
    drop(shared);
    Ok(handle)
}

/// Ends the process's runtime, if one was started, as [`Runtime::shutdown`] does; later calls of
/// [`shared`] fail. Like [`Runtime::shutdown`], it must be called from outside any tokio task.
pub fn shutdown_shared(timeout: Duration) {
    let shared = {
        let mut shared = SHARED.lock().unwrap_or_else(PoisonError::into_inner);
        std::mem::replace(&mut *shared, Shared::ShutDown)
    };
    if let Shared::Running(runtime) = shared {
        runtime.shutdown(timeout);
    }
}

#[cfg(test)]
#[path = "runtime-test.rs"]
mod tests;
