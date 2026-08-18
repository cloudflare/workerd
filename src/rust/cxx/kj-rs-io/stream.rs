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

impl TokioStream {
    pub fn from_tcp(stream: TcpStream) -> Self {
        Self {
            inner: Some(Inner::Tcp(stream)),
        }
    }

    #[cfg(unix)]
    #[must_use]
    pub fn from_unix(stream: UnixStream) -> Self {
        Self {
            inner: Some(Inner::Unix(stream)),
        }
    }

    /// Recovers the native tokio TCP stream, if this is a (non-hollow) TCP stream.
    #[must_use]
    pub fn into_tcp_stream(self) -> Option<TcpStream> {
        match self.inner {
            Some(Inner::Tcp(stream)) => Some(stream),
            _ => None,
        }
    }

    /// Recovers the native tokio Unix-domain stream, if this is one.
    #[cfg(unix)]
    #[must_use]
    pub fn into_unix_stream(self) -> Option<UnixStream> {
        match self.inner {
            Some(Inner::Unix(stream)) => Some(stream),
            _ => None,
        }
    }

    fn inner(&self) -> Result<&Inner> {
        self.inner
            .as_ref()
            .ok_or_else(|| KjIoError::other("kj_rs_io", "stream was unwrapped (hollow wrapper)"))
    }

    async fn ready(&self, interest: Interest) -> Result<()> {
        match self.inner()? {
            Inner::Tcp(s) => s.ready(interest).await,
            #[cfg(unix)]
            Inner::Unix(s) => s.ready(interest).await,
        }
        .map_err(op("poll()"))?;
        Ok(())
    }

    #[expect(
        clippy::expect_used,
        reason = "only called from try_read_min/write_all, which call self.inner()? first, so `inner` is Some here; None occurs only for a hollow (unwrapped) wrapper, which is never read"
    )]
    fn try_read(&self, buf: &mut [u8]) -> std::io::Result<usize> {
        match self.inner.as_ref().expect("checked by caller") {
            Inner::Tcp(s) => s.try_read(buf),
            #[cfg(unix)]
            Inner::Unix(s) => s.try_read(buf),
        }
    }

    #[expect(
        clippy::expect_used,
        reason = "only called from write_all, which calls self.inner()? first, so `inner` is Some here; None occurs only for a hollow (unwrapped) wrapper, which is never written"
    )]
    fn try_write(&self, buf: &[u8]) -> std::io::Result<usize> {
        match self.inner.as_ref().expect("checked by caller") {
            Inner::Tcp(s) => s.try_write(buf),
            #[cfg(unix)]
            Inner::Unix(s) => s.try_write(buf),
        }
    }
}
