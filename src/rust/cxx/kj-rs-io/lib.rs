//! Rust half of kj-rs-io: tokio-backed implementations of KJ's async I/O interfaces.
//!
//! The C++ side (`async-io.h`) implements `kj::AsyncIoStream`, `kj::ConnectionReceiver`,
//! `kj::NetworkAddress`, `kj::Network`, `kj::AsyncIoProvider` and `kj::LowLevelAsyncIoProvider`
//! as thin wrappers over the opaque Rust types in this crate. All async operations are plain
//! `async fn`s bridged to `kj::Promise<T>` by workerd-cxx; dropping the promise drops the Rust
//! future, which releases any tokio readiness interest (cancellation is implicit).
//!
//! Every future returned from this crate must be polled on the thread that owns the
//! `kj_rs_tokio::TokioEventPort` runtime: tokio I/O objects register with that runtime's I/O
//! driver, which is only driven while the KJ event loop sleeps inside the port's
//! `wait()`/`poll()`.
//!
//! A Rust-originated stream wrapped as `kj::AsyncIoStream` can be recovered as its native
//! tokio object so Rust servers can drive the connection without crossing the
//! FFI per read: [`unwrap_kj_stream`] from Rust, `kj_rs_io::unwrapTokioStream()` from C++.
//! Foreign streams fail to unwrap with a `kj::Exception`. The native-serve entry points
//! ([`serve_kj_stream`], [`take_kj_socket`]) build on the [`serve`] module's `ServeIo` / pump
//! machinery.

// Safety & panic enforcement walls. Test code exempted.
//
// `unsafe` is quarantined into a single named FFI island: the crate root denies `unsafe_code`, so
// the serve / net / stream / error / runtime / readiness / signal business logic is
// *compiler-proven* free of hand-written unsafe. The one island that opts back in via
// `#![allow(unsafe_code)]` is `ffi.rs`: it holds the `#[cxx::bridge] mod bridge` (re-exported as
// `crate::ffi::*`) plus all the fd / sockaddr laundering and the one remaining `pub unsafe fn`
// FFI entry point (`unwrap_kj_stream` — borrow-based, C++ keeps the wrapper). The owning entry
// points (`take_kj_socket`, `serve_kj_stream`) are safe fns in `serve.rs`: ownership arrives as
// a `KjOwn`, the pump drives it through compiler-checked shared borrows (shared-receiver shims,
// see unwrap.h), and no raw pointer crosses the public surface.
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

pub use stream::TokioStream;

mod error;
mod ffi;
mod net;
mod readiness;
mod runtime;
pub mod serve;
mod signal;
mod stream;

/// Opaque binding of `kj::AsyncIoStream` (see [`unwrap_kj_stream`]).
pub use ffi::KjAsyncIoStream;
pub use ffi::unwrap_kj_stream;
pub use serve::ServeIo;
pub use serve::ServePath;
pub use serve::ServedKjStream;
pub use serve::StreamPump;
pub use serve::TakeSocketError;
pub use serve::serve_kj_stream;
pub use serve::take_kj_socket;
