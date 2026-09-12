//! The FFI island of kj-rs-io: the `#[cxx::bridge]` wire plus every hand-written `unsafe` in the
//! crate, in one file (file-top `#![allow(unsafe_code)]`; the crate root denies it).
//!
//! Three kinds of raw thing cross the bridge and are converted into typed Rust here, at the entry
//! point that receives them, before any other module sees them:
//!
//! 1. **Raw OS socket handles** arriving from C++ as an `i64` (a Unix fd or a win32 `SOCKET`).
//!    The conversions ([`prepare_fd`] / [`prepare_socket`], [`own_fd_from_raw`]) are `unsafe fn`s: an
//!    integer cannot prove that a descriptor is open or that its ownership was transferred. The
//!    bridge entry points that receive one are `unsafe fn`s too, so the requirement is visible in
//!    Rust signatures, not only in the bridge declaration; each discharges it in one `unsafe`
//!    block naming the C++ contract (`prepareFd` in async-io.c++) it relies on. Behind those
//!    entry points everything is a `socket2::Socket`, an `OwnedFd` or a tokio type.
//! 2. **Caller-owned output storage** for `tryRead` / `getsockopt`: KJ lets callers pass
//!    uninitialized memory, so it arrives as a raw pointer + length (never as `&mut [u8]`, whose
//!    elements must be initialized) and becomes `&mut [MaybeUninit<u8>]` — see [`uninit_slice`].
//!    Those entry points are `unsafe fn`s as well (pointer validity is a precondition).
//! 3. **`struct sockaddr` bytes** in both directions ([`sockaddr_to_bytes`] /
//!    [`sockaddr_from_bytes`]) and the raw `getsockopt(2)` / `setsockopt(2)` passthrough, whose
//!    socklen in/out semantics no safe std/socket2 API expresses.
//!
//! What this does and does not establish: the modules outside this file contain no `unsafe` and
//! cannot manufacture ownership from integers or aliases from pointers. Soundness still rests on
//! the C++ contracts named at each `unsafe` block here (a transferred handle is open and owned;
//! a read buffer stays valid until its promise settles), and those are KJ's documented interface
//! contracts, not something the type system checks. This file is the one to audit by hand.
//!
//! No bridged future borrows the Rust object it was called on: every opaque type is a handle to
//! `Rc`-shared state and its operations own a share (stream.rs, "Operations own their state"),
//! so a C++ wrapper destroyed with an operation pending is memory-safe.
#![allow(unsafe_code)]

use core::future::Future;
use core::mem::MaybeUninit;
use core::pin::Pin;

/// Opaque binding of `kj::AsyncIoStream`, and the cxx-bridged operations on it. Re-exported as
/// `crate::ffi::*` so the rest of the crate keeps using `ffi::`.
pub use bridge::AcceptedStream;
pub use bridge::KjAsyncIoStream;
pub use bridge::KjPieces;
pub use bridge::NetworkFilter;
pub use bridge::PeerCredentials;
pub use bridge::PeerFilter;
pub use bridge::is_tokio_stream;
pub use bridge::kj_piece;
pub use bridge::kj_pieces_count;
pub use bridge::network_filter_should_allow;
pub use bridge::peer_filter_should_allow_parse;
pub use bridge::unwrap_tokio_stream;
use cxx::KjException;
use kj_rs::KjOwn;

use crate::error::Result;
use crate::error::op;
use crate::net::OwnedConnectingSocket;
use crate::net::TokioAddress;
use crate::net::TokioListener;
use crate::net::address_clone;
use crate::net::address_connect;
use crate::net::address_listen;
use crate::net::address_to_string;
use crate::net::listener_accept;
use crate::net::listener_local_addr;
use crate::net::listener_port;
use crate::net::network_get_sockaddr;
use crate::net::network_parse_address;
use crate::net::wrap_connecting_socket;
use crate::signal::wait_for_signal;
use crate::stream::TokioInputFd;
use crate::stream::TokioOutputFd;
use crate::stream::TokioStream;
use crate::stream::output_fd_write;
use crate::stream::output_fd_write_pieces;
use crate::stream::stream_abort_read;
use crate::stream::stream_local_addr;
use crate::stream::stream_peer_addr;
use crate::stream::stream_peer_credentials;
use crate::stream::stream_shutdown_write;
use crate::stream::stream_take;
use crate::stream::stream_try_raw_handle;
use crate::stream::stream_when_write_disconnected;
use crate::stream::stream_write;
use crate::stream::stream_write_pieces;
use crate::watcher::TokioFileWatcher;
use crate::watcher::file_watcher_on_change;
use crate::watcher::file_watcher_watch;
use crate::watcher::new_file_watcher;

#[cxx::bridge(namespace = "kj_rs_io")]
// unnecessary_box_returns: returning an opaque Rust type to C++ as `Box<T>` is the cxx idiom.
#[expect(clippy::unnecessary_box_returns)]
// missing_safety_doc fires (or not) deep inside the macro expansion depending on which bridge
// items are publicly re-exported, so an `#[expect]` could go unfulfilled.
#[expect(clippy::allow_attributes)]
#[allow(clippy::missing_safety_doc)]
mod bridge {
    /// An accepted connection: the stream plus the peer's `struct sockaddr` bytes as reported by
    /// `accept(2)` itself — not re-derived with a later `getpeername()`, which can fail once the
    /// peer has already reset the connection (KJ's `accept4()` parity).
    struct AcceptedStream {
        stream: Box<TokioStream>,
        peer: Vec<u8>,
    }

    /// A Unix-domain peer's process credentials (`kj::LocalPeerIdentity::Credentials`): each
    /// value present only when the OS reported a valid one, as KJ decides (`uid != -1`,
    /// `pid > 0`).
    struct PeerCredentials {
        has_pid: bool,
        pid: i32,
        has_uid: bool,
        uid: u32,
    }

    extern "Rust" {
        type TokioStream;
        type TokioListener;
        type TokioAddress;
        type TokioInputFd;
        type TokioOutputFd;
        type OwnedConnectingSocket;

        // ==================================================================================
        // Streams (TCP or Unix domain, behind kj::AsyncIoStream)

        /// Reads until at least `min_bytes` are available (or EOF), up to `len` bytes into the
        /// caller's storage at `buf`, which may be uninitialized (KJ's `tryRead` allows it).
        /// Returns the number of bytes read; fewer than `min_bytes` indicates EOF. The storage
        /// must stay valid, and untouched by anyone else, until the returned promise settles —
        /// KJ's `AsyncInputStream::tryRead` contract.
        async unsafe fn stream_try_read<'a>(
            stream: &'a TokioStream,
            buf: *mut u8,
            len: usize,
            min_bytes: usize,
        ) -> Result<usize>;

        /// Writes the entire buffer (write-all semantics).
        async unsafe fn stream_write<'a>(stream: &'a TokioStream, buf: &'a [u8]) -> Result<()>;

