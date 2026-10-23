//! Rust half of the tokio-backed KJ event loop foundation.
//!
//! This crate owns a per-thread tokio `current_thread` runtime and exposes the primitives the
//! C++ `kj_rs_tokio::TokioEventPort` (see `tokio-event-port.h` for the full contract) needs to
//! implement `kj::EventPort`: parking the thread in tokio's `block_on` (which drives the
//! whole tokio scheduler while C++ is "blocked"), a bounded non-blocking `poll`, and the
//! cross-thread `wake()` latch that `kj::Executor` and
//! `kj::newPromiseAndCrossThreadFulfiller` depend on, and the `notify_kj_service` entry point KJ
//! reaches (via `setRunnable(true)` / the port's `TimerImpl::SleepHooks`) to hand the thread
//! back whenever a tokio task has queued KJ work.
//!
//! [`Runtime`] is that loop for a Rust `main`: it owns the C++ context and blocks on a future.

// `unsafe` is quarantined into a single named FFI island: `ffi.rs`, the `#[cxx::bridge]` wire, is
// the one module that opts in via `#![allow(unsafe_code)]`, so the entire event-port business
// logic (`TokioPort`, the `wait`/`poll`/`wake` machinery) is *compiler-proven* to contain no
// hand-written unsafe.

pub use ffi::TokioAsyncIoContext;
pub use port::TokioPort;
pub use port::current_handle;
pub use port::spawn;
pub use runtime::Runtime;

mod ffi;
mod port;
mod runtime;
