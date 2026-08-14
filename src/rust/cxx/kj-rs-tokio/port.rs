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

#[cfg(unix)]
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

impl TokioPort {
    /// # Panics
    ///
    /// Panics if the tokio runtime cannot be built.
    #[must_use]
    pub fn new() -> Self {
        #[expect(
            clippy::expect_used,
            reason = "startup-only: building the per-thread current_thread runtime fails only under resource exhaustion, at which point fail-fast at port construction is the correct behavior"
        )]
        let runtime = Builder::new_current_thread()
            .enable_time()
            // The I/O driver dispatches readiness for tokio sockets (used by kj-rs-io's
            // tokio-backed KJ streams). It is only *driven* while this runtime is inside
            // `block_on` (i.e. in `wait_*`/`poll`), which is exactly when the KJ loop sleeps.
            .enable_io()
            .build()
            .expect("failed to build current_thread tokio runtime");
        LOOP_RUNTIME_HANDLE.with(|h| {
            let mut slot = h.borrow_mut();
            assert!(
                slot.is_none(),
                "a kj-rs-tokio runtime already exists on this thread (one KJ event loop per \
                 thread, hence one TokioEventPort per thread)"
            );
            *slot = Some(runtime.handle().clone());
        });
        // The `LocalSet` that `spawn()` enqueues onto and `wait_*`/`poll` drive. Owned by the
        // thread-local (not by `TokioPort`, which must stay `Send + Sync`); dropped in `drop()`.
        LOOP_LOCAL_SET.with(|l| {
            *l.borrow_mut() = Some(Rc::new(LocalSet::new()));
        });
        let state = Arc::new(SharedState {
            notify: Notify::new(),
            woken: AtomicBool::new(false),
            sleeping: AtomicBool::new(false),
        });
        Self {
            runtime,
            #[cfg(unix)]
            hires: HiResTimer::new(Arc::clone(&state)),
            state,
        }
    }

    /// Handle to this port's runtime, usable to spawn tasks from any thread.
    #[must_use]
    pub fn handle(&self) -> Handle {
        self.runtime.handle().clone()
    }

    pub(crate) fn wait_forever(&self) -> bool {
        self.wait_impl(None)
    }

    pub(crate) fn wait_timeout_ns(&self, timeout_ns: u64) -> bool {
        self.wait_impl(Some(Duration::from_nanos(timeout_ns)))
    }

    fn wait_impl(&self, timeout: Option<Duration>) -> bool {
        let state = &self.state;
        state.sleeping.store(true, Ordering::SeqCst);

        // Sub-millisecond deadlines cannot be met by tokio's ~1 ms timer wheel: hand them to
        // the high-resolution timer thread (see HiResTimer).
        #[cfg(unix)]
        let hires_armed = match timeout {
            Some(t) if !t.is_zero() && t < HIRES_TIMEOUT_THRESHOLD => {
                self.hires.arm(Instant::now() + t);
                true
            }
            _ => false,
        };

        // This `block_on` is where tokio owns the thread: it drives *all* tasks — those spawned
        // onto the port's `LocalSet` via `spawn()` (driven by `LocalSet::block_on`'s `run_until`)
        // as well as any `tokio::spawn`ed tasks on the current_thread runtime — not just the
        // future passed to it. Wake-up sources: `wake()` from another thread (with the `woken`
        // latch set), `notify_runnable()` (a task inside this very `block_on` re-entered C++ and
        // armed a KJ event), and the next KJ timer deadline — via the tokio wheel for long
        // sleeps, via the high-res timer thread (same `notify`) for sub-millisecond ones.
        // `Notify` stores a permit if `notify_one()` arrives before `notified()` is polled, so
        // there is no lost-wakeup window; spurious early returns are explicitly allowed by the
        // `kj::EventPort::wait()` contract.
        #[expect(
            clippy::expect_used,
            reason = "TokioPort::new registers this thread's LocalSet; wait() only runs while this port drives its own thread, so the LocalSet is always present — absence is an unreachable internal invariant"
        )]
        let local =
            current_local_set().expect("TokioPort is driving without a registered LocalSet");
        local.block_on(&self.runtime, async {
            match timeout {
                Some(t) => {
                    let _ = tokio::time::timeout(t, state.notify.notified()).await;
                }
                None => state.notify.notified().await,
            }
        });

        #[cfg(unix)]
        if hires_armed {
            self.hires.disarm();
        }

        state.sleeping.store(false, Ordering::SeqCst);
        self.take_wake_latch()
    }

    pub(crate) fn poll(&self) -> bool {
        // Bounded, non-blocking pump: the main future is always immediately ready to run again
        // after `yield_now`, so the scheduler never parks; it just interleaves any ready spawned
        // tasks (LocalSet + runtime) with our yields until the budget is spent.
        #[expect(
            clippy::expect_used,
            reason = "TokioPort::new registers this thread's LocalSet; poll() only runs while this port drives its own thread, so the LocalSet is always present — absence is an unreachable internal invariant"
        )]
        let local =
            current_local_set().expect("TokioPort is driving without a registered LocalSet");
        local.block_on(&self.runtime, async {
            for _ in 0..POLL_YIELD_BUDGET {
                tokio::task::yield_now().await;
            }
        });
        self.take_wake_latch()
    }

    pub(crate) fn wake(&self) {
        self.state.woken.store(true, Ordering::SeqCst);
        self.state.notify.notify_one();
    }

    pub(crate) fn notify_runnable(&self) {
        // Only meaningful while parked in `wait_impl`; `setRunnable(true)` is only ever called on
        // the loop thread, so if `sleeping` is set we are inside `block_on` and the caller is a
        // tokio task that re-entered C++ and armed a KJ event. Without this nudge the loop would
        // keep sleeping until the next timer/wake even though it has work queued.
        if self.state.sleeping.load(Ordering::SeqCst) {
            self.state.notify.notify_one();
        }
    }

    /// Consumes the wake latch: returns `true` iff `wake()` was called since the last `true`
    /// return from `wait_*`/`poll`.
    fn take_wake_latch(&self) -> bool {
        self.state.woken.swap(false, Ordering::SeqCst)
    }
}