        /// Writes every piece, in order, with write-all semantics, using vectored writes
        /// (`writev`) so a multi-piece `kj::AsyncOutputStream::write()` is one bridged
        /// operation and as few syscalls as the kernel allows.
        async unsafe fn stream_write_pieces<'a>(
            stream: &'a TokioStream,
            pieces: &'a KjPieces,
        ) -> Result<()>;

        /// Resolves when the stream has become disconnected such that new writes will fail.
        /// See `TokioStream::when_write_disconnected` for the mechanism and platform caveats.
        async unsafe fn stream_when_write_disconnected<'a>(stream: &'a TokioStream) -> Result<()>;

        /// `shutdown(SHUT_WR)`: cleanly shut down the write end, keeping the read end open.
        fn stream_shutdown_write(stream: &TokioStream) -> Result<()>;

        /// `shutdown(SHUT_RD)` (`kj::AsyncIoStream::abortRead`): a pending read observes EOF.
        fn stream_abort_read(stream: &TokioStream) -> Result<()>;

        /// Raw `struct sockaddr` bytes of the socket's locally-bound address (the
        /// `getsockname()` passthrough).
        fn stream_local_addr(stream: &TokioStream) -> Result<Vec<u8>>;

        /// Raw `struct sockaddr` bytes of the connected peer's address (the `getpeername()`
        /// passthrough).
        fn stream_peer_addr(stream: &TokioStream) -> Result<Vec<u8>>;

        /// The connected Unix-domain peer's credentials (`SO_PEERCRED` / `LOCAL_PEERCRED` +
        /// `LOCAL_PEERPID`, via tokio), for `kj::LocalPeerIdentity`. Errors for non-Unix
        /// sockets.
        fn stream_peer_credentials(stream: &TokioStream) -> Result<PeerCredentials>;

        /// `getsockopt(2)` on the underlying socket into the caller's (possibly uninitialized)
        /// `len` bytes at `value`; `len` is the in-length (the kernel truncates the option value
        /// to it). Returns the syscall's reported out-length, which the caller mirrors back
        /// exactly (raw socklen in/out semantics).
        unsafe fn stream_getsockopt(
            stream: &TokioStream,
            level: i32,
            option: i32,
            value: *mut u8,
            len: usize,
        ) -> Result<usize>;

        /// `setsockopt(2)` on the underlying socket.
        fn stream_setsockopt(
            stream: &TokioStream,
            level: i32,
            option: i32,
            value: &[u8],
        ) -> Result<()>;

        /// Moves the native stream out, leaving `stream` hollow (all further ops error). Fails
        /// (rather than aliasing live borrows) if any I/O future is in flight on `stream`: the
        /// `TokioStream` tracks in-flight operations itself, so this is a checked operation,
        /// not a caller contract.
        fn stream_take(stream: &TokioStream) -> Result<Box<TokioStream>>;
        /// The underlying raw OS socket handle (fd on unix, `SOCKET` on windows) as an `i64`,
        /// backing `kj::AsyncIoStream::getFd()` / `getWin32Handle()`; -1 when the wrapper is
        /// hollow (those return a `kj::Maybe`, so no exception-as-control-flow).
        fn stream_try_raw_handle(stream: &TokioStream) -> i64;

        // ==================================================================================
        // Network addresses (kj::Network::parseAddress grammar subset)

        /// Parses a KJ address string ("1.2.3.4:80", "[::1]:80", "host:80", "*", "*:80",
        /// "unix:/path", "unix-abstract:name"), resolving hostnames via DNS. `port_hint` fills
        /// in a missing port. A *literal* whose family `filter` forbids is rejected with KJ's
        /// parse-time error text (DNS results are judged at connect(), as in KJ). (Owned
        /// `String`: the future must not borrow the caller's buffer across the DNS suspension.)
        async fn network_parse_address(
            addr: String,
            port_hint: u16,
            filter: KjRc<PeerFilter>,
        ) -> Result<Box<TokioAddress>>;

        /// Builds an address from a raw `struct sockaddr` (AF_INET / AF_INET6 / AF_UNIX).
        fn network_get_sockaddr(sockaddr: &[u8]) -> Result<Box<TokioAddress>>;

        /// KJ's `NetworkAddressImpl::connect()`: tries each resolved address in order, skipping
        /// (with "connect() blocked by restrictPeers()") the ones `filter` forbids; the last
        /// address's failure propagates. The future owns its targets and the filter share.
        async unsafe fn address_connect<'a>(
            addr: &'a TokioAddress,
            filter: KjOwn<NetworkFilter>,
        ) -> Result<Box<TokioStream>>;

        /// Binds + listens on every resolved address (KJ's aggregate receiver). Wildcard
        /// addresses bind dual-stack.
        fn address_listen(addr: &TokioAddress) -> Result<Box<TokioListener>>;

        fn address_clone(addr: &TokioAddress) -> Box<TokioAddress>;
        fn address_to_string(addr: &TokioAddress) -> String;

        // ==================================================================================
        // Listeners (kj::ConnectionReceiver)

        /// Accepts the next connection `filter` allows on any of the listener's sockets:
        /// disallowed peers are dropped silently and accepting continues, and KJ's set of
        /// transient per-connection failures is retried rather than surfaced (see
        /// `net.rs::is_transient_accept_error`).
        async unsafe fn listener_accept<'a>(
            listener: &'a TokioListener,
            filter: KjOwn<NetworkFilter>,
        ) -> Result<AcceptedStream>;

        /// The locally-bound port of the first socket (0 for Unix domain sockets), mirroring
        /// KJ's receivers.
        fn listener_port(listener: &TokioListener) -> Result<u16>;

        /// Raw `struct sockaddr` bytes of the first socket's bound address (the `getsockname()`
        /// passthrough; KJ's aggregate receiver reports its first child too).
        fn listener_local_addr(listener: &TokioListener) -> Result<Vec<u8>>;

        /// `getsockopt(2)` on the first listening socket; same storage and length semantics as
        /// `stream_getsockopt`.
        unsafe fn listener_getsockopt(
            listener: &TokioListener,
            level: i32,
            option: i32,
            value: *mut u8,
            len: usize,
        ) -> Result<usize>;

        /// `setsockopt(2)` on every listening socket (KJ's aggregate receiver applies to all).
        fn listener_setsockopt(
            listener: &TokioListener,
            level: i32,
            option: i32,
            value: &[u8],
        ) -> Result<()>;

        // ==================================================================================
        // Socket-handle wrapping (kj::LowLevelAsyncIoProvider).
        //
        // The `i64` is a raw OS socket handle: a Unix fd or a win32 `SOCKET` (`i64` fits both
        // losslessly, with `-1` ≡ `INVALID_SOCKET` as the shared sentinel), accompanied by KJ's
        // `LowLevelAsyncIoProvider::Flags` verbatim (TAKE_OWNERSHIP / ALREADY_CLOEXEC /
        // ALREADY_NONBLOCK; async-io.c++ static_asserts the encoding). Rust applies the flags
        // itself (`prepare_fd`): it dup's when the caller keeps ownership, sets CLOEXEC and
        // non-blocking mode as needed, and ends up with a typed owner. Each entry point is an
        // `unsafe fn`: the handle must be open (and, under TAKE_OWNERSHIP, exclusively ours).

        /// Wraps a connected stream socket handle (TCP or Unix domain, detected automatically).
        unsafe fn wrap_socket_fd(handle: i64, flags: u32) -> Result<Box<TokioStream>>;

        /// Wraps a bound+listening socket handle (TCP or Unix domain, detected automatically).
        unsafe fn wrap_listen_fd(handle: i64, flags: u32) -> Result<Box<TokioListener>>;

        /// Takes ownership of an unconnected stream socket handle synchronously, before the
        /// async connect operation exists (so dropping the connect promise unpolled still
        /// closes it).
        unsafe fn own_connecting_socket(
            handle: i64,
            flags: u32,
        ) -> Result<Box<OwnedConnectingSocket>>;

        /// Connects an owned socket to `sockaddr` (a raw `struct sockaddr`: AF_INET /
        /// AF_INET6, or AF_UNIX on unix; owned copy, since the caller's pointer need not outlive
        /// the call).
        async fn wrap_connecting_socket_fd(
            socket: Box<OwnedConnectingSocket>,
            sockaddr: Vec<u8>,
        ) -> Result<Box<TokioStream>>;

        /// Wraps a readable fd (pipe, character device, socket). Regular files are rejected by
        /// the OS readiness API (same as KJ's epoll-based provider). Unix only (the pipe tier
        /// keeps `i32` fds).
        unsafe fn wrap_input_fd(fd: i32, flags: u32) -> Result<Box<TokioInputFd>>;

        /// `tryRead` on a wrapped fd; storage semantics as `stream_try_read`.
        async unsafe fn input_fd_try_read<'a>(
            stream: &'a TokioInputFd,
            buf: *mut u8,
            len: usize,
            min_bytes: usize,
        ) -> Result<usize>;

        /// Wraps a writable fd (pipe, character device, socket).
        unsafe fn wrap_output_fd(fd: i32, flags: u32) -> Result<Box<TokioOutputFd>>;

        async unsafe fn output_fd_write<'a>(stream: &'a TokioOutputFd, buf: &'a [u8])
        -> Result<()>;

        /// Writes every piece in order (write-all semantics), as one bridged operation.
        async unsafe fn output_fd_write_pieces<'a>(
            stream: &'a TokioOutputFd,
            pieces: &'a KjPieces,
        ) -> Result<()>;

        // ==================================================================================
        // Signals (kj::UnixEventPort::onSignal replacement; see signal.rs for semantics)

        /// Resolves when the process receives signal `signum` (on Windows: the mapped
        /// SIGTERM/SIGINT console control event).
        async fn wait_for_signal(signum: i32) -> Result<()>;

        // ==================================================================================
        // File watching (workerd's --watch; see watcher.rs)

        /// The `--watch` file watcher over the `notify` crate.
        type TokioFileWatcher;
        /// Creates a watcher (needs the loop runtime for its event channel).
        fn new_file_watcher() -> Result<Box<TokioFileWatcher>>;
        /// Watches `path` (a file; its parent directory must exist, the file need not).
        fn file_watcher_watch(watcher: &TokioFileWatcher, path: &str) -> Result<()>;
        /// Resolves the next time a watched file changes (immediately if one is queued). At
        /// most one may be outstanding per watcher.
        async unsafe fn file_watcher_on_change<'a>(watcher: &'a TokioFileWatcher) -> Result<()>;
    }

    unsafe extern "C++" {
        include!("kj-rs-io/unwrap.h");

        /// The pieces of a `kj::AsyncOutputStream::write(pieces)` call (unwrap.h), read through
        /// the two accessors below. Owned by the C++ coroutine frame awaiting
        /// `stream_write_pieces`, so the borrow the bridged future holds is always valid.
        type KjPieces;
        #[cxx_name = "kjPiecesCount"]
        fn kj_pieces_count(pieces: &KjPieces) -> usize;
        /// The `index`th piece. Borrowed from `pieces`; `index < kj_pieces_count(pieces)`.
        #[cxx_name = "kjPiece"]
        fn kj_piece<'a>(pieces: &'a KjPieces, index: usize) -> &'a [u8];

        /// `kj::AsyncIoStream`, opaque. Used by [`unwrap_kj_stream`].
        #[namespace = "kj"]
        #[cxx_name = "AsyncIoStream"]
        type KjAsyncIoStream;

        /// `kj::LowLevelAsyncIoProvider::NetworkFilter` (via the `kj_rs_io::NetworkFilter`
        /// alias in unwrap.h): the restrictPeers policy object, C++-owned. Rust holds it as a
        /// `KjOwn` share for the duration of a connect or accept.
        type NetworkFilter;
        /// `NetworkFilter::shouldAllow` over raw `struct sockaddr` bytes. `Result`: a filter is
        /// caller-provided C++ and may throw (KJ's own asserts on a malformed address, or a
        /// custom filter); an infallible declaration would abort the process instead of
        /// rejecting the operation.
        #[cxx_name = "networkFilterShouldAllow"]
        fn network_filter_should_allow(
            filter: Pin<&mut NetworkFilter>,
            addr: &[u8],
        ) -> Result<bool>;

        /// kj-rs-io's own (refcounted) filter (peer-filter.h), which additionally exposes KJ's
        /// parse-time check; Rust holds it as a `KjRc` share.
        type PeerFilter;
        /// `kj::_::NetworkFilter::shouldAllowParse` over raw `struct sockaddr` bytes
        /// (`Result` for the same reason as `network_filter_should_allow`).
        #[cxx_name = "peerFilterShouldAllowParse"]
        fn peer_filter_should_allow_parse(filter: &PeerFilter, addr: &[u8]) -> Result<bool>;

        /// Whether the stream is the kj-rs-io wrapper accepted by `unwrap_tokio_stream`.
        #[cxx_name = "isTokioStream"]
        fn is_tokio_stream(stream: &KjAsyncIoStream) -> bool;

        /// Implemented in `async-io.c++`: downcasts to the kj-rs-io wrapper and moves the native
        /// stream out. Throws (surfaced as `Err`) for foreign streams.
        #[cxx_name = "unwrapTokioStream"]
        fn unwrap_tokio_stream(stream: Pin<&mut KjAsyncIoStream>) -> Result<Box<TokioStream>>;

        // The two directions of a pumped foreign `kj::AsyncIoStream` (serve_kj_stream's
        // duplex-pump fallback, serve.rs), as two distinct C++ objects (unwrap.h). A kj two-way
        // stream supports one read and one write in flight at once; giving each direction its
        // own object lets Rust hold a genuinely exclusive `Pin<&mut _>` per direction with no
        // const_cast and no aliasing `&mut` of the stream. Both ends refer to the stream, so
        // they must not outlive it — [`split_kj_stream`] ties them to a `&mut` borrow of its
        // owner. All returned futures must be polled on the KJ event-loop thread owning the
        // stream.

        type KjStreamReadEnd;
        type KjStreamWriteEnd;

        /// The read direction of `stream`. Fallible only in that `kj::heap` may throw.
        #[cxx_name = "kjStreamReadEnd"]
        fn kj_stream_read_end(stream: Pin<&mut KjAsyncIoStream>) -> Result<KjOwn<KjStreamReadEnd>>;
        /// The write direction of `stream`.
        #[cxx_name = "kjStreamWriteEnd"]
        fn kj_stream_write_end(
            stream: Pin<&mut KjAsyncIoStream>,
        ) -> Result<KjOwn<KjStreamWriteEnd>>;

        /// `kj::AsyncIoStream::tryRead(buffer, min_bytes, buffer.len())`. The buffer is a
        /// Rust-owned, initialized `Vec` here (the pump's), so `&mut [u8]` is the right type.
        #[cxx_name = "kjReadEndTryRead"]
        async fn kj_read_end_try_read(
            end: Pin<&mut KjStreamReadEnd>,
            buffer: &mut [u8],
            min_bytes: usize,
        ) -> Result<usize>;

        /// `kj::AsyncIoStream::write(buffer)` (write-all semantics).
        #[cxx_name = "kjWriteEndWrite"]
        async fn kj_write_end_write(end: Pin<&mut KjStreamWriteEnd>, buffer: &[u8]) -> Result<()>;

        /// `kj::AsyncIoStream::shutdownWrite()`. `Result`: the C++ side can throw (e.g.
        /// `shutdown(2)` on an already-reset socket), and an infallible CXX declaration would
        /// convert that exception to a Rust panic.
        #[cxx_name = "kjWriteEndShutdownWrite"]
        fn kj_write_end_shutdown_write(end: Pin<&mut KjStreamWriteEnd>) -> Result<()>;
    }
}

