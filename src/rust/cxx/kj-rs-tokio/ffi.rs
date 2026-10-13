//! The `#[cxx::bridge]` FFI island for kj-rs-tokio.
//!
//! This is the crate's single dedicated FFI-island file (file-top `#![allow(unsafe_code)]`). It
//! holds the `#[cxx::bridge] mod bridge` -- the C++ <-> Rust wire the C++
//! `kj_rs_tokio::TokioEventPort` drives (see `tokio-event-port.h`), and the C++ context the Rust
//! [`Runtime`](crate::Runtime) owns -- and [`EnteredLoop`], the one hand-written `unsafe` in the
//! crate (a lifetime extension on tokio's `EnterGuard`). Everything else -- `lib.rs`, the
//! event-port logic in `port.rs`, `runtime.rs` -- is wholly-safe, compiler-proven unsafe-free: no
//! other module allows `unsafe_code`.
#![allow(
    unsafe_code,
    reason = "the crate's FFI island: the cxx bridge and the one hand-written `unsafe`"
)]

use std::mem::ManuallyDrop;
use std::ops::Deref;
use std::ops::DerefMut;

pub use bridge::TokioAsyncIoContext;
pub use bridge::new_tokio_async_io_context;
use tokio::runtime::EnterGuard;
use tokio::runtime::LocalEventLoop;

use crate::port::TokioPort;
use crate::port::TokioWaker;
use crate::port::new_tokio_port;
use crate::port::new_tokio_waker;
use crate::runtime::BlockOnTask;
use crate::runtime::block_on_task_join;

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

/// A tokio `LocalEventLoop` whose runtime context is entered on the owning thread for the loop's
/// whole life.
///
/// The KJ event loop turns its events *outside* `tick` and `park`, yet code running in those
/// turns -- bridged futures being polled, C++ calling into kj-rs-io -- creates tokio resources
/// (`TcpStream::from_std`, `AsyncFd`, `signal()`, timers) that must be registered with this
/// runtime's drivers, and `kj_rs_tokio::spawn` is `tokio::task::spawn_local`, which finds the
/// `LocalRuntime` the same way. tokio finds the runtime through the thread's current context, so
/// without this the loop thread would have to re-enter the context around every single poll of
/// every I/O future (a TLS swap plus an `Arc` clone each time) and around every synchronous
/// resource creation. Entering once for the thread's lifetime removes all of that:
/// `Handle::current()` on the loop thread is this runtime, always. `tick` and `park` still work
/// underneath (`Handle::enter` sets only the current handle, not the "inside a runtime" flag
/// they check for nesting).
///
/// This is the one self-referential pair in the crate: `EnterGuard<'a>` borrows the loop it came
/// from, but only nominally -- its `'a` is a `PhantomData<&'a Handle>`; the guard holds no
/// pointer into the loop and its `Drop` only restores the thread's previous context. We extend
/// the lifetime to `'static` and guarantee by construction (a manual `Drop`) that the guard is
/// dropped before the loop. Like the loop it wraps, this type is neither `Send` nor `Sync`.
pub struct EnteredLoop {
    // Dropped first, explicitly, in `Drop::drop`; declared first as documentation of that order.
    guard: ManuallyDrop<EnterGuard<'static>>,
    event_loop: ManuallyDrop<LocalEventLoop>,
}

impl EnteredLoop {
    pub fn new(event_loop: LocalEventLoop) -> Self {
        let guard: EnterGuard<'_> = event_loop.enter();
        TOKIO_CONTEXT_SENTINEL.with(|_| {});
        // SAFETY: `EnterGuard<'a>`'s lifetime parameter exists only as `PhantomData<&'a Handle>`
        // (tokio `runtime/handle.rs`); the guard stores no reference or pointer into the loop,
        // and its `Drop` touches only thread-local context state. Extending `'a` therefore cannot
        // create a dangling access. We additionally uphold the *intent* of the borrow -- the
        // guard does not outlive the loop -- by dropping `guard` before `event_loop` in `Drop`.
        let guard: EnterGuard<'static> =
            unsafe { std::mem::transmute::<EnterGuard<'_>, EnterGuard<'static>>(guard) };
        Self {
            guard: ManuallyDrop::new(guard),
            event_loop: ManuallyDrop::new(event_loop),
        }
    }
}

