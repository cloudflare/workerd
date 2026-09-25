//! The FFI island of kj-rs-io: the `#[cxx::bridge]` plus every hand-written `unsafe` in the
//! crate, in one file (file-top `#![allow(unsafe_code)]`; the crate root denies it).
//!
//! Two kinds of raw thing cross the bridge and are converted into typed Rust here, at the entry
//! point that receives them, before any other module sees them:
//!
//! 1. **Raw OS socket handles** arriving from C++ as an `i64` (a Unix fd or a win32 `SOCKET`).
//!    The conversions ([`prepare_fd`] / [`prepare_socket`], [`own_fd_from_raw`]) are `unsafe fn`s:
//!    an integer cannot prove that a descriptor is open or that its ownership was transferred.
//!    The bridge entry points that receive one are `unsafe fn`s too, so the requirement is
//!    visible in Rust signatures, not only in the bridge declaration; each discharges it in one
//!    `unsafe` block naming the C++ contract it relies on. Behind those entry points everything
//!    is a `socket2::Socket`, an `OwnedFd` or a tokio type.
//! 2. **Caller-owned output storage** for `tryRead`: KJ lets callers pass uninitialized memory,
//!    so it arrives as a raw pointer + length (never as `&mut [u8]`, whose elements must be
//!    initialized) and becomes `&mut [MaybeUninit<u8>]` -- see [`uninit_slice`]. Those entry
//!    points are `unsafe fn`s as well (pointer validity is a precondition).
//!
//! Socket addresses are not a third kind: they cross as the typed [`SocketAddress`] struct
//! below in both directions, so no `struct sockaddr` bytes are viewed or built on the Rust side
//! (net.rs, "Typed addresses"). The only other `unsafe` here is the process-wide `SIGPIPE`
//! disposition ([`ignore_sigpipe_once`], via nix).
//!
//! No bridged future borrows the Rust object it was called on: every opaque type is a handle to
//! `Arc`-shared state and its operations own a share (lib.rs, "Ownership of in-flight
//! operations"), so the bridge declarations below carry no lifetime on the object parameters --
//! the `impl Future + use<..>` return types prove it.
#![allow(unsafe_code)]

use core::future::Future;
use core::mem::MaybeUninit;

pub use bridge::AddressKind;
pub use bridge::KjPieces;
pub use bridge::PeerCredentials;
pub use bridge::PeerStream;
pub use bridge::ReceivedDatagram;
pub use bridge::SocketAddress;
pub use bridge::kj_piece;
pub use bridge::kj_pieces_count;

use crate::error::KjIoError;
use crate::error::Result;
use crate::error::op;
use crate::loopback::LoopbackRegistry;
use crate::net::TokioAddress;
use crate::net::TokioDatagram;
use crate::net::TokioListener;
use crate::net::address_bind_datagram;
use crate::net::address_clone;
use crate::net::address_listen;
use crate::net::address_targets;
use crate::net::address_to_string;
use crate::net::connect_target;
use crate::net::datagram_port;
use crate::net::datagram_receive;
use crate::net::datagram_send;
use crate::net::listener_accept;
use crate::net::listener_clone;
use crate::net::listener_local_addr;
use crate::net::listener_port;
use crate::net::network_address_from;
use crate::net::parse_address;
use crate::net::socket_pair;
use crate::signal::wait_for_signal;
use crate::stream::TokioStream;
use crate::stream::stream_abort_read;
use crate::stream::stream_local_addr;
use crate::stream::stream_peer_addr;
use crate::stream::stream_peer_credentials;
use crate::stream::stream_raw_handle;
use crate::stream::stream_shutdown_write;
use crate::stream::stream_when_write_disconnected;
use crate::stream::stream_write;
use crate::stream::stream_write_pieces;
use crate::watcher::TokioFileWatcher;
use crate::watcher::file_watcher_on_change;
use crate::watcher::file_watcher_watch;
use crate::watcher::new_file_watcher;

