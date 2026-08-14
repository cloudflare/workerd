use std::future::Future;
use std::pin::Pin;
use std::sync::Arc;
use std::sync::atomic::AtomicBool;
use std::sync::atomic::AtomicU64;
use std::sync::atomic::Ordering;
use std::task::Context;
use std::task::Poll;
use std::time::Duration;

use crate::Error;
use crate::Result;

/// Process-wide counter of completed `spawn_task_on_runtime` tasks, so C++ tests can verify the
/// spawned task really ran (on the loop runtime) rather than the value being produced some other
/// way.
static COMPLETED_TASKS: AtomicU64 = AtomicU64::new(0);

pub fn completed_task_count() -> u64 {
    COMPLETED_TASKS.load(Ordering::SeqCst)
}

pub fn has_loop_runtime_handle() -> bool {
    kj_rs_tokio::current_handle().is_some()
}

pub async fn spawn_task_on_runtime(delay_ms: u64, value: u32) -> Result<u32> {
    let join_handle = kj_rs_tokio::spawn(async move {
        tokio::time::sleep(Duration::from_millis(delay_ms)).await;
        COMPLETED_TASKS.fetch_add(1, Ordering::SeqCst);
        value
    });
    join_handle.await.map_err(Error::other)
}

pub async fn tokio_sleep_on_runtime(delay_ms: u64) -> Result<()> {
    let join_handle = kj_rs_tokio::spawn(async move {
        tokio::time::sleep(Duration::from_millis(delay_ms)).await;
    });
    join_handle.await.map_err(Error::other)
}

pub fn spawn_pending_task() {
    // Deliberately detached: dropping the JoinHandle does not cancel the task; it stays pending
    // in the runtime until the runtime itself is dropped with the TokioEventPort.
    drop(kj_rs_tokio::spawn(std::future::pending::<()>()));
}

/// A future that, on its first poll, spawns a task on the loop's own tokio runtime which sleeps
/// briefly and then wakes a clone of the future's waker. Because that runtime is driven by the
/// `TokioEventPort` on the event loop's own thread, the wake arrives *same-thread*. Exercises a
/// bridged future being suspended and re-driven by an asynchronous same-thread wake (via a cloned
/// waker) under the tokio-backed event port.
struct DelayedWakeFuture {
    spawned: bool,
    done: Arc<AtomicBool>,
}

impl Future for DelayedWakeFuture {
    type Output = ();

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<()> {
        if self.done.load(Ordering::SeqCst) {
            return Poll::Ready(());
        }
        if !self.spawned {
            self.spawned = true;
            let waker = cx.waker().clone();
            let done = Arc::clone(&self.done);
            // Detached task on the loop runtime; it runs on the loop thread when the port drives
            // the runtime, so `waker.wake()` arms the FuturePollEvent same-thread.
            drop(kj_rs_tokio::spawn(async move {
                tokio::time::sleep(Duration::from_millis(10)).await;
                done.store(true, Ordering::SeqCst);
                waker.wake();
            }));
        }
        Poll::Pending
    }
}

pub async fn threaded_wake_future() -> Result<()> {
    DelayedWakeFuture {
        spawned: false,
        done: Arc::new(AtomicBool::new(false)),
    }
    .await;
    Ok(())
}
