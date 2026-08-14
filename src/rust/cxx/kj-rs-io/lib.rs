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
//!
//! # Object relationships
//!
//! C++ owns the KJ-facing objects; each holds an opaque Rust object by `rust::Box`:
//!
//! ```text
//! TokioAsyncIoContext (C++)            -- kj::setupAsyncIo() analogue: composes
//!     │                                   kj_rs_tokio::TokioAsyncIoContext (port -> loop,
//!     │                                   runtime, timer; WaitScope) with the providers below;
//!     │                                   teardown is member order (providers, then the base)
//!     ├── kj_rs_tokio::TokioAsyncIoContext -- the loop's tokio runtime (see kj-rs-tokio)
//!     ├── TokioLowLevelAsyncIoProvider  -- wrap*Fd(): owned fd/SOCKET as i64 -> Rust owns it
//!     └── TokioAsyncIoProvider
//!             └── TokioNetwork          -- Rc<PeerFilter> (restrictPeers chain; children
//!                     │                    share ownership, nothing outlives anything by
//!                     │                    convention)
//!                     ├── TokioNetworkAddress  -- Box<TokioAddress> + Rc<PeerFilter> share
//!                     │       └── TokioConnectionReceiver -- Box<TokioListener> + filter
//!                     └── TokioAsyncIoStream   -- Box<TokioStream>
//!
//! TokioStream (this crate)             -- RefCell<Option<Inner>>: every I/O op holds a
//!     │                                   shared borrow across its await; take() needs the
//!     │                                   exclusive borrow, so unwrapping with I/O in flight
//!     │                                   is an Err, not aliasing. None = hollow wrapper.
//!     └── Inner::Tcp / Inner::Unix     -- the native tokio socket
//!
//! serve_kj_stream(KjOwn<AsyncIoStream>) -> ServedKjStream
//!     ├── native path: unwrap -> ServeIo::Tcp/Unix, hollow wrapper destroyed
//!     └── pump path:   ServeIo::Duplex (consumer end) + StreamPump (!Send) owning the KjOwn
//!                      and the other duplex end, polled on the KJ thread
//!
//! FileWatcher::Impl (C++)              -- owns the inotify/kqueue fd, parses events
//!     └── Box<TokioFdWatcher>          -- owns a dup of that fd + one I/O-driver registration
//! ```

// Safety & panic enforcement walls. Test code exempted.
//
// `unsafe` is quarantined into a single named FFI island: the crate root denies `unsafe_code`, so
// the serve / net / stream / error / runtime / readiness / signal business logic is
// *compiler-proven* free of hand-written unsafe. The one island that opts back in via
// `#![allow(unsafe_code)]` is `ffi.rs`: it holds the `#[cxx::bridge] mod bridge` (re-exported as
// `crate::ffi::*`) plus all the fd / sockaddr laundering. The public surface has no `unsafe fn`
// at all: `unwrap_kj_stream` is a safe fn (the in-flight-I/O conflict it used to leave to the
// caller is detected on the Rust side), and the owning entry points (`take_kj_socket`,
// `serve_kj_stream`) are safe fns in `serve.rs`: ownership arrives as a `KjOwn`, the pump drives
// it through compiler-checked shared borrows (shared-receiver shims, see unwrap.h), and no raw
// pointer crosses the public surface.
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
