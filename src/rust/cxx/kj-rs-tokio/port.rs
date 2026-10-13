//! The Rust half of `TokioEventPort`: a tokio [`LocalEventLoop`] that KJ's `EventPort` drives.

use std::cell::RefCell;
use std::future::Future;
use std::sync::Arc;
use std::sync::atomic::AtomicBool;
use std::sync::atomic::Ordering;
use std::time::Duration;

use tokio::runtime::Builder;
use tokio::runtime::Handle;
use tokio::runtime::LocalOptions;
use tokio::runtime::Unparker;
use tokio::task::JoinHandle;

use crate::ffi::EnteredLoop;

/// How many ready tasks one `tick` (hence one `wait()` or `poll()`) runs before handing the
/// thread back to KJ.
///
/// Tasks left ready make tokio's next `park` return at once, so KJ calls `wait()` again as soon
/// as its own queue is empty: leftover work is not lost, it is interleaved with KJ's events.
/// The value only bounds how long a burst of tokio work withholds the thread from KJ.
const TICK_BUDGET: usize = 64;

thread_local! {
    /// Handle to the runtime of the `TokioPort` on this thread, if any. Registered by
    /// `TokioPort::new` and cleared on drop; kj-rs-io checks it before registering a resource.
    static LOOP_RUNTIME_HANDLE: RefCell<Option<Handle>> = const { RefCell::new(None) };
}

/// Returns a handle to this thread's KJ-loop tokio runtime, if a `TokioEventPort` exists on this
/// thread.
#[must_use]
pub fn current_handle() -> Option<Handle> {
    LOOP_RUNTIME_HANDLE.with(|h| h.borrow().clone())
}

/// Spawns a future onto this thread's KJ-loop tokio runtime.
///
/// Tasks run from the port's `wait()` and `poll()`, in bounded batches between KJ's own events.
/// The future need not be `Send`: the runtime is a `LocalRuntime`, pinned to this thread, so a
/// task may hold `!Send` KJ handles (`OwnPromiseNode`, `kj::Own`, ...). Dropping the returned
/// `JoinHandle` detaches the task, matching `tokio::spawn`; port teardown cancels detached tasks.
///
/// A task may use KJ freely: fulfill a `kj::PromiseFulfiller`, arm a KJ timer, create and await
/// bridged promises. Each of those arms a KJ event, which KJ runs once the batch returns. The one
/// thing a task must not do is re-enter `promise.wait()` / `waitScope.poll()` on this thread:
/// the loop is already inside a `wait()` or `poll()`, and KJ refuses the nested one with a
/// `kj::Exception`.
///
/// # Panics
///
/// Panics if no `TokioEventPort` has been created on this thread, or if its tasks have been
/// cancelled (`TokioPort::cancel_spawned_tasks`).
pub fn spawn<F>(future: F) -> JoinHandle<F::Output>
where
    F: Future + 'static,
    F::Output: 'static,
{
    #[expect(
        clippy::expect_used,
        reason = "documented `# Panics` on this public API: calling spawn() without first creating a TokioEventPort on this thread is a caller-contract violation, not a recoverable runtime path"
    )]
    let handle = current_handle()
        .expect("no kj-rs-tokio runtime on this thread; create a TokioEventPort first");
    // `tokio::task::spawn_local` resolves through the thread's current runtime, which is the
    // port's for its whole life (see `EnteredLoop`) except under another runtime's `block_on`;
    // entering the port's handle pins it either way. tokio checks that the runtime is a
    // `LocalRuntime` of this thread.
    let _entered = handle.enter();
    tokio::task::spawn_local(future)
}

/// What `kj::EventPort::wake()` touches: shared with any thread through [`TokioWaker`].
struct WakeState {
    /// Ends a `park` in progress, or makes the next one return at once.
    unparker: Unparker,
    /// The `kj::EventPort::wake()` latch: set by `wake()`, consumed by the return value of
    /// `wait_*`/`poll`. The KJ event loop uses a `true` return to know it must drain
    /// cross-thread events (`kj::Executor`, `CrossThreadPromiseFulfiller`).
    woken: AtomicBool,
}

