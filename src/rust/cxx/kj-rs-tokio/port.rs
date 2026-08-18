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
/// State shared with `wake()` callers on other threads.
struct SharedState {
    /// Unblocks the `block_on(...)` inside `wait_*` when `wake()` or `notify_runnable()` fires.
    notify: Notify,

    /// The `kj::EventPort::wake()` latch: set by `wake()`, consumed (swapped to `false`) by the
    /// return value of `wait_*`/`poll`. The KJ event loop uses a `true` return to know it must
    /// drain cross-thread events (`kj::Executor`, `CrossThreadPromiseFulfiller`).
    woken: AtomicBool,

    /// True while the loop thread is parked inside `wait_*`'s `block_on`. Only mutated from the
    /// loop thread; read by `notify_runnable` (also loop-thread-only, but kept atomic so the
    /// whole struct is `Sync` and the flag is safe if that ever changes).
    sleeping: AtomicBool,
}

/// A lazily-started dedicated thread delivering high-resolution wakeups for short
/// `wait_timeout_ns` sleeps (see [`HIRES_TIMEOUT_THRESHOLD`]).
///
/// For a short timeout, `wait_impl` arms this timer with an absolute deadline before parking in
/// `block_on`; the thread performs a precise absolute sleep (`mach_wait_until` on macOS,
/// `clock_nanosleep(TIMER_ABSTIME)` on Linux) and then pokes the port's `Notify`. This composes
/// with every other wake-up source because it *is* the same mechanism (`wake()` and
/// `notify_runnable()` hit the same `Notify`); the tokio-side `timeout` stays armed as a coarse
/// backstop, so a lost or late high-res wakeup only degrades to the wheel's ~1 ms behavior,
/// never a hang. While unused the thread parks on a condvar (zero CPU); joined on drop.
#[cfg(unix)]
struct HiResTimer {
    shared: Arc<HiResShared>,
    /// Lazily spawned by the first `arm()`; joined on drop. Only the loop thread arms, but the
    /// handle sits behind a mutex so the struct is `Sync` without further reasoning.
    thread: Mutex<Option<std::thread::JoinHandle<()>>>,
}

#[cfg(unix)]
struct HiResShared {
    /// The port state whose `notify` the timer thread pokes when a deadline is reached.
    port: Arc<SharedState>,
    request: Mutex<HiResRequest>,
    condvar: Condvar,
}

#[cfg(unix)]
struct HiResRequest {
    /// Absolute deadline of the currently armed request, if any.
    deadline: Option<Instant>,
    /// Bumped by `arm()` and `disarm()`. A wakeup is only delivered if the generation still
    /// matches once the sleep finishes, so a canceled request (the wait was woken early by
    /// `wake()`/`notify_runnable()`) does not leak a stale `Notify` permit into a later wait.
    /// A lost race here is harmless: spurious early returns are explicitly allowed by the
    /// `kj::EventPort::wait()` contract.
    generation: u64,
    shutdown: bool,
}
impl HiResTimer {
    fn new(port: Arc<SharedState>) -> Self {
        Self {
            shared: Arc::new(HiResShared {
                port,
                request: Mutex::new(HiResRequest {
                    deadline: None,
                    generation: 0,
                    shutdown: false,
                }),
                condvar: Condvar::new(),
            }),
            thread: Mutex::new(None),
        }
    }

