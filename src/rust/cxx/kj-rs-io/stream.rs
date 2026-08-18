//! Tokio-backed byte streams behind KJ's stream interfaces.
//!
//! `TokioStream` (TCP or Unix domain) implements the `kj::AsyncIoStream` operations; all I/O
//! uses tokio's `&self` readiness API (`ready()` + `try_read`/`try_write`), which supports
//! concurrent reads and writes on one stream and is cancel-safe: dropping a pending future
//! (i.e. dropping the wrapping `kj::Promise`) merely deregisters the waker, releasing the
//! readiness interest so the stream can be reused or dropped sanely.

use std::io::Read;
use std::io::Write;

use tokio::io::Interest;
use tokio::net::TcpStream;
#[cfg(unix)]
use tokio::net::UnixStream;

use crate::error::KjIoError;
use crate::error::Result;
use crate::error::op;
use crate::runtime::with_runtime;

/// A native tokio stream (the "unwrap fast path" object).
///
/// C++ holds one of these inside every kj-rs-io `kj::AsyncIoStream`; Rust code can take it
/// back out via [`crate::unwrap_kj_stream`] and drive the connection natively.
pub struct TokioStream {
    /// `None` after the native stream has been moved out by [`TokioStream::take`] (the C++
    /// wrapper is then "hollow" and every operation fails).
    inner: Option<Inner>,
}

enum Inner {
    Tcp(TcpStream),
    #[cfg(unix)]
    Unix(UnixStream),
}
