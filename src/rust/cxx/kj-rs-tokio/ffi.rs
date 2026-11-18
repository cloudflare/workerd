//! The `#[cxx::bridge]` FFI island for kj-rs-tokio.
//!
//! This is the crate's single dedicated FFI-island file (file-top `#![allow(unsafe_code)]`). It
//! holds the `#[cxx::bridge] mod bridge` -- the C++ <-> Rust wire the C++
//! `kj_rs_tokio::TokioEventPort` drives (see `tokio-event-port.h`), and the C++ context the Rust
//! [`Runtime`](crate::Runtime) owns -- and [`EnteredRuntime`], the one hand-written `unsafe` in
//! the crate (a lifetime extension on tokio's `EnterGuard`). Everything else -- `lib.rs`, the
//! event-port business logic in `port.rs`, `runtime.rs` -- is wholly-safe, compiler-proven
//! unsafe-free: no other module allows `unsafe_code`.
#![allow(
    unsafe_code,
    reason = "the crate's FFI island: the cxx bridge and the one hand-written `unsafe`"
)]

use std::mem::ManuallyDrop;
use std::ops::Deref;

pub use bridge::LoopUnitContext;
pub use bridge::TokioAsyncIoContext;
pub use bridge::new_loop_unit_context;
pub use bridge::new_tokio_async_io_context;
use kj_rs::KjOwn;
use tokio::runtime::EnterGuard;
use tokio::runtime::Runtime;

use crate::port::TokioPort;
use crate::port::new_tokio_port;
use crate::runtime::BlockOnTask;
use crate::runtime::block_on_task_join;
use crate::unit::UnitFuture;
use crate::unit::UnitTask;
use crate::unit::UnitWake;
use crate::unit::unit_future_run;
use crate::unit::unit_task_start;

struct TokioContextSentinel;

impl Drop for TokioContextSentinel {
    fn drop(&mut self) {}
}

thread_local! {
    // Initialized after Tokio's context TLS. During thread teardown this sentinel is therefore
    // destroyed first, making `try_with()` a reliable guard against touching Tokio's context
    // after its destructor has run.
    static TOKIO_CONTEXT_SENTINEL: TokioContextSentinel = const { TokioContextSentinel };
}

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
    runtime: ManuallyDrop<Runtime>,
}