// ======================================================================================
// Caller-owned, possibly uninitialized output storage.

/// Views `len` bytes of caller-owned storage at `ptr` as `&mut [MaybeUninit<u8>]`: the honest
/// type for KJ read buffers, which callers routinely declare uninitialized (`kj::byte buf[16];`).
/// A `&mut [u8]` here would assert initialized contents the storage does not have.
///
/// # Safety
///
/// `ptr` must be valid for reads and writes of `len` bytes for the whole lifetime `'a` the
/// caller chooses, and nothing else may access that memory meanwhile. The bridge entry points
/// below pick `'a` as the lifetime of the future they return, which is exactly the validity
/// KJ's `tryRead` / `getsockopt` contracts grant a buffer: until the returned promise settles
/// (or, for the synchronous `getsockopt`, until the call returns).
unsafe fn uninit_slice<'a>(ptr: *mut u8, len: usize) -> &'a mut [MaybeUninit<u8>] {
    if len == 0 {
        // A zero-length read passes whatever pointer the caller had; never dereference it.
        return &mut [];
    }
    // Safety: per this function's contract, `ptr..ptr+len` is valid, exclusively ours for `'a`,
    // and `MaybeUninit<u8>` has no validity requirement on its contents.
    unsafe { std::slice::from_raw_parts_mut(ptr.cast::<MaybeUninit<u8>>(), len) }
}

/// # Safety
///
/// `buf` must be valid for reads and writes of `len` bytes, and untouched by anyone else, until
/// the returned future settles or is dropped (KJ's `tryRead` contract; the C++ adapter is the
/// only caller).
pub unsafe fn stream_try_read(
    stream: &TokioStream,
    buf: *mut u8,
    len: usize,
    min_bytes: usize,
) -> impl Future<Output = Result<usize>> + use<> {
    // Safety: forwarded from this function's contract; the future is the buffer's lifetime.
    let buf = unsafe { uninit_slice(buf, len) };
    stream.try_read_min(buf, min_bytes)
}

/// # Safety
///
/// As [`stream_try_read`].
pub unsafe fn input_fd_try_read(
    stream: &TokioInputFd,
    buf: *mut u8,
    len: usize,
    min_bytes: usize,
) -> impl Future<Output = Result<usize>> + use<> {
    #[cfg(unix)]
    {
        // Safety: forwarded from this function's contract.
        let buf = unsafe { uninit_slice(buf, len) };
        stream.try_read_min(buf, min_bytes)
    }
    #[cfg(windows)]
    {
        // Never reached: the C++ side routes win32 wrapInputFd through wrap_socket_fd, so no
        // TokioInputFd exists there. Kept total for the bridge.
        let _ = (stream, buf, len, min_bytes);
        core::future::ready(Err(crate::error::KjIoError::other(
            "read()",
            "kj-rs-io's pipe-fd tier is unix only",
        )))
    }
}

