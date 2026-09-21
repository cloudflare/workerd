//! Rust half of kj-rs-io: tokio-backed implementations of KJ's async I/O interfaces.
//!
//! The C++ side (async-io.h) implements `kj::AsyncIoStream`, `kj::ConnectionReceiver`,
//! `kj::NetworkAddress`, `kj::Network`, `kj::AsyncIoProvider` and `kj::LowLevelAsyncIoProvider`
//! as thin adapters over the opaque Rust types in this crate. Every `async fn` in the bridge
//! (ffi.rs) becomes a `kj::Promise`; dropping the promise drops the future, which releases any
//! tokio readiness interest (cancellation is implicit). This file is the canonical home of the
//! crate-wide rules; other files point here rather than restating them.
//!
//! # Ownership of in-flight operations
//!
//! Every opaque Rust type is a handle to `Arc`-shared state, and every bridged operation owns a
//! share of it rather than borrowing the handle: the entry points are `fn -> impl Future +
//! use<..>` capturing no borrow of the object (stream.rs, "Operations own their state"). A C++
//! wrapper destroyed while one of its promises is pending is therefore memory-safe: the socket
//! lives until the operation settles or is cancelled. What this does not cover is a caller's
//! `tryRead` buffer, which a pending read still writes into, exactly as under KJ; KJ's rule that
//! no I/O promise may be outstanding when a stream is destroyed remains the usage contract for
//! buffers.
//!
//! # Threads
//!
//! Every Rust handle is `Send + Sync` by type (`Arc`, atomics, `Mutex`, tokio's own resources),
//! asserted at compile time at the bottom of this file, so a `rust::Box` that C++ carries to
//! another thread is never a memory-safety question. The C++ adapters' one piece of shared state,
//! the `restrictPeers` filter, is a `kj::Arc` (atomic) for the same reason: KJ allows an address
//! to be cloned on another thread. KJ's rule that an I/O object is *used* on the loop that created
//! it remains the usage contract, and it is checked: see the next section.
//!
//! # The tokio runtime
//!
//! This is ordinary tokio code. tokio registers a resource with the runtime *entered* on the
//! calling thread, and kj-rs-tokio makes a `TokioEventPort` thread a tokio runtime thread for its
//! whole life (port.rs, `EnteredRuntime`), so `TcpStream::connect`, `UnixListener::bind_addr`,
//! `AsyncFd::with_interest`, `signal()`, `spawn_blocking` and friends register with the loop's
//! driver as they are. Two checks are added, both cheap (two thread-local reads):
//! [`ensure_loop_thread`] before every *registration* (connect, listen, wrap, resolve, signals,
//! the hangup watch), so a call on a thread without a port, or under another runtime entered
//! over the port's, fails with a `kj::Exception` instead of tokio's "no reactor running" panic
//! (a process abort at the bridge); and [`ensure_owner_loop`] at the point an operation is about
//! to *wait* for readiness -- a read or write whose syscall completes at once never touches the
//! reactor and pays nothing -- so a stream or listener carried to another loop thread fails there
//! instead of waiting on its creator's driver forever. Accept and the hangup watch always wait, so
//! they check up front. The file watcher is runtime-independent (notify delivers from its own
//! thread; `tokio::sync::Notify` stores wakeups without a runtime) and checks nothing.
//!
//! # Scope: workerd's provider
//!
//! This crate implements KJ's I/O interfaces for **workerd**, which is their only consumer. It is not a drop-in for every KJ program, on purpose. The rule for
//! leaving a KJ feature out: no consumer in workerd's production code *or its configuration
//! surface* (`workerd.capnp` documents what `Socket.address` and the like accept -- grepping
//! `src/workerd` for a literal is not enough, and a review caught exactly that for
//! `unix-abstract:`), and hand-written libc / sockaddr / fd code to keep. Left out, each site
//! saying so: kj's unix pipe/character-device tier (`wrapInputFd` / `wrapOutputFd` take sockets,
//! kj's win32 definition; `newOneWayPipe` is a socket pair), `wrapConnectingSocketFd`,
//! `wrapListenSocketFd` with a caller-owned filter, `strtoul`'s octal/hex ports, KJ's resolver
//! re-sort, KJ's parse-time filter check, and content hashing in the file watcher. Kept because
//! workerd uses them: `restrictPeers` (KJ's own policy), `connectAuthenticated` /
//! `acceptAuthenticated` identities, unix peer credentials, `whenWriteDisconnected` (at one
//! extra descriptor per stream; stream.rs), `getSockaddr`, `wrapListenSocketFd` with KJ's fd
//! flags, `onSignal`, `getaddrinfo` with KJ's hints and service names, `unix:` and
//! `unix-abstract:` addresses, `SO_REUSEADDR`, `TCP_NODELAY`, the accept retry set, SIGPIPE.
//! Added for workerd, with no KJ counterpart: `loopback:` addresses (loopback.rs), which
//! `workerd test` uses to exercise the network stack end to end inside one process.
//!
//! # Objects
//!
//! ```text
//! TokioAsyncIoContext (C++)           kj::setupAsyncIo() analogue; teardown is member order
//!     ├── kj_rs_tokio::TokioAsyncIoContext   the loop's tokio runtime (kj-rs-tokio)
//!     ├── TokioLowLevelAsyncIoProvider       wrap*Fd(): an owned fd/SOCKET as i64 -> Rust owns it
//!     └── TokioAsyncIoProvider
//!             └── TokioNetwork               kj::Arc<PeerFilter> + Box<LoopbackRegistry>,
//!                     │                          both shared down the restrictPeers chain:
//!                     ├── TokioNetworkAddress    Box<TokioAddress> + filter share
//!                     │       ├── TokioConnectionReceiver  Box<TokioListener> + filter share
//!                     │       └── TokioDatagramPort        Box<TokioDatagram> + filter share
//!                     └── TokioAsyncIoStream     Box<TokioStream>
//!
//! TokioStream    (stream.rs)   Arc<Inner>: the socket, plus a lazily dup'd hangup watch
//! TokioListener  (net.rs)      Arc<..>: one listening socket per resolved address
//! TokioDatagram  (net.rs)      Arc<..>: one datagram socket
//! TokioAddress   (net.rs)      the parsed address: SocketAddr list, a unix name, or a loopback
//!                              queue (loopback.rs; `loopback:` addresses, workerd test only)
//! TokioFileWatcher (watcher.rs) Arc<..>: notify watcher + metadata stamps; workerd's
//!                              FileWatcher (async-io.h) wraps it for C++
//! ```
//!
//! Addresses cross the bridge as the typed `SocketAddress` struct (ffi.rs); the filter never
//! crosses at all -- the adapter applies it (net.rs, "Where filtering happens").
//!
//! # Platforms
//!
//! Unix and Windows are the supported targets; anything else fails to compile below rather than
//! carrying dead fallback arms. The Windows arms are built and their tests run by workerd's CI
//! matrix (`.github/workflows/test.yml` runs `//src/...` on a `windows-2025` runner).