#[cxx::bridge(namespace = "kj_rs_io")]
#[expect(clippy::allow_attributes)]
#[allow(clippy::missing_safety_doc)]
#[allow(clippy::unnecessary_box_returns)]
mod bridge {
    /// The family of a [`SocketAddress`], and which of its fields are meaningful.
    #[derive(Clone, Copy, PartialEq, Eq, Debug)]
    enum AddressKind {
        /// `ip[..4]`, `port`.
        Ipv4,
        /// `ip`, `port`, `flowinfo`, `scope_id`.
        Ipv6,
        /// `name`: the socket path, as bytes, no terminating NUL.
        UnixPath,
        /// `name`: a Linux abstract-namespace name, without its leading NUL.
        UnixAbstract,
        /// No fields: `accept(2)` on a unix socket reports this for an unbound peer.
        UnixUnnamed,
        /// `name`: a `loopback:` name (loopback.rs). Not a socket address: it has no `struct
        /// sockaddr` form and is not subject to `restrictPeers()`.
        Loopback,
    }

    /// One socket address, typed. This is the only form an address takes on the bridge: Rust
    /// produces it from tokio's / std's address types (a safe conversion) and reads it back into
    /// them; C++ builds it from a caller's `struct sockaddr` and encodes it into one only at the
    /// two KJ interfaces that speak raw sockaddrs (`getSockaddr`, `getsockname`/`getpeername`)
    /// and for KJ's own `NetworkFilter`. Nothing on either side memcpy's an address through.
    #[derive(Clone, PartialEq, Eq, Debug)]
    struct SocketAddress {
        kind: AddressKind,
        /// Network byte order; the first 4 bytes for `Ipv4`, all 16 for `Ipv6`.
        ip: [u8; 16],
        /// Host byte order.
        port: u16,
        flowinfo: u32,
        scope_id: u32,
        name: Vec<u8>,
    }

    /// A connected stream plus the peer it is connected to: what `accept(2)` reported, or the
    /// address `connect(2)` was given. The C++ adapter applies `restrictPeers()` to `peer` and
    /// builds the `kj::PeerIdentity` from it.
    struct PeerStream {
        stream: Box<TokioStream>,
        peer: SocketAddress,
    }

    struct ReceivedDatagram {
        data: Vec<u8>,
        source: SocketAddress,
        truncated: bool,
    }

    /// `kj::LocalPeerIdentity::Credentials`: `pid` / `uid` of a unix-socket peer, each with a
    /// validity flag (kj::Maybe has no cxx mapping).
    struct PeerCredentials {
        has_pid: bool,
        pid: i32,
        has_uid: bool,
        uid: u32,
    }

    struct SocketPair {
        first: Box<TokioStream>,
        second: Box<TokioStream>,
    }

    extern "Rust" {
        type TokioStream;
        type TokioListener;
        type TokioAddress;
        type TokioDatagram;
        type LoopbackRegistry;

        // --- kj::AsyncIoStream (stream.rs). `buf` is the caller's, uninitialized storage
        // allowed, valid until the promise settles (KJ's contract): hence `unsafe`.
        async unsafe fn stream_try_read(
            stream: &TokioStream,
            buf: *mut u8,
            len: usize,
            min_bytes: usize,
        ) -> Result<usize>;
        async unsafe fn stream_write<'a>(stream: &'a TokioStream, buf: &'a [u8]) -> Result<()>;
        async unsafe fn stream_write_pieces<'a>(
            stream: &'a TokioStream,
            pieces: &'a KjPieces,
        ) -> Result<()>;
        async fn stream_when_write_disconnected(stream: &TokioStream) -> Result<()>;
        fn stream_shutdown_write(stream: &TokioStream) -> Result<()>;
        fn stream_abort_read(stream: &TokioStream) -> Result<()>;
        fn stream_local_addr(stream: &TokioStream) -> Result<SocketAddress>;
        fn stream_peer_addr(stream: &TokioStream) -> Result<SocketAddress>;
        fn stream_peer_credentials(stream: &TokioStream) -> Result<PeerCredentials>;
        fn stream_raw_handle(stream: &TokioStream) -> i64;
        fn new_socket_pair() -> Result<SocketPair>;