/// `read(2)` into possibly uninitialized storage. std's `Read::read` needs an initialized
/// `&mut [u8]`, so the pipe tier reads through the raw syscall instead. Returns the count read
/// (0 at EOF); `WouldBlock` for an empty non-blocking pipe, as `AsyncFd::try_io` expects.
#[cfg(unix)]
pub fn read_uninit(
    fd: std::os::fd::BorrowedFd<'_>,
    buf: &mut [MaybeUninit<u8>],
) -> std::io::Result<usize> {
    use std::os::fd::AsRawFd;
    // Safety: `fd` is a live descriptor for the duration of this call; `buf` is exclusively
    // ours and `read` writes at most `buf.len()` bytes into it.
    let n = unsafe { libc::read(fd.as_raw_fd(), buf.as_mut_ptr().cast(), buf.len()) };
    // Safety of the count: a non-negative return is the number of bytes the kernel wrote,
    // bounded by `buf.len()`.
    usize::try_from(n).map_err(|_| std::io::Error::last_os_error())
}

/// `write(2)` on a borrowed fd, the pipe tier's counterpart of [`read_uninit`] (std's `Write`
/// is only implemented for owning types, and a `File` view over a borrowed fd would need
/// `from_raw_fd`). Returns the count written; `WouldBlock` for a full non-blocking pipe.
#[cfg(unix)]
pub fn write_fd(fd: std::os::fd::BorrowedFd<'_>, buf: &[u8]) -> std::io::Result<usize> {
    use std::os::fd::AsRawFd;
    // Safety: `fd` is a live descriptor for the duration of this call; `buf` is initialized
    // memory of `buf.len()` bytes that the kernel only reads.
    let n = unsafe { libc::write(fd.as_raw_fd(), buf.as_ptr().cast(), buf.len()) };
    usize::try_from(n).map_err(|_| std::io::Error::last_os_error())
}

// ======================================================================================
// Raw socket handles / file descriptors.

/// Takes ownership of an open file descriptor that arrived across the bridge as an `i32`.
///
/// # Safety
///
/// `fd` must be an open descriptor whose ownership is being transferred to the caller: nothing
/// else may close it, and the returned `OwnedFd` becomes its sole owner (closing it on drop).
/// The bridge entry points establish this from KJ's `TAKE_OWNERSHIP` flag (see [`prepare_fd`]).
#[cfg(unix)]
#[must_use]
pub unsafe fn own_fd_from_raw(fd: i32) -> std::os::fd::OwnedFd {
    use std::os::fd::FromRawFd;
    // `OwnedFd`'s invariant is "an open fd, never -1" (-1 is its niche). prepareFd's
    // all-flags-set path performs no syscall that would catch a bad fd, so enforce the numeric
    // half of the contract here instead of inheriting library UB.
    assert!(fd >= 0, "invalid fd crossed the FFI bridge: {fd}");
    // Safety: per this function's contract `fd` is open and owned by us from this point on.
    unsafe { std::os::fd::OwnedFd::from_raw_fd(fd) }
}

/// Takes ownership of an open winsock `SOCKET` that arrived across the bridge as an `i64`
/// (`u64`-shaped `RawSocket`; a live SOCKET fits in an `i64` without colliding with the -1
/// sentinel, which is `INVALID_SOCKET` and never crosses the bridge as an owned handle).
///
/// # Safety
///
/// `handle` must be an open `SOCKET` whose ownership is transferred to the caller.
#[cfg(windows)]
#[must_use]
pub unsafe fn own_socket_from_raw(handle: i64) -> socket2::Socket {
    use std::os::windows::io::FromRawSocket;
    use std::os::windows::io::OwnedSocket;
    use std::os::windows::io::RawSocket;
    assert!(
        handle >= 0,
        "invalid SOCKET crossed the FFI bridge: {handle}"
    );
    // The bridge carries the SOCKET's bits verbatim.
    #[allow(clippy::cast_sign_loss)]
    let raw = handle as RawSocket;
    // Safety: per this function's contract `handle` is an open SOCKET owned by us from now on.
    let owned = unsafe { OwnedSocket::from_raw_socket(raw) };
    socket2::Socket::from(owned)
}

// ======================================================================================
// KJ's fd-wrapping flags (kj::LowLevelAsyncIoProvider::Flags, encoding pinned by a
// static_assert in async-io.c++), applied here so the C++ adapters hand over raw handles as KJ
// gave them.

/// The returned object owns the handle (closes it on drop). Without it, the caller keeps the
/// handle and Rust works on a duplicate.
const TAKE_OWNERSHIP: u32 = 1 << 0;
/// The handle already has close-on-exec set (only meaningful with `TAKE_OWNERSHIP`).
#[cfg(unix)]
const ALREADY_CLOEXEC: u32 = 1 << 1;
/// The handle is already non-blocking. (Not declared by KJ on Windows, where non-blocking mode
/// is always set.)
#[cfg(unix)]
const ALREADY_NONBLOCK: u32 = 1 << 2;

/// Sets `FD_CLOEXEC` if it is not set already.
#[cfg(unix)]
fn set_cloexec(fd: std::os::fd::BorrowedFd<'_>) -> Result<()> {
    use std::os::fd::AsRawFd;
    // Safety: plain fcntl on a live descriptor; no memory is passed.
    let flags = unsafe { libc::fcntl(fd.as_raw_fd(), libc::F_GETFD) };
    if flags < 0 {
        return Err(op("fcntl(F_GETFD)")(std::io::Error::last_os_error()));
    }
    if flags & libc::FD_CLOEXEC == 0 {
        // Safety: as above.
        if unsafe { libc::fcntl(fd.as_raw_fd(), libc::F_SETFD, flags | libc::FD_CLOEXEC) } < 0 {
            return Err(op("fcntl(F_SETFD)")(std::io::Error::last_os_error()));
        }
    }
    Ok(())
}

/// Sets `O_NONBLOCK` if it is not set already (observed through every duplicate of the open
/// file description, as with KJ).
#[cfg(unix)]
fn set_nonblocking(fd: std::os::fd::BorrowedFd<'_>) -> Result<()> {
    use std::os::fd::AsRawFd;
    // Safety: plain fcntl on a live descriptor; no memory is passed.
    let flags = unsafe { libc::fcntl(fd.as_raw_fd(), libc::F_GETFL) };
    if flags < 0 {
        return Err(op("fcntl(F_GETFL)")(std::io::Error::last_os_error()));
    }
    if flags & libc::O_NONBLOCK == 0 {
        // Safety: as above.
        if unsafe { libc::fcntl(fd.as_raw_fd(), libc::F_SETFL, flags | libc::O_NONBLOCK) } < 0 {
            return Err(op("fcntl(F_SETFL)")(std::io::Error::last_os_error()));
        }
    }
    Ok(())
}

/// Applies KJ's flags to a raw fd and produces its owner: a `F_DUPFD_CLOEXEC` duplicate when
/// the caller keeps ownership, else the fd itself with CLOEXEC ensured; non-blocking mode set
/// unless declared.
///
/// # Safety
///
/// `fd` must be an open descriptor. With `TAKE_OWNERSHIP` it must be exclusively ours from now
/// on (nothing else may close it); without, it must stay open for the duration of this call.
#[cfg(unix)]
unsafe fn prepare_fd(fd: i32, flags: u32) -> Result<std::os::fd::OwnedFd> {
    use std::os::fd::AsFd;
    let owned = if flags & TAKE_OWNERSHIP == 0 {
        assert!(fd >= 0, "invalid fd crossed the FFI bridge: {fd}");
        // Safety: forwarded from this function's contract (open for the duration of the call);
        // the borrow ends when the dup returns.
        unsafe { std::os::fd::BorrowedFd::borrow_raw(fd) }
            .try_clone_to_owned()
            .map_err(op("fcntl(F_DUPFD_CLOEXEC)"))?
    } else {
        // Safety: forwarded from this function's contract (ownership transferred).
        let owned = unsafe { own_fd_from_raw(fd) };
        if flags & ALREADY_CLOEXEC == 0 {
            set_cloexec(owned.as_fd())?;
        }
        owned
    };
    if flags & ALREADY_NONBLOCK == 0 {
        set_nonblocking(owned.as_fd())?;
    }
    Ok(owned)
}

