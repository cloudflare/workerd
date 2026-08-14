//! Per-thread tokio `current_thread` runtime management and the Rust half of `TokioEventPort`.

use std::cell::RefCell;
use std::future::Future;
use std::rc::Rc;
use std::sync::Arc;
use std::sync::atomic::AtomicBool;
use std::sync::atomic::Ordering;
use std::time::Duration;

use tokio::runtime::Builder;
use tokio::runtime::Handle;
use tokio::runtime::Runtime;
use tokio::sync::Notify;
use tokio::task::JoinHandle;
use tokio::task::LocalSet;

/// How many scheduler turns `poll()` grants the runtime. Each `yield_now` re-queues the main
/// future at the back of the run queue, so every already-ready spawned task gets a chance to run
/// (repeatedly, up to the budget) without ever parking the thread.
///
/// The value is a latency/throughput compromise, not derived from any tokio internal: large
/// enough to drain a typical burst of already-ready tasks in one `poll()` call, small enough to
/// bound how long `poll()` withholds control from the KJ loop when spawned tasks keep re-readying
/// each other. Safe to retune if profiling shows either starvation or excessive poll latency.
const POLL_YIELD_BUDGET: u32 = 16;

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
    /// `wake()`); ownership lives here. It is dropped -- cancelling every still-pending spawned
    /// task -- by [`TokioPort::cancel_spawned_tasks`], which the C++ `TokioEventPort` destructor
    /// calls while the KJ event loop and timer are still alive (spawned tasks may own KJ
    /// promises), with `TokioPort::drop` as the fallback.
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
/// A spawned task may use KJ freely: complete bridged futures, fulfill `kj::PromiseFulfiller`s,
/// arm KJ timers, create and await bridged promises. Every one of those either arms a KJ event,
/// which KJ reports to the port through `EventPort::setRunnable(true)`, or moves the next KJ
/// timer deadline, which KJ reports through the `TimerImpl::SleepHooks` the port installs while
/// sleeping; either way the port ends the park. The one thing a task must not do is re-enter
/// `promise.wait()` / `waitScope.poll()` on this thread: that nests `block_on` inside
/// `block_on`, which tokio rejects (the panic surfaces as a `kj::Exception`).
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
    /// Unblocks the `block_on(...)` inside `wait_*` when `wake()` fires, or when KJ reports (via
    /// `notify_kj_service`) that it has work.
    notify: Notify,

    /// The `kj::EventPort::wake()` latch: set by `wake()`, consumed (swapped to `false`) by the
    /// return value of `wait_*`/`poll`. The KJ event loop uses a `true` return to know it must
    /// drain cross-thread events (`kj::Executor`, `CrossThreadPromiseFulfiller`).
    woken: AtomicBool,

    /// True while the loop thread is inside `wait_*`'s `block_on`. `notify_kj_service` only acts
    /// then: KJ also reports runnable transitions while it is turning events itself, and a permit
    /// stored then would only make the next wait return spuriously once. Only mutated from the
    /// loop thread; atomic so the struct stays `Sync`.
    in_wait: AtomicBool,
}

