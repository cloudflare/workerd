#![allow(clippy::unused_async)]
#![allow(clippy::missing_errors_doc)]
#![allow(clippy::must_use_candidate)]
#![allow(clippy::missing_panics_doc)]

mod test_helpers;

use test_helpers::completed_task_count;
use test_helpers::has_loop_runtime_handle;
use test_helpers::spawn_pending_task;
use test_helpers::spawn_task_on_runtime;
use test_helpers::threaded_wake_future;
use test_helpers::tokio_sleep_on_runtime;

type Result<T> = std::io::Result<T>;
type Error = std::io::Error;

#[cxx::bridge(namespace = "kj_rs_tokio_test")]
mod ffi {
    extern "Rust" {
        /// Spawns a task on this thread's KJ-loop tokio runtime which sleeps `delay_ms` (real
        /// tokio time) and then produces `value`; awaits its `JoinHandle`. The returned future is
        /// bridged to a `kj::Promise` by kj-rs and polled by the KJ event loop; the spawned task
        /// itself only runs while the loop sleeps inside `TokioEventPort::wait()`/`poll()`.
        async fn spawn_task_on_runtime(delay_ms: u64, value: u32) -> Result<u32>;

        /// Spawns a `tokio::time::sleep` on the loop runtime and awaits it (via the task's
        /// `JoinHandle`).
        async fn tokio_sleep_on_runtime(delay_ms: u64) -> Result<()>;

        /// A bridged Rust future that, on its first poll, spawns a task on the loop runtime which
        /// sleeps ~10ms (real tokio time) and then wakes a clone of the future's waker. The wake
        /// therefore arrives same-thread (the runtime is driven by the port on the loop thread),
        /// exercising the future⇄promise bridge's asynchronous same-thread re-drive path.
        async fn threaded_wake_future() -> Result<()>;

        /// Detaches a never-completing task onto the loop runtime (for teardown testing: the
        /// runtime must drop it cleanly when the port is destroyed).
        fn spawn_pending_task();

        /// Number of `spawn_task_on_runtime` tasks that ran to completion (process-wide).
        fn completed_task_count() -> u64;

        /// True if `kj_rs_tokio::current_handle()` finds a runtime on this thread.
        fn has_loop_runtime_handle() -> bool;
    }
}
