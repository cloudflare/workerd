//! Rust half of the tokio-backed KJ event loop foundation: one tokio `current_thread` runtime per
//! KJ loop thread, in one of two modes (see `tokio-event-port.h` for the full contract).
//!
//! - **KJ-driven** (`setupTokioAsyncIo()` in C++): the KJ loop schedules the thread and
//!   `promise.wait()` works. This crate gives the C++ `TokioEventPort` what it needs to implement
//!   `kj::EventPort`: parking the thread in `Runtime::block_on` (which drives the whole tokio
//!   scheduler while C++ is "blocked"), a bounded non-blocking `poll`, the cross-thread `wake()`
//!   latch that `kj::Executor` and `kj::newPromiseAndCrossThreadFulfiller` depend on, and the
//!   `notify_kj_service` entry point KJ reaches (via `setRunnable(true)` / the port's
//!   `TimerImpl::SleepHooks`) to hand the thread back whenever a tokio task has queued KJ work.
//! - **Tokio-driven** ([`Runtime`]): tokio schedules the thread -- Rust `main` calls
//!   [`Runtime::block_on`] -- and the KJ loop is a participant, run to idle by a driver task
//!   (runtime.rs). Nothing may block the loop thread; the same `notify_kj_service` signal wakes
//!   the parked driver instead of ending a `wait()`.
//!
//! In both modes [`spawn`] puts a `!Send` future on the loop thread's `LocalSet` and
//! [`current_handle`] names the runtime.

// Safety & panic enforcement walls. Test code exempted.
//
// `unsafe` is quarantined into a single named FFI island: the crate root denies `unsafe_code`, so
// the entire event-port business logic (`TokioPort`, the `wait`/`poll`/`wake` machinery) is
// *compiler-proven* to contain no hand-written unsafe. The one island that opts back in
// via `#![allow(unsafe_code)]` is `ffi.rs`: the `#[cxx::bridge]` wire.
#![deny(unsafe_op_in_unsafe_fn)]
#![deny(unsafe_code)]
#![deny(clippy::undocumented_unsafe_blocks)]
#![deny(
    clippy::unwrap_used,
    clippy::expect_used,
    clippy::panic,
    clippy::unreachable,
    clippy::todo,
    clippy::unimplemented
)]
#![cfg_attr(
    test,
    allow(
        clippy::unwrap_used,
        clippy::expect_used,
        clippy::panic,
        clippy::unreachable,
        clippy::todo,
        clippy::unimplemented
    )
)]

pub use ffi::TokioAsyncIoContext;
pub use port::TokioPort;
pub use port::current_handle;
pub use port::spawn;
pub use runtime::Runtime;

mod ffi;
mod port;
mod runtime;