impl EnteredRuntime {
    pub fn new(runtime: Runtime) -> Self {
        let guard: EnterGuard<'_> = runtime.enter();
        TOKIO_CONTEXT_SENTINEL.with(|_| {});
        // SAFETY: `EnterGuard<'a>`'s lifetime parameter exists only as `PhantomData<&'a Handle>`
        // (tokio `runtime/handle.rs`); the guard stores no reference or pointer into the runtime,
        // and its `Drop` touches only thread-local context state. Extending `'a` therefore cannot
        // create a dangling access. We additionally uphold the *intent* of the borrow -- the
        // guard does not outlive the runtime -- by dropping `guard` before `runtime` in `Drop`.
        let guard: EnterGuard<'static> =
            unsafe { std::mem::transmute::<EnterGuard<'_>, EnterGuard<'static>>(guard) };
        Self {
            guard: ManuallyDrop::new(guard),
            runtime: ManuallyDrop::new(runtime),
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
        if TOKIO_CONTEXT_SENTINEL.try_with(|_| {}).is_ok() {
            // SAFETY: The sentinel proves Tokio's thread-local context is still available.
            unsafe { ManuallyDrop::drop(&mut self.guard) };
        }
        // SAFETY: `runtime` is initialized in `new`, never taken elsewhere, and this `Drop`
        // implementation runs once. Background shutdown cancels async work without waiting
        // indefinitely for user-submitted blocking tasks.
        let runtime = unsafe { ManuallyDrop::take(&mut self.runtime) };
        runtime.shutdown_background();
    }
}

/// A [`LoopUnit`](crate::LoopUnit)'s C++ context -- the KJ event loop and, through its root
/// promise, every object created on it -- as the task that drives the unit holds it.
pub struct UnitContext(pub KjOwn<LoopUnitContext>);

// SAFETY: the context and everything on its loop (KJ objects, `Rc`s, the root future) is
// single-threaded state, which is sound to move between threads as a whole when no two threads
// touch it at once and no thread keeps a way into it after it moved on. Each condition, and
// what holds it:
//
// 1. Only the driving task reaches the context. `UnitContext` is private to this crate and
//    `LoopUnit` hands out no reference to it; the task is polled by one thread at a time, with
//    `&mut`, which is the exclusion. The one reference that leaves is the
//    `Pin<&mut LoopUnitContext>` `make` gets, inside a call on the loop, for the duration of
//    that call.
// 2. Nothing on the loop is reachable from outside the unit except through handles that are
//    thread-safe themselves: the port's `UnitWake` (`Arc`, atomics, a mutex), a `kj::Executor`
//    and cross-thread fulfillers (KJ's own cross-thread interface), kj-rs wakers (`ArcWaker`,
//    atomically counted, which take the same-thread path only when `Executor::isCurrent()`
//    says the caller has this very loop entered), and tokio's resources. `make` is `Send` and
//    the root future's output is `Send`, so no `Rc` or KJ object enters or leaves by value;
//    what the root future creates it creates on the loop, and can only hand to another thread
//    through an API that requires `Send`.
// 3. No thread keeps a piece of the unit between polls. KJ binds a loop to a thread only while
//    a `kj::WaitScope` exists (`EventLoop::enterScope`), and every entry (`start`, `turn`,
//    `turnPinned`, `finish`, the destructor) makes one for the duration of the call; KJ's other
//    thread-locals (the firing event, the exception callback stack, the async-destructor
//    scopes) are scoped to frames inside those calls. This crate's own `LOOP_RUNTIME_HANDLE`
//    is set by `EnteredLoop` around each entry and holds a runtime handle, not a piece of the
//    unit; a unit has no `LocalSet`. Code running on the loop must follow the same rule:
//    thread-local state it sets must be scoped to a stack frame or keyed by the loop
//    (`kj::EventLoopLocal`).
// 4. No stack frame of the unit survives a poll on a thread the unit then leaves. The only
//    frames that outlive a turn are those of a suspended fiber, and a unit with a fiber is
//    pinned (`loop-unit.h`): `turn()` returns `UNIT_PINNED` instead of going idle and never
//    yields, and the driver stays in `turn_pinned()` on the same thread until the fiber is
//    gone. Pinning is the fiber's creator's duty (`LoopUnitContext::setPinCheck`).
// 5. The context is destroyed with its loop entered, on a thread that has no other loop
//    entered: by the poll that completes the unit or by `LoopUnit`'s drop, which leaks it
//    rather than destroy it where it cannot enter the loop.
unsafe impl Send for UnitContext {}

#[cxx::bridge(namespace = "kj_rs_tokio")]
// FFI island: the cxx bridge macro generates the `unsafe` extern shims.
// unnecessary_box_returns: returning the opaque `TokioPort` to C++ boxed is the cxx idiom. The
// lint's firing is platform-dependent (it has a size threshold and `TokioPort`'s size differs by
// target), so `#[expect]` would be unfulfilled on some targets.
#[expect(
    clippy::allow_attributes,
    reason = "`unnecessary_box_returns` does not fire on every target, where `expect` would be unfulfilled"
)]
#[allow(
    clippy::unnecessary_box_returns,
    reason = "cxx takes the opaque `TokioPort` boxed"
)]
mod bridge {
    // None of these are `Result`: they cannot fail in a way C++ could handle. The panics that CAN
    // occur -- a second port on one thread (`TokioPort::new`), a `block_on` nested inside another
    // tokio runtime's (`wait_*`/`poll`), the LocalSet slot being gone -- are
    // caller-contract violations, and the in-tree cxx fork converts every panic escaping an
    // `extern "Rust"` fn into a `kj::Exception` thrown at the C++ call site.
    extern "Rust" {
        type TokioPort;

        fn new_tokio_port() -> Box<TokioPort>;

        /// Cancel every task spawned onto this thread's `LocalSet` (dropping their state now, on
        /// this thread). `TokioEventPort`'s destructor calls this before destroying the KJ event
        /// loop and timer it owns, because spawned tasks may own KJ promises. This terminal
        /// operation is a no-op when repeated; the port must not be driven afterward.
        fn cancel_spawned_tasks(&self);

        /// Block until `wake()` or `notify_kj_service()` is called, running tokio tasks in the
        /// meantime. Returns the wake latch (see `TokioPort::take_wake_latch`).
        fn wait_forever(&self) -> bool;

        /// Like `wait_forever`, but additionally returns after `timeout_ns` nanoseconds. A zero
        /// timeout behaves like `poll()`. The C++ side computes the timeout from
        /// `kj::TimerImpl::timeoutToNextEvent()`.
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

    extern "Rust" {
        /// What [`Runtime::block_on`](crate::Runtime::block_on) has the loop wait for; `blockOn`
        /// waits on `block_on_task_join`, which resolves when the task block_on spawned has ended.
        type BlockOnTask;
        async fn block_on_task_join(task: Box<BlockOnTask>);
    }

    extern "Rust" {
        /// The Rust half of a `UnitEventPort`: the wake latch and the driving task's waker.
        type UnitWake;

        /// Sets the wake latch, wakes the task that drives the unit and ends a concurrent
        /// `block`. Callable from any thread.
        fn wake(self: &UnitWake);

        /// Takes the wake latch.
        fn take(self: &UnitWake) -> bool;

        /// Blocks the calling thread until the wake latch is set or `timeout_ns` nanoseconds
        /// passed (`UNIT_NO_TIMER`: until the latch is set). Leaves the latch as it is.
        fn block(self: &UnitWake, timeout_ns: u64);

        /// What makes a unit's root future, and that future (unit.rs).
        type UnitTask;
        type UnitFuture;
        fn unit_task_start(
            task: Box<UnitTask>,
            context: Pin<&mut LoopUnitContext>,
        ) -> Box<UnitFuture>;
        async fn unit_future_run(future: Box<UnitFuture>);
    }

    unsafe extern "C++" {
        include!("kj-rs-tokio/loop-unit.h");

        /// A unit's KJ event loop with its port and timer (see loop-unit.h). Every method
        /// enters the loop on the calling thread for the duration of the call.
        type LoopUnitContext;

        #[cxx_name = "newLoopUnitContext"]
        fn new_loop_unit_context(wake: Box<UnitWake>) -> KjOwn<LoopUnitContext>;

        /// `Result`: `make` may panic, which the bridge surfaces as a `kj::Exception`.
        fn start(self: Pin<&mut LoopUnitContext>, task: Box<UnitTask>) -> Result<()>;

        /// `Result`: a `kj::Exception` thrown by an event that nothing caught comes out of it.
        fn turn(self: Pin<&mut LoopUnitContext>, max_turns: u32) -> Result<u8>;

        #[cxx_name = "turnPinned"]
        fn turn_pinned(self: Pin<&mut LoopUnitContext>) -> Result<()>;

        #[cxx_name = "nextTimerNs"]
        fn next_timer_ns(self: Pin<&mut LoopUnitContext>) -> u64;

        /// `Result`: the exception the root future failed with.
        fn finish(self: Pin<&mut LoopUnitContext>) -> Result<()>;
    }

    unsafe extern "C++" {
        include!("kj-rs-tokio/tokio-event-port.h");

        /// The C++ context: the port, its `kj::EventLoop` and `kj::Timer`, and the
        /// `kj::WaitScope` (see tokio-event-port.h).
        type TokioAsyncIoContext;

        #[cxx_name = "newTokioAsyncIoContext"]
        fn new_tokio_async_io_context() -> Result<KjOwn<TokioAsyncIoContext>>;

        /// `Result`: the wait runs KJ events, and a `kj::Exception` thrown by one that nothing
        /// caught comes out of it.
        #[cxx_name = "blockOn"]
        fn block_on(self: Pin<&mut TokioAsyncIoContext>, task: Box<BlockOnTask>) -> Result<()>;
    }
}