        // --- kj::Network / kj::NetworkAddress (net.rs). Peer filtering is the C++ adapter's:
        // `address_targets` lists what connect() would try, in order, for it to filter and
        // connect one at a time; `listener_accept` reports the peer for it to judge. Every
        // kj::Network holds a LoopbackRegistry (loopback.rs), the namespace `loopback:`
        // addresses resolve in once enabled; restrictPeers() children share their parent's.
        fn new_loopback_registry() -> Box<LoopbackRegistry>;
        fn loopback_registry_clone(registry: &LoopbackRegistry) -> Box<LoopbackRegistry>;
        fn loopback_registry_enable(registry: &LoopbackRegistry);
        async fn network_parse_address(
            addr: &[u8],
            port_hint: u16,
            loopback: &LoopbackRegistry,
        ) -> Result<Box<TokioAddress>>;
        fn network_address_from(addr: &SocketAddress) -> Result<Box<TokioAddress>>;
        fn address_targets(addr: &TokioAddress) -> Result<Vec<SocketAddress>>;
        async fn connect_target(
            addr: &TokioAddress,
            target: SocketAddress,
        ) -> Result<Box<TokioStream>>;
        fn address_listen(addr: &TokioAddress) -> Result<Box<TokioListener>>;
        fn address_bind_datagram(addr: &TokioAddress) -> Result<Box<TokioDatagram>>;
        fn address_clone(addr: &TokioAddress) -> Box<TokioAddress>;
        fn address_to_string(addr: &TokioAddress) -> Vec<u8>;
        async fn listener_accept(listener: &TokioListener) -> Result<PeerStream>;
        fn listener_clone(listener: &TokioListener) -> Box<TokioListener>;
        fn listener_port(listener: &TokioListener) -> Result<u16>;
        fn listener_local_addr(listener: &TokioListener) -> Result<SocketAddress>;
        async fn datagram_send(
            datagram: &TokioDatagram,
            data: &[u8],
            destination: SocketAddress,
        ) -> Result<usize>;
        async fn datagram_receive(
            datagram: &TokioDatagram,
            capacity: usize,
        ) -> Result<ReceivedDatagram>;
        fn datagram_port(datagram: &TokioDatagram) -> Result<u16>;

        // --- kj::LowLevelAsyncIoProvider. `handle` is an open fd / SOCKET, owned by the callee
        // under TAKE_OWNERSHIP (KJ's contract): hence `unsafe`.
        unsafe fn wrap_socket_fd(handle: i64, flags: u32) -> Result<Box<TokioStream>>;
        unsafe fn wrap_listen_fd(handle: i64, flags: u32) -> Result<Box<TokioListener>>;

        fn ignore_sigpipe_once() -> Result<()>;
        async fn wait_for_signal(signum: i32) -> Result<()>;

        // --- The --watch file watcher (watcher.rs). `path` is `kj::Path::toNativeString`
        // output: arbitrary bytes on unix, UTF-8 on Windows.
        type TokioFileWatcher;
        fn new_file_watcher() -> Result<Box<TokioFileWatcher>>;
        fn file_watcher_watch(watcher: &TokioFileWatcher, path: &[u8]) -> Result<()>;
        async fn file_watcher_on_change(watcher: &TokioFileWatcher) -> Result<()>;
    }

    unsafe extern "C++" {
        include!("kj-rs-io/bridge.h");

        /// The pieces of a `kj::AsyncOutputStream::write(pieces)` call (bridge.h), read through
        /// the two accessors; owned by the C++ coroutine frame awaiting the write.
        type KjPieces;
        #[cxx_name = "kjPiecesCount"]
        fn kj_pieces_count(pieces: &KjPieces) -> usize;
        #[cxx_name = "kjPiece"]
        fn kj_piece<'a>(pieces: &'a KjPieces, index: usize) -> Result<&'a [u8]>;
    }
}

/// Views `len` bytes of caller-owned storage at `ptr` as `&mut [MaybeUninit<u8>]`: the honest
/// type for KJ read buffers, which callers routinely declare uninitialized (`kj::byte buf[16];`).
/// A `&mut [u8]` here would assert initialized contents the storage does not have.
///
/// # Safety
///
/// `ptr` must be valid for reads and writes of `len` bytes for the whole lifetime `'a` the
/// caller chooses, and nothing else may access that memory meanwhile. The bridge entry points
/// below pick `'a` as the lifetime of the future they return, which is exactly the validity
/// KJ's `tryRead` contract grants a buffer: until the returned promise settles.
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

/// Copies the address text before the future exists (KJ's `parseAddress` only guarantees the
/// caller's buffer for the duration of the call).
pub fn network_parse_address(
    addr: &[u8],
    port_hint: u16,
    loopback: &LoopbackRegistry,
) -> impl Future<Output = Result<Box<TokioAddress>>> + use<> {
    let addr = addr.to_vec();
    let loopback = loopback.clone_handle();
    async move { parse_address(&addr, port_hint, &loopback).await }
}