impl Default for TokioPort {
    fn default() -> Self {
        Self::new()
    }
}

impl Drop for TokioPort {
    fn drop(&mut self) {
        // `try_with`, not `with`: a port owned by an object destroyed during thread/process
        // teardown (e.g. under KJ_CLEAN_SHUTDOWN) can be dropped after this thread's Rust TLS
        // destructors have already run, where `with` panics with an AccessError. If the TLS is
        // gone, its values (including the LocalSet) were already dropped with it, so there is
        // nothing left to clear.
        let _ = LOOP_RUNTIME_HANDLE.try_with(|h| {
            *h.borrow_mut() = None;
        });
        // Drop the LocalSet, canceling all still-pending spawned tasks. This runs on the loop
        // thread (the only thread that may drop a `!Send` LocalSet), after
        // `kj::WaitScope`/`kj::EventLoop` destruction (see `TokioAsyncIoContext` member order),
        // so canceled tasks must not touch KJ objects from their `Drop` impls.
        let _ = LOOP_LOCAL_SET.try_with(|l| {
            *l.borrow_mut() = None;
        });
        // Dropping `self.runtime` cancels any tasks spawned via `tokio::spawn` (as opposed to the
        // LocalSet). Same threading/ordering constraints as above.
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn wake_latch_semantics() {
        let port = TokioPort::new();
        // No wake: a timed-out wait reports false.
        assert!(!port.wait_timeout_ns(1_000_000));
        // Wake before wait: latch is reported exactly once.
        port.wake();
        assert!(port.wait_timeout_ns(1_000_000));
        assert!(!port.wait_timeout_ns(1_000_000));
        // Wake is also consumed by poll().
        port.wake();
        assert!(port.poll());
        assert!(!port.poll());
    }

    #[test]
    fn wake_from_other_thread_unblocks_wait_forever() {
        let port = Arc::new(TokioPort::new());
        let port2 = Arc::clone(&port);
        let thread = std::thread::spawn(move || {
            std::thread::sleep(Duration::from_millis(10));
            port2.wake();
        });
        assert!(port.wait_forever());
        thread.join().unwrap();
    }

    #[test]
    fn spawned_tasks_run_during_wait() {
        let port = TokioPort::new();
        let (tx, mut rx) = tokio::sync::oneshot::channel::<u32>();
        let mut jh = spawn(async move {
            tokio::time::sleep(Duration::from_millis(5)).await;
            tx.send(42).unwrap();
        });
        // The task only runs inside wait_impl's block_on.
        let mut done = false;
        for _ in 0..100 {
            let _ = port.wait_timeout_ns(20_000_000);
            if let Ok(v) = rx.try_recv() {
                assert_eq!(v, 42);
                done = true;
                break;
            }
        }
        assert!(done);
        // The JoinHandle should complete promptly now.
        port.runtime.block_on(&mut jh).unwrap();
    }

    #[test]
    fn poll_never_sleeps() {
        let port = TokioPort::new();
        // A pending spawned task must not make poll() block.
        let _jh = spawn(std::future::pending::<()>());
        let start = std::time::Instant::now();
        assert!(!port.poll());
        assert!(start.elapsed() < Duration::from_millis(100));
    }

    /// `spawn` accepts `!Send` futures because it is backed by `LocalSet::spawn_local`. This
    /// future holds an `Rc` — which is `!Send` — across an await point, exercising that path,
    /// and proves such a task actually runs to completion on the loop thread.
    #[test]
    fn spawn_accepts_non_send_futures() {
        use std::cell::Cell;
        let port = TokioPort::new();
        let counter = Rc::new(Cell::new(0u32));
        let task_counter = Rc::clone(&counter);
        // Detached on purpose; the `Rc` capture makes the future `!Send`.
        let _jh = spawn(async move {
            for _ in 0..3 {
                tokio::task::yield_now().await;
            }
            task_counter.set(task_counter.get() + 1);
        });
        let mut done = false;
        for _ in 0..100 {
            let _ = port.wait_timeout_ns(1_000_000);
            if counter.get() == 1 {
                done = true;
                break;
            }
        }
        assert!(done, "non-Send spawned task did not run to completion");
    }

    /// The high-res short-sleep path: a 100 µs timeout must not be quantized to tokio's ~1 ms
    /// timer wheel.
    ///
    /// The wheel failure mode is a FLOOR — under quantization every sample takes >= ~1 ms —
    /// while CI load only inflates some samples (loaded macOS CI VMs push the MEDIAN past
    /// 500 µs). So assert on the minimum of 31 samples: load can't push all of them up, the
    /// wheel floor pushes every one of them past the bound.
    #[cfg(unix)]
    #[test]
    fn short_timeouts_are_sub_millisecond() {
        let port = TokioPort::new();
        // Warm-up: lazily spawns the hires timer thread and faults in the block_on paths.
        let _ = port.wait_timeout_ns(100_000);

        let mut samples: Vec<Duration> = (0..31)
            .map(|_| {
                let start = std::time::Instant::now();
                let _ = port.wait_timeout_ns(100_000);
                start.elapsed()
            })
            .collect();
        samples.sort();
        let fastest = samples[0];
        assert!(
            fastest >= Duration::from_micros(100),
            "woke before the deadline: fastest {fastest:?}"
        );
        assert!(
            fastest < Duration::from_micros(500),
            "100us timeout quantized: fastest {fastest:?}"
        );
    }

    /// Long sleeps stay on the plain tokio path and remain accurate (and, by construction,
    /// never touch the hires thread — see `HIRES_TIMEOUT_THRESHOLD`).
    #[test]
    fn long_timeouts_still_accurate() {
        let port = TokioPort::new();
        let start = std::time::Instant::now();
        let _ = port.wait_timeout_ns(20_000_000);
        let elapsed = start.elapsed();
        assert!(
            elapsed >= Duration::from_millis(19),
            "woke early: {elapsed:?}"
        );
        assert!(
            elapsed < Duration::from_millis(500),
            "woke far too late: {elapsed:?}"
        );
    }

    /// `wake()` must still interrupt a wait that has the high-res timer armed, and the stale
    /// hires wakeup for the canceled deadline must not corrupt the latch of later waits.
    #[cfg(unix)]
    #[test]
    fn wake_interrupts_short_timeout_wait() {
        let port = Arc::new(TokioPort::new());
        // Arm the hires machinery once so the thread exists.
        let _ = port.wait_timeout_ns(100_000);

        let port2 = Arc::clone(&port);
        let thread = std::thread::spawn(move || {
            std::thread::sleep(Duration::from_millis(2));
            port2.wake();
        });
        // A chain of short waits; one of them must observe the wake latch.
        let mut woken = false;
        for _ in 0..1000 {
            if port.wait_timeout_ns(500_000) {
                woken = true;
                break;
            }
        }
        assert!(woken);
        thread.join().unwrap();
        // The latch was consumed; subsequent short waits time out normally with latch false.
        assert!(!port.wait_timeout_ns(100_000));
    }
}
