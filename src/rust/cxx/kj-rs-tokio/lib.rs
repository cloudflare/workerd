//! Rust half of the tokio-backed KJ event loop foundation.
//!
//! This crate owns a per-thread tokio `current_thread` runtime and exposes the primitives the
//! C++ `kj_rs_tokio::TokioEventPort` (see `tokio-event-port.h` for the full contract) needs to
//! implement `kj::EventPort`: parking the thread in `Runtime::block_on` (which drives the
//! whole tokio scheduler while C++ is "blocked"), a bounded non-blocking `poll`, and the
//! cross-thread `wake()` latch that `kj::Executor` and
//! `kj::newPromiseAndCrossThreadFulfiller` depend on, and the `notify_kj_service` entry point KJ
//! reaches (via `setRunnable(true)` / the port's `TimerImpl::SleepHooks`) to hand the thread
//! back whenever a tokio task has queued KJ work.

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

pub use port::TokioPort;
pub use port::current_handle;
pub use port::spawn;

mod ffi;
mod port;