// Unsafe-code boundary. The crate root denies `unsafe_code`; exactly one module, `ffi.rs`,
// re-allows it and holds the `#[cxx::bridge]` plus every hand-written `unsafe` in the crate:
// raw-handle -> owned-fd conversions (as `unsafe fn`s, discharged at the bridge entry points),
// raw read-buffer pointers -> `&mut [MaybeUninit<u8>]`, and the SIGPIPE disposition. The other
// modules cannot write `unsafe` at all, and no `unsafe fn` is part of the crate's public surface.
// What this buys is precisely that: the code to audit for memory safety is one file, and its
// `// Safety:` comments name the C++ contracts (KJ's buffer-validity and fd-ownership rules) it
// relies on. Concentrating `unsafe` does not by itself make the safe interfaces sound: an
// unchanged KJ API that accepts raw buffers and integer descriptors cannot prove caller ownership
// and lifetimes through Rust types alone, so those two contracts stay KJ's documented interface
// contracts, checked where the crate can (in-flight operations own their socket; every operation
// checks the loop thread) and named where it cannot.
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
pub use watcher::TokioFileWatcher;

mod error;
mod ffi;
mod loopback;
mod net;
mod signal;
mod stream;
mod watcher;

/// The check that precedes every tokio registration in this crate (see the module docs, "The
/// tokio runtime"): the calling thread has a `TokioEventPort`, and its runtime is the one
/// entered. Both failures would otherwise be a tokio panic (a process abort at the bridge) or a
/// resource registered with a driver that never turns.
pub(crate) fn current_loop_runtime_id() -> error::Result<tokio::runtime::Id> {
    let port = kj_rs_tokio::current_handle().ok_or_else(|| {
        error::KjIoError::other(
            "kj_rs_io",
            "no TokioEventPort on this thread; kj-rs-io objects are created and used on the KJ \
             loop thread set up by kj_rs_io::setupTokioAsyncIo()",
        )
    })?;
    match tokio::runtime::Handle::try_current() {
        Ok(entered) if entered.id() == port.id() => Ok(port.id()),
        _ => Err(error::KjIoError::other(
            "kj_rs_io",
            "a tokio runtime other than this thread's TokioEventPort runtime is entered",
        )),
    }
}

