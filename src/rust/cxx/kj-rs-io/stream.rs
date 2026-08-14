//! Tokio-backed byte streams behind KJ's stream interfaces.
//!
//! `TokioStream` (TCP or Unix domain) implements the `kj::AsyncIoStream` operations; all I/O
//! uses tokio's `&self` readiness API (`ready()` + `try_read`/`try_write`), which supports
//! concurrent reads and writes on one stream and is cancel-safe: dropping a pending future
//! (i.e. dropping the wrapping `kj::Promise`) merely deregisters the waker, releasing the
//! readiness interest so the stream can be reused or dropped sanely.

use std::cell::Ref;
use std::cell::RefCell;
use std::io::IoSlice;
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
///
/// # Ownership of the native stream while I/O is in flight
///
/// Every I/O operation borrows the native stream for its whole duration (across its awaits),
/// and [`TokioStream::take`] moves it out. Those two must never overlap -- the moved-out object
/// is exactly what the pending operation is using. Rather than making that a caller contract
/// ("no I/O promises may be in flight when unwrapping"), the `RefCell` makes it checked: each
/// operation holds a shared borrow (`Ref`) of the slot for as long as it runs, and `take()`
/// needs the exclusive borrow, so unwrapping with I/O in flight fails with an error instead of
/// aliasing live borrows. This is why `TokioStream` is `!Sync`: it is a KJ-loop-thread object
/// (its owner, the C++ `kj::AsyncIoStream` wrapper, is), never shared across threads.
pub struct TokioStream {
    /// `None` after the native stream has been moved out by [`TokioStream::take`] (the C++
    /// wrapper is then "hollow" and every operation fails).
    inner: RefCell<Option<Inner>>,
}

enum Inner {
    Tcp(TcpStream),
    #[cfg(unix)]
    Unix(UnixStream),
}

impl Inner {
    async fn ready(&self, interest: Interest) -> Result<()> {
        match self {
            Self::Tcp(s) => s.ready(interest).await,
            #[cfg(unix)]
            Self::Unix(s) => s.ready(interest).await,
        }
        .map_err(op("poll()"))?;
        Ok(())
    }

    fn try_read(&self, buf: &mut [u8]) -> std::io::Result<usize> {
        match self {
            Self::Tcp(s) => s.try_read(buf),
            #[cfg(unix)]
            Self::Unix(s) => s.try_read(buf),
        }
    }

    fn try_write(&self, buf: &[u8]) -> std::io::Result<usize> {
        match self {
            Self::Tcp(s) => s.try_write(buf),
            #[cfg(unix)]
            Self::Unix(s) => s.try_write(buf),
        }
    }

    fn try_write_vectored(&self, bufs: &[IoSlice<'_>]) -> std::io::Result<usize> {
        match self {
            Self::Tcp(s) => s.try_write_vectored(bufs),
            #[cfg(unix)]
            Self::Unix(s) => s.try_write_vectored(bufs),
        }
    }

    /// Borrows the live tokio socket's fd (tokio streams implement `AsFd`), for dup-based
    /// operations that must not conjure a raw fd out of an integer.
    #[cfg(unix)]
    fn as_borrowed_fd(&self) -> std::os::fd::BorrowedFd<'_> {
        use std::os::fd::AsFd;
        match self {
            Self::Tcp(s) => s.as_fd(),
            Self::Unix(s) => s.as_fd(),
        }
    }

    /// Borrows the live tokio socket's `SOCKET` (tokio's `TcpStream` implements `AsSocket`):
    /// the Windows counterpart of [`Inner::as_borrowed_fd`]. On Windows only the Tcp variant
    /// exists. Validated by Windows CI.
    #[cfg(windows)]
    fn as_borrowed_socket(&self) -> std::os::windows::io::BorrowedSocket<'_> {
        use std::os::windows::io::AsSocket;
        match self {
            Self::Tcp(s) => s.as_socket(),
        }
    }
}

fn hollow() -> KjIoError {
    KjIoError::other("kj_rs_io", "stream was unwrapped (hollow wrapper)")
}

