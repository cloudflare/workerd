//! The FFI island of kj-rs-io: the `#[cxx::bridge]` wire plus the crate's hand-written `unsafe`
//! boundary, in one dedicated file (file-top `#![allow(unsafe_code)]`).
//!
//! It holds:
//!
//! - the `#[cxx::bridge] mod bridge` (namespace `kj_rs_io`) — the cxx-generated C++ <-> Rust wire,
//!   re-exported as `crate::ffi::*`; and
//! - every hand-written `unsafe` the macro does not generate, so the serve / net / stream modules
//!   can carry the crate-root `#![deny(unsafe_code)]` and be *compiler-proven* free of hand-written
//!   unsafe. Three kinds of raw thing cross the FFI boundary and are laundered into safe Rust types
//!   here:
//!
//!   1. **Raw OS socket handles** arriving from C++ as an `i64` (a Unix fd or a win32 `SOCKET`;
//!      see [`own_socket_from_raw`], the one platform conversion point) — the C++ side has
//!      already normalized KJ's `TAKE_OWNERSHIP` / `ALREADY_CLOEXEC` flags, dup'ing when not
//!      transferring ownership — plus [`dup_raw_fd`] (dup a borrowed fd into an owned one; the
//!      unix-only handle tier of [`take_kj_socket`]) and [`own_fd_from_raw`] (the unix-only
//!      pipe tier's `i32` fds).
//!   2. **`struct sockaddr` bytes** crossing in both directions — [`sockaddr_to_bytes`] /
//!      [`sockaddr_from_bytes`].
//!   3. **Raw `getsockopt(2)` / `setsockopt(2)`** — the socket-option passthrough behind
//!      `kj::AsyncIoStream` / `kj::ConnectionReceiver` carries caller-owned option buffers whose
//!      length semantics (socklen in/out) no safe std/socket2 API expresses, so the raw syscalls
//!      live here ([`stream_getsockopt`] and friends).
//!
//! Because this file opts back into `unsafe` (`#![allow(unsafe_code)]`), it is the one module in
//! the crate whose soundness must be audited by hand; the rest is enforced by the compiler.
#![allow(unsafe_code)]

use core::pin::Pin;

/// Opaque binding of `kj::AsyncIoStream`, and the cxx-bridged operations on it (shared-receiver
/// shims; safe to call). Re-exported as `crate::ffi::*` so the rest of the crate (and the
/// crate-root re-exports) keep using `ffi::`.
pub use bridge::KjAsyncIoStream;
pub use bridge::KjPieces;
pub use bridge::kj_piece;
pub use bridge::kj_pieces_count;
pub use bridge::kj_stream_get_handle;
pub use bridge::unwrap_tokio_stream;
use cxx::KjException;
use kj_rs::KjOwn;

use crate::error::Result;
use crate::error::op;
use crate::net::TokioAddress;
use crate::net::TokioListener;
use crate::net::address_clone;
use crate::net::address_connect_index;
use crate::net::address_count;
use crate::net::address_listen;
use crate::net::address_raw_sockaddr;
use crate::net::address_to_string;
use crate::net::listener_accept;
use crate::net::listener_local_addr;
use crate::net::listener_port;
use crate::net::network_get_sockaddr;
use crate::net::network_parse_address;
use crate::net::wrap_connecting_socket_fd;
use crate::net::wrap_listen_fd;
use crate::net::wrap_socket_fd;
use crate::readiness::TokioFdWatcher;
use crate::readiness::new_fd_watcher;
use crate::signal::wait_for_signal;
use crate::stream::TokioInputFd;
use crate::stream::TokioOutputFd;
use crate::stream::TokioStream;
use crate::stream::input_fd_try_read;
use crate::stream::output_fd_write;
use crate::stream::stream_local_addr;
use crate::stream::stream_peer_addr;
use crate::stream::stream_raw_handle;
use crate::stream::stream_shutdown_write;
use crate::stream::stream_take;
use crate::stream::stream_try_raw_handle;
use crate::stream::stream_try_read;
use crate::stream::stream_when_write_disconnected;
use crate::stream::stream_write;
use crate::stream::stream_write_pieces;
use crate::stream::wrap_input_fd;
use crate::stream::wrap_output_fd;

