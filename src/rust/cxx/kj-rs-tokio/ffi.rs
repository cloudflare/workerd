//! The `#[cxx::bridge]` FFI island for kj-rs-tokio.
//!
//! This is the crate's single dedicated FFI-island file (file-top `#![allow(unsafe_code)]`): the
//! `#[cxx::bridge] mod bridge` is the C++ <-> Rust wire the C++ `kj_rs_tokio::TokioEventPort`
//! drives (see `tokio-event-port.h`). Everything else -- `lib.rs` and the entire event-port
//! business logic in `port.rs` -- is wholly-safe, compiler-proven unsafe-free under the crate-root
//! `#![deny(unsafe_code)]`.
#![allow(unsafe_code)]

use crate::port::TokioPort;
use crate::port::new_tokio_port;

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
