#![allow(clippy::unused_async)]
#![allow(clippy::missing_errors_doc)]
#![allow(clippy::must_use_candidate)]
#![allow(clippy::missing_panics_doc)]

#[cfg(test)]
mod runtime_tests;
mod test_helpers;

// The tokio-driven suite's C++ helpers (runtime_tests.rs, which reaches them through `ffi`), as
// the crate's public surface.
pub use ffi::clear_executor;
pub use ffi::clear_test_context;
pub use ffi::cross_thread_fulfill_from_thread;
pub use ffi::execute_async_on;
pub use ffi::execute_sync_from_thread;
pub use ffi::install_test_context;
pub use ffi::kj_awaits_rust_sleep;
pub use ffi::kj_yield_until_would_sleep;
pub use ffi::publish_executor;
pub use ffi::test_fulfiller_promise;
use test_helpers::completed_task_count;
use test_helpers::has_loop_runtime_handle;
use test_helpers::nested_wait_from_task;
use test_helpers::spawn_detached_completing_task;
use test_helpers::spawn_panicking_task;
use test_helpers::spawn_pending_task;
use test_helpers::spawn_task_awaiting_kj_never_promise;
use test_helpers::spawn_task_holding_kj_timer;
use test_helpers::spawn_task_on_runtime;
use test_helpers::spawn_yield_loop_task;
use test_helpers::stash_waker_future;
use test_helpers::stashed_future_poll_count;
use test_helpers::std_thread_wake_future;
use test_helpers::task_awaits_kj_timer;
use test_helpers::task_fulfills_kj_fulfiller;
use test_helpers::threaded_wake_future;
use test_helpers::tokio_sleep_on_runtime;
use test_helpers::wake_stashed_waker;

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

        /// Detaches a task onto the loop's LocalSet that awaits a 60s KJ timer promise (via
        /// `kjTimerDelay`), so the task HOLDS live KJ objects -- a TimerPromiseAdapter in the
        /// port's TimerImpl and an armed RustPromiseAwaiter Event -- when the context is torn
        /// down. Teardown-ordering regression: the LocalSet must be dropped while those KJ
        /// objects' owners (timer, event loop) are still alive.
        fn spawn_task_holding_kj_timer();
        /// Same shape, but the task awaits a never-resolving bridged KJ promise.
        fn spawn_task_awaiting_kj_never_promise();
        /// Woken from a plain std::thread while the loop is parked in the tokio port.
        async fn std_thread_wake_future() -> Result<()>;
        /// A task that yields forever; poll() must stay bounded.
        fn spawn_yield_loop_task();
        /// A task re-enters promise.wait(); must surface as an Err, not an abort.
        async fn nested_wait_from_task() -> Result<()>;
        /// A spawned task panics; the JoinHandle error surfaces as an Err (never an abort).
        async fn spawn_panicking_task() -> Result<()>;
        /// Detaches a task (drops its JoinHandle) that still runs to completion, bumping
        /// completed_task_count().
        fn spawn_detached_completing_task();

        /// Spawns a task that sleeps `delay_ms` (tokio time, i.e. while the KJ loop is parked
        /// inside the port) and then fulfills the test fulfiller installed by C++
        /// (`setTestFulfiller`) with `value` -- a KJ event armed from a tokio task, by a means
        /// other than a bridged waker, while the loop is parked. KJ's setRunnable(true) must
        /// hand the thread back to KJ; nothing else will.
        fn task_fulfills_kj_fulfiller(delay_ms: u64, value: i32);

        /// Spawns a task that sleeps `delay_ms` (tokio time), then arms and awaits a KJ timer of
        /// `timer_ms` (via `kjTimerDelay`) to completion, and awaits the task's JoinHandle. The
        /// KJ timer is registered while the loop is parked with a longer (or no) planned
        /// deadline; the port's TimerImpl::SleepHooks must notice the sooner deadline.
        async fn task_awaits_kj_timer(delay_ms: u64, timer_ms: u64) -> Result<()>;

        /// A bridged future that stashes a clone of its waker on every Pending poll (as a channel
        /// or oneshot would) and completes once `wake_stashed_waker()` has run; counts its polls.
        async fn stash_waker_future() -> Result<()>;
        /// Wakes the stashed waker on the calling (loop) thread.
        fn wake_stashed_waker();
        /// How many times `stash_waker_future`'s future has been polled.
        fn stashed_future_poll_count() -> u64;
    }

    #[namespace = "kj_rs_tokio"]
    unsafe extern "C++" {
        include!("kj-rs-tokio/tokio-event-port.h");

        type TokioAsyncIoContext = kj_rs_tokio::TokioAsyncIoContext;
    }

    unsafe extern "C++" {
        include!("kj-rs-tokio-test/test-helpers.h");

        /// `timer.afterDelay(ms)` on the test's current context timer (see setTestTimer).
        #[cxx_name = "kjTimerDelay"]
        async fn kj_timer_delay(ms: u64);
        /// `kj::NEVER_DONE` as a bridged promise.
        #[cxx_name = "kjNeverPromise"]
        async fn kj_never_promise();
        /// Re-enters `promise.wait()` on the test's WaitScope from wherever it is called. Throws
        /// (-> `Err`) when called from inside a spawned task, or on a tokio-driven loop.
        #[cxx_name = "nestedWait"]
        fn nested_wait() -> Result<()>;
        /// Fulfills the kj::PromiseFulfiller the C++ test installed with `setTestFulfiller`.
        #[cxx_name = "fulfillTestFulfiller"]
        fn fulfill_test_fulfiller(value: i32) -> Result<()>;

        // The tokio-driven suite's helpers (runtime_tests.rs); see test-helpers.h.

        /// Installs the context's timer and WaitScope as the helpers' current ones.
        #[cxx_name = "installTestContext"]
        fn install_test_context(context: Pin<&mut TokioAsyncIoContext>);
        #[cxx_name = "clearTestContext"]
        fn clear_test_context();
        /// A promise fulfilled by `fulfill_test_fulfiller`.
        #[cxx_name = "testFulfillerPromise"]
        async fn test_fulfiller_promise() -> i32;
        /// `value * 2`, computed on this loop by `kj::Executor::executeSync()` from a thread
        /// started for the call.
        #[cxx_name = "executeSyncFromThread"]
        async fn execute_sync_from_thread(value: i32) -> i32;
        /// `value`, through a cross-thread fulfiller fulfilled from a thread after `delay_ms`.
        #[cxx_name = "crossThreadFulfillFromThread"]
        async fn cross_thread_fulfill_from_thread(delay_ms: u64, value: i32) -> i32;
        /// This loop's Executor into `slot` (0 or 1); `execute_async_on` runs `value` on the
        /// loop that published `slot`, blocking until it has.
        #[cxx_name = "publishExecutor"]
        fn publish_executor(slot: u8) -> Result<()>;
        #[cxx_name = "clearExecutor"]
        fn clear_executor(slot: u8) -> Result<()>;
        #[cxx_name = "executeAsyncOn"]
        async fn execute_async_on(slot: u8, value: i32) -> i32;
        #[cxx_name = "kjYieldUntilWouldSleep"]
        async fn kj_yield_until_would_sleep();
        /// A KJ coroutine awaiting `tokio_sleep_on_runtime(ms)`.
        #[cxx_name = "kjAwaitsRustSleep"]
        async fn kj_awaits_rust_sleep(ms: u64);
    }
}