#[cxx::bridge(namespace = "kj_rs_io")]
// FFI island: the cxx bridge macro generates the `unsafe` extern shims, and this module declares
// the hand-written `unsafe extern "C++"` / `async unsafe fn` bridge surface.
// unnecessary_box_returns: returning an opaque Rust type to C++ as `Box<T>` is the cxx idiom.
#[expect(clippy::unnecessary_box_returns)]
// missing_safety_doc fires (or not) deep inside the macro expansion depending on which bridge
// items are publicly re-exported, so an `#[expect]` could go unfulfilled.
#[expect(clippy::allow_attributes)]
#[allow(clippy::missing_safety_doc)]
mod bridge {
    extern "Rust" {
        type TokioStream;
        type TokioListener;
        type TokioAddress;
        type TokioInputFd;
        type TokioOutputFd;

        // ==================================================================================
        // Streams (TCP or Unix domain, behind kj::AsyncIoStream)

        /// Reads until at least `min_bytes` are available (or EOF), up to `buf.len()`. Returns
        /// the number of bytes read; fewer than `min_bytes` indicates EOF. The kj `tryRead`
        /// contract.
        async unsafe fn stream_try_read<'a>(
            stream: &'a TokioStream,
            buf: &'a mut [u8],
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

        /// The underlying raw OS socket handle (fd on unix, `SOCKET` on windows) as an `i64`,
        /// backing `kj::AsyncIoStream::getFd()` / `getWin32Handle()`.
        fn stream_raw_handle(stream: &TokioStream) -> Result<i64>;

        /// Raw `struct sockaddr` bytes of the socket's locally-bound address (the
        /// `getsockname()` passthrough).
        fn stream_local_addr(stream: &TokioStream) -> Result<Vec<u8>>;

        /// Raw `struct sockaddr` bytes of the connected peer's address (the `getpeername()`
        /// passthrough, also used by the accept-loop peer-filter check).
        fn stream_peer_addr(stream: &TokioStream) -> Result<Vec<u8>>;

        /// `getsockopt(2)` on the underlying socket. `value.len()` is the caller's in-length
        /// (the kernel truncates the option value to it); returns the syscall's reported
        /// out-length, which the caller must mirror back exactly (raw socklen in/out
        /// semantics).
        fn stream_getsockopt(
            stream: &TokioStream,
            level: i32,
            option: i32,
            value: &mut [u8],
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
        /// Like `stream_raw_handle`, but -1 instead of an error when the wrapper is hollow (for
        /// the `kj::Maybe`-returning `getFd()`/`getWin32Handle()`).
        fn stream_try_raw_handle(stream: &TokioStream) -> i64;

        // ==================================================================================
        // Network addresses (kj::Network::parseAddress grammar subset)

        /// Parses a KJ address string ("1.2.3.4:80", "[::1]:80", "host:80", "*", "*:80",
        /// "unix:/path"), resolving hostnames via DNS. `port_hint` fills in a missing port.
        /// (Owned `String`: the future must not borrow the caller's buffer across the DNS
        /// suspension.)
        async fn network_parse_address(addr: String, port_hint: u16) -> Result<Box<TokioAddress>>;

        /// Builds an address from a raw `struct sockaddr` (AF_INET / AF_INET6 / AF_UNIX).
        fn network_get_sockaddr(sockaddr: &[u8]) -> Result<Box<TokioAddress>>;

        /// Connects to exactly the `index`th resolved address (no fallback). The C++ side
        /// drives the try-each-address loop so it can apply restrictPeers() filtering per
        /// address (KJ parity).
        async unsafe fn address_connect_index<'a>(
            addr: &'a TokioAddress,
            index: usize,
        ) -> Result<Box<TokioStream>>;

        /// Number of resolved socket addresses behind this address (>= 1).
        fn address_count(addr: &TokioAddress) -> usize;

        /// Raw `struct sockaddr` bytes of the `index`th resolved address, for C++-side
        /// kj::_::NetworkFilter (restrictPeers) checks.
        fn address_raw_sockaddr(addr: &TokioAddress, index: usize) -> Result<Vec<u8>>;

        /// Binds + listens on the (first) address. Wildcard addresses bind dual-stack.
        fn address_listen(addr: &TokioAddress) -> Result<Box<TokioListener>>;

        fn address_clone(addr: &TokioAddress) -> Box<TokioAddress>;
        fn address_to_string(addr: &TokioAddress) -> String;

        // ==================================================================================
        // Listeners (kj::ConnectionReceiver)

        async unsafe fn listener_accept<'a>(
            listener: &'a TokioListener,
        ) -> Result<Box<TokioStream>>;

