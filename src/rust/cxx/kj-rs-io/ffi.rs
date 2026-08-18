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
