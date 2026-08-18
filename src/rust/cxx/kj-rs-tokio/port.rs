//! Per-thread tokio `current_thread` runtime management and the Rust half of `TokioEventPort`.

use std::cell::RefCell;
use std::future::Future;
use std::rc::Rc;
use std::sync::Arc;
#[cfg(unix)]
use std::sync::Condvar;
#[cfg(unix)]
use std::sync::Mutex;
#[cfg(unix)]
use std::sync::PoisonError;
use std::sync::atomic::AtomicBool;
use std::sync::atomic::Ordering;
use std::time::Duration;
#[cfg(unix)]
use std::time::Instant;

use tokio::runtime::Builder;
use tokio::runtime::Handle;
use tokio::runtime::Runtime;
use tokio::sync::Notify;
use tokio::task::JoinHandle;
use tokio::task::LocalSet;

#[cfg(unix)]
use crate::ffi;

/// How many scheduler turns `poll()` grants the runtime. Each `yield_now` re-queues the main
/// future at the back of the run queue, so every already-ready spawned task gets a chance to run
/// (repeatedly, up to the budget) without ever parking the thread.
///
/// The value is a latency/throughput compromise, not derived from any tokio internal: large
/// enough to drain a typical burst of already-ready tasks in one `poll()` call, small enough to
/// bound how long `poll()` withholds control from the KJ loop when spawned tasks keep re-readying
/// each other. Safe to retune if profiling shows either starvation or excessive poll latency.
const POLL_YIELD_BUDGET: u32 = 16;

/// Timeouts strictly below this go through the [`HiResTimer`] short-sleep path: tokio's timer
/// wheel has ~1 ms granularity, which would quantize sub-millisecond KJ timers (e.g. a 100 µs
/// `timer.afterDelay`) to a ~1 ms sleep. At and above a couple of milliseconds the wheel's
/// error is proportionally small, so long sleeps stay on the plain tokio path.
#[cfg(unix)]
const HIRES_TIMEOUT_THRESHOLD: Duration = Duration::from_millis(2);
thread_local! {
    /// Handle to the `TokioPort` runtime driving the KJ event loop on this thread, if any.
    /// Registered by `TokioPort::new` and cleared on drop. Used by code (e.g. kj-rs-io) that
    /// needs to *enter* the runtime context so tokio resources can register with its I/O driver
    /// and timers.
    static LOOP_RUNTIME_HANDLE: RefCell<Option<Handle>> = const { RefCell::new(None) };

    /// The `LocalSet` onto which [`spawn`] enqueues tasks, and which `wait_*`/`poll` drive. Held
    /// behind an `Rc` so `spawn` (which has no `&TokioPort`) can reach it while the port keeps
    /// driving it. The `LocalSet` is `!Send`/`!Sync` and lives entirely on the loop thread, which
    /// is why `TokioPort` itself does NOT hold it (it must stay `Send + Sync` for cross-thread
    /// `wake()`); ownership lives here and is dropped in `TokioPort::drop`.
    static LOOP_LOCAL_SET: RefCell<Option<Rc<LocalSet>>> = const { RefCell::new(None) };
}

/// Returns a handle to this thread's KJ-loop tokio runtime, if a `TokioEventPort` exists on this
/// thread.
#[must_use]
pub fn current_handle() -> Option<Handle> {
    LOOP_RUNTIME_HANDLE.with(|h| h.borrow().clone())
}

/// Returns this thread's KJ-loop `LocalSet`, if a `TokioEventPort` exists on this thread.
fn current_local_set() -> Option<Rc<LocalSet>> {
    LOOP_LOCAL_SET.with(|l| l.borrow().clone())
}

/// Spawns a future onto this thread's KJ-loop tokio runtime.
///
/// The task runs whenever the KJ event loop sleeps (i.e. whenever C++ is blocked in
/// `promise.wait(waitScope)` or pumping via `poll()`), driven by the port's [`LocalSet`].
///
/// The future is spawned with [`LocalSet::spawn_local`], so it is pinned to this (the loop)
/// thread and does **not** need to be `Send`: the per-thread `current_thread` runtime never
/// migrates a task to another thread. This is what lets bridged futures that hold `!Send` KJ
/// handles (`OwnPromiseNode`, `kj::Own`, ...) be spawned directly. `JoinHandle`/drop-cancels
/// semantics are the same as `tokio::spawn`.
///
/// # Panics
///
/// Panics if no `TokioEventPort` has been created on this thread.
pub fn spawn<F>(future: F) -> JoinHandle<F::Output>
where
    F: Future + 'static,
    F::Output: 'static,
{
    #[expect(
        clippy::expect_used,
        reason = "documented `# Panics` on this public API: calling spawn() without first creating a TokioEventPort on this thread is a caller-contract violation, not a recoverable runtime path"
    )]
    let local = current_local_set()
        .expect("no kj-rs-tokio runtime on this thread; create a TokioEventPort first");
    local.spawn_local(future)
}