        /// The locally-bound port (0 for Unix domain sockets, mirroring KJ).
        fn listener_port(listener: &TokioListener) -> Result<u16>;

        /// Raw `struct sockaddr` bytes of the listener's bound address (the `getsockname()`
        /// passthrough).
        fn listener_local_addr(listener: &TokioListener) -> Result<Vec<u8>>;

        /// `getsockopt(2)` on the listening socket; same length semantics as
        /// `stream_getsockopt`.
        fn listener_getsockopt(
            listener: &TokioListener,
            level: i32,
            option: i32,
            value: &mut [u8],
        ) -> Result<usize>;

        /// `setsockopt(2)` on the listening socket.
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
        // losslessly, with `-1` ≡ `INVALID_SOCKET` as the shared sentinel). All of these take
        // ownership of the handle. The C++ side normalizes
        // TAKE_OWNERSHIP/ALREADY_CLOEXEC/ALREADY_NONBLOCK flags (dup'ing when not taking
        // ownership) before calling in; [`own_socket_from_raw`] is the single point where the
        // raw handle becomes an owned socket.

        /// Wraps a connected stream socket handle (TCP or Unix domain, detected automatically).
        fn wrap_socket_fd(handle: i64) -> Result<Box<TokioStream>>;

        /// Wraps a bound+listening socket handle (TCP or Unix domain, detected automatically).
        fn wrap_listen_fd(handle: i64) -> Result<Box<TokioListener>>;

        /// Wraps an unconnected TCP socket handle and connects it to `sockaddr` (a raw
        /// `struct sockaddr`, AF_INET/AF_INET6 only; owned copy, since the caller's pointer
        /// need not outlive the call).
        async fn wrap_connecting_socket_fd(
            handle: i64,
            sockaddr: Vec<u8>,
        ) -> Result<Box<TokioStream>>;

        /// Wraps a readable fd (pipe, character device, socket). Regular files are rejected by
        /// the OS readiness API (same as KJ's epoll-based provider). Unix only (the pipe tier
        /// keeps `i32` fds).
        fn wrap_input_fd(fd: i32) -> Result<Box<TokioInputFd>>;