/// Socket form of [`prepare_fd`].
///
/// # Safety
///
/// As [`prepare_fd`], for a socket handle.
#[cfg(unix)]
unsafe fn prepare_socket(handle: i64, flags: u32) -> Result<socket2::Socket> {
    assert!(
        (0..=i64::from(i32::MAX)).contains(&handle),
        "invalid socket fd crossed the FFI bridge: {handle}"
    );
    #[expect(clippy::cast_possible_truncation)]
    let fd = handle as i32;
    // Safety: forwarded from this function's contract.
    Ok(socket2::Socket::from(unsafe { prepare_fd(fd, flags) }?))
}

/// Windows form of [`prepare_socket`]: kj's own win32 provider never dups a borrowed socket
/// (it merely skips `closesocket()` on destruction) and ignores `ALREADY_CLOEXEC` (handle
/// inheritance is the analogue); Rust's `OwnedSocket` has no "don't close" mode, so a borrowed
/// socket is duplicated (`WSADuplicateSocketW`, via `BorrowedSocket::try_clone_to_owned`).
/// tokio/mio's readiness model needs non-blocking sockets, so `FIONBIO` is always set (the
/// duplicate shares the underlying socket state, so the caller observes it too, like the unix
/// `dup` + `O_NONBLOCK`).
///
/// # Safety
///
/// As [`prepare_fd`], for a `SOCKET`.
#[cfg(windows)]
unsafe fn prepare_socket(handle: i64, flags: u32) -> Result<socket2::Socket> {
    use std::os::windows::io::BorrowedSocket;
    use std::os::windows::io::RawSocket;
    assert!(
        handle >= 0,
        "invalid SOCKET crossed the FFI bridge: {handle}"
    );
    #[allow(clippy::cast_sign_loss)]
    let raw = handle as RawSocket;
    let socket = if flags & TAKE_OWNERSHIP == 0 {
        // Safety: forwarded from this function's contract (open for the duration of the call).
        let dup = unsafe { BorrowedSocket::borrow_raw(raw) }
            .try_clone_to_owned()
            .map_err(op("WSADuplicateSocketW()"))?;
        socket2::Socket::from(dup)
    } else {
        // Safety: forwarded from this function's contract (ownership transferred).
        unsafe { own_socket_from_raw(handle) }
    };
    socket
        .set_nonblocking(true)
        .map_err(op("ioctlsocket(FIONBIO)"))?;
    Ok(socket)
}

// ======================================================================================
// Bridge entry points that receive raw handles. Each converts the handle into a typed owner in
// one `unsafe` block, then hands the owner to the implementation module.

/// # Safety
///
/// `handle` must be an open socket, exclusively ours if `flags` has `TAKE_OWNERSHIP`, else open
/// for the duration of the call (KJ's `LowLevelAsyncIoProvider::wrap*Fd` contract; the C++
/// adapter is the only caller).
pub unsafe fn wrap_socket_fd(handle: i64, flags: u32) -> Result<Box<TokioStream>> {
    // Safety: forwarded from this function's contract.
    let socket = unsafe { prepare_socket(handle, flags) }?;
    crate::net::wrap_socket(socket)
}

/// # Safety
///
/// As [`wrap_socket_fd`].
pub unsafe fn wrap_listen_fd(handle: i64, flags: u32) -> Result<Box<TokioListener>> {
    // Safety: forwarded from this function's contract.
    let socket = unsafe { prepare_socket(handle, flags) }?;
    crate::net::wrap_listener(socket)
}

/// # Safety
///
/// As [`wrap_socket_fd`].
pub unsafe fn own_connecting_socket(handle: i64, flags: u32) -> Result<Box<OwnedConnectingSocket>> {
    // Safety: forwarded from this function's contract.
    let socket = unsafe { prepare_socket(handle, flags) }?;
    Ok(Box::new(OwnedConnectingSocket::new(socket)))
}

pub async fn wrap_connecting_socket_fd(
    socket: Box<OwnedConnectingSocket>,
    sockaddr: Vec<u8>,
) -> Result<Box<TokioStream>> {
    wrap_connecting_socket(*socket, &sockaddr).await
}

/// # Safety
///
/// As [`wrap_socket_fd`], for a pipe/character-device/socket fd.
pub unsafe fn wrap_input_fd(fd: i32, flags: u32) -> Result<Box<TokioInputFd>> {
    #[cfg(unix)]
    {
        // Safety: forwarded from this function's contract.
        let owned = unsafe { prepare_fd(fd, flags) }?;
        Ok(Box::new(TokioInputFd::new(owned)?))
    }
    #[cfg(windows)]
    {
        // Never reached: the C++ side routes win32 wrapInputFd through wrap_socket_fd (kj's own
        // win32 provider has no pipe tier; its Fd is a SOCKET). Kept total for the bridge.
        let _ = (fd, flags);
        Err(crate::error::KjIoError::other(
            "wrapInputFd",
            "kj-rs-io's pipe-fd tier is unix only",
        ))
    }
}

/// # Safety
///
/// As [`wrap_input_fd`].
pub unsafe fn wrap_output_fd(fd: i32, flags: u32) -> Result<Box<TokioOutputFd>> {
    #[cfg(unix)]
    {
        // Safety: forwarded from this function's contract.
        let owned = unsafe { prepare_fd(fd, flags) }?;
        Ok(Box::new(TokioOutputFd::new(owned)?))
    }
    #[cfg(windows)]
    {
        let _ = (fd, flags);
        Err(crate::error::KjIoError::other(
            "wrapOutputFd",
            "kj-rs-io's pipe-fd tier is unix only",
        ))
    }
}

// ======================================================================================
// `struct sockaddr` <-> bytes.

/// Copies a `socket2::SockAddr`'s initialized `struct sockaddr` bytes into an owned `Vec`, to
/// hand across the bridge for the C++ side's restrictPeers checks and `getsockname()` /
/// `getpeername()` passthroughs.
#[must_use]
pub fn sockaddr_to_bytes(sockaddr: &socket2::SockAddr) -> Vec<u8> {
    // Safety: as_ptr()/len() delimit an initialized sockaddr owned by `sockaddr`.
    let bytes = unsafe {
        std::slice::from_raw_parts(sockaddr.as_ptr().cast::<u8>(), sockaddr.len() as usize)
    };
    bytes.to_vec()
}

/// Decodes raw `struct sockaddr` bytes (arriving from C++) into a `socket2::SockAddr`.
///
/// # Errors
///
/// Errors if the byte length is too short to hold a family, exceeds `sockaddr_storage`, or (on
/// unix) is shorter than the family's own address struct -- an `AF_INET` in four bytes is
/// garbage, not an address.
pub fn sockaddr_from_bytes(bytes: &[u8]) -> Result<socket2::SockAddr> {
    use crate::error::KjIoError;

    let mut storage = socket2::SockAddrStorage::zeroed();
    let storage_size = std::mem::size_of::<socket2::SockAddrStorage>();
    // Every sockaddr starts with its family (on BSDs preceded by a length byte); anything
    // shorter than that header cannot even be classified.
    #[cfg(unix)]
    let header_size = std::mem::offset_of!(libc::sockaddr, sa_data);
    #[cfg(windows)]
    let header_size = std::mem::size_of::<socket2::sa_family_t>();
    if bytes.len() < header_size || bytes.len() > storage_size {
        return Err(KjIoError::other("sockaddr", "invalid sockaddr length"));
    }
    // Safety: SockAddrStorage is plain-old-data large enough for any sockaddr; we copy
    // `bytes.len() <= size_of::<SockAddrStorage>()` bytes into it.
    unsafe {
        std::ptr::copy_nonoverlapping(
            bytes.as_ptr(),
            std::ptr::from_mut(&mut storage).cast::<u8>(),
            bytes.len(),
        );
    }
    #[expect(clippy::cast_possible_truncation)]
    let len = bytes.len() as socket2::socklen_t;
    // Safety: `storage` is a zeroed sockaddr_storage with the caller's `len` bytes copied in,
    // satisfying SockAddr::new's layout/length requirements. Reading a known family's struct
    // out of it below is always in bounds (storage is full-size and zero-filled), so a
    // length/family mismatch is a wrong address, never an out-of-bounds read; it is rejected
    // as an error just after. Families socket2 does not understand (e.g. AF_NETLINK) make the
    // accessors `as_socket()`/`as_pathname()` return `None`, and callers surface that as an
    // "unsupported sockaddr family" error (see `net.rs::network_get_sockaddr`).
    let addr = unsafe { socket2::SockAddr::new(storage, len) };
    #[cfg(unix)]
    {
        let min_len = match i32::from(addr.family()) {
            libc::AF_INET => std::mem::size_of::<libc::sockaddr_in>(),
            libc::AF_INET6 => std::mem::size_of::<libc::sockaddr_in6>(),
            libc::AF_UNIX => std::mem::offset_of!(libc::sockaddr_un, sun_path),
            _ => 0,
        };
        if bytes.len() < min_len {
            return Err(KjIoError::other(
                "sockaddr",
                format!(
                    "sockaddr too short for its family: {} bytes, family {} needs at least {min_len}",
                    bytes.len(),
                    addr.family()
                ),
            ));
        }
    }
    Ok(addr)
}