/// The Rust backing of one `kj_rs_tokio::TokioEventPort` (C++). Owns the per-thread
/// `current_thread` tokio runtime.
///
/// One instance per KJ event loop, created on (and driven by) that loop's thread. Only `wake()`
/// may be called from other threads.
pub struct TokioPort {
    runtime: Runtime,
    state: Arc<SharedState>,
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
        let state = Arc::new(SharedState {
            notify: Notify::new(),
            woken: AtomicBool::new(false),
            in_wait: AtomicBool::new(false),
        });
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
        Self { runtime, state }
    }

    /// Handle to this port's runtime, usable to spawn tasks from any thread.
    #[must_use]
    pub fn handle(&self) -> Handle {
        self.runtime.handle().clone()
    }

    /// Cancels every task spawned onto this thread's `LocalSet` via [`spawn`], by dropping the
    /// `LocalSet` (their `Drop` impls run synchronously, here, on the loop thread).
    ///
    /// Spawned tasks routinely own KJ objects -- a bridged `PromiseFuture` holds an
    /// `OwnPromiseNode` and an armed `RustPromiseAwaiter` event; a KJ timer promise holds a
    /// registration in the port's `kj::TimerImpl` -- and dropping those requires the KJ event
    /// loop and timer to still exist. The C++ `TokioEventPort` owns both and calls this FIRST in
    /// its destructor, before any member is destroyed, which makes the teardown order a
    /// structural guarantee rather than a rule spawned tasks have to follow. Idempotent; a later
    /// call (including the one in `Drop`) finds nothing to do. Only ever affects the calling
    /// thread's `LocalSet`: the slot is thread-local, so a call from any other thread is a no-op.
    ///
    /// Tasks spawned with plain `tokio::spawn` onto the runtime are unaffected (they are
    /// cancelled when the runtime drops); being `Send`, they cannot hold KJ objects
    /// (`OwnPromiseNode` and friends are `!Send`), so their order relative to the loop does
    /// not matter.
    pub fn cancel_spawned_tasks(&self) {
        // Take the `Rc` out of the slot and drop it AFTER the borrow ends, so a cancelled
        // task's `Drop` that itself borrows the slot does not hit a re-entrant-borrow panic.
        // (A task `Drop` that tries to *spawn* during cancellation is unsupported: the slot is
        // already `None`, so `spawn()` panics per its documented contract. Rescheduling from a
        // destructor during teardown is not a sane pattern.)
        let local_set = LOOP_LOCAL_SET
            .try_with(|l| l.borrow_mut().take())
            .ok()
            .flatten();
        drop(local_set);
    }

    pub(crate) fn wait_forever(&self) -> bool {
        self.wait_impl(None)
    }

    pub(crate) fn wait_timeout_ns(&self, timeout_ns: u64) -> bool {
        self.wait_impl(Some(Duration::from_nanos(timeout_ns)))
    }

    fn wait_impl(&self, timeout: Option<Duration>) -> bool {
        let state = &self.state;

        // This `block_on` is where tokio owns the thread: it drives *all* tasks — those spawned
        // onto the port's `LocalSet` via `spawn()` (driven by `LocalSet::block_on`'s `run_until`)
        // as well as any `tokio::spawn`ed tasks on the current_thread runtime — not just the
        // future passed to it. Wake-up sources: `wake()` from another thread (with the `woken`
        // latch set), KJ itself via `notify_kj_service` (a task in this very `block_on` armed a
        // KJ event -- `EventPort::setRunnable(true)` -- or a sooner KJ timer -- the port's
        // `TimerImpl::SleepHooks`), and the next KJ timer deadline via the tokio timer wheel.
        // `Notify` stores a permit if `notify_one()` arrives before `notified()` is polled, so
        // there is no lost-wakeup window; spurious early returns are explicitly allowed by the
        // `kj::EventPort::wait()` contract.
        #[expect(
            clippy::expect_used,
            reason = "TokioPort::new registers this thread's LocalSet; wait() only runs while this port drives its own thread, so the LocalSet is always present — absence is an unreachable internal invariant"
        )]
        let local =
            current_local_set().expect("TokioPort is driving without a registered LocalSet");
        state.in_wait.store(true, Ordering::Relaxed);
        local.block_on(&self.runtime, async {
            match timeout {
                Some(t) => {
                    let _ = tokio::time::timeout(t, state.notify.notified()).await;
                }
                None => state.notify.notified().await,
            }
        });
        state.in_wait.store(false, Ordering::Relaxed);

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

    /// See the bridge doc (ffi.rs): KJ has told the C++ port it has work (`setRunnable(true)`,
    /// or a sooner timer through the port's `TimerImpl::SleepHooks`). Wake the `notified()`
    /// future if we are parked in `wait_*`; a no-op otherwise.
    pub(crate) fn notify_kj_service(&self) {
        if self.state.in_wait.load(Ordering::Relaxed) {
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
        // Fallback cancellation of spawned tasks (normally already done, while the KJ loop was
        // still alive, by the C++ `TokioEventPort` destructor -- see `cancel_spawned_tasks`). A
        // bare `TokioPort` (Rust unit tests) reaches here with its tasks still pending.
        self.cancel_spawned_tasks();
        // Dropping `self.runtime` cancels any tasks spawned via `tokio::spawn` (as opposed to the
        // LocalSet); those are `Send` and so cannot hold KJ objects.
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bare_port_drop_cancels_spawned_tasks() {
        // A TokioPort NOT owned by a TokioEventPort: dropping it must still cancel tasks
        // spawned onto its LocalSet (the Drop fallback), and leave the thread clean so a fresh
        // port can be created afterward.
        {
            let port = TokioPort::new();
            drop(spawn(std::future::pending::<()>()));
            assert!(current_handle().is_some());
            drop(port);
        }
        // TLS is cleared: a new port on this thread succeeds (would panic-on-double-register
        // otherwise).
        assert!(current_handle().is_none());
        let port2 = TokioPort::new();
        assert!(current_handle().is_some());
        drop(port2);
    }

    #[test]
    fn concurrent_wake_storm_from_many_threads_terminates() {
        // TSAN stressor for SharedState (notify + woken latch): four threads hammer wake()
        // concurrently while the loop thread services wait_timeout_ns. The assertion is
        // deliberately interleaving-agnostic -- it only requires termination and that at least
        // one wake was observed -- because exact latch counts are inherently racy.
        use std::sync::atomic::AtomicUsize;
        let port = Arc::new(TokioPort::new());
        let done = Arc::new(AtomicUsize::new(0));
        let mut threads = Vec::new();
        for _ in 0..4 {
            let p = Arc::clone(&port);
            let d = Arc::clone(&done);
            threads.push(std::thread::spawn(move || {
                for _ in 0..250 {
                    p.wake();
                }
                d.fetch_add(1, Ordering::SeqCst);
            }));
        }
        let mut latched = 0u64;
        loop {
            if port.wait_timeout_ns(1_000_000) {
                latched += 1;
            }
            // Once every waker has finished, one more wait must eventually drain to `false`.
            if done.load(Ordering::SeqCst) == 4 && !port.wait_timeout_ns(1_000_000) {
                break;
            }
        }
        for t in threads {
            t.join().unwrap();
        }
        assert!(latched >= 1);
    }

    #[test]
    fn handle_spawns_a_runtime_task_from_another_thread() {
        // `handle()` + `tokio::spawn` (the runtime-driven path, distinct from spawn()/LocalSet)
        // from a foreign thread: the Send task runs when the loop next drives block_on.
        let port = Arc::new(TokioPort::new());
        let flag = Arc::new(AtomicBool::new(false));
        let handle = port.handle();
        let f2 = Arc::clone(&flag);
        std::thread::spawn(move || {
            handle.spawn(async move {
                f2.store(true, Ordering::SeqCst);
            });
        })
        .join()
        .unwrap();
        let mut ran = false;
        for _ in 0..1000 {
            let _ = port.wait_timeout_ns(1_000_000);
            if flag.load(Ordering::SeqCst) {
                ran = true;
                break;
            }
        }
        assert!(ran);
    }

    #[test]
    fn poll_advances_a_ready_local_task() {
        // Complements poll_never_sleeps (pending task): a ready LocalSet task (no await) is
        // actually driven to completion by poll() within its budget, and poll() never latches.
        let port = TokioPort::new();
        let flag = Arc::new(AtomicBool::new(false));
        let f2 = Arc::clone(&flag);
        let _jh = spawn(async move {
            f2.store(true, Ordering::SeqCst);
        });
        let mut ran = false;
        for _ in 0..10 {
            assert!(!port.poll(), "poll() must not report a wake latch here");
            if flag.load(Ordering::SeqCst) {
                ran = true;
                break;
            }
        }
        assert!(ran);
    }

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

    /// Timed waits stay on tokio's timer wheel and remain accurate to its ~1 ms granularity --
    /// the same granularity KJ's own epoll-based port has (`epoll_pwait` takes a millisecond
    /// timeout).
    #[test]
    fn timeouts_are_accurate_to_the_wheel() {
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
}