        async unsafe fn input_fd_try_read<'a>(
            stream: &'a TokioInputFd,
            buf: &'a mut [u8],
            min_bytes: usize,
        ) -> Result<usize>;

        /// Wraps a writable fd (pipe, character device, socket).
        fn wrap_output_fd(fd: i32) -> Result<Box<TokioOutputFd>>;

        async unsafe fn output_fd_write<'a>(stream: &'a TokioOutputFd, buf: &'a [u8])
        -> Result<()>;

        // ==================================================================================
        // Signals (kj::UnixEventPort::onSignal replacement; see signal.rs for semantics)

        /// Resolves when the process receives signal `signum` (on Windows: the mapped
        /// SIGTERM/SIGINT console control event).
        async fn wait_for_signal(signum: i32) -> Result<()>;

        // ==================================================================================
        // Fd readiness (kj::UnixEventPort::FdObserver::whenBecomesReadable replacement,
        // backing kj_rs_io::FileWatcher in file-watcher.h; see readiness.rs for semantics)

        /// A readiness watcher over a notification fd (inotify / kqueue). Owns its own dup of
        /// the fd and a single registration with the I/O driver, so there is no "keep the fd
        /// open" or "register once" contract for C++ to uphold. Unix only.
        type TokioFdWatcher;
        /// Create a watcher for `fd`: the fd is only borrowed for the duration of this call
        /// (to dup it); the watcher owns the dup.
        fn new_fd_watcher(fd: i32) -> Result<Box<TokioFdWatcher>>;
        /// Resolves when the fd becomes readable (readiness already pending is reported
        /// immediately). Multiple concurrent callers are fine.
        async unsafe fn readable<'a>(self: &'a TokioFdWatcher) -> Result<()>;
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

        /// Implemented in `async-io.c++`: downcasts to the kj-rs-io wrapper and moves the native
        /// stream out. Throws (surfaced as `Err`) for foreign streams.
        #[cxx_name = "unwrapTokioStream"]
        fn unwrap_tokio_stream(stream: Pin<&mut KjAsyncIoStream>) -> Result<Box<TokioStream>>;

        // Bridged operations on a foreign `kj::AsyncIoStream`, backing `serve_kj_stream`'s
        // duplex-pump fallback (serve.rs). Shared receivers (`&KjAsyncIoStream`, const_cast
        // shims in unwrap.h): a kj two-way stream supports one concurrent read and one write,
        // which the pump models as concurrent shared borrows of the stream it owns. All
        // returned futures must be polled on the KJ event-loop thread owning the stream.

        /// Corresponds to `kj::AsyncIoStream::tryRead(buffer, min_bytes, buffer.len())`.
        #[cxx_name = "kjStreamTryRead"]
        async fn kj_stream_try_read(
            stream: &KjAsyncIoStream,
            buffer: &mut [u8],
            min_bytes: usize,
        ) -> Result<usize>;

        /// Corresponds to `kj::AsyncIoStream::write(buffer)` (write-all semantics).
        #[cxx_name = "kjStreamWrite"]
        async fn kj_stream_write(stream: &KjAsyncIoStream, buffer: &[u8]) -> Result<()>;

        /// Corresponds to `kj::AsyncIoStream::shutdownWrite()`. `Result`: the C++ side can
        /// throw (e.g. `shutdown(2)` on an already-reset socket), and a C++ exception crossing a
        /// non-`Result` shim would abort the process.
        #[cxx_name = "kjStreamShutdownWrite"]
        fn kj_stream_shutdown_write(stream: &KjAsyncIoStream) -> Result<()>;

        /// The stream's underlying raw OS socket handle (fd on unix, `SOCKET` on windows;
        /// `kj::AsyncIoStream::getFd()` / `getWin32Handle()`) as an `i64`, or -1 if it exposes
        /// none. Backs the handle tier of [`take_kj_socket`].
        #[cxx_name = "kjStreamGetHandle"]
        fn kj_stream_get_handle(stream: &KjAsyncIoStream) -> i64;
    }
}

// ======================================================================================
// Raw socket handles / file descriptors.

/// Materializes an owned file descriptor from a raw `i32` that arrived across the FFI bridge
/// (the unix-only pipe tier — `wrap_input_fd`/`wrap_output_fd`; sockets go through
/// [`own_socket_from_raw`]).
///
/// Callable from safe code: the invariant it relies on (`fd` is open and its ownership has been
/// transferred to us) is structurally upheld by the cxx bridge — the C++ side normalizes KJ's
/// `TAKE_OWNERSHIP` / `ALREADY_CLOEXEC` flags and dup's the fd when the caller is not handing
/// over ownership. The returned `OwnedFd` becomes the sole owner and closes it on drop.
#[cfg(unix)]
#[must_use]
pub fn own_fd_from_raw(fd: i32) -> std::os::fd::OwnedFd {
    use std::os::fd::FromRawFd;
    // `OwnedFd`'s invariant is "an open fd, never -1" (-1 is its niche), so a negative value
    // here would be library-level UB rather than an error. C++ callers normalize through
    // prepareFd, but its all-flags-set path (TAKE_OWNERSHIP|ALREADY_CLOEXEC|ALREADY_NONBLOCK)
    // performs no syscall that would catch a bad fd — enforce the contract at THE conversion
    // point instead of inheriting the UB.
    assert!(fd >= 0, "invalid fd crossed the FFI bridge: {fd}");
    // Safety: per the bridge contract `fd` is open and owned by us from this point on.
    unsafe { std::os::fd::OwnedFd::from_raw_fd(fd) }
}