// ======================================================================================
// Raw `getsockopt(2)` / `setsockopt(2)`.
//
// The socket-option passthrough behind `kj::AsyncIoStream::get/setsockopt` and
// `kj::ConnectionReceiver::get/setsockopt`. The option buffer is caller-owned opaque bytes with
// raw socklen in/out semantics (the caller's buffer may be smaller than the option value, and the
// syscall's reported length must be surfaced verbatim), which no safe std/socket2 API expresses.
// The unix arms call libc's declarations; winsock's live in ws2_32, which the libc crate does not
// bind, so the windows arms declare them.

/// Raw `getsockopt(2)` on a borrowed socket fd into the caller's (possibly uninitialized)
/// storage. `value.len()` is passed as the in `optlen`; the reported out `optlen` is returned so
/// the C++ caller can mirror `*length = socklen` exactly as `KJ_SYSCALL(::getsockopt(...))` did.
#[cfg(unix)]
fn getsockopt_raw(
    fd: std::os::fd::BorrowedFd<'_>,
    level: i32,
    option: i32,
    value: &mut [MaybeUninit<u8>],
) -> Result<usize> {
    use std::os::fd::AsRawFd;
    #[expect(clippy::cast_possible_truncation)]
    let mut optlen = value.len() as libc::socklen_t;
    // Safety: `fd` is a live socket fd (borrowed from the tokio object for the duration of the
    // call); `value.as_mut_ptr()` with in-`optlen == value.len()` delimits writable memory the
    // kernel fills (never past `optlen`); `&raw mut optlen` is a valid in/out pointer.
    let rc = unsafe {
        libc::getsockopt(
            fd.as_raw_fd(),
            level,
            option,
            value.as_mut_ptr().cast(),
            &raw mut optlen,
        )
    };
    if rc != 0 {
        return Err(op("getsockopt()")(std::io::Error::last_os_error()));
    }
    Ok(optlen as usize)
}

/// Raw `setsockopt(2)` on a borrowed socket fd.
#[cfg(unix)]
fn setsockopt_raw(
    fd: std::os::fd::BorrowedFd<'_>,
    level: i32,
    option: i32,
    value: &[u8],
) -> Result<()> {
    use std::os::fd::AsRawFd;
    #[expect(clippy::cast_possible_truncation)]
    let optlen = value.len() as libc::socklen_t;
    // Safety: `fd` is a live socket fd for the duration of the call; `value.as_ptr()` with
    // `optlen == value.len()` delimits readable memory the kernel only reads.
    let rc =
        unsafe { libc::setsockopt(fd.as_raw_fd(), level, option, value.as_ptr().cast(), optlen) };
    if rc != 0 {
        return Err(op("setsockopt()")(std::io::Error::last_os_error()));
    }
    Ok(())
}

/// Raw ws2_32 `getsockopt` on a borrowed `SOCKET`. Same socklen in/out semantics as the unix
/// arm above.
#[cfg(windows)]
fn getsockopt_raw(
    sock: std::os::windows::io::BorrowedSocket<'_>,
    level: i32,
    option: i32,
    value: &mut [MaybeUninit<u8>],
) -> Result<usize> {
    use core::ffi::c_char;
    use core::ffi::c_int;
    use std::os::windows::io::AsRawSocket;
    use std::os::windows::io::RawSocket;
    #[link(name = "ws2_32")]
    unsafe extern "system" {
        fn getsockopt(
            s: RawSocket,
            level: c_int,
            optname: c_int,
            optval: *mut c_char,
            optlen: *mut c_int,
        ) -> c_int;
    }
    #[allow(clippy::cast_possible_truncation, clippy::cast_possible_wrap)]
    let mut optlen = value.len() as c_int;
    // Safety: `sock` is a live `SOCKET` for the duration of the call; `value.as_mut_ptr()` with
    // in-`optlen == value.len()` delimits writable memory winsock fills (never past `optlen`);
    // `&raw mut optlen` is a valid in/out pointer for the call.
    let rc = unsafe {
        getsockopt(
            sock.as_raw_socket(),
            level,
            option,
            value.as_mut_ptr().cast::<c_char>(),
            &raw mut optlen,
        )
    };
    if rc != 0 {
        // rc is SOCKET_ERROR (-1); `last_os_error()` reads `WSAGetLastError()` on Windows.
        return Err(op("getsockopt()")(std::io::Error::last_os_error()));
    }
    // The out-length winsock reports is non-negative (and bounded by the in-length).
    #[allow(clippy::cast_sign_loss)]
    let reported = optlen as usize;
    Ok(reported)
}

/// Raw ws2_32 `setsockopt` on a borrowed `SOCKET`.
#[cfg(windows)]
fn setsockopt_raw(
    sock: std::os::windows::io::BorrowedSocket<'_>,
    level: i32,
    option: i32,
    value: &[u8],
) -> Result<()> {
    use core::ffi::c_char;
    use core::ffi::c_int;
    use std::os::windows::io::AsRawSocket;
    use std::os::windows::io::RawSocket;
    #[link(name = "ws2_32")]
    unsafe extern "system" {
        fn setsockopt(
            s: RawSocket,
            level: c_int,
            optname: c_int,
            optval: *const c_char,
            optlen: c_int,
        ) -> c_int;
    }
    #[allow(clippy::cast_possible_truncation, clippy::cast_possible_wrap)]
    let optlen = value.len() as c_int;
    // Safety: `sock` is a live `SOCKET` for the duration of the call; `value.as_ptr()` with
    // `optlen == value.len()` delimits readable memory winsock only reads.
    let rc = unsafe {
        setsockopt(
            sock.as_raw_socket(),
            level,
            option,
            value.as_ptr().cast::<c_char>(),
            optlen,
        )
    };
    if rc != 0 {
        return Err(op("setsockopt()")(std::io::Error::last_os_error()));
    }
    Ok(())
}

/// # Safety
///
/// `value` must be valid for reads and writes of `len` bytes for the duration of the call
/// (KJ's `getsockopt` contract).
pub unsafe fn stream_getsockopt(
    stream: &TokioStream,
    level: i32,
    option: i32,
    value: *mut u8,
    len: usize,
) -> Result<usize> {
    // Safety: forwarded from this function's contract.
    let value = unsafe { uninit_slice(value, len) };
    #[cfg(unix)]
    {
        stream.with_borrowed_fd(|fd| getsockopt_raw(fd, level, option, value))?
    }
    #[cfg(windows)]
    {
        stream.with_borrowed_socket(|sock| getsockopt_raw(sock, level, option, value))?
    }
}

pub fn stream_setsockopt(
    stream: &TokioStream,
    level: i32,
    option: i32,
    value: &[u8],
) -> Result<()> {
    #[cfg(unix)]
    {
        stream.with_borrowed_fd(|fd| setsockopt_raw(fd, level, option, value))?
    }
    #[cfg(windows)]
    {
        stream.with_borrowed_socket(|sock| setsockopt_raw(sock, level, option, value))?
    }
}

/// # Safety
///
/// As [`stream_getsockopt`].
pub unsafe fn listener_getsockopt(
    listener: &TokioListener,
    level: i32,
    option: i32,
    value: *mut u8,
    len: usize,
) -> Result<usize> {
    // Safety: forwarded from this function's contract.
    let value = unsafe { uninit_slice(value, len) };
    #[cfg(unix)]
    {
        getsockopt_raw(listener.first_borrowed_fd(), level, option, value)
    }
    #[cfg(windows)]
    {
        getsockopt_raw(listener.first_borrowed_socket(), level, option, value)
    }
}

pub fn listener_setsockopt(
    listener: &TokioListener,
    level: i32,
    option: i32,
    value: &[u8],
) -> Result<()> {
    // KJ's aggregate receiver applies setsockopt to every child.
    #[cfg(unix)]
    {
        listener.for_each_borrowed_fd(|fd| setsockopt_raw(fd, level, option, value))
    }
    #[cfg(windows)]
    {
        listener.for_each_borrowed_socket(|sock| setsockopt_raw(sock, level, option, value))
    }
}

