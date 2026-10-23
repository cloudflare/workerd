// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The server's background tasks: futures no request owns (an actor's on-broken monitor, its
//! idle shutdown, a namespace's container cleanup, a dropped stub's deferred unlink, the
//! `workerd test` loop).
//!
//! They run in the worker factory's `kj::TaskSet`, polled by the KJ event loop like every other
//! future the server hands C++, so they are ordered with the events they react to: a monitor
//! sees its actor break before the failed request's reply reaches the caller. They also end
//! with the server (its drop clears them; `Factory::settle_tasks` ends the ones the drop
//! spawned), before V8 goes away. Neither holds for tasks on the tokio `LocalSet`
//! (`kj_rs_tokio::spawn`), which run only once the KJ loop idles and live until the event port is
//! torn down.

use std::future::Future;

use futures::FutureExt;
use futures::future::LocalBoxFuture;
use futures::future::RemoteHandle;

use crate::Result;
use crate::bridge::ffi;
use crate::config::Factory;

/// A spawned task's owner. Dropping the handle cancels the task: its future is dropped at the
/// task's next poll, never mid-poll, so a task may drop its own handle.
pub type TaskHandle = RemoteHandle<()>;

/// The future `ffi::factory_spawn` hands to the C++ task set, polled there by `task_run`.
pub struct SpawnedTask(LocalBoxFuture<'static, ()>);

/// Runs a spawned task to its end; the C++ side awaits this as the task's promise.
pub async fn task_run(task: Box<SpawnedTask>) -> Result<()> {
    task.0.await;
    Ok(())
}

impl Factory {
    /// Spawns `future` as a background task of the run; it starts on the next turn of the event
    /// loop. The returned handle resolves to its output, and cancels it when dropped.
    pub fn spawn<F: Future + 'static>(&self, future: F) -> RemoteHandle<F::Output> {
        let (task, handle) = future.remote_handle();
        self.spawn_detached(task);
        handle
    }

    /// Spawns `future` to run until it ends or the run does.
    pub fn spawn_detached(&self, future: impl Future<Output = ()> + 'static) {
        ffi::factory_spawn(self.raw(), Box::new(SpawnedTask(future.boxed_local())));
    }

    /// Ends the background tasks the server's drop spawned (a dropped stub unlinks its worker
    /// on the next turn): they run, and whatever remains is dropped.
    pub async fn settle_tasks(&self) -> Result<()> {
        Ok(ffi::factory_settle_tasks(self.raw()).await?)
    }
}