/// Materializes an owned socket from a raw OS socket handle that arrived across the FFI bridge
/// as an `i64`: a Unix fd here, a win32 `SOCKET` in the `cfg(windows)` twin below. This is THE
/// one platform conversion point — behind it everything is a uniform `socket2::Socket` /
/// std/tokio socket type.
///
/// Callable from safe code: the invariant it relies on (`handle` is an open socket whose
/// ownership has been transferred to us) is structurally upheld by the cxx bridge — the C++
/// side normalizes KJ's fd-wrapping flags, dup'ing when the caller is not handing over
/// ownership. The returned socket becomes the sole owner and closes it on drop.
#[cfg(unix)]
#[must_use]
pub fn own_socket_from_raw(handle: i64) -> socket2::Socket {
    // A unix fd is a non-negative int: the bridge widened it losslessly to i64, so the
    // narrowing back to i32 cannot truncate for any legitimate handle. Enforce that (rejecting
    // -1/garbage) here at THE conversion point rather than inheriting `OwnedFd`'s niche UB.
    assert!(
        (0..=i64::from(i32::MAX)).contains(&handle),
        "invalid socket fd crossed the FFI bridge: {handle}"
    );
    #[expect(clippy::cast_possible_truncation)]
    let fd = handle as i32;
    socket2::Socket::from(own_fd_from_raw(fd))
}

/// The `cfg(windows)` twin of [`own_socket_from_raw`]: the raw handle is a winsock `SOCKET`
/// (`u64`-shaped `RawSocket`; a live SOCKET fits in an `i64` without colliding with the -1
/// sentinel, which is `INVALID_SOCKET` and never crosses the bridge as an owned handle).
// Validated by Windows CI; mirrors the unix arm.
#[cfg(windows)]
#[must_use]
pub fn own_socket_from_raw(handle: i64) -> socket2::Socket {
    use std::os::windows::io::FromRawSocket;
    use std::os::windows::io::OwnedSocket;
    use std::os::windows::io::RawSocket;
    // Live SOCKET values are non-negative in i64 (the -1 sentinel is INVALID_SOCKET, which
    // `OwnedSocket` forbids as its niche and which must never cross the bridge as an owned
    // handle) — enforce at THE conversion point rather than inheriting the niche UB.
    assert!(
        handle >= 0,
        "invalid SOCKET crossed the FFI bridge: {handle}"
    );
    // The bridge carries the SOCKET's bits verbatim.
    #[allow(clippy::cast_sign_loss)]
    let raw = handle as RawSocket;
    // Safety: per the bridge contract `handle` is an open SOCKET owned by us from this point on.
    let owned = unsafe { OwnedSocket::from_raw_socket(raw) };
    socket2::Socket::from(owned)
}

/// Duplicates a *borrowed* raw fd into an independently-owned fd (`F_DUPFD_CLOEXEC`), for the
/// handle tier of [`take_kj_socket`]: the kj stream keeps its own fd, we get a fresh dup.
///
/// Unix only, deliberately: no windows twin is needed. The handle tier only fires for *foreign*
/// handle-backed kj streams, and under the all-rust mode on Windows every socket-backed stream
/// originates in kj-rs-io (tier-1 unwrap); the other in-process dup users don't dup on windows
/// either (`when_write_disconnected` is unix-only — never-resolving on windows, KJ parity — and
/// windows `shutdown_write` borrows via `SockRef` instead of dup'ing). If a windows twin is
/// ever needed, `std::os::windows::io::BorrowedSocket::try_clone_to_owned`
/// (`WSADuplicateSocketW`) is the same-process equivalent.
///
/// # Errors
///
/// Returns the `dup()` `io::Error` (mapped to a `kj::Exception`) if the syscall fails.
#[cfg(unix)]
pub fn dup_raw_fd(fd: i32) -> Result<std::os::fd::OwnedFd> {
    // Safety: the caller owns whatever `fd` belongs to (take_kj_socket holds the kj stream;
    // new_fd_watcher is called from FileWatcher::Impl's constructor, which owns the fd) and so
    // keeps it open for the duration of the call; we immediately dup it into an
    // independently-owned fd and never touch `fd` again.
    unsafe { std::os::fd::BorrowedFd::borrow_raw(fd) }
        .try_clone_to_owned()
        .map_err(op("dup()"))
}

