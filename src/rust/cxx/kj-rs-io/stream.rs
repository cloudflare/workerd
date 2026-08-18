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

impl TokioStream {
    async fn try_read_min(&self, buf: &mut [u8], min_bytes: usize) -> Result<usize> {
        self.inner()?;
        let min_bytes = min_bytes.min(buf.len());
        let mut total = 0;
        while total < min_bytes {
            match self.try_read(&mut buf[total..]) {
                Ok(0) => break, // EOF: return what we have (< min_bytes signals EOF to KJ).
                Ok(n) => total += n,
                Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                    self.ready(Interest::READABLE).await?;
                }
                Err(e) if e.kind() == std::io::ErrorKind::Interrupted => {}
                Err(e) => return Err(op("read()")(e)),
            }
        }
        Ok(total)
    }

    /// Write-all semantics.
    async fn write_all(&self, buf: &[u8]) -> Result<()> {
        self.inner()?;
        let mut written = 0;
        while written < buf.len() {
            match self.try_write(&buf[written..]) {
                Ok(0) => {
                    // try_write on a socket signals "would block" via Err(WouldBlock), so a
                    // zero-byte result for a non-empty buffer means the connection is gone.
                    return Err(KjIoError::other(
                        "write()",
                        "wrote zero bytes (connection closed)",
                    ));
                }
                Ok(n) => written += n,
                Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                    self.ready(Interest::WRITABLE).await?;
                }
                Err(e) if e.kind() == std::io::ErrorKind::Interrupted => {}
                Err(e) => return Err(op("write()")(e)),
            }
        }
        Ok(())
    }
}

impl TokioStream {
    /// Resolves once new writes are doomed to fail (peer reset / hangup observed).
    ///
    /// tokio has no direct primitive for this, so we register a *duplicate* of the socket fd
    /// with the I/O driver for WRITABLE interest and wait — explicitly clearing plain-writable
    /// readiness — until the OS reports write-closed (kqueue: `EV_EOF` on the write filter;
    /// epoll: `EPOLLHUP`/`EPOLLERR`) or an error. Like KJ's own implementation this does *not*
    /// fire on a mere half-close (peer FIN / `EPOLLRDHUP`): reads hitting EOF must not count as
    /// write-disconnect.
    ///
    /// Windows behavior (see the arm below): a never-resolving future, which IS KJ
    /// parity — KJ's *current* Windows behavior (`whenWriteDisconnected` returns `NEVER_DONE`).
    /// Win32 has no documented primitive for detecting disconnect without a read/write; KJ's own
    /// TODO points at the undocumented-but-stable `IOCTL_AFD_POLL` ioctl (capnproto
    /// `async-io-win32.c++:289` — the mechanism `select()` itself is built on). An AFD-poll
    /// implementation remains an optional upgrade over this documented parity behavior.
    #[cfg(unix)]
    async fn when_write_disconnected(&self) -> Result<()> {
        use std::os::fd::AsRawFd;

        // Borrow the live socket's fd directly (tokio streams implement `AsFd`) and dup it, so
        // no raw fd is ever materialized without an owner.
        let borrowed = self.as_borrowed_fd()?;
        let owned = borrowed.try_clone_to_owned().map_err(op("dup()"))?;
        debug_assert_ne!(owned.as_raw_fd(), borrowed.as_raw_fd());
        // The dup shares the underlying open socket (and its O_NONBLOCK status), but has its own
        // registration with the I/O driver, so clearing readiness here never disturbs reads or
        // writes on the primary registration.
        let async_fd = tokio::io::unix::AsyncFd::with_interest(owned, Interest::WRITABLE)
            .map_err(op("whenWriteDisconnected"))?;
        loop {
            let mut guard = async_fd
                .ready(Interest::WRITABLE)
                .await
                .map_err(op("whenWriteDisconnected"))?;
            let ready = guard.ready();
            if ready.is_write_closed() || ready.is_error() {
                return Ok(());
            }
            // Plain "writable": clear it so the next wait sleeps until an actual state-change
            // event (edge-triggered), rather than spinning on an always-writable socket.
            guard.clear_ready();
        }
    }