impl Deref for EnteredLoop {
    type Target = LocalEventLoop;
    fn deref(&self) -> &LocalEventLoop {
        &self.event_loop
    }
}

impl DerefMut for EnteredLoop {
    fn deref_mut(&mut self) -> &mut LocalEventLoop {
        &mut self.event_loop
    }
}

impl Drop for EnteredLoop {
    fn drop(&mut self) {
        // SAFETY: `guard` is initialized (constructed in `new`, never taken out) and this is the
        // only place it is dropped -- `Drop::drop` runs exactly once. It must go before
        // `event_loop` (which the compiler drops right after this body), restoring the thread's
        // previous tokio context while the runtime it referred to is still alive.
        if TOKIO_CONTEXT_SENTINEL.try_with(|_| {}).is_ok() {
            // SAFETY: The sentinel proves Tokio's thread-local context is still available.
            unsafe { ManuallyDrop::drop(&mut self.guard) };
        }
        // SAFETY: `event_loop` is initialized in `new`, never taken elsewhere, and this `Drop`
        // implementation runs once. Background shutdown drops every task now, on this thread,
        // without waiting for user-submitted blocking tasks.
        let event_loop = unsafe { ManuallyDrop::take(&mut self.event_loop) };
        event_loop.shutdown_background();
    }
}

#[cxx::bridge(namespace = "kj_rs_tokio")]
// FFI island: the cxx bridge macro generates the `unsafe` extern shims.
// unnecessary_box_returns: returning the opaque types to C++ boxed is the cxx idiom. The lint's
// firing is platform-dependent (it has a size threshold and the types' sizes differ by target),
// so `#[expect]` would be unfulfilled on some targets.
#[expect(
    clippy::allow_attributes,
    reason = "`unnecessary_box_returns` does not fire on every target, where `expect` would be unfulfilled"
)]
#[allow(
    clippy::unnecessary_box_returns,
    reason = "cxx takes the opaque types boxed"
)]
mod bridge {
    // None of these are `Result`: they cannot fail in a way C++ could handle. The panics that CAN
    // occur -- a second port on one thread (`TokioPort::new`), a `tick`/`park` nested inside a
    // tokio runtime, a port driven after `cancel_spawned_tasks` -- are caller-contract
    // violations, and the in-tree cxx fork converts every panic escaping an `extern "Rust"` fn
    // into a `kj::Exception` thrown at the C++ call site.
    extern "Rust" {
        type TokioPort;

        fn new_tokio_port() -> Box<TokioPort>;

        /// Shut the runtime down, dropping every spawned task now, on this thread.
        /// `TokioEventPort`'s destructor calls this before destroying the KJ event loop and
        /// timer it owns, because spawned tasks may own KJ promises. This terminal operation is
        /// a no-op when repeated; the port must not be driven afterward.
        fn cancel_spawned_tasks(&mut self);

        /// Sleep in tokio's driver until it has work, a tokio timer is due, or `wake()` is
        /// called. Runs no tasks: nothing on this thread touches KJ while it sleeps.
        fn park_forever(&mut self);

        /// Like `park_forever`, but the sleep also ends after `timeout_ns` nanoseconds. The C++
        /// side computes the timeout from `kj::TimerImpl::timeoutToNextEvent()`.
        fn park_timeout_ns(&mut self, timeout_ns: u64);

        /// Non-blocking: one driver turn and one batch of tasks. Returns the wake latch (see
        /// `TokioPort::take_wake_latch`).
        fn tick(&mut self) -> bool;

    }

    extern "Rust" {
        /// The port's cross-thread end: `kj::EventPort::wake()` goes through it.
        type TokioWaker;

        fn new_tokio_waker(port: &TokioPort) -> Box<TokioWaker>;

        /// Set the wake latch and end a concurrent (or the next) sleep. Callable from any thread.
        fn wake(&self);
    }

    extern "Rust" {
        /// What [`Runtime::block_on`](crate::Runtime::block_on) has the loop wait for; `blockOn`
        /// waits on `block_on_task_join`, which resolves when the task block_on spawned has ended.
        type BlockOnTask;
        async fn block_on_task_join(task: Box<BlockOnTask>);
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