// ======================================================================================
// `struct sockaddr` <-> bytes.

/// Copies a `socket2::SockAddr`'s initialized `struct sockaddr` bytes into an owned `Vec`, to
/// hand across the bridge for the C++ side's `kj::_::NetworkFilter` (restrictPeers) checks.
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
#[cfg(any(unix, windows))]
pub fn sockaddr_from_bytes(bytes: &[u8]) -> Result<socket2::SockAddr> {
    use crate::error::KjIoError;

    let mut storage = socket2::SockAddrStorage::zeroed();
    let storage_size = std::mem::size_of::<socket2::SockAddrStorage>();
    // Every sockaddr starts with its family (on BSDs preceded by a length byte); anything
    // shorter than that header cannot even be classified.
    #[cfg(unix)]
    let header_size = std::mem::offset_of!(libc::sockaddr, sa_data);
    #[cfg(not(unix))]
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
// syscall's reported length must be surfaced verbatim), which no safe std/socket2 API expresses —
// so the raw syscalls are declared and called here, in the unsafe island.

/// Raw `getsockopt(2)` on a borrowed socket fd. `value.len()` is passed as the in `optlen` (the
/// kernel truncates the option value to it); the syscall's reported out `optlen` is returned so
/// the C++ caller can mirror `*length = socklen` exactly as `KJ_SYSCALL(::getsockopt(...))` did.
#[cfg(unix)]
fn getsockopt_raw(
    fd: std::os::fd::BorrowedFd<'_>,
    level: i32,
    option: i32,
    value: &mut [u8],
) -> Result<usize> {
    use core::ffi::c_int;
    use core::ffi::c_void;
    use std::os::fd::AsRawFd;
    unsafe extern "C" {
        fn getsockopt(
            sockfd: c_int,
            level: c_int,
            optname: c_int,
            optval: *mut c_void,
            optlen: *mut socket2::socklen_t,
        ) -> c_int;
    }
    #[expect(clippy::cast_possible_truncation)]
    let mut optlen = value.len() as socket2::socklen_t;
    // Safety: simple syscall wrapper. `fd` is a live socket fd (borrowed from the tokio object
    // for the duration of the call); `value.as_mut_ptr()` with in-`optlen == value.len()`
    // delimits writable caller memory the kernel fills (never past `optlen`); `&raw mut optlen`
    // is a valid in/out pointer for the call.
    let rc = unsafe {
        getsockopt(
            fd.as_raw_fd(),
            level,
            option,
            value.as_mut_ptr().cast::<c_void>(),
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
    use core::ffi::c_int;
    use core::ffi::c_void;
    use std::os::fd::AsRawFd;
    unsafe extern "C" {
        fn setsockopt(
            sockfd: c_int,
            level: c_int,
            optname: c_int,
            optval: *const c_void,
            optlen: socket2::socklen_t,
        ) -> c_int;
    }
    #[expect(clippy::cast_possible_truncation)]
    let optlen = value.len() as socket2::socklen_t;
    // Safety: simple syscall wrapper. `fd` is a live socket fd (borrowed from the tokio object
    // for the duration of the call); `value.as_ptr()` with `optlen == value.len()` delimits
    // readable caller memory the kernel only reads.
    let rc = unsafe {
        setsockopt(
            fd.as_raw_fd(),
            level,
            option,
            value.as_ptr().cast::<c_void>(),
            optlen,
        )
    };
    if rc != 0 {
        return Err(op("setsockopt()")(std::io::Error::last_os_error()));
    }
    Ok(())
}

/// Raw ws2_32 `getsockopt` on a borrowed `SOCKET`. Same socklen in/out semantics as the unix
/// arm above: `value.len()` is passed as the in `optlen`, and the reported out `optlen` is
/// returned verbatim.
// Validated by Windows CI; mirrors the unix arm.
#[cfg(windows)]
fn getsockopt_raw(
    sock: std::os::windows::io::BorrowedSocket<'_>,
    level: i32,
    option: i32,
    value: &mut [u8],
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
    // Safety: simple syscall wrapper. `sock` is a live `SOCKET` (borrowed from the tokio object
    // for the duration of the call); `value.as_mut_ptr()` with in-`optlen == value.len()`
    // delimits writable caller memory winsock fills (never past `optlen`); `&raw mut optlen` is
    // a valid in/out pointer for the call.
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
// Validated by Windows CI; mirrors the unix arm.
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
    // Safety: simple syscall wrapper. `sock` is a live `SOCKET` (borrowed from the tokio object
    // for the duration of the call); `value.as_ptr()` with `optlen == value.len()` delimits
    // readable caller memory winsock only reads.
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
        // rc is SOCKET_ERROR (-1); `last_os_error()` reads `WSAGetLastError()` on Windows.
        return Err(op("setsockopt()")(std::io::Error::last_os_error()));
    }
    Ok(())
}

pub fn stream_getsockopt(
    stream: &TokioStream,
    level: i32,
    option: i32,
    value: &mut [u8],
) -> Result<usize> {
    #[cfg(unix)]
    {
        stream.with_borrowed_fd(|fd| getsockopt_raw(fd, level, option, value))?
    }
    // Validated by Windows CI; mirrors the unix arm.
    #[cfg(windows)]
    {
        stream.with_borrowed_socket(|sock| getsockopt_raw(sock, level, option, value))?
    }
    #[cfg(not(any(unix, windows)))]
    {
        let _ = (stream, level, option, value);
        Err(crate::error::KjIoError::other(
            "getsockopt",
            "not implemented by kj-rs-io on this platform",
        ))
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
    // Validated by Windows CI; mirrors the unix arm.
    #[cfg(windows)]
    {
        stream.with_borrowed_socket(|sock| setsockopt_raw(sock, level, option, value))?
    }
    #[cfg(not(any(unix, windows)))]
    {
        let _ = (stream, level, option, value);
        Err(crate::error::KjIoError::other(
            "setsockopt",
            "not implemented by kj-rs-io on this platform",
        ))
    }
}

pub fn listener_getsockopt(
    listener: &TokioListener,
    level: i32,
    option: i32,
    value: &mut [u8],
) -> Result<usize> {
    #[cfg(unix)]
    {
        getsockopt_raw(listener.as_borrowed_fd(), level, option, value)
    }
    // Validated by Windows CI; mirrors the unix arm.
    #[cfg(windows)]
    {
        getsockopt_raw(listener.as_borrowed_socket(), level, option, value)
    }
    #[cfg(not(any(unix, windows)))]
    {
        let _ = (listener, level, option, value);
        Err(crate::error::KjIoError::other(
            "getsockopt",
            "not implemented by kj-rs-io on this platform",
        ))
    }
}

pub fn listener_setsockopt(
    listener: &TokioListener,
    level: i32,
    option: i32,
    value: &[u8],
) -> Result<()> {
    #[cfg(unix)]
    {
        setsockopt_raw(listener.as_borrowed_fd(), level, option, value)
    }
    // Validated by Windows CI; mirrors the unix arm.
    #[cfg(windows)]
    {
        setsockopt_raw(listener.as_borrowed_socket(), level, option, value)
    }
    #[cfg(not(any(unix, windows)))]
    {
        let _ = (listener, level, option, value);
        Err(crate::error::KjIoError::other(
            "setsockopt",
            "not implemented by kj-rs-io on this platform",
        ))
    }
}

// ======================================================================================
// Typed read/write halves of a pumped `kj::AsyncIoStream`.
//
// kj's stream contract — at most one read and one write may be in flight at once — is prose in
// kj; these halves make the borrow checker enforce it. Each half's operations take `&mut self`,
// so an in-flight operation's future exclusively borrows its half (a second overlapping read is
// a compile error), and `split_kj_stream` takes the owner's `&mut`, so while the halves live
// nothing else (an unwrap, another split) can touch the stream. The bridged operations behind
// them are not re-exported: the halves are the only way to drive a foreign stream.

/// The read direction of a pumped stream. See the module comment above.
pub struct KjStreamReadHalf<'a>(&'a KjAsyncIoStream);

/// The write direction of a pumped stream (writes and the write-side shutdown). See the module
/// comment above.
pub struct KjStreamWriteHalf<'a>(&'a KjAsyncIoStream);

/// Splits the owned stream into its two directions. Holding the owner's `&mut` for the halves'
/// lifetime proves exactly one pair exists and reserves the stream for them.
// The unused `&mut` is the point (see the doc comment): it reserves the stream for the halves.
#[expect(clippy::needless_pass_by_ref_mut)]
pub fn split_kj_stream(
    stream: &mut KjOwn<KjAsyncIoStream>,
) -> (KjStreamReadHalf<'_>, KjStreamWriteHalf<'_>) {
    let stream = &**stream;
    (KjStreamReadHalf(stream), KjStreamWriteHalf(stream))
}

impl KjStreamReadHalf<'_> {
    /// `kj::AsyncIoStream::tryRead(buffer, min_bytes, buffer.len())`.
    // The unused `&mut self` is the point: an in-flight read's future exclusively borrows the
    // read half (kj's one-read-in-flight contract), see the section comment above.
    #[expect(clippy::needless_pass_by_ref_mut)]
    pub(crate) async fn try_read(
        &mut self,
        buf: &mut [u8],
        min_bytes: usize,
    ) -> std::result::Result<usize, KjException> {
        bridge::kj_stream_try_read(self.0, buf, min_bytes).await
    }
}