    /// Never resolves: KJ parity, not a gap — capnproto's win32 `whenWriteDisconnected` returns
    /// `NEVER_DONE` today (its `IOCTL_AFD_POLL` idea is only a TODO; see the Windows-behavior note
    /// on the unix arm above). Validated by Windows CI.
    #[cfg(windows)]
    async fn when_write_disconnected(&self) -> Result<()> {
        self.inner()?;
        std::future::pending::<()>().await;
        unreachable!()
    }

    #[cfg(not(any(unix, windows)))]
    async fn when_write_disconnected(&self) -> Result<()> {
        self.inner()?;
        Err(KjIoError::other(
            "whenWriteDisconnected",
            "not implemented by kj-rs-io on this platform",
        ))
    }
}

impl TokioStream {
    fn shutdown_write(&self) -> Result<()> {
        #[cfg(unix)]
        {
            let dup = self
                .as_borrowed_fd()?
                .try_clone_to_owned()
                .map_err(op("dup()"))?;
            // shutdown() acts on the socket itself, so performing it through a dup'd fd
            // affects the shared socket, and dropping the dup only closes the duplicate.
            let result = match self.inner()? {
                Inner::Tcp(_) => std::net::TcpStream::from(dup).shutdown(std::net::Shutdown::Write),
                Inner::Unix(_) => {
                    std::os::unix::net::UnixStream::from(dup).shutdown(std::net::Shutdown::Write)
                }
            };
            result.map_err(op("shutdown(SHUT_WR)"))
        }
        // Validated by Windows CI; mirrors the unix arm. No dup: `with_sock_ref` borrows the
        // live socket (`SockRef`), and winsock `shutdown` acts on the underlying socket either
        // way — the unix arm dups only because std's `shutdown` is a method on owning types,
        // whereas the windows `BorrowedSocket::try_clone_to_owned` equivalent
        // (WSADuplicateSocketW) would be strictly heavier than the borrow.
        #[cfg(windows)]
        {
            self.with_sock_ref("shutdown(SD_SEND)", |sock| {
                sock.shutdown(std::net::Shutdown::Write)
            })
        }
        #[cfg(not(any(unix, windows)))]
        {
            Err(KjIoError::other(
                "shutdownWrite",
                "not implemented on this platform",
            ))
        }
    }
}

impl TokioStream {
    /// Borrows the live tokio socket's fd (tokio streams implement `AsFd`), for dup-based
    /// operations that must not conjure a raw fd out of an integer. Errs if the wrapper is
    /// hollow.
    #[cfg(unix)]
    pub(crate) fn as_borrowed_fd(&self) -> Result<std::os::fd::BorrowedFd<'_>> {
        use std::os::fd::AsFd;
        Ok(match self.inner()? {
            Inner::Tcp(s) => s.as_fd(),
            Inner::Unix(s) => s.as_fd(),
        })
    }

    /// Borrows the live tokio socket's `SOCKET` (tokio's `TcpStream` implements `AsSocket`):
    /// the Windows counterpart of [`TokioStream::as_borrowed_fd`]. Errs if the wrapper is
    /// hollow. On Windows only the Tcp variant of `Inner` exists.
    // Validated by Windows CI; mirrors the unix arm.
    #[cfg(windows)]
    pub(crate) fn as_borrowed_socket(&self) -> Result<std::os::windows::io::BorrowedSocket<'_>> {
        use std::os::windows::io::AsSocket;
        Ok(match self.inner()? {
            Inner::Tcp(s) => s.as_socket(),
        })
    }

    /// Runs `f` on a `socket2::SockRef` borrowing the live socket — the shared body of the
    /// `getsockname()`/`getpeername()` passthroughs; only the socket borrow is per-platform.
    fn with_sock_ref<T>(
        &self,
        op_name: &'static str,
        f: impl FnOnce(&socket2::SockRef<'_>) -> std::io::Result<T>,
    ) -> Result<T> {
        #[cfg(unix)]
        {
            let fd = self.as_borrowed_fd()?;
            f(&socket2::SockRef::from(&fd)).map_err(op(op_name))
        }
        // Validated by Windows CI; mirrors the unix arm.
        #[cfg(windows)]
        {
            let sock = self.as_borrowed_socket()?;
            f(&socket2::SockRef::from(&sock)).map_err(op(op_name))
        }
        #[cfg(not(any(unix, windows)))]
        {
            let _ = f;
            self.inner()?;
            Err(KjIoError::other(
                op_name,
                "not implemented by kj-rs-io on this platform",
            ))
        }
    }
}

