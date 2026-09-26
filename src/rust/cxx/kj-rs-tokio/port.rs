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
use tokio::sync::Notify;
use tokio::task::JoinHandle;
use tokio::task::LocalSet;

use crate::ffi::EnteredRuntime;

/// How many bounded yield points `poll()` grants the runtime (and, tokio-driven, how many the
/// driver grants between running the KJ loop to idle and parking; runtime.rs). This gives ready
/// work opportunities to run without promising an ordering or an exact number of task or driver
/// polls, which are Tokio scheduler implementation details.
///
/// The value is a latency/throughput compromise, not derived from any tokio internal: large
/// enough to drain a typical burst of already-ready tasks in one `poll()` call, small enough to
/// bound how long `poll()` withholds control from the KJ loop when spawned tasks keep re-readying
/// each other. Safe to retune if profiling shows either starvation or excessive poll latency.
pub const POLL_YIELD_BUDGET: u32 = 16;

thread_local! {
    /// Handle to the `TokioPort` runtime driving the KJ event loop on this thread, if any.
    /// Registered by `TokioPort::new` and cleared on drop. Used by code (e.g. kj-rs-io) that
    /// needs to *enter* the runtime context so tokio resources can register with its I/O driver
    /// and timers.
    static LOOP_RUNTIME_HANDLE: RefCell<Option<Handle>> = const { RefCell::new(None) };

    /// The `LocalSet` onto which [`spawn`] enqueues tasks, and which `wait_*`/`poll` drive. Held
    /// behind an `Rc` so `spawn` (which has no `&TokioPort`) can reach it while the port keeps
    /// driving it. The `LocalSet` is `!Send`/`!Sync` and lives entirely on the loop thread, so it
    /// cannot be stored in the `Sync` TokioPort shared with cross-thread `wake()` callers;
    /// ownership lives here. It is dropped -- cancelling every still-pending spawned
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
pub fn current_local_set() -> Option<Rc<LocalSet>> {
    LOOP_LOCAL_SET.with(|l| l.borrow().clone())
}

/// Spawns a future onto this thread's KJ-loop tokio runtime.
///
/// In KJ-driven mode the task runs whenever the KJ event loop sleeps (i.e. whenever C++ is
/// blocked in `promise.wait(waitScope)` or pumping via `poll()`), driven by the port's
/// [`LocalSet`]. In tokio-driven mode [`Runtime::block_on`](crate::Runtime::block_on) drives
/// that `LocalSet`.
///
/// The future is spawned with [`LocalSet::spawn_local`], so it is pinned to this (the loop)
/// thread and does **not** need to be `Send`: the per-thread `current_thread` runtime never
/// migrates a task to another thread. This is what lets bridged futures that hold `!Send` KJ
/// handles (`OwnPromiseNode`, `kj::Own`, ...) be spawned directly. Dropping the returned
/// `JoinHandle` detaches the task, matching `tokio::spawn`; port teardown cancels detached tasks.
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

/// Registers this thread as a KJ-loop thread of the runtime `handle` belongs to: installs the
/// handle for [`current_handle`] and the `LocalSet` that [`spawn`] enqueues onto (owned by the
/// thread-local, not by `TokioPort`, which must stay `Send + Sync`; dropped by
/// `TokioPort::cancel_spawned_tasks`).
///
/// # Panics
///
/// Panics if this thread already has one (one KJ event loop per thread, hence one port).
fn register_loop_thread(handle: Handle) {
    LOOP_RUNTIME_HANDLE.with(|h| {
        let mut slot = h.borrow_mut();
        assert!(
            slot.is_none(),
            "a kj-rs-tokio runtime already exists on this thread (one KJ event loop per thread, \
             hence one TokioEventPort per thread)"
        );
        *slot = Some(handle);
    });
    LOOP_LOCAL_SET.with(|l| {
        *l.borrow_mut() = Some(Rc::new(LocalSet::new()));
    });
}

/// State shared with `wake()` callers on other threads, and (tokio-driven) with the
/// [`Runtime`](crate::Runtime) driver that parks on it.
pub struct SharedState {
    /// Unblocks the sleeper -- KJ-driven the `block_on(...)` inside `wait_*`, tokio-driven the
    /// parked driver -- when `wake()` fires, or when KJ reports (via `notify_kj_service`) that
    /// it has work.
    pub(crate) notify: Notify,

    /// The `kj::EventPort::wake()` latch: set by `wake()`, consumed (swapped to `false`) by the
    /// return value of `wait_*`/`poll`. The KJ event loop uses a `true` return to know it must
    /// drain cross-thread events (`kj::Executor`, `CrossThreadPromiseFulfiller`).
    woken: AtomicBool,

    /// True while the loop thread is inside `wait_*`'s `block_on`. KJ-driven, `notify_kj_service`
    /// only acts then: KJ also reports runnable transitions while it is turning events itself,
    /// and a permit stored then would only make the next wait return spuriously once. Only
    /// mutated from the loop thread; atomic so the struct stays `Sync`.
    in_wait: AtomicBool,
}

impl SharedState {
    pub(crate) fn new() -> Arc<Self> {
        Arc::new(Self {
            notify: Notify::new(),
            woken: AtomicBool::new(false),
            in_wait: AtomicBool::new(false),
        })
    }
}

/// The Rust backing of one `kj_rs_tokio::TokioEventPort` (C++): owns the per-thread
/// `current_thread` tokio runtime.
///
/// The loop thread stays inside that runtime's context for the port's whole life (see
/// [`EnteredRuntime`]): tokio resources created anywhere on the loop thread -- inside or outside
/// `block_on` -- register with this runtime's drivers without any per-call `enter()`.
///
/// One instance per KJ event loop, created on (and driven by) that loop's thread. Only `wake()`
/// may be called from other threads. `!Send` by type: the entered context must be left on the
/// thread that entered it, which is also the only thread that may drive or drop the port.
///
/// Tokio-driven (`new_tokio_driven`), the port owns no runtime: the [`Runtime`](crate::Runtime)
/// that built it does, and drives the loop itself, so `wait_*` and `poll` are never reached (the
/// C++ port refuses `wait()` and answers `poll()` from the wake latch alone).
pub struct TokioPort {
    /// `None` when tokio-driven.
    runtime: Option<EnteredRuntime>,
    state: Arc<SharedState>,
    owner_thread: std::thread::ThreadId,
}

// `wake()` is called through a `&TokioPort` shared with arbitrary threads, so the type must be
// `Sync` (`Runtime`, `Notify`, the atomics and the entered guard all are). It is deliberately
// NOT `Send` (the guard is not), matching the one-thread contract above.
const _: () = {
    const fn assert_sync<T: Sync>() {}
    assert_sync::<TokioPort>();
};

// Opaque cxx types must cross the bridge boxed. The lint's firing is platform-dependent (it has
// a size threshold and `TokioPort`'s size differs by target), so `#[expect]` would be unfulfilled
// on some targets.
#[expect(
    clippy::allow_attributes,
    reason = "`unnecessary_box_returns` does not fire on every target, where `expect` would be unfulfilled"
)]
#[allow(
    clippy::unnecessary_box_returns,
    reason = "cxx takes the opaque `TokioPort` boxed"
)]
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
        // Enter the runtime context for the life of the port (see EnteredRuntime): from here on,
        // `Handle::current()` on this thread is this runtime.
        let runtime = EnteredRuntime::new(runtime);
        register_loop_thread(runtime.handle().clone());
        Self {
            runtime: Some(runtime),
            state: SharedState::new(),
            owner_thread: std::thread::current().id(),
        }
    }

    /// The Rust half of a tokio-driven port (see [`Runtime`](crate::Runtime)): `handle` is the
    /// Runtime's, already entered on this thread; `state` is shared with the driver, which parks
    /// on its `notify`.
    ///
    /// # Panics
    ///
    /// Panics if a kj-rs-tokio runtime already exists on this thread (`Runtime::new` checks first).
    pub(crate) fn new_tokio_driven(handle: Handle, state: Arc<SharedState>) -> Self {
        register_loop_thread(handle);
        Self {
            runtime: None,
            state,
            owner_thread: std::thread::current().id(),
        }
    }

    /// Handle to this port's runtime, usable to spawn tasks from any thread. (On the loop thread
    /// itself `tokio::runtime::Handle::current()` is the same runtime; see [`EnteredRuntime`].)
    ///
    /// # Panics
    ///
    /// Panics on a tokio-driven port, which owns no runtime (`Runtime::handle` is the one to use).
    #[must_use]
    pub fn handle(&self) -> Handle {
        #[expect(
            clippy::expect_used,
            reason = "documented `# Panics`: a tokio-driven port has no runtime of its own"
        )]
        self.runtime
            .as_ref()
            .expect("a tokio-driven TokioPort owns no runtime")
            .handle()
            .clone()
    }

    /// Cancels every task spawned onto this thread's `LocalSet` via [`spawn`], by dropping the
    /// `LocalSet` (their `Drop` impls run synchronously, here, on the loop thread).
    ///
    /// Spawned tasks routinely own KJ objects -- a bridged `PromiseFuture` holds an
    /// `OwnPromiseNode` and an armed `RustPromiseAwaiter` event; a KJ timer promise holds a
    /// registration in the port's `kj::TimerImpl` -- and dropping those requires the KJ event
    /// loop and timer to still exist. The C++ `TokioEventPort` owns both and calls this FIRST in
    /// its destructor, before any member is destroyed, which makes the teardown order a
    /// structural guarantee rather than a rule spawned tasks have to follow. This is a terminal
    /// operation: repeated calls find nothing to do, but later spawn, wait, or poll operations are
    /// invalid. A call from another thread is a no-op and never touches that caller's `LocalSet`.
    ///
    /// Tasks spawned with plain `tokio::spawn` onto the runtime are unaffected (they are
    /// cancelled when the runtime drops); being `Send`, they cannot hold KJ objects
    /// (`OwnPromiseNode` and friends are `!Send`), so their order relative to the loop does
    /// not matter.
    pub fn cancel_spawned_tasks(&self) {
        if self.owner_thread != std::thread::current().id() {
            return;
        }
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
        // Tokio-driven, the C++ port never calls this (its wait() throws first); the latch is the
        // only sensible answer if it did.
        let Some(runtime) = &self.runtime else {
            return self.take_wake_latch();
        };
        state.in_wait.store(true, Ordering::Relaxed);
        local.block_on(runtime, async {
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
        // Bounded, non-blocking pump: each yield gives ready LocalSet and runtime work an
        // opportunity to run. Tokio does not guarantee exact scheduling order or driver-poll
        // counts here, so only the number of yield attempts is part of this implementation.
        #[expect(
            clippy::expect_used,
            reason = "TokioPort::new registers this thread's LocalSet; poll() only runs while this port drives its own thread, so the LocalSet is always present — absence is an unreachable internal invariant"
        )]
        let local =
            current_local_set().expect("TokioPort is driving without a registered LocalSet");
        // Tokio-driven, the C++ port answers poll() from `take_wake_latch` directly: the driver is
        // already inside the runtime's block_on, which cannot nest.
        let Some(runtime) = &self.runtime else {
            return self.take_wake_latch();
        };
        local.block_on(runtime, async {
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
    /// or a sooner timer through the port's `TimerImpl::SleepHooks`). KJ-driven, wake the
    /// `notified()` future if we are parked in `wait_*` and do nothing otherwise. Tokio-driven,
    /// always signal: the driver may be parked on `notify` at any moment KJ is not turning, and
    /// `Notify` stores the permit if it is not, so a redundant signal costs the driver one idle
    /// iteration whereas a missed one would hang the loop.
    pub(crate) fn notify_kj_service(&self) {
        if self.runtime.is_none() || self.state.in_wait.load(Ordering::Relaxed) {
            self.state.notify.notify_one();
        }
    }

    /// Consumes the wake latch: returns `true` iff `wake()` was called since the last `true`
    /// return from `wait_*`/`poll`/`take_wake_latch`.
    pub(crate) fn take_wake_latch(&self) -> bool {
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
#[path = "port-test.rs"]
mod tests;