/// The cross-thread end of a [`TokioPort`]: `wake()` for `kj::EventPort::wake()`, callable from
/// any thread, while the port itself stays on its loop thread.
pub struct TokioWaker {
    state: Arc<WakeState>,
}

// `wake()` is called through a `&TokioWaker` shared with arbitrary threads.
const _: () = {
    const fn assert_send_sync<T: Send + Sync>() {}
    assert_send_sync::<TokioWaker>();
};

impl TokioWaker {
    pub(crate) fn wake(&self) {
        self.state.woken.store(true, Ordering::SeqCst);
        self.state.unparker.unpark();
    }
}

/// The Rust backing of one `kj_rs_tokio::TokioEventPort` (C++): the per-thread tokio
/// `LocalEventLoop`, driven from the port's `wait()` and `poll()`.
///
/// The loop thread stays inside the runtime's context for the port's whole life (see
/// [`EnteredLoop`]): tokio resources created anywhere on the loop thread -- inside a task or in a
/// KJ event -- register with this runtime's drivers without any per-call `enter()`.
///
/// One instance per KJ event loop, created on and driven by that loop's thread, and neither
/// `Send` nor `Sync` (as a `LocalRuntime` is): the entered context must be left on the thread
/// that entered it, which is also the only thread that may drive or drop the port. Other threads
/// reach it only through a [`TokioWaker`].
pub struct TokioPort {
    /// `None` once `cancel_spawned_tasks` has shut the runtime down.
    event_loop: Option<EnteredLoop>,
    state: Arc<WakeState>,
}

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

#[expect(
    clippy::allow_attributes,
    reason = "`unnecessary_box_returns` does not fire on every target, where `expect` would be unfulfilled"
)]
#[allow(
    clippy::unnecessary_box_returns,
    reason = "cxx takes the opaque `TokioWaker` boxed"
)]
pub fn new_tokio_waker(port: &TokioPort) -> Box<TokioWaker> {
    Box::new(port.waker())
}