impl TokioStream {
    /// Raw `struct sockaddr` bytes of the socket's locally-bound address (the `getsockname()`
    /// passthrough behind `kj::AsyncIoStream::getsockname`).
    fn local_addr_bytes(&self) -> Result<Vec<u8>> {
        let addr = self.with_sock_ref("getsockname()", |sock| sock.local_addr())?;
        Ok(crate::ffi::sockaddr_to_bytes(&addr))
    }

    /// Raw `struct sockaddr` bytes of the connected peer's address (the `getpeername()`
    /// passthrough behind `kj::AsyncIoStream::getpeername` and the accept-loop peer-filter
    /// check).
    fn peer_addr_bytes(&self) -> Result<Vec<u8>> {
        let addr = self.with_sock_ref("getpeername()", |sock| sock.peer_addr())?;
        Ok(crate::ffi::sockaddr_to_bytes(&addr))
    }

    /// The underlying raw OS socket handle, widened to `i64`: a Unix fd
    /// (`kj::AsyncIoStream::getFd()`) or a win32 `SOCKET` (`getWin32Handle()`).
    fn raw_handle(&self) -> Result<i64> {
        #[cfg(unix)]
        {
            use std::os::fd::AsRawFd;
            Ok(i64::from(match self.inner()? {
                Inner::Tcp(s) => s.as_raw_fd(),
                Inner::Unix(s) => s.as_raw_fd(),
            }))
        }
        // Validated by Windows CI; mirrors the unix arm.
        #[cfg(windows)]
        {
            use std::os::windows::io::AsRawSocket;
            let raw = match self.inner()? {
                Inner::Tcp(s) => s.as_raw_socket(),
            };
            // A live SOCKET fits in i64 (Windows handles fit in 32 bits); the bridge carries
            // its bits verbatim.
            #[allow(clippy::cast_possible_wrap)]
            Ok(raw as i64)
        }
        #[cfg(not(any(unix, windows)))]
        {
            self.inner()?;
            Err(KjIoError::other(
                "getFd",
                "file descriptors are not available on this platform",
            ))
        }
    }
}

impl TokioStream {
    /// Recovers whichever native tokio object this is, as a [`crate::serve::ServeIo`]
    /// (the unwrap fast path of [`crate::serve_kj_stream`]). `None` if hollow.
    pub(crate) fn into_serve_io(self) -> Option<crate::serve::ServeIo> {
        match self.inner? {
            Inner::Tcp(stream) => Some(crate::serve::ServeIo::Tcp(stream)),
            #[cfg(unix)]
            Inner::Unix(stream) => Some(crate::serve::ServeIo::Unix(stream)),
        }
    }

    fn take(&mut self) -> Result<Box<Self>> {
        let inner = self.inner.take().ok_or_else(|| {
            KjIoError::other("kj_rs_io", "stream was already unwrapped (hollow wrapper)")
        })?;
        Ok(Box::new(Self { inner: Some(inner) }))
    }
}