#[expect(clippy::unnecessary_box_returns)]
pub fn new_loopback_registry() -> Box<LoopbackRegistry> {
    Box::new(LoopbackRegistry::new())
}

#[expect(clippy::unnecessary_box_returns)]
pub fn loopback_registry_clone(registry: &LoopbackRegistry) -> Box<LoopbackRegistry> {
    Box::new(registry.clone_handle())
}

pub fn loopback_registry_enable(registry: &LoopbackRegistry) {
    registry.enable();
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
unsafe fn own_fd_from_raw(fd: i32) -> Result<std::os::fd::OwnedFd> {
    use std::os::fd::FromRawFd;
    // `OwnedFd`'s invariant is "an open fd, never -1" (-1 is its niche). The all-flags-set path
    // performs no syscall that would catch a bad fd, so enforce the numeric half of the contract
    // here, as an error (the crate's no-panic policy), instead of inheriting library UB.
    if fd < 0 {
        return Err(invalid_handle(i64::from(fd)));
    }
    // Safety: per this function's contract `fd` is open and owned by us from this point on.
    Ok(unsafe { std::os::fd::OwnedFd::from_raw_fd(fd) })
}

/// A negative (or, on unix, fd-range-exceeding) handle crossed the bridge: KJ's provider hands
/// out live handles only, so this is a caller bug, reported as a `kj::Exception`.
fn invalid_handle(handle: i64) -> KjIoError {
    KjIoError::other(
        "wrapFd",
        format!("invalid socket handle crossed the FFI bridge: {handle}"),
    )
}

/// Takes ownership of an open winsock `SOCKET` that arrived across the bridge as an `i64`
/// (`u64`-shaped `RawSocket`; a live SOCKET fits in an `i64` without colliding with the -1
/// sentinel, which is `INVALID_SOCKET` and never crosses the bridge as an owned handle).
///
/// # Safety
///
/// `handle` must be an open `SOCKET` whose ownership is transferred to the caller.
#[cfg(windows)]
unsafe fn own_socket_from_raw(handle: i64) -> Result<socket2::Socket> {
    use std::os::windows::io::FromRawSocket;
    use std::os::windows::io::OwnedSocket;
    use std::os::windows::io::RawSocket;
    if handle < 0 {
        return Err(invalid_handle(handle));
    }
    // The bridge carries the SOCKET's bits verbatim.
    #[allow(clippy::cast_sign_loss)]
    let raw = handle as RawSocket;
    // Safety: per this function's contract `handle` is an open SOCKET owned by us from now on.
    let owned = unsafe { OwnedSocket::from_raw_socket(raw) };
    Ok(socket2::Socket::from(owned))
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
    use nix::fcntl::FcntlArg;
    use nix::fcntl::FdFlag;
    use nix::fcntl::fcntl;
    let mut flags = FdFlag::from_bits_retain(
        fcntl(fd, FcntlArg::F_GETFD).map_err(nix_error("fcntl(F_GETFD)"))?,
    );
    if !flags.contains(FdFlag::FD_CLOEXEC) {
        flags.insert(FdFlag::FD_CLOEXEC);
        fcntl(fd, FcntlArg::F_SETFD(flags)).map_err(nix_error("fcntl(F_SETFD)"))?;
    }
    Ok(())
}

/// Sets `O_NONBLOCK` if it is not set already (observed through every duplicate of the open
/// file description, as with KJ).
#[cfg(unix)]
fn set_nonblocking(fd: std::os::fd::BorrowedFd<'_>) -> Result<()> {
    use nix::fcntl::FcntlArg;
    use nix::fcntl::OFlag;
    use nix::fcntl::fcntl;
    let mut flags =
        OFlag::from_bits_retain(fcntl(fd, FcntlArg::F_GETFL).map_err(nix_error("fcntl(F_GETFL)"))?);
    if !flags.contains(OFlag::O_NONBLOCK) {
        flags.insert(OFlag::O_NONBLOCK);
        fcntl(fd, FcntlArg::F_SETFL(flags)).map_err(nix_error("fcntl(F_SETFL)"))?;
    }
    Ok(())
}

#[cfg(unix)]
fn nix_error(op_name: &'static str) -> impl Fn(nix::Error) -> KjIoError {
    move |errno| op(op_name)(std::io::Error::from(errno))
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
        if fd < 0 {
            return Err(invalid_handle(i64::from(fd)));
        }
        // Safety: forwarded from this function's contract (open for the duration of the call);
        // the borrow ends when the dup returns.
        unsafe { std::os::fd::BorrowedFd::borrow_raw(fd) }
            .try_clone_to_owned()
            .map_err(op("fcntl(F_DUPFD_CLOEXEC)"))?
    } else {
        // Safety: forwarded from this function's contract (ownership transferred).
        let owned = unsafe { own_fd_from_raw(fd) }?;
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
    let fd = i32::try_from(handle)
        .ok()
        .filter(|fd| *fd >= 0)
        .ok_or_else(|| invalid_handle(handle))?;
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
    if handle < 0 {
        return Err(invalid_handle(handle));
    }
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
        unsafe { own_socket_from_raw(handle) }?
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

// ======================================================================================
// `struct sockaddr` <-> bytes.

// KJ parity (`kj::UnixEventPort`'s constructor, kj/async-unix.c++): "We disable SIGPIPE
/// because users of `UnixEventPort` almost certainly don't want it." A write to a peer that has
/// gone away then fails with `EPIPE` -- a DISCONNECTED `kj::Exception` -- instead of terminating
/// the process. Once per process; the C++ runtime installs no disposition for us, and neither
/// does linking Rust into a C++ executable (Rust's own `main` shim, which would ignore SIGPIPE,
/// never runs here).
#[cfg(unix)]
pub fn ignore_sigpipe_once() -> Result<()> {
    use std::sync::OnceLock;
    static IGNORED: OnceLock<std::result::Result<(), nix::Error>> = OnceLock::new();
    let outcome: std::result::Result<(), nix::Error> = *IGNORED.get_or_init(|| {
        // Safety: SIG_IGN installs no handler function, so no Rust code runs in signal
        // context; changing the process-wide disposition is the documented intent.
        unsafe {
            nix::sys::signal::signal(
                nix::sys::signal::SIGPIPE,
                nix::sys::signal::SigHandler::SigIgn,
            )
        }
        .map(|_previous| ())
    });
    outcome.map_err(nix_error("signal(SIGPIPE, SIG_IGN)"))
}

/// Windows has no SIGPIPE; nothing to do.
#[cfg(windows)]
pub fn ignore_sigpipe_once() -> Result<()> {
    Ok(())
}

pub fn new_socket_pair() -> Result<bridge::SocketPair> {
    let (first, second) = socket_pair()?;
    Ok(bridge::SocketPair { first, second })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[cfg(unix)]
    #[test]
    fn own_fd_from_raw_rejects_negative_fds() {
        // -1 is OwnedFd's niche: turning it into an OwnedFd would be library UB, so the
        // conversion point must refuse it, as an error (the crate's no-panic policy).
        // Safety: rejected before constructing anything.
        assert!(unsafe { own_fd_from_raw(-1) }.is_err());
    }

    #[cfg(unix)]
    #[test]
    fn prepare_socket_rejects_negative_handles() {
        // Safety: rejected before constructing anything.
        assert!(unsafe { prepare_socket(-1, TAKE_OWNERSHIP) }.is_err());
    }

    #[cfg(unix)]
    #[test]
    fn prepare_socket_rejects_handles_that_do_not_fit_an_fd() {
        // Safety: rejected before constructing anything.
        assert!(unsafe { prepare_socket(i64::from(i32::MAX) + 1, TAKE_OWNERSHIP) }.is_err());
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
        let (c, d) = std::os::unix::net::UnixStream::pair().unwrap();
        let raw_c = c.into_raw_fd();
        // Safety: `into_raw_fd` transferred ownership.
        let owned = unsafe { prepare_fd(raw_c, TAKE_OWNERSHIP) }.unwrap();
        assert_eq!(owned.as_raw_fd(), raw_c);
        assert_ne!(fd_flags(raw_c).0 & libc::FD_CLOEXEC, 0);
        assert_ne!(fd_flags(raw_c).1 & libc::O_NONBLOCK, 0);
        drop(owned);
        // Observed through the peer, not by probing the released number (another thread could
        // reuse it): the pair's other end reads EOF once the owned end is closed.
        assert_eq!(
            std::io::Read::read(&mut &d, &mut [0u8; 1]).unwrap(),
            0,
            "owned: closed on drop"
        );
    }
}