impl TokioPort {
    /// # Panics
    ///
    /// Panics if the tokio runtime cannot be built, or if this thread already has a port.
    #[must_use]
    pub fn new() -> Self {
        #[expect(
            clippy::expect_used,
            reason = "startup-only: building the per-thread runtime fails only under resource exhaustion, at which point fail-fast at port construction is the correct behavior"
        )]
        let event_loop = Builder::new_current_thread()
            .enable_time()
            // The I/O driver dispatches readiness for tokio sockets (kj-rs-io's streams). It
            // turns in `park` and `tick`, i.e. from the port's `wait()` and `poll()`.
            .enable_io()
            .build_local_event_loop(LocalOptions::default())
            .expect("failed to build the tokio LocalEventLoop");
        let state = Arc::new(WakeState {
            unparker: event_loop.unparker(),
            woken: AtomicBool::new(false),
        });
        // Enter the runtime context for the life of the port (see EnteredLoop): from here on,
        // `Handle::current()` on this thread is this runtime.
        let event_loop = EnteredLoop::new(event_loop);
        LOOP_RUNTIME_HANDLE.with(|h| {
            let mut slot = h.borrow_mut();
            assert!(
                slot.is_none(),
                "a kj-rs-tokio runtime already exists on this thread (one KJ event loop per \
                 thread, hence one TokioEventPort per thread)"
            );
            *slot = Some(event_loop.handle().clone());
        });
        Self {
            event_loop: Some(event_loop),
            state,
        }
    }

    /// Handle to this port's runtime, usable to spawn `Send` tasks from any thread. (On the loop
    /// thread itself `tokio::runtime::Handle::current()` is the same runtime; see
    /// [`EnteredLoop`].)
    ///
    /// # Panics
    ///
    /// Panics after `cancel_spawned_tasks`.
    #[must_use]
    pub fn handle(&self) -> Handle {
        self.event_loop().handle().clone()
    }

    /// The cross-thread end of this port, for `kj::EventPort::wake()`.
    #[must_use]
    pub fn waker(&self) -> TokioWaker {
        TokioWaker {
            state: Arc::clone(&self.state),
        }
    }

    /// Shuts the runtime down, dropping every task spawned onto it here, on the loop thread.
    ///
    /// Spawned tasks routinely own KJ objects -- a bridged `PromiseFuture` holds an
    /// `OwnPromiseNode` and an armed `RustPromiseAwaiter` event; a KJ timer promise holds a
    /// registration in the port's `kj::TimerImpl` -- and dropping those requires the KJ event
    /// loop and timer to still exist. The C++ `TokioEventPort` owns both and calls this FIRST in
    /// its destructor, before any member is destroyed, which makes the teardown order a
    /// structural guarantee rather than a rule spawned tasks have to follow. This is a terminal
    /// operation: repeated calls find nothing to do, but later spawn, wait, or poll operations
    /// panic.
    pub fn cancel_spawned_tasks(&mut self) {
        // The runtime is left before the tasks are dropped: a task `Drop` that tried to spawn
        // during teardown finds no runtime and panics per `spawn`'s contract, rather than
        // enqueueing onto a runtime that is going away. Rescheduling from a destructor during
        // teardown is not a sane pattern. `try_with`: a port destroyed at process exit (a static
        // owner under KJ_CLEAN_SHUTDOWN) may get here after this thread's Rust TLS destructors
        // have run, and then there is nothing to clear.
        let _ = LOOP_RUNTIME_HANDLE.try_with(|h| {
            *h.borrow_mut() = None;
        });
        drop(self.event_loop.take());
    }

    pub(crate) fn park_forever(&mut self) {
        self.event_loop_mut().park(None);
    }

    pub(crate) fn park_timeout_ns(&mut self, timeout_ns: u64) {
        self.event_loop_mut()
            .park(Some(Duration::from_nanos(timeout_ns)));
    }

    /// One driver turn and one batch of tasks, never sleeping: `kj::EventPort::poll()`, and the
    /// second half of `wait()`. Returns the wake latch.
    pub(crate) fn tick(&mut self) -> bool {
        self.event_loop_mut().tick(TICK_BUDGET);
        self.take_wake_latch()
    }

    /// Consumes the wake latch: returns `true` iff `wake()` was called since the last `true`
    /// return from `wait_*`/`poll`.
    fn take_wake_latch(&self) -> bool {
        self.state.woken.swap(false, Ordering::SeqCst)
    }

    #[expect(
        clippy::expect_used,
        reason = "documented `# Panics`: driving a port after `cancel_spawned_tasks` is a caller-contract violation"
    )]
    fn event_loop(&self) -> &EnteredLoop {
        self.event_loop
            .as_ref()
            .expect("TokioPort used after cancel_spawned_tasks")
    }

    #[expect(
        clippy::expect_used,
        reason = "documented `# Panics`: driving a port after `cancel_spawned_tasks` is a caller-contract violation"
    )]
    fn event_loop_mut(&mut self) -> &mut EnteredLoop {
        self.event_loop
            .as_mut()
            .expect("TokioPort used after cancel_spawned_tasks")
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
        // destructors have already run, where `with` panics with an AccessError.
        let _ = LOOP_RUNTIME_HANDLE.try_with(|h| {
            *h.borrow_mut() = None;
        });
        // Normally already done, while the KJ loop was still alive, by the C++ `TokioEventPort`
        // destructor; a bare `TokioPort` (Rust unit tests) reaches here with its tasks pending.
        drop(self.event_loop.take());
    }
}

#[cfg(test)]
#[path = "port-test.rs"]
mod tests;