/// The check before a tokio *registration*: this thread's `TokioEventPort` runtime is the one
/// entered (module docs, "The tokio runtime").
pub(crate) fn ensure_loop_thread() -> error::Result<()> {
    current_loop_runtime_id().map(drop)
}

/// The check before an operation *waits* on a registered resource: the port on this thread is
/// the one the resource was registered with (`owner`, recorded at construction). A stream or
/// listener carried to another loop thread passes [`ensure_loop_thread`] there and would then
/// wait on its creator's driver, which that thread never turns.
pub(crate) fn ensure_owner_loop(owner: tokio::runtime::Id) -> error::Result<()> {
    if current_loop_runtime_id()? == owner {
        Ok(())
    } else {
        Err(error::KjIoError::other(
            "kj_rs_io",
            "this I/O object belongs to a different TokioEventPort (it was created on another \
             loop thread)",
        ))
    }
}

// Every handle C++ owns is `Send + Sync` by type (see the module docs, "Threads"). Checked here,
// at compile time, rather than stated: a field that is not (an `Rc`, a `Cell`, a `RefCell`)
// fails the build.
const _: () = {
    const fn send_sync<T: Send + Sync>() {}
    send_sync::<TokioStream>();
    send_sync::<TokioAddress>();
    send_sync::<net::TokioDatagram>();
    send_sync::<net::TokioListener>();
    send_sync::<loopback::LoopbackRegistry>();
    send_sync::<watcher::TokioFileWatcher>();
};

#[cfg(test)]
mod tests {
    use super::*;

    fn description(err: error::KjIoError) -> String {
        cxx::KjError::from(err).description().to_owned()
    }

    #[test]
    fn a_thread_without_a_port_is_refused_not_panicked() {
        let err = std::thread::spawn(|| ensure_loop_thread().unwrap_err())
            .join()
            .unwrap();
        assert!(description(err).contains("no TokioEventPort"));
        // A plain tokio runtime is not a TokioEventPort either.
        let runtime = tokio::runtime::Builder::new_current_thread()
            .build()
            .unwrap();
        let _entered = runtime.enter();
        assert!(description(ensure_loop_thread().unwrap_err()).contains("no TokioEventPort"));
    }

    #[test]
    fn a_port_thread_passes_unless_another_runtime_is_entered_over_it() {
        let _port = kj_rs_tokio::TokioPort::new();
        ensure_loop_thread().unwrap();
        let auxiliary = tokio::runtime::Builder::new_current_thread()
            .build()
            .unwrap();
        {
            let _entered = auxiliary.enter();
            assert!(
                description(ensure_loop_thread().unwrap_err())
                    .contains("other than this thread's TokioEventPort runtime")
            );
        }
        ensure_loop_thread().unwrap();
    }
}