impl TokioStream {
    fn new(inner: Inner) -> Self {
        Self {
            inner: RefCell::new(Some(inner)),
        }
    }

    #[must_use]
    pub fn from_tcp(stream: TcpStream) -> Self {
        Self::new(Inner::Tcp(stream))
    }

    #[cfg(unix)]
    #[must_use]
    pub fn from_unix(stream: UnixStream) -> Self {
        Self::new(Inner::Unix(stream))
    }

    /// Recovers the native tokio TCP stream, if this is a (non-hollow) TCP stream.
    #[must_use]
    pub fn into_tcp_stream(self) -> Option<TcpStream> {
        match self.inner.into_inner() {
            Some(Inner::Tcp(stream)) => Some(stream),
            _ => None,
        }
    }

    /// Recovers the native tokio Unix-domain stream, if this is one.
    #[cfg(unix)]
    #[must_use]
    pub fn into_unix_stream(self) -> Option<UnixStream> {
        match self.inner.into_inner() {
            Some(Inner::Unix(stream)) => Some(stream),
            _ => None,
        }
    }

    /// Borrows the live native stream for the duration of an operation. The returned `Ref` is
    /// what makes a concurrent [`TokioStream::take`] fail (see the type docs). Errors if the
    /// wrapper is hollow.
    fn live(&self) -> Result<Ref<'_, Inner>> {
        // `take()` is synchronous and never yields while holding the exclusive borrow, so a
        // failed shared borrow here cannot happen in practice; report it rather than panic.
        let slot = self
            .inner
            .try_borrow()
            .map_err(|_| KjIoError::other("kj_rs_io", "stream is being unwrapped concurrently"))?;
        Ref::filter_map(slot, Option::as_ref).map_err(|_| hollow())
    }

    /// Runs `f` on the live socket's borrowed fd. Errors if the wrapper is hollow.
    #[cfg(unix)]
    pub(crate) fn with_borrowed_fd<T>(
        &self,
        f: impl FnOnce(std::os::fd::BorrowedFd<'_>) -> T,
    ) -> Result<T> {
        let inner = self.live()?;
        Ok(f(inner.as_borrowed_fd()))
    }

    /// Runs `f` on the live socket's borrowed `SOCKET`. Errors if the wrapper is hollow.
    /// Validated by Windows CI; mirrors the unix arm.
    #[cfg(windows)]
    pub(crate) fn with_borrowed_socket<T>(
        &self,
        f: impl FnOnce(std::os::windows::io::BorrowedSocket<'_>) -> T,
    ) -> Result<T> {
        let inner = self.live()?;
        Ok(f(inner.as_borrowed_socket()))
    }

    /// KJ `tryRead` semantics: loop until at least `min_bytes` (or EOF), up to `buf.len()`.
    // Holding the `Ref` across awaits is the whole point (see the type docs): it reserves the
    // native stream for this operation so a concurrent `take()`/unwrap fails cleanly. Sound
    // here: the only conflicting accessor is `take()`, which uses `try_borrow_mut` and returns
    // an error rather than panicking, and concurrent read+write just stack shared `Ref`s.
    #[expect(
        clippy::await_holding_refcell_ref,
        reason = "intentional in-flight guard; see above"
    )]
    async fn try_read_min(&self, buf: &mut [u8], min_bytes: usize) -> Result<usize> {
        let inner = self.live()?;
        let min_bytes = min_bytes.min(buf.len());
        let mut total = 0;
        while total < min_bytes {
            match inner.try_read(&mut buf[total..]) {
                Ok(0) => break, // EOF: return what we have (< min_bytes signals EOF to KJ).
                Ok(n) => total += n,
                Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                    inner.ready(Interest::READABLE).await?;
                }
                Err(e) if e.kind() == std::io::ErrorKind::Interrupted => {}
                Err(e) => return Err(op("read()")(e)),
            }
        }
        Ok(total)
    }

    /// Write-all semantics.
    // Holding the `Ref` across awaits is the whole point (see the type docs): it reserves the
    // native stream for this operation so a concurrent `take()`/unwrap fails cleanly. Sound
    // here: the only conflicting accessor is `take()`, which uses `try_borrow_mut` and returns
    // an error rather than panicking, and concurrent read+write just stack shared `Ref`s.
    #[expect(
        clippy::await_holding_refcell_ref,
        reason = "intentional in-flight guard; see above"
    )]
    async fn write_all(&self, buf: &[u8]) -> Result<()> {
        let inner = self.live()?;
        let mut written = 0;
        while written < buf.len() {
            match inner.try_write(&buf[written..]) {
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
                    inner.ready(Interest::WRITABLE).await?;
                }
                Err(e) if e.kind() == std::io::ErrorKind::Interrupted => {}
                Err(e) => return Err(op("write()")(e)),
            }
        }
        Ok(())
    }

    /// Write-all semantics over several pieces, as one operation: `writev` until every piece is
    /// fully written. `IoSlice::advance_slices` drops fully-written leading pieces and trims a
    /// partially-written one, so a short write resumes exactly where the kernel stopped.
    // Holding the `Ref` across awaits is the whole point (see the type docs): it reserves the
    // native stream for this operation so a concurrent `take()`/unwrap fails cleanly. Sound
    // here: the only conflicting accessor is `take()`, which uses `try_borrow_mut` and returns
    // an error rather than panicking, and concurrent read+write just stack shared `Ref`s.
    #[expect(
        clippy::await_holding_refcell_ref,
        reason = "intentional in-flight guard; see above"
    )]
    async fn write_all_pieces(&self, pieces: &crate::ffi::KjPieces) -> Result<()> {
        let inner = self.live()?;
        let count = crate::ffi::kj_pieces_count(pieces);
        let mut slices: Vec<IoSlice<'_>> = (0..count)
            .map(|index| IoSlice::new(crate::ffi::kj_piece(pieces, index)))
            .collect();
        let mut bufs: &mut [IoSlice<'_>] = &mut slices;
        loop {
            // All-empty remainder (including all-empty input): nothing left to write. Checked
            // before the syscall because writev of zero bytes returns 0, which below means
            // "connection closed".
            if bufs.iter().all(|piece| piece.is_empty()) {
                return Ok(());
            }
            match inner.try_write_vectored(bufs) {
                Ok(0) => {
                    return Err(KjIoError::other(
                        "writev()",
                        "wrote zero bytes (connection closed)",
                    ));
                }
                Ok(n) => IoSlice::advance_slices(&mut bufs, n),
                Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                    inner.ready(Interest::WRITABLE).await?;
                }
                Err(e) if e.kind() == std::io::ErrorKind::Interrupted => {}
                Err(e) => return Err(op("writev()")(e)),
            }
        }
    }

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
    // Holding the `Ref` across awaits is the whole point (see the type docs): it reserves the
    // native stream for this operation so a concurrent `take()`/unwrap fails cleanly. Sound
    // here: the only conflicting accessor is `take()`, which uses `try_borrow_mut` and returns
    // an error rather than panicking, and concurrent read+write just stack shared `Ref`s.
    #[expect(
        clippy::await_holding_refcell_ref,
        reason = "intentional in-flight guard; see above"
    )]
    async fn when_write_disconnected(&self) -> Result<()> {
        use std::os::fd::AsRawFd;

        // Borrow the live socket's fd directly (tokio streams implement `AsFd`) and dup it, so
        // no raw fd is ever materialized without an owner. The `Ref` is held for the whole wait:
        // this IS an in-flight operation as far as `take()` is concerned.
        let inner = self.live()?;
        let borrowed = inner.as_borrowed_fd();
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
    #[expect(
        clippy::await_holding_refcell_ref,
        reason = "intentional in-flight guard; see above"
    )]
    async fn when_write_disconnected(&self) -> Result<()> {
        // Holds the in-flight guard (like the unix arm) and never resolves. `pending()` infers
        // the Result<()> return type, so there is no unreachable tail (crate-level
        // deny(clippy::unreachable)).
        let _inner = self.live()?;
        std::future::pending().await
    }

    #[cfg(not(any(unix, windows)))]
    async fn when_write_disconnected(&self) -> Result<()> {
        self.live()?;
        Err(KjIoError::other(
            "whenWriteDisconnected",
            "not implemented by kj-rs-io on this platform",
        ))
    }

    fn shutdown_write(&self) -> Result<()> {
        // `shutdown(2)` acts on the socket, not on a descriptor, so a `SockRef` borrow of the
        // live socket is all it needs: no dup, no owning std type, identical on unix and windows.
        #[cfg(any(unix, windows))]
        {
            self.with_sock_ref("shutdown(SHUT_WR)", |sock| {
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

    /// Runs `f` on a `socket2::SockRef` borrowing the live socket — the shared body of the
    /// `getsockname()`/`getpeername()` passthroughs; only the socket borrow is per-platform.
    fn with_sock_ref<T>(
        &self,
        op_name: &'static str,
        f: impl FnOnce(&socket2::SockRef<'_>) -> std::io::Result<T>,
    ) -> Result<T> {
        #[cfg(unix)]
        {
            self.with_borrowed_fd(|fd| f(&socket2::SockRef::from(&fd)))?
                .map_err(op(op_name))
        }
        // Validated by Windows CI; mirrors the unix arm.
        #[cfg(windows)]
        {
            self.with_borrowed_socket(|sock| f(&socket2::SockRef::from(&sock)))?
                .map_err(op(op_name))
        }
        #[cfg(not(any(unix, windows)))]
        {
            let _ = f;
            self.live()?;
            Err(KjIoError::other(
                op_name,
                "not implemented by kj-rs-io on this platform",
            ))
        }
    }

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
            self.with_borrowed_fd(|fd| i64::from(fd.as_raw_fd()))
        }
        // Validated by Windows CI; mirrors the unix arm.
        #[cfg(windows)]
        {
            use std::os::windows::io::AsRawSocket;
            // A live SOCKET fits in i64 (Windows handles fit in 32 bits); the bridge carries
            // its bits verbatim.
            #[allow(clippy::cast_possible_wrap)]
            self.with_borrowed_socket(|sock| sock.as_raw_socket() as i64)
        }
        #[cfg(not(any(unix, windows)))]
        {
            self.live()?;
            Err(KjIoError::other(
                "getFd",
                "file descriptors are not available on this platform",
            ))
        }
    }

    /// Recovers whichever native tokio object this is, as a [`crate::serve::ServeIo`]
    /// (the unwrap fast path of [`crate::serve_kj_stream`]). `None` if hollow.
    pub(crate) fn into_serve_io(self) -> Option<crate::serve::ServeIo> {
        match self.inner.into_inner()? {
            Inner::Tcp(stream) => Some(crate::serve::ServeIo::Tcp(stream)),
            #[cfg(unix)]
            Inner::Unix(stream) => Some(crate::serve::ServeIo::Unix(stream)),
        }
    }

    /// Moves the native stream out into a fresh wrapper, leaving this one hollow. Fails if the
    /// wrapper is already hollow, or if any I/O operation is in flight (see the type docs):
    /// that is the checked replacement for the old "no I/O promises may be outstanding" caller
    /// contract.
    fn take(&self) -> Result<Box<Self>> {
        let mut slot = self.inner.try_borrow_mut().map_err(|_| {
            KjIoError::other(
                "kj_rs_io",
                "cannot unwrap a stream while I/O operations are in flight on it",
            )
        })?;
        let inner = slot.take().ok_or_else(|| {
            KjIoError::other("kj_rs_io", "stream was already unwrapped (hollow wrapper)")
        })?;
        Ok(Box::new(Self::new(inner)))
    }
}

// ======================================================================================
// Bridge entry points (see lib.rs). Every async fn wraps its body in `with_runtime` so tokio
// resources created while polling on the KJ thread can reach the loop runtime's I/O driver.

pub async fn stream_try_read(
    stream: &TokioStream,
    buf: &mut [u8],
    min_bytes: usize,
) -> Result<usize> {
    with_runtime(stream.try_read_min(buf, min_bytes)).await
}

pub async fn stream_write(stream: &TokioStream, buf: &[u8]) -> Result<()> {
    with_runtime(stream.write_all(buf)).await
}

pub async fn stream_write_pieces(
    stream: &TokioStream,
    pieces: &crate::ffi::KjPieces,
) -> Result<()> {
    with_runtime(stream.write_all_pieces(pieces)).await
}

pub async fn stream_when_write_disconnected(stream: &TokioStream) -> Result<()> {
    with_runtime(stream.when_write_disconnected()).await
}

pub fn stream_shutdown_write(stream: &TokioStream) -> Result<()> {
    stream.shutdown_write()
}

pub fn stream_raw_handle(stream: &TokioStream) -> Result<i64> {
    stream.raw_handle()
}

/// Non-throwing variant for `kj::AsyncIoStream::getFd()`/`getWin32Handle()`, which return a
/// `kj::Maybe`: -1 when the wrapper is hollow (or the platform has no handle), so C++ does not
/// have to use exception catching as control flow.
pub fn stream_try_raw_handle(stream: &TokioStream) -> i64 {
    stream.raw_handle().unwrap_or(-1)
}

pub fn stream_local_addr(stream: &TokioStream) -> Result<Vec<u8>> {
    stream.local_addr_bytes()
}

pub fn stream_peer_addr(stream: &TokioStream) -> Result<Vec<u8>> {
    stream.peer_addr_bytes()
}

pub fn stream_take(stream: &TokioStream) -> Result<Box<TokioStream>> {
    stream.take()
}

// ======================================================================================
// Arbitrary readable/writable fds (kj::LowLevelAsyncIoProvider::wrapInputFd/wrapOutputFd).
// Unix only: implemented over AsyncFd, which supports pipes, character devices and sockets
// (regular files are rejected by epoll/kqueue, matching KJ's fd-observer-based provider).
// Deliberately no windows arm: kj's win32 LowLevelAsyncIoProvider has no pipe-fd tier — its
// `Fd` is documented as a SOCKET (capnproto async-io.h) and its wrapInputFd/wrapOutputFd are
// implemented identically to wrapSocketFd (async-io-win32.c++) — so the C++ side
// (async-io.c++) routes win32 wrapInputFd/wrapOutputFd through the tested socket path
// (`wrap_socket_fd`) and never calls these entry points there; the `not(unix)` arms below are
// totality backstops only.

#[cfg(unix)]
type FdIo = tokio::io::unix::AsyncFd<std::fs::File>;

pub struct TokioInputFd {
    #[cfg(unix)]
    inner: FdIo,
}

pub struct TokioOutputFd {
    #[cfg(unix)]
    inner: FdIo,
}

#[cfg(unix)]
fn fd_io_from_raw(fd: i32, interest: Interest) -> Result<FdIo> {
    let owned = crate::ffi::own_fd_from_raw(fd);
    let _guard = crate::runtime::runtime_handle()?.enter();
    tokio::io::unix::AsyncFd::with_interest(std::fs::File::from(owned), interest)
        .map_err(op("wrapFd"))
}

pub fn wrap_input_fd(fd: i32) -> Result<Box<TokioInputFd>> {
    #[cfg(unix)]
    {
        Ok(Box::new(TokioInputFd {
            inner: fd_io_from_raw(fd, Interest::READABLE)?,
        }))
    }
    #[cfg(not(unix))]
    {
        let _ = fd;
        Err(KjIoError::other(
            "wrapInputFd",
            "not implemented on this platform",
        ))
    }
}

pub fn wrap_output_fd(fd: i32) -> Result<Box<TokioOutputFd>> {
    #[cfg(unix)]
    {
        Ok(Box::new(TokioOutputFd {
            inner: fd_io_from_raw(fd, Interest::WRITABLE)?,
        }))
    }
    #[cfg(not(unix))]
    {
        let _ = fd;
        Err(KjIoError::other(
            "wrapOutputFd",
            "not implemented on this platform",
        ))
    }
}

pub async fn input_fd_try_read(
    stream: &TokioInputFd,
    buf: &mut [u8],
    min_bytes: usize,
) -> Result<usize> {
    #[cfg(unix)]
    {
        with_runtime(async move {
            let min_bytes = min_bytes.min(buf.len());
            let mut total = 0;
            while total < min_bytes {
                let mut guard = stream
                    .inner
                    .ready(Interest::READABLE)
                    .await
                    .map_err(op("poll()"))?;
                match guard.try_io(|inner| {
                    let mut file: &std::fs::File = inner.get_ref();
                    file.read(&mut buf[total..])
                }) {
                    Ok(Ok(0)) => break, // EOF
                    Ok(Ok(n)) => total += n,
                    Ok(Err(e)) if e.kind() == std::io::ErrorKind::Interrupted => {}
                    Ok(Err(e)) => return Err(op("read()")(e)),
                    Err(_would_block) => {}
                }
            }
            Ok(total)
        })
        .await
    }
    #[cfg(not(unix))]
    {
        let _ = (stream, buf, min_bytes);
        Err(KjIoError::other(
            "read()",
            "not implemented on this platform",
        ))
    }
}

pub async fn output_fd_write(stream: &TokioOutputFd, buf: &[u8]) -> Result<()> {
    #[cfg(unix)]
    {
        with_runtime(async move {
            let mut written = 0;
            while written < buf.len() {
                let mut guard = stream
                    .inner
                    .ready(Interest::WRITABLE)
                    .await
                    .map_err(op("poll()"))?;
                match guard.try_io(|inner| {
                    let mut file: &std::fs::File = inner.get_ref();
                    file.write(&buf[written..])
                }) {
                    Ok(Ok(0)) => {
                        return Err(KjIoError::other("write()", "wrote zero bytes"));
                    }
                    Ok(Ok(n)) => written += n,
                    Ok(Err(e)) if e.kind() == std::io::ErrorKind::Interrupted => {}
                    Ok(Err(e)) => return Err(op("write()")(e)),
                    Err(_would_block) => {}
                }
            }
            Ok(())
        })
        .await
    }
    #[cfg(not(unix))]
    {
        let _ = (stream, buf);
        Err(KjIoError::other(
            "write()",
            "not implemented on this platform",
        ))
    }
}

#[cfg(test)]
mod tests {
    use std::task::Context;
    use std::task::Waker;

    use cxx::KjError;
    use static_assertions::assert_impl_all;
    use static_assertions::assert_not_impl_any;

    use super::*;

    // `TokioStream` is a KJ-loop-thread object whose in-flight-operation tracking lives in a
    // `RefCell` (see the type docs): it must never become `Sync` by accident. It stays `Send`,
    // like the native tokio sockets inside it, so `unwrap_kj_stream`'s `Box<TokioStream>` can be
    // handed to a connection task.
    assert_not_impl_any!(TokioStream: Sync);
    assert_impl_all!(TokioStream: Send);

    /// A connected localhost TCP pair as tokio streams registered with `port`'s runtime.
    fn connected_pair(port: &kj_rs_tokio::TokioPort) -> (TokioStream, std::net::TcpStream) {
        let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let client = std::net::TcpStream::connect(listener.local_addr().unwrap()).unwrap();
        let (server, _) = listener.accept().unwrap();
        server.set_nonblocking(true).unwrap();
        let _guard = port.handle().enter();
        (
            TokioStream::from_tcp(TcpStream::from_std(server).unwrap()),
            client,
        )
    }

    #[test]
    fn hollow_wrapper_rejects_every_operation_and_second_take() {
        let port = kj_rs_tokio::TokioPort::new();
        let (stream, _client) = connected_pair(&port);

        let taken = match stream.take() {
            Ok(taken) => taken,
            Err(e) => panic!("first take: {}", KjError::from(e).description()),
        };
        assert!(
            taken.live().is_ok(),
            "the taken wrapper holds the live stream"
        );

        // The original is hollow now.
        let err = |r: Result<()>| match r {
            Ok(()) => panic!("expected an error from a hollow wrapper"),
            Err(e) => KjError::from(e).description().to_owned(),
        };
        assert!(stream.live().is_err());
        assert!(err(stream.shutdown_write()).contains("hollow"));
        assert!(stream.raw_handle().is_err());
        assert_eq!(stream_try_raw_handle(&stream), -1);
        assert!(stream.local_addr_bytes().is_err());
        assert!(err(stream.take().map(drop)).contains("already unwrapped"));
        assert!(stream.into_tcp_stream().is_none());
    }

    #[test]
    fn take_while_an_operation_is_in_flight_is_an_error_not_aliasing() {
        let port = kj_rs_tokio::TokioPort::new();
        let (stream, _client) = connected_pair(&port);

        // Start a read and park it: nothing has been written, so it registers readiness
        // interest and returns Pending while holding its borrow of the native stream. The
        // runtime guard is held across the poll so the tokio reactor is reachable.
        let mut buf = [0u8; 8];
        let mut read = Box::pin(stream.try_read_min(&mut buf, 1));
        let mut cx = Context::from_waker(Waker::noop());
        {
            let _guard = port.handle().enter();
            assert!(read.as_mut().poll(&mut cx).is_pending());
        }

        // Unwrapping now would alias that live borrow; it is refused instead.
        let err = match stream.take() {
            Ok(_) => panic!("take() must fail while a read is in flight"),
            Err(e) => KjError::from(e),
        };
        assert!(
            err.description().contains("in flight"),
            "{}",
            err.description()
        );

        // Once the operation is gone the unwrap goes through, and the native stream is intact.
        drop(read);
        let taken = match stream.take() {
            Ok(taken) => taken,
            Err(e) => panic!(
                "take after the read was dropped: {}",
                KjError::from(e).description()
            ),
        };
        assert!(taken.into_tcp_stream().is_some());
    }

    #[test]
    fn concurrent_read_and_write_both_in_flight_share_the_borrow() {
        // The load-bearing invariant of the RefCell design: shared borrows stack, so a read and
        // a write can be in flight at once (kj's one-read + one-write contract), and `take()`
        // fails while EITHER is alive, succeeding only once BOTH are dropped.
        let port = kj_rs_tokio::TokioPort::new();
        let (stream, _client) = connected_pair(&port);

        let mut buf = [0u8; 8];
        let mut read = Box::pin(stream.try_read_min(&mut buf, 1));
        // 1 MiB: larger than the socket buffer, so the write cannot drain in one go and the
        // future stays pending, holding its borrow.
        let payload = vec![0u8; 1024 * 1024];
        let mut write = Box::pin(stream.write_all(&payload));
        let mut cx = Context::from_waker(Waker::noop());
        {
            let _guard = port.handle().enter();
            assert!(read.as_mut().poll(&mut cx).is_pending());
            assert!(write.as_mut().poll(&mut cx).is_pending());
        }
        // Both borrows are live and coexist; take() is refused.
        assert!(
            stream.take().is_err(),
            "take() must fail while read+write are both in flight"
        );
        drop(read);
        assert!(
            stream.take().is_err(),
            "take() must still fail while the write is in flight"
        );
        drop(write);
        assert!(
            stream.take().is_ok(),
            "take() succeeds once both operations are gone"
        );
    }

    #[test]
    fn hollow_into_serve_io_is_none() {
        let port = kj_rs_tokio::TokioPort::new();
        let (stream, _client) = connected_pair(&port);
        let _taken = stream.take().expect("first take");
        // The original is hollow; recovering a ServeIo yields None (the into_inner()? == None arm).
        assert!(stream.into_serve_io().is_none());
    }

    #[cfg(unix)]
    #[test]
    fn write_disconnected_wait_counts_as_in_flight() {
        let port = kj_rs_tokio::TokioPort::new();
        let (stream, _client) = connected_pair(&port);
        let mut wait = Box::pin(stream.when_write_disconnected());
        let mut cx = Context::from_waker(Waker::noop());
        {
            let _guard = port.handle().enter();
            assert!(wait.as_mut().poll(&mut cx).is_pending());
        }
        assert!(
            stream.take().is_err(),
            "whenWriteDisconnected holds the stream too"
        );
        drop(wait);
        assert!(stream.take().is_ok());
    }
}