// ======================================================================================
// Typed read/write halves of a pumped `kj::AsyncIoStream`.
//
// kj's stream contract — at most one read and one write may be in flight at once — is prose in
// kj; these halves make the borrow checker enforce it. Each half exclusively owns one C++ "end"
// object (unwrap.h) referring to the stream and drives it through `Pin<&mut _>`, so an in-flight
// operation's future exclusively borrows its half (a second overlapping read is a compile
// error). `split_kj_stream` borrows the owner mutably for the halves' lifetime, so while they
// live nothing else (an unwrap, another split) can touch the stream, and the ends cannot outlive
// it. The bridged operations behind them are not re-exported: the halves are the only way to
// drive a foreign stream.

/// The read direction of a pumped stream. See the module comment above.
pub struct KjStreamReadHalf<'a> {
    end: KjOwn<bridge::KjStreamReadEnd>,
    stream: core::marker::PhantomData<&'a mut KjOwn<KjAsyncIoStream>>,
}

/// The write direction of a pumped stream (writes and the write-side shutdown). See the module
/// comment above.
pub struct KjStreamWriteHalf<'a> {
    end: KjOwn<bridge::KjStreamWriteEnd>,
    stream: core::marker::PhantomData<&'a mut KjOwn<KjAsyncIoStream>>,
}

/// Splits the owned stream into its two directions. The halves borrow `stream` mutably for
/// their whole lifetime: exactly one pair exists and the stream is reserved for them.
///
/// # Errors
///
/// Only if allocating an end object throws (`kj::heap`).
pub fn split_kj_stream(
    stream: &mut KjOwn<KjAsyncIoStream>,
) -> std::result::Result<(KjStreamReadHalf<'_>, KjStreamWriteHalf<'_>), KjException> {
    let read = bridge::kj_stream_read_end(stream.as_mut())?;
    let write = bridge::kj_stream_write_end(stream.as_mut())?;
    Ok((
        KjStreamReadHalf {
            end: read,
            stream: core::marker::PhantomData,
        },
        KjStreamWriteHalf {
            end: write,
            stream: core::marker::PhantomData,
        },
    ))
}

impl KjStreamReadHalf<'_> {
    /// `kj::AsyncIoStream::tryRead(buffer, min_bytes, buffer.len())`.
    pub(crate) async fn try_read(
        &mut self,
        buf: &mut [u8],
        min_bytes: usize,
    ) -> std::result::Result<usize, KjException> {
        bridge::kj_read_end_try_read(self.end.as_mut(), buf, min_bytes).await
    }
}

impl KjStreamWriteHalf<'_> {
    /// `kj::AsyncIoStream::write(buffer)` (write-all semantics).
    pub(crate) async fn write(&mut self, buf: &[u8]) -> std::result::Result<(), KjException> {
        bridge::kj_write_end_write(self.end.as_mut(), buf).await
    }

    /// `kj::AsyncIoStream::shutdownWrite()`.
    pub(crate) fn shutdown_write(&mut self) -> std::result::Result<(), KjException> {
        bridge::kj_write_end_shutdown_write(self.end.as_mut())
    }
}

// ======================================================================================
// Borrow-based unwrap entry point (`Pin<&mut kj::AsyncIoStream>`). Safe: the in-flight-I/O
// conflict it must avoid is detected by TokioStream's RefCell guard (see stream.rs).
//
// The owning native-serve entry points (`take_kj_socket`, `serve_kj_stream`) are safe fns in
// [`crate::serve`] — ownership arrives as a `KjOwn` and no raw pointer crosses the crate's
// public surface. Only the borrow-based unwrap remains here: C++ keeps the (hollow) wrapper,
// so its "no I/O in flight" precondition cannot be expressed structurally.

/// Recovers the native [`TokioStream`] out of a `kj::AsyncIoStream` that was created by
/// kj-rs-io, leaving the C++ wrapper hollow (any further I/O through it fails).
///
/// # Errors
///
/// Returns an error if the stream is not a kj-rs-io tokio-backed stream, was already unwrapped,
/// or has I/O operations (reads, writes, `whenWriteDisconnected`) in flight -- their futures
/// borrow the native object this function moves out, and `TokioStream` tracks those borrows,
/// so the conflict is detected rather than being a caller contract.
pub fn unwrap_kj_stream(
    stream: Pin<&mut KjAsyncIoStream>,
) -> std::result::Result<Box<TokioStream>, KjException> {
    bridge::unwrap_tokio_stream(stream)
}

#[cfg(test)]
mod tests {
    #[cfg(unix)]
    use cxx::KjError;

    use super::*;

    #[test]
    fn sockaddr_round_trips_v4_and_v6() {
        for text in ["1.2.3.4:80", "[::1]:443", "[fe80::1]:0"] {
            let addr: std::net::SocketAddr = text.parse().unwrap();
            let original = socket2::SockAddr::from(addr);
            let bytes = sockaddr_to_bytes(&original);
            assert_eq!(bytes.len(), original.len() as usize);
            let decoded = sockaddr_from_bytes(&bytes).unwrap();
            assert_eq!(decoded.as_socket(), Some(addr), "{text}");
        }
    }

    #[cfg(unix)]
    #[test]
    fn sockaddr_round_trips_unix_paths() {
        let original = socket2::SockAddr::unix("/tmp/kj-rs-io-test.sock").unwrap();
        let bytes = sockaddr_to_bytes(&original);
        let decoded = sockaddr_from_bytes(&bytes).unwrap();
        assert!(decoded.is_unix());
        assert_eq!(
            decoded.as_pathname(),
            Some(std::path::Path::new("/tmp/kj-rs-io-test.sock"))
        );
    }

    #[test]
    fn sockaddr_from_bytes_rejects_bad_lengths() {
        assert!(sockaddr_from_bytes(&[]).is_err());
        assert!(
            sockaddr_from_bytes(&[0u8]).is_err(),
            "shorter than a family"
        );
        let too_long = vec![0u8; std::mem::size_of::<socket2::SockAddrStorage>() + 1];
        assert!(sockaddr_from_bytes(&too_long).is_err());
    }

    /// A family whose struct does not fit in the given length is garbage, not an address: a
    /// truncated `sockaddr_in`, or an `AF_INET6` claimed in `sockaddr_in`'s size, must be rejected
    /// rather than decoded out of the zero-filled tail of the storage.
    #[cfg(unix)]
    #[test]
    fn sockaddr_from_bytes_rejects_family_length_mismatch() {
        let v4: std::net::SocketAddr = "1.2.3.4:80".parse().unwrap();
        let v4_bytes = sockaddr_to_bytes(&socket2::SockAddr::from(v4));
        // Truncated sockaddr_in (family present, address cut off).
        let truncated = &v4_bytes[..std::mem::size_of::<libc::sockaddr_in>() - 1];
        let err = KjError::from(sockaddr_from_bytes(truncated).unwrap_err());
        assert!(
            err.description().contains("too short for its family"),
            "{}",
            err.description()
        );

        // An AF_INET6 family in only sockaddr_in's worth of bytes.
        let v6: std::net::SocketAddr = "[::1]:443".parse().unwrap();
        let v6_bytes = sockaddr_to_bytes(&socket2::SockAddr::from(v6));
        let short_v6 = &v6_bytes[..std::mem::size_of::<libc::sockaddr_in>()];
        assert!(sockaddr_from_bytes(short_v6).is_err());

        // The exact struct sizes are accepted.
        assert!(sockaddr_from_bytes(&v4_bytes[..std::mem::size_of::<libc::sockaddr_in>()]).is_ok());
        assert!(
            sockaddr_from_bytes(&v6_bytes[..std::mem::size_of::<libc::sockaddr_in6>()]).is_ok()
        );

        // A unix sockaddr needs at least its header (family + sun_path offset).
        let un = sockaddr_to_bytes(&socket2::SockAddr::unix("/tmp/x").unwrap());
        assert!(
            sockaddr_from_bytes(&un[..std::mem::offset_of!(libc::sockaddr_un, sun_path) - 1])
                .is_err()
        );
    }

