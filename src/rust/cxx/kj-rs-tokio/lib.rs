//! Rust half of the tokio-backed KJ event loop foundation.
//!
//! This crate owns a per-thread tokio `LocalEventLoop` and exposes the primitives the C++
//! `kj_rs_tokio::TokioEventPort` (see `tokio-event-port.h` for the full contract) needs to
//! implement `kj::EventPort`: a `wait` that sleeps in tokio's driver (`park`) and then runs one
//! batch of tasks (`tick`), a `poll` that is the batch alone, and the cross-thread `wake()` latch
//! that `kj::Executor` and `kj::newPromiseAndCrossThreadFulfiller` depend on, over tokio's
//! `Unparker`.
//!
//! [`Runtime`] is that loop for a Rust `main`: it owns the C++ context and blocks on a future.

// `unsafe` is quarantined into a single named FFI island: `ffi.rs`, the `#[cxx::bridge]` wire, is
// the one module that opts in via `#![allow(unsafe_code)]`, so the entire event-port business
// logic (`TokioPort`, the `wait`/`poll`/`wake` machinery) is *compiler-proven* to contain no
// hand-written unsafe.

pub use ffi::TokioAsyncIoContext;
pub use port::TokioPort;
pub use port::TokioWaker;
pub use port::current_handle;
pub use port::spawn;
pub use runtime::Runtime;

mod ffi;
mod port;
mod runtime;
