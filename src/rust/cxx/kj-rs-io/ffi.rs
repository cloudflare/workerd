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
use crate::readiness::wait_fd_readable;
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
use crate::stream::stream_try_read;
use crate::stream::stream_when_write_disconnected;
use crate::stream::stream_write;
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

        /// Moves the native stream out, leaving `stream` hollow (all further ops error).
        /// Unsafe contract: no I/O futures may currently borrow `stream`.
        fn stream_take(stream: &mut TokioStream) -> Result<Box<TokioStream>>;
    }

    extern "Rust" {
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
    }
}
