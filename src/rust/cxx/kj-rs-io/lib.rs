//! Rust half of kj-rs-io: tokio-backed implementations of KJ's async I/O interfaces.
//!
//! The C++ side (`async-io.h`) implements `kj::AsyncIoStream`, `kj::ConnectionReceiver`,
//! `kj::NetworkAddress`, `kj::Network`, `kj::AsyncIoProvider` and `kj::LowLevelAsyncIoProvider`
//! as thin wrappers over the opaque Rust types in this crate. All async operations are plain
//! `async fn`s bridged to `kj::Promise<T>` by workerd-cxx; dropping the promise drops the Rust
//! future, which releases any tokio readiness interest (cancellation is implicit). Bridged
//! futures are cold until first polled; the C++ adapters start every I/O promise they return
//! eagerly (see async-io.h, "Operation-start policy"), matching KJ's native streams, which issue
//! the syscall inside the call.
//!
//! # Ownership of in-flight operations
//!
//! Every opaque Rust type here is a handle to `Rc`-shared state, and every bridged operation
//! owns a share of it rather than borrowing the handle (see stream.rs, "Operations own their
//! state"). Destroying a C++ wrapper -- `TokioAsyncIoStream`, a receiver, an fd stream, a
//! `FileWatcher` -- while one of its promises is pending is therefore memory-safe: the operation
//! keeps the socket alive until it settles or is cancelled. KJ's rule that no I/O promise may be
//! outstanding when a stream is destroyed still describes correct usage; it is just no longer a
//! memory-safety boundary. The handles are `!Send` like the C++ objects owning them; the native
//! sockets they yield (`ServeIo`, `into_tcp_stream`) are `Send`.
//!
//! # Reactor affinity
//!
//! Every tokio resource this crate creates registers with the runtime current on the creating
//! thread, which on a `kj_rs_tokio::TokioEventPort` thread is the port's runtime (checked at
//! creation, see runtime.rs), and is driven only while that KJ loop parks in the port. A
//! resource moved to another thread -- a `ServeIo` native socket handed to a connection task --
//! keeps that registration: it is independent of the kj stream it came from, not of the loop
//! that created it (see serve.rs, "Where the result may be driven").
//!
//! A Rust-originated stream wrapped as `kj::AsyncIoStream` can be recovered as its native
//! tokio object so Rust servers can drive the connection without crossing the
//! FFI per read: [`unwrap_kj_stream`] from Rust, `kj_rs_io::unwrapTokioStream()` from C++.
//! Foreign streams fail to unwrap with a `kj::Exception`. The native-serve entry points
//! ([`serve_kj_stream`], [`take_kj_socket`]) build on the `serve` module's `ServeIo` / pump
//! machinery; kj-hyper is their consumer. `TokioAddress::from_socket_addrs`,
//! `TokioStream::into_tcp_stream` / `into_unix_stream` and `unwrap_kj_stream` are likewise
//! reserved for native Rust consumers (and the tests); nothing in the C++ adapters calls them.
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
//!                     │       └── TokioConnectionReceiver -- Box<TokioListener> (one socket
//!                     │                                      per resolved address) + filter
//!                     └── TokioAsyncIoStream   -- Box<TokioStream>
//!
//! TokioStream (this crate)             -- RefCell<Option<Inner>>: every I/O op holds a
//!     │                                   shared borrow across its await; take() needs the
//!     │                                   exclusive borrow, so unwrapping with I/O in flight
//!     │                                   is an Err, not aliasing. None = hollow wrapper.
//!     └── Socket::Tcp / Socket::Unix   -- the native tokio socket (+ a lazily dup'd
//!                                         whenWriteDisconnected registration)
//!
//! serve_kj_stream(KjOwn<AsyncIoStream>) -> Result<ServedKjStream, TakeSocketError>
//!     ├── native path: unwrap -> ServeIo::Tcp/Unix, hollow wrapper destroyed
//!     └── pump path:   ServeIo::Duplex (consumer end) + StreamPump (!Send) owning the KjOwn
//!                      and the other duplex end, polled on the KJ thread
//!
//! FileWatcher (C++)                    -- thin kj-interface wrapper
//!     └── Box<TokioFileWatcher>        -- notify watcher + event channel (watcher.rs)
//! ```
//!
//! # Platforms
//!
//! Unix and Windows are the supported targets; anything else fails to compile below rather than
//! carrying dead fallback arms. The Windows arms are built and their tests run by workerd's CI
//! matrix (`.github/workflows/test.yml` runs `//src/...` on a `windows-2025` runner), which is
//! what "kept in step with the unix arm" means wherever a comment says so.

// Unsafe-code boundary. The crate root denies `unsafe_code`; exactly one module, `ffi.rs`,
// re-allows it (`#![allow(unsafe_code)]`) and holds the `#[cxx::bridge]` plus every hand-written
// `unsafe` in the crate: raw-handle -> owned-fd conversions (as `unsafe fn`s, discharged at the
// bridge entry points), raw read-buffer pointers -> `&mut [MaybeUninit<u8>]`, `struct sockaddr`
// byte views, and the raw `getsockopt`/`setsockopt` syscalls. The other modules cannot write
// `unsafe` at all, and no `unsafe fn` is part of the crate's public surface. What this buys is
// precisely that: the code to audit for memory safety is one file, and its `// Safety:`
// comments name the C++ contracts (KJ's buffer-validity and fd-ownership rules) it relies on.
// It does not make those contracts checked -- they remain KJ's documented interface contracts.
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

#[cfg(not(any(unix, windows)))]
compile_error!("kj-rs-io supports Unix and Windows targets only");

pub use net::TokioAddress;
pub use stream::TokioStream;

mod error;
mod ffi;
mod net;
mod runtime;
mod serve;
mod signal;
mod stream;
mod watcher;

/// Opaque binding of `kj::AsyncIoStream` (see [`unwrap_kj_stream`]).
pub use ffi::KjAsyncIoStream;
pub use ffi::unwrap_kj_stream;
pub use serve::PumpedStream;
pub use serve::ServeIo;
pub use serve::ServePath;
pub use serve::ServedKjStream;
pub use serve::StreamPump;
pub use serve::TakeSocketError;
pub use serve::serve_kj_stream;
pub use serve::take_kj_socket;
