//! The `#[cxx::bridge]` FFI island for kj-rs-tokio.
//!
//! This is the crate's single dedicated FFI-island file (file-top `#![allow(unsafe_code)]`). It
//! holds the `#[cxx::bridge] mod bridge` -- the C++ <-> Rust wire the C++
//! `kj_rs_tokio::TokioEventPort` drives (see `tokio-event-port.h`) -- and [`EnteredRuntime`], the
//! one hand-written `unsafe` in the crate (a lifetime extension on tokio's `EnterGuard`).
//! Everything else -- `lib.rs` and the entire event-port business logic in `port.rs` -- is
//! wholly-safe, compiler-proven unsafe-free under the crate-root `#![deny(unsafe_code)]`.
#![allow(unsafe_code)]

use std::mem::ManuallyDrop;
use std::ops::Deref;

use tokio::runtime::EnterGuard;
use tokio::runtime::Runtime;

use crate::port::TokioPort;
use crate::port::new_tokio_port;

/// A tokio runtime whose context is entered on the owning thread for the runtime's whole life.
///
/// The KJ event loop turns its events *outside* `block_on` (only `wait()`/`poll()` are inside),
/// yet code running in those turns -- bridged futures being polled, C++ calling into kj-rs-io --
/// creates tokio resources (`TcpStream::from_std`, `AsyncFd`, `signal()`, timers) that must be
/// registered with this runtime's drivers. tokio finds the runtime through the thread's current
/// context, so without this the loop thread would have to re-enter the context around every
/// single poll of every I/O future (a TLS swap plus an `Arc` clone each time) and around every
/// synchronous resource creation. Entering once for the thread's lifetime removes all of that:
/// `Handle::current()` on the loop thread is this runtime, always. `block_on` still works
/// underneath (`Handle::enter` sets only the current handle, not the "inside a runtime" flag
/// that `block_on` checks for nesting).
///
/// This is the one self-referential pair in the crate: `EnterGuard<'a>` borrows the `Runtime`
/// it came from, but only nominally -- its `'a` is a `PhantomData<&'a Handle>`; the guard holds
/// no pointer into the runtime and its `Drop` only restores the thread's previous context. We
/// extend the lifetime to `'static` and guarantee by construction (a manual `Drop`) that the
/// guard is dropped before the runtime. Like the guard it wraps, this type is `!Send` (the
/// context must be left on the thread that entered it) and `Sync`.
pub struct EnteredRuntime {
    // Dropped first, explicitly, in `Drop::drop`; declared first as documentation of that order.
    guard: ManuallyDrop<EnterGuard<'static>>,
    runtime: Runtime,
}

impl EnteredRuntime {
    pub fn new(runtime: Runtime) -> Self {
        let guard: EnterGuard<'_> = runtime.enter();
        // SAFETY: `EnterGuard<'a>`'s lifetime parameter exists only as `PhantomData<&'a Handle>`
        // (tokio `runtime/handle.rs`); the guard stores no reference or pointer into the runtime,
        // and its `Drop` touches only thread-local context state. Extending `'a` therefore cannot
        // create a dangling access. We additionally uphold the *intent* of the borrow -- the
        // guard does not outlive the runtime -- by dropping `guard` before `runtime` in `Drop`.
        let guard: EnterGuard<'static> =
            unsafe { std::mem::transmute::<EnterGuard<'_>, EnterGuard<'static>>(guard) };
        Self {
            guard: ManuallyDrop::new(guard),
            runtime,
        }
    }
}

impl Deref for EnteredRuntime {
    type Target = Runtime;
    fn deref(&self) -> &Runtime {
        &self.runtime
    }
}

impl Drop for EnteredRuntime {
    fn drop(&mut self) {
        // SAFETY: `guard` is initialized (constructed in `new`, never taken out) and this is the
        // only place it is dropped -- `Drop::drop` runs exactly once. It must go before `runtime`
        // (which the compiler drops right after this body), restoring the thread's previous
        // tokio context while the runtime it referred to is still alive.
        unsafe { ManuallyDrop::drop(&mut self.guard) };
    }
}

#[cxx::bridge(namespace = "kj_rs_tokio")]
// FFI island: the cxx bridge macro generates the `unsafe` extern shims.
// unnecessary_box_returns: returning the opaque `TokioPort` to C++ boxed is the cxx idiom. The
// lint's firing is platform-dependent (it has a size threshold and `TokioPort`'s size differs by
// target), so `#[expect]` would be unfulfilled on some targets.
#[expect(clippy::allow_attributes)]
#[allow(clippy::unnecessary_box_returns)]
mod bridge {
    // None of these are `Result`: they cannot fail in a way C++ could handle. The panics that CAN
    // occur -- a second port on one thread (`TokioPort::new`), a nested `block_on` from a task
    // that re-entered `promise.wait()` (`wait_*`/`poll`), the LocalSet slot being gone -- are
    // caller-contract violations, and the in-tree cxx fork converts every panic escaping an
    // `extern "Rust"` fn into a `kj::Exception` thrown at the C++ call site.
    extern "Rust" {
        type TokioPort;

        fn new_tokio_port() -> Box<TokioPort>;

        /// Cancel every task spawned onto this thread's `LocalSet` (dropping their state now, on
        /// this thread). `TokioEventPort`'s destructor calls this before destroying the KJ event
        /// loop and timer it owns, because spawned tasks may own KJ promises. Idempotent.
        fn cancel_spawned_tasks(&self);

        /// Block until `wake()` or `notify_kj_service()` is called, running tokio tasks in the
        /// meantime. Returns the wake latch (see `TokioPort::take_wake_latch`).
        fn wait_forever(&self) -> bool;

        /// Like `wait_forever`, but additionally returns after `timeout_ns` nanoseconds. The
        /// C++ side computes the timeout from `kj::TimerImpl::timeoutToNextEvent()`.
        fn wait_timeout_ns(&self, timeout_ns: u64) -> bool;

        /// Non-blocking: let the tokio scheduler run already-ready tasks for a bounded number of
        /// turns. Never sleeps. Returns the wake latch.
        fn poll(&self) -> bool;

        /// Set the wake latch and unblock a concurrent `wait_*`. Callable from any thread.
        fn wake(&self);

        /// Loop thread only: KJ has told the port it needs the thread back -- through
        /// `EventPort::setRunnable(true)` (an event was armed) or through the port's
        /// `TimerImpl::SleepHooks` (a sooner timer was armed while sleeping). Unblocks a
        /// concurrent `wait_*` without setting the wake latch; a no-op outside `wait_*`.
        fn notify_kj_service(&self);
    }
}
