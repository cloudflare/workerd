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

/// See the bridge doc on `spawn_task_holding_kj_timer` (lib.rs).
pub fn spawn_task_holding_kj_timer() {
    drop(kj_rs_tokio::spawn(async {
        let _ = crate::ffi::kj_timer_delay(60_000).await;
    }));
}

/// Like `spawn_task_holding_kj_timer`, but the detached task awaits a bridged KJ promise that
/// never resolves (`kjNeverPromise`): at teardown it holds an `OwnPromiseNode` and an armed
/// `RustPromiseAwaiter` event registered with the loop, which the `LocalSet` cancellation must
/// drop while the loop still exists.
pub fn spawn_task_awaiting_kj_never_promise() {
    drop(kj_rs_tokio::spawn(async {
        let _ = crate::ffi::kj_never_promise().await;
    }));
}

/// A bridged future woken from a plain `std::thread` (no tokio, no KJ) ~20ms after its first
/// poll, while the loop is parked in the port's `wait()`. Exercises the full cross-thread path
/// into a PARKED tokio port: `FutureWakerCell` cross-thread fulfiller -> loop `Executor` -> port
/// `wake()` -> `block_on` unpark. (Prep's cross-thread tests all run on a plain `kj::EventLoop`.)
pub async fn std_thread_wake_future() -> Result<()> {
    struct F {
        woken: std::sync::Arc<std::sync::atomic::AtomicBool>,
        spawned: bool,
    }
    impl std::future::Future for F {
        type Output = ();
        fn poll(
            mut self: std::pin::Pin<&mut Self>,
            cx: &mut std::task::Context<'_>,
        ) -> std::task::Poll<()> {
            if self.woken.load(Ordering::SeqCst) {
                return std::task::Poll::Ready(());
            }
            if !self.spawned {
                self.spawned = true;
                let waker = cx.waker().clone();
                let woken = std::sync::Arc::clone(&self.woken);
                std::thread::spawn(move || {
                    std::thread::sleep(Duration::from_millis(20));
                    woken.store(true, Ordering::SeqCst);
                    waker.wake();
                });
            }
            std::task::Poll::Pending
        }
    }
    F {
        woken: std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false)),
        spawned: false,
    }
    .await;
    Ok(())
}

/// Detaches a task that re-readies itself forever (`yield_now` in a loop). `poll()` must stay
/// bounded (`POLL_YIELD_BUDGET`) and the KJ loop must not be starved; the task is cancelled at
/// teardown.
pub fn spawn_yield_loop_task() {
    drop(kj_rs_tokio::spawn(async {
        loop {
            tokio::task::yield_now().await;
        }
    }));
}

/// Spawns a task that re-enters `promise.wait()` on the loop thread (via the C++ helper
/// `nestedWait`), which nests `block_on` inside the port's `block_on`. The resulting panic must
/// surface to the task as a catchable `kj::Exception` (an `Err` here), never an abort, and the
/// outer loop must keep working. Resolves `Ok` if the task observed the error.
pub async fn nested_wait_from_task() -> Result<()> {
    let observed_error = kj_rs_tokio::spawn(async { crate::ffi::nested_wait().is_err() })
        .await
        .map_err(Error::other)?;
    if observed_error {
        Ok(())
    } else {
        Err(Error::other(
            "nested wait() inside a spawned task unexpectedly succeeded",
        ))
    }
}

/// Spawns a task that panics and awaits its `JoinHandle`. tokio isolates the panic into a
/// `JoinError` (never an abort), which this maps to an `Err` -> a rejected bridged promise. The
/// C++ test asserts it throws and that the loop stays healthy afterward. This is the only
/// coverage of the `JoinHandle` error path (`join_handle.await.map_err(...)`).
pub async fn spawn_panicking_task() -> Result<()> {
    kj_rs_tokio::spawn(async {
        panic!("deliberate panic in a spawned task");
    })
    .await
    .map_err(Error::other)?;
    Ok(())
}

/// Detaches (drops the `JoinHandle` of) a task that, after a short delay, bumps
/// `COMPLETED_TASKS`. The positive counterpart of `spawn_pending_task`: a detached task still
/// runs to completion (dropping the handle does not cancel it).
pub fn spawn_detached_completing_task() {
    drop(kj_rs_tokio::spawn(async {
        tokio::time::sleep(Duration::from_millis(5)).await;
        COMPLETED_TASKS.fetch_add(1, Ordering::SeqCst);
    }));
}

/// See the bridge doc on `task_fulfills_kj_fulfiller` (lib.rs).
pub fn task_fulfills_kj_fulfiller(delay_ms: u64, value: i32) {
    drop(kj_rs_tokio::spawn(async move {
        tokio::time::sleep(Duration::from_millis(delay_ms)).await;
        let _ = crate::ffi::fulfill_test_fulfiller(value);
    }));
}

/// See the bridge doc on `task_awaits_kj_timer` (lib.rs).
pub async fn task_awaits_kj_timer(delay_ms: u64, timer_ms: u64) -> Result<()> {
    kj_rs_tokio::spawn(async move {
        tokio::time::sleep(Duration::from_millis(delay_ms)).await;
        // Infallible on the C++ side; the bridge still types it as a Result.
        let _ = crate::ffi::kj_timer_delay(timer_ms).await;
    })
    .await
    .map_err(Error::other)
}