impl KjStreamWriteHalf<'_> {
    /// `kj::AsyncIoStream::write(buffer)` (write-all semantics).
    // The unused `&mut self` is the point: an in-flight write's future exclusively borrows the
    // write half (kj's one-write-in-flight contract), see the section comment above.
    #[expect(clippy::needless_pass_by_ref_mut)]
    pub(crate) async fn write(&mut self, buf: &[u8]) -> std::result::Result<(), KjException> {
        bridge::kj_stream_write(self.0, buf).await
    }

    /// `kj::AsyncIoStream::shutdownWrite()`.
    // The unused `&mut self` is the point: an exclusive borrow of the write half serializes
    // write-side operations (kj's one-write-in-flight contract), see the section comment above.
    #[expect(clippy::needless_pass_by_ref_mut)]
    pub(crate) fn shutdown_write(&mut self) -> std::result::Result<(), KjException> {
        bridge::kj_stream_shutdown_write(self.0)
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

    #[test]
    #[should_panic(expected = "invalid fd crossed the FFI bridge")]
    fn own_fd_from_raw_rejects_negative_fds() {
        // -1 is OwnedFd's niche: turning it into an OwnedFd would be library UB, so the single
        // conversion point must refuse it loudly (a panic here becomes a kj::Exception).
        let _ = own_fd_from_raw(-1);
    }

    #[cfg(unix)]
    #[test]
    #[should_panic(expected = "invalid socket fd crossed the FFI bridge")]
    fn own_socket_from_raw_rejects_negative_handles() {
        let _ = own_socket_from_raw(-1);
    }

    #[cfg(unix)]
    #[test]
    #[should_panic(expected = "invalid socket fd crossed the FFI bridge")]
    fn own_socket_from_raw_rejects_handles_that_do_not_fit_an_fd() {
        let _ = own_socket_from_raw(i64::from(i32::MAX) + 1);
    }

    /// The happy path of THE conversion point: an fd released by std becomes a socket2 socket
    /// that owns it (closes it on drop) and is fully usable.
    #[cfg(unix)]
    #[test]
    fn own_socket_from_raw_takes_ownership_of_a_live_socket() {
        use std::os::fd::AsRawFd;
        use std::os::fd::IntoRawFd;
        let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let port = listener.local_addr().unwrap().port();
        let raw = listener.into_raw_fd();
        let socket = own_socket_from_raw(i64::from(raw));
        assert_eq!(socket.as_raw_fd(), raw);
        assert_eq!(
            socket.local_addr().unwrap().as_socket().unwrap().port(),
            port
        );
        // `socket` is the sole owner: dropping it closes the fd (socket2::Socket's drop glue).
    }
}