    /// Randomized: `sockaddr_from_bytes` must never panic on arbitrary input, and whatever it
    /// accepts must be self-consistent (family and length agree; accessors do not read past
    /// the given length's worth of meaning). Seeded xorshift, so a failure is reproducible.
    #[cfg(unix)]
    #[test]
    fn sockaddr_from_bytes_never_panics_on_random_input() {
        let mut state: u64 = 0x9e37_79b9_7f4a_7c15;
        let mut next = move || {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            state
        };
        let max = std::mem::size_of::<socket2::SockAddrStorage>() + 4;
        for _ in 0..20_000 {
            #[expect(clippy::cast_possible_truncation)]
            let len = (next() % (max as u64 + 1)) as usize;
            let mut bytes: Vec<u8> = (0..len).map(|_| (next() & 0xff) as u8).collect();
            // Bias the family field toward the interesting ones half of the time.
            if len >= 2 && next() % 2 == 0 {
                let fam = [
                    libc::AF_INET,
                    libc::AF_INET6,
                    libc::AF_UNIX,
                    libc::AF_UNSPEC,
                ][(next() % 4) as usize];
                #[expect(clippy::cast_possible_truncation, clippy::cast_sign_loss)]
                let fam = fam as socket2::sa_family_t;
                let off = std::mem::offset_of!(libc::sockaddr, sa_family);
                let fam_bytes = fam.to_ne_bytes();
                bytes[off..off + fam_bytes.len().min(len - off)]
                    .copy_from_slice(&fam_bytes[..fam_bytes.len().min(len - off)]);
            }
            if let Ok(addr) = sockaddr_from_bytes(&bytes) {
                let min_len = match i32::from(addr.family()) {
                    libc::AF_INET => std::mem::size_of::<libc::sockaddr_in>(),
                    libc::AF_INET6 => std::mem::size_of::<libc::sockaddr_in6>(),
                    libc::AF_UNIX => std::mem::offset_of!(libc::sockaddr_un, sun_path),
                    _ => 0,
                };
                assert!(
                    len >= min_len,
                    "accepted {len} bytes for family {}",
                    addr.family()
                );
                // Accessors must be safe to call on anything accepted.
                let _ = addr.as_socket();
                let _ = addr.as_pathname();
                let _ = sockaddr_to_bytes(&addr);
            }
        }
    }

    #[cfg(unix)]
    #[test]
    #[should_panic(expected = "invalid fd crossed the FFI bridge")]
    fn own_fd_from_raw_rejects_negative_fds() {
        // -1 is OwnedFd's niche: turning it into an OwnedFd would be library UB, so the
        // conversion point must refuse it loudly (a panic here becomes a kj::Exception).
        // Safety: the call is expected to panic before constructing anything.
        let _ = unsafe { own_fd_from_raw(-1) };
    }

    #[cfg(unix)]
    #[test]
    #[should_panic(expected = "invalid socket fd crossed the FFI bridge")]
    fn prepare_socket_rejects_negative_handles() {
        // Safety: expected to panic before constructing anything.
        let _ = unsafe { prepare_socket(-1, TAKE_OWNERSHIP) };
    }

    #[cfg(unix)]
    #[test]
    #[should_panic(expected = "invalid socket fd crossed the FFI bridge")]
    fn prepare_socket_rejects_handles_that_do_not_fit_an_fd() {
        // Safety: expected to panic before constructing anything.
        let _ = unsafe { prepare_socket(i64::from(i32::MAX) + 1, TAKE_OWNERSHIP) };
    }

    /// The happy path of the conversion point: an fd released by std becomes a socket2 socket
    /// that owns it (closes it on drop) and is fully usable.
    #[cfg(unix)]
    #[test]
    fn prepare_socket_takes_ownership_of_a_live_socket() {
        use std::os::fd::AsRawFd;
        use std::os::fd::IntoRawFd;
        let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let port = listener.local_addr().unwrap().port();
        let raw = listener.into_raw_fd();
        // Safety: `into_raw_fd` transferred ownership of an open socket to us.
        let socket = unsafe { prepare_socket(i64::from(raw), TAKE_OWNERSHIP) }.unwrap();
        assert_eq!(socket.as_raw_fd(), raw);
        assert_eq!(
            socket.local_addr().unwrap().as_socket().unwrap().port(),
            port
        );
        // `socket` is the sole owner: dropping it closes the fd (socket2::Socket's drop glue).
    }

    #[cfg(unix)]
    #[test]
    fn dropping_an_unpolled_connect_closes_the_transferred_socket() {
        use std::os::fd::IntoRawFd;

        let socket = socket2::Socket::new(
            socket2::Domain::IPV4,
            socket2::Type::STREAM,
            Some(socket2::Protocol::TCP),
        )
        .unwrap();
        let fd = socket.into_raw_fd();

        // Safety: `into_raw_fd` transferred ownership of an open socket to us.
        let socket = unsafe { own_connecting_socket(i64::from(fd), TAKE_OWNERSHIP) }.unwrap();
        let connect = wrap_connecting_socket_fd(socket, Vec::new());
        drop(connect);

        // Safety: F_GETFD only inspects the numeric descriptor; EBADF is the expected result
        // after ownership passed into and was dropped with the unpolled future.
        assert_eq!(unsafe { libc::fcntl(fd, libc::F_GETFD) }, -1);
        assert_eq!(
            std::io::Error::last_os_error().raw_os_error(),
            Some(libc::EBADF)
        );
    }

    /// `uninit_slice` must not touch the pointer for a zero-length request (KJ callers may pass
    /// a null or dangling pointer with `maxBytes == 0`), and must cover exactly `len` bytes
    /// otherwise.
    #[test]
    fn uninit_slice_respects_length() {
        // Safety: a null pointer with len 0 is never dereferenced by contract.
        let empty = unsafe { uninit_slice(std::ptr::null_mut(), 0) };
        assert!(empty.is_empty());
        let mut storage = [MaybeUninit::<u8>::uninit(); 8];
        // Safety: `storage` is live and exclusively ours for the slice's lifetime.
        let view = unsafe { uninit_slice(storage.as_mut_ptr().cast::<u8>(), 5) };
        assert_eq!(view.len(), 5);
        view[0].write(7);
        // Safety: element 0 was just initialized above.
        assert_eq!(unsafe { storage[0].assume_init() }, 7);
    }

    /// KJ's flag semantics: without `TAKE_OWNERSHIP` the caller's fd is untouched except for
    /// `O_NONBLOCK` (shared through the dup) and Rust works on a `CLOEXEC` duplicate; with it
    /// the fd itself is owned and gains `CLOEXEC`.
    #[cfg(unix)]
    #[test]
    fn prepare_fd_applies_kj_flags() {
        use std::os::fd::AsRawFd;
        use std::os::fd::IntoRawFd;
        fn fd_flags(fd: i32) -> (i32, i32) {
            // Safety: F_GETFD/F_GETFL only read flags of a live descriptor.
            unsafe {
                (
                    libc::fcntl(fd, libc::F_GETFD),
                    libc::fcntl(fd, libc::F_GETFL),
                )
            }
        }
        let (a, _b) = std::os::unix::net::UnixStream::pair().unwrap();
        let raw = a.as_raw_fd();
        // Borrowed: a distinct, CLOEXEC dup; the original is now non-blocking too (shared
        // open file description) but still open.
        // Safety: `a` keeps `raw` open for the call.
        let dup = unsafe { prepare_fd(raw, 0) }.unwrap();
        assert_ne!(dup.as_raw_fd(), raw);
        assert_ne!(fd_flags(dup.as_raw_fd()).0 & libc::FD_CLOEXEC, 0);
        assert_ne!(fd_flags(dup.as_raw_fd()).1 & libc::O_NONBLOCK, 0);
        assert_ne!(
            fd_flags(raw).1 & libc::O_NONBLOCK,
            0,
            "O_NONBLOCK is per open file"
        );
        drop(dup);
        assert!(fd_flags(raw).0 >= 0, "the caller's fd is still open");

        // Owned, nothing declared: same fd, CLOEXEC added.
        let (c, _d) = std::os::unix::net::UnixStream::pair().unwrap();
        let raw_c = c.into_raw_fd();
        // Safety: `into_raw_fd` transferred ownership.
        let owned = unsafe { prepare_fd(raw_c, TAKE_OWNERSHIP) }.unwrap();
        assert_eq!(owned.as_raw_fd(), raw_c);
        assert_ne!(fd_flags(raw_c).0 & libc::FD_CLOEXEC, 0);
        assert_ne!(fd_flags(raw_c).1 & libc::O_NONBLOCK, 0);
        drop(owned);
        assert_eq!(fd_flags(raw_c).0, -1, "owned: closed on drop");
    }
}