    /// Requests a `notify` on the port at `deadline`. Called on the loop thread right before it
    /// parks in `block_on`.
    fn arm(&self, deadline: Instant) {
        {
            let mut req = self
                .shared
                .request
                .lock()
                .unwrap_or_else(PoisonError::into_inner);
            req.deadline = Some(deadline);
            req.generation += 1;
        }
        self.shared.condvar.notify_one();
        // Lazily start the thread on first use, so loops that never schedule sub-millisecond
        // timers never pay for it.
        let mut thread = self.thread.lock().unwrap_or_else(PoisonError::into_inner);
        if thread.is_none() {
            let shared = Arc::clone(&self.shared);
            #[expect(
                clippy::expect_used,
                reason = "OS thread spawn only fails under resource exhaustion; the hi-res timer thread is required for sub-millisecond KJ timer precision and there is no recovery at this site (fail-fast). Graceful degradation to tokio-wheel-only timing is tracked as a design debt."
            )]
            let thread_handle = std::thread::Builder::new()
                .name("kj-rs-hires-timer".into())
                .spawn(move || hires_timer_main(&shared))
                .expect("failed to spawn kj-rs hires timer thread");
            *thread = Some(thread_handle);
        }
    }

    /// Cancels any armed request. An in-flight precise sleep cannot be interrupted, but the
    /// generation bump turns its delivery into a no-op; worst case it delays the *next* arm's
    /// wakeup by up to the threshold, which the tokio backstop bounds.
    fn disarm(&self) {
        let mut req = self
            .shared
            .request
            .lock()
            .unwrap_or_else(PoisonError::into_inner);
        req.deadline = None;
        req.generation += 1;
    }
}
#[cfg(unix)]
impl Drop for HiResTimer {
    fn drop(&mut self) {
        {
            let mut req = self
                .shared
                .request
                .lock()
                .unwrap_or_else(PoisonError::into_inner);
            req.shutdown = true;
        }
        self.shared.condvar.notify_one();
        let handle = self
            .thread
            .lock()
            .unwrap_or_else(PoisonError::into_inner)
            .take();
        if let Some(handle) = handle {
            let _ = handle.join();
        }
    }
}
#[cfg(unix)]
fn hires_timer_main(shared: &HiResShared) {
    ffi::boost_current_thread_priority();
    let mut req = shared
        .request
        .lock()
        .unwrap_or_else(PoisonError::into_inner);
    loop {
        if req.shutdown {
            return;
        }
        if let Some(deadline) = req.deadline.take() {
            let generation = req.generation;
            drop(req);
            ffi::sleep_until(deadline);
            req = shared
                .request
                .lock()
                .unwrap_or_else(PoisonError::into_inner);
            if req.shutdown {
                return;
            }
            if req.generation == generation {
                // Still the request we were armed with: unblock the loop thread. `Notify`
                // stores a permit if the loop has not reached `notified()` yet, so this wakeup
                // cannot be lost.
                shared.port.notify.notify_one();
            }
        } else {
            // Nothing armed: park until the next arm() or shutdown. Zero CPU while idle.
            req = shared
                .condvar
                .wait(req)
                .unwrap_or_else(PoisonError::into_inner);
        }
    }
}
/// The Rust backing of one `kj_rs_tokio::TokioEventPort` (C++). Owns the per-thread
/// `current_thread` tokio runtime.
///
/// One instance per KJ event loop, created on (and driven by) that loop's thread. Only `wake()`
/// may be called from other threads.
pub struct TokioPort {
    runtime: Runtime,
    state: Arc<SharedState>,
    /// High-resolution wakeup source for sub-millisecond `wait_timeout_ns` sleeps; see
    /// [`HIRES_TIMEOUT_THRESHOLD`]. On non-unix targets short sleeps stay on the tokio wheel.
    #[cfg(unix)]
    hires: HiResTimer,
}

// `wake()` is called through a `&TokioPort` shared with arbitrary threads, so the type must be
// `Sync` (`Runtime`, `Notify` and the atomics all are).
const _: () = {
    const fn assert_send_sync<T: Send + Sync>() {}
    assert_send_sync::<TokioPort>();
};

// Opaque cxx types must cross the bridge boxed. The lint's firing is platform-dependent (it has
// a size threshold and `TokioPort`'s size differs by target), so `#[expect]` would be unfulfilled
// on some targets.
#[expect(clippy::allow_attributes)]
#[allow(clippy::unnecessary_box_returns)]
pub fn new_tokio_port() -> Box<TokioPort> {
    Box::new(TokioPort::new())
}
