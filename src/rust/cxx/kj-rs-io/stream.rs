//! Tokio-backed `kj::AsyncIoStream` backend and the arbitrary-fd streams behind
//! `kj::LowLevelAsyncIoProvider::wrapInputFd` / `wrapOutputFd`.
//!
//! # Operations own their state
//!
//! A [`TokioStream`] is a handle to an `Rc`-shared cell holding the native socket. Every bridged
//! operation clones that `Rc` into the future it returns and never touches the `&TokioStream` it
//! was called with again (the entry points are `fn -> impl Future + use<..>`, capturing no
//! borrow of the handle). So the C++ wrapper that owns the handle may be destroyed while a read
//! is pending -- a KJ contract violation, but one `kj::AsyncIoStream` consumers can commit --
//! and nothing dangles: the socket lives until the last operation settles or is cancelled.
//! `TokioStream` is therefore `!Send` (an `Rc`), like the loop-thread C++ object owning it; the
//! native socket recovered by [`TokioStream::into_tcp_stream`] stays `Send`.
//!
//! # In-flight operation tracking
//!
//! The cell is a `RefCell<Option<_>>`. Every I/O operation holds a *shared* borrow across its
//! awaits (the `Ref` lives in the future), and [`TokioStream::take`] -- the unwrap fast path --
//! needs the *exclusive* borrow. So unwrapping a stream with a read, write or
//! `whenWriteDisconnected` in flight is a checked error rather than aliasing, and read + write in
//! flight at once (kj's contract) simply stack shared borrows. That is the whole reason for
//! holding a `Ref` across an await here, hence the module-level `expect` of
//! `clippy::await_holding_refcell_ref`: sound because the only exclusive accessor, `take`, uses
//! `try_borrow_mut` and reports failure instead of panicking.
//!
//! # Read buffers
//!
//! KJ callers may hand `tryRead` uninitialized storage, so reads take `&mut [MaybeUninit<u8>]`
//! (ffi.rs builds it from the raw pointer that crosses the bridge) and fill it through APIs that
//! never read the uninitialized bytes: tokio's `try_read_buf` over `bytes::BufMut`, and
//! `read(2)` directly for the fd tier.
#![expect(
    clippy::await_holding_refcell_ref,
    reason = "the held Ref IS the in-flight guard that makes take() a checked operation; see the module docs"
)]

use std::cell::Ref;
use std::cell::RefCell;
use std::future::Future;
use std::io::IoSlice;
use std::mem::MaybeUninit;
use std::rc::Rc;

use tokio::io::Interest;
use tokio::net::TcpStream;
#[cfg(unix)]
use tokio::net::UnixStream;

use crate::error::KjIoError;
use crate::error::Result;
use crate::error::op;

/// A connected tokio stream (TCP or Unix domain) behind a `kj::AsyncIoStream`. See the module
/// docs for its ownership model; loop-thread only (`!Send`).
pub struct TokioStream {
    cell: Rc<StreamCell>,
}

/// The shared state behind a [`TokioStream`] and every operation in flight on it.
struct StreamCell {
    /// `None` after the native stream has been moved out by [`TokioStream::take`] (the C++
    /// wrapper is then "hollow" and every operation fails).
    inner: RefCell<Option<Inner>>,
}

struct Inner {
    socket: Socket,
    /// The `dup(2)` + I/O-driver registration behind `whenWriteDisconnected`, created on the
    /// first call and shared by every later one (kj-http calls it once per server connection;
    /// KJ itself forks one observation per stream). One extra fd per stream at most, not per
    /// call. See [`StreamCell::when_write_disconnected`] for why it is a separate registration
    /// at all.
    #[cfg(unix)]
    hangup_watch: std::cell::OnceCell<tokio::io::unix::AsyncFd<std::os::fd::OwnedFd>>,
}

enum Socket {
    Tcp(TcpStream),
    #[cfg(unix)]
    Unix(UnixStream),
}

impl Socket {
    async fn ready(&self, interest: Interest) -> Result<()> {
        match self {
            Self::Tcp(s) => s.ready(interest).await,
            #[cfg(unix)]
            Self::Unix(s) => s.ready(interest).await,
        }
        .map_err(op("poll()"))?;
        Ok(())
    }

    /// One non-blocking read into possibly uninitialized storage, returning the count read.
    /// tokio drives the `BufMut` impl for `&mut [MaybeUninit<u8>]`: it only ever writes into the
    /// storage, never reads the uninitialized bytes.
    fn try_read_buf(&self, buf: &mut [MaybeUninit<u8>]) -> std::io::Result<usize> {
        let mut dst = buf;
        match self {
            Self::Tcp(s) => s.try_read_buf(&mut dst),
            #[cfg(unix)]
            Self::Unix(s) => s.try_read_buf(&mut dst),
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
    /// the Windows counterpart of [`Socket::as_borrowed_fd`]. Only the Tcp variant exists on
    /// Windows.
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

impl StreamCell {
    fn new(socket: Socket) -> Rc<Self> {
        Rc::new(Self {
            inner: RefCell::new(Some(Inner {
                socket,
                #[cfg(unix)]
                hangup_watch: std::cell::OnceCell::new(),
            })),
        })
    }

    /// Borrows the live native stream for the duration of an operation. The returned `Ref` is
    /// what makes a concurrent [`TokioStream::take`] fail (see the module docs). Errors if the
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

    /// KJ `tryRead` semantics (`AsyncStreamFd::tryReadInternal`): read until at least
    /// `min_bytes` are available, EOF, or -- when `min_bytes` is already satisfied, including
    /// `min_bytes == 0` -- the socket would block, in which case whatever one read attempt
    /// produced is returned. `buf` may be uninitialized; only the returned count's worth of
    /// leading bytes is initialized afterwards.
    async fn try_read_min(&self, buf: &mut [MaybeUninit<u8>], min_bytes: usize) -> Result<usize> {
        let inner = self.live()?;
        let min_bytes = min_bytes.min(buf.len());
        let mut total = 0;
        while total < buf.len() {
            match inner.socket.try_read_buf(&mut buf[total..]) {
                Ok(0) => break, // EOF: return what we have (< min_bytes signals EOF to KJ).
                Ok(n) => {
                    total += n;
                    if total >= min_bytes {
                        break;
                    }
                }
                Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                    if total >= min_bytes {
                        break;
                    }
                    inner.socket.ready(Interest::READABLE).await?;
                }
                Err(e) if e.kind() == std::io::ErrorKind::Interrupted => {}
                Err(e) => return Err(op("read()")(e)),
            }
        }
        Ok(total)
    }

    /// Write-all semantics.
    async fn write_all(&self, buf: &[u8]) -> Result<()> {
        let inner = self.live()?;
        let mut written = 0;
        while written < buf.len() {
            match inner.socket.try_write(&buf[written..]) {
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
                    inner.socket.ready(Interest::WRITABLE).await?;
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
    ///
    /// Empty pieces are dropped up front: the kernel only looks at the first `IOV_MAX` entries
    /// of a `writev`, so a run of empty pieces longer than that would make it write zero bytes
    /// -- indistinguishable from a closed connection -- while data still waits further down.
    async fn write_all_pieces(&self, pieces: &crate::ffi::KjPieces) -> Result<()> {
        let inner = self.live()?;
        let count = crate::ffi::kj_pieces_count(pieces);
        let mut slices: Vec<IoSlice<'_>> = (0..count)
            .map(|index| crate::ffi::kj_piece(pieces, index))
            .filter(|piece| !piece.is_empty())
            .map(IoSlice::new)
            .collect();
        let mut bufs: &mut [IoSlice<'_>] = &mut slices;
        while !bufs.is_empty() {
            match inner.socket.try_write_vectored(bufs) {
                Ok(0) => {
                    return Err(KjIoError::other(
                        "writev()",
                        "wrote zero bytes (connection closed)",
                    ));
                }
                Ok(n) => IoSlice::advance_slices(&mut bufs, n),
                Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                    inner.socket.ready(Interest::WRITABLE).await?;
                }
                Err(e) if e.kind() == std::io::ErrorKind::Interrupted => {}
                Err(e) => return Err(op("writev()")(e)),
            }
        }
        Ok(())
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
    /// Why a second fd rather than the socket's own registration: tokio caches readiness per
    /// registration, and `try_write` consults that cache before making the syscall -- if the
    /// cached WRITABLE bit is clear it returns `WouldBlock` and the writer parks in
    /// `ready(WRITABLE)` until the *next* epoll/kqueue edge. This waiter must clear WRITABLE to
    /// avoid spinning on an always-writable socket, so sharing the registration with concurrent
    /// writers would park them on an edge that never comes (the socket stays writable): a hang,
    /// not a slowdown. A `dup(2)` is its own registration, so clearing its readiness disturbs no
    /// one. The dup is created once per stream and shared by every call (concurrent waiters all
    /// want the same edge, and none of them writes through it).
    ///
    /// Windows behavior (see the arm below): a never-resolving future, which IS KJ parity —
    /// capnproto's win32 `whenWriteDisconnected` returns `NEVER_DONE` today (its
    /// `IOCTL_AFD_POLL` idea, the mechanism `select()` is built on, is only a TODO there).
    #[cfg(unix)]
    async fn when_write_disconnected(&self) -> Result<()> {
        // The `Ref` is held for the whole wait: this IS an in-flight operation as far as
        // `take()` is concerned.
        let inner = self.live()?;
        let async_fd = if let Some(existing) = inner.hangup_watch.get() {
            existing
        } else {
            // Borrow the live socket's fd and dup it, so no raw fd is ever materialized
            // without an owner. The dup shares the underlying open socket (and its
            // O_NONBLOCK status), but has its own registration with the I/O driver.
            let owned = inner
                .socket
                .as_borrowed_fd()
                .try_clone_to_owned()
                .map_err(op("dup()"))?;
            let async_fd = tokio::io::unix::AsyncFd::with_interest(owned, Interest::WRITABLE)
                .map_err(op("whenWriteDisconnected"))?;
            // No one can have raced us here: we hold the RefCell borrow and have not yielded
            // since `get()`, so this stores `async_fd`.
            inner.hangup_watch.get_or_init(|| async_fd)
        };
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

    /// Never resolves: KJ parity, not a gap (see the unix arm's docs).
    #[cfg(windows)]
    async fn when_write_disconnected(&self) -> Result<()> {
        // Holds the in-flight guard (like the unix arm) and never resolves. `pending()` infers
        // the Result<()> return type, so there is no unreachable tail (crate-level
        // deny(clippy::unreachable)).
        let _inner = self.live()?;
        std::future::pending().await
    }

    /// Runs `f` on a `socket2::SockRef` borrowing the live socket — the shared body of the
    /// `shutdown` / `getsockname()` / `getpeername()` passthroughs; only the socket borrow is
    /// per-platform.
    fn with_sock_ref<T>(
        &self,
        op_name: &'static str,
        f: impl FnOnce(&socket2::SockRef<'_>) -> std::io::Result<T>,
    ) -> Result<T> {
        let inner = self.live()?;
        #[cfg(unix)]
        let sock = inner.socket.as_borrowed_fd();
        #[cfg(windows)]
        let sock = inner.socket.as_borrowed_socket();
        f(&socket2::SockRef::from(&sock)).map_err(op(op_name))
    }
}

impl TokioStream {
    fn new(socket: Socket) -> Self {
        Self {
            cell: StreamCell::new(socket),
        }
    }

    #[must_use]
    pub fn from_tcp(stream: TcpStream) -> Self {
        Self::new(Socket::Tcp(stream))
    }

    #[cfg(unix)]
    #[must_use]
    pub fn from_unix(stream: UnixStream) -> Self {
        Self::new(Socket::Unix(stream))
    }

    /// A share of the state, for an operation's future to own (see the module docs).
    fn shared(&self) -> Rc<StreamCell> {
        Rc::clone(&self.cell)
    }

    /// Moves the native stream out of the cell (leaving every handle to it hollow). `None` if
    /// it is hollow already or an operation is in flight on it.
    fn take_inner(&self) -> Option<Inner> {
        self.cell.inner.try_borrow_mut().ok()?.take()
    }

    /// Recovers the native tokio TCP stream, if this is a (non-hollow) TCP stream with no
    /// operation in flight. Reserved for native Rust consumers (a Rust server driving the
    /// connection after `unwrap_kj_stream`); the result is `Send`, this handle is not.
    #[must_use]
    pub fn into_tcp_stream(self) -> Option<TcpStream> {
        match self.take_inner()?.socket {
            Socket::Tcp(stream) => Some(stream),
            #[cfg(unix)]
            Socket::Unix(_) => None,
        }
    }

    /// Recovers the native tokio Unix-domain stream, if this is one (see
    /// [`TokioStream::into_tcp_stream`]).
    #[cfg(unix)]
    #[must_use]
    pub fn into_unix_stream(self) -> Option<UnixStream> {
        match self.take_inner()?.socket {
            Socket::Unix(stream) => Some(stream),
            Socket::Tcp(_) => None,
        }
    }

    /// Runs `f` on the live socket's borrowed fd. Errors if the wrapper is hollow.
    #[cfg(unix)]
    pub(crate) fn with_borrowed_fd<T>(
        &self,
        f: impl FnOnce(std::os::fd::BorrowedFd<'_>) -> T,
    ) -> Result<T> {
        let inner = self.cell.live()?;
        Ok(f(inner.socket.as_borrowed_fd()))
    }

    /// Runs `f` on the live socket's borrowed `SOCKET`. Errors if the wrapper is hollow.
    #[cfg(windows)]
    pub(crate) fn with_borrowed_socket<T>(
        &self,
        f: impl FnOnce(std::os::windows::io::BorrowedSocket<'_>) -> T,
    ) -> Result<T> {
        let inner = self.cell.live()?;
        Ok(f(inner.socket.as_borrowed_socket()))
    }

    /// `kj::AsyncIoStream::tryRead` as a future owning its share of the stream. `buf` may be
    /// uninitialized; it is the caller's, valid until the future settles (KJ's contract).
    pub(crate) fn try_read_min<'b>(
        &self,
        buf: &'b mut [MaybeUninit<u8>],
        min_bytes: usize,
    ) -> impl Future<Output = Result<usize>> + use<'b> {
        let cell = self.shared();
        async move { cell.try_read_min(buf, min_bytes).await }
    }

    /// `kj::AsyncIoStream::write` (write-all) as a future owning its share of the stream.
    pub(crate) fn write_all<'b>(
        &self,
        buf: &'b [u8],
    ) -> impl Future<Output = Result<()>> + use<'b> {
        let cell = self.shared();
        async move { cell.write_all(buf).await }
    }

    /// `kj::AsyncIoStream::write(pieces)` as a future owning its share of the stream.
    pub(crate) fn write_all_pieces<'b>(
        &self,
        pieces: &'b crate::ffi::KjPieces,
    ) -> impl Future<Output = Result<()>> + use<'b> {
        let cell = self.shared();
        async move { cell.write_all_pieces(pieces).await }
    }

    /// `kj::AsyncIoStream::whenWriteDisconnected` as a future owning its share of the stream.
    pub(crate) fn when_write_disconnected(&self) -> impl Future<Output = Result<()>> + use<> {
        let cell = self.shared();
        async move { cell.when_write_disconnected().await }
    }

    /// `shutdown(SHUT_WR)`: cleanly shut down the write end, keeping the read end open.
    fn shutdown_write(&self) -> Result<()> {
        // `shutdown(2)` acts on the socket, not on a descriptor, so a `SockRef` borrow of the
        // live socket is all it needs: no dup, no owning std type, identical on unix and windows.
        self.cell.with_sock_ref("shutdown(SHUT_WR)", |sock| {
            sock.shutdown(std::net::Shutdown::Write)
        })
    }

    /// `kj::AsyncIoStream::abortRead`: `shutdown(SHUT_RD)`, as KJ's `AsyncStreamFd::abortRead`
    /// does. A pending read observes EOF (kj-http and the WebSocket abort path rely on this to
    /// terminate reads). Errors surface as KJ's own `KJ_SYSCALL` would.
    fn abort_read(&self) -> Result<()> {
        self.cell.with_sock_ref("shutdown(SHUT_RD)", |sock| {
            sock.shutdown(std::net::Shutdown::Read)
        })
    }

    /// Raw `struct sockaddr` bytes of the socket's locally-bound address (the `getsockname()`
    /// passthrough behind `kj::AsyncIoStream::getsockname`).
    fn local_addr_bytes(&self) -> Result<Vec<u8>> {
        let addr = self
            .cell
            .with_sock_ref("getsockname()", |sock| sock.local_addr())?;
        Ok(crate::ffi::sockaddr_to_bytes(&addr))
    }

    /// Raw `struct sockaddr` bytes of the connected peer's address (the `getpeername()`
    /// passthrough behind `kj::AsyncIoStream::getpeername`).
    fn peer_addr_bytes(&self) -> Result<Vec<u8>> {
        let addr = self
            .cell
            .with_sock_ref("getpeername()", |sock| sock.peer_addr())?;
        Ok(crate::ffi::sockaddr_to_bytes(&addr))
    }

    /// The connected Unix-domain peer's process credentials, with KJ's validity rules
    /// (`kj/async-io-unix.c++` `getIdentity`): a pid is reported only if positive, a uid only
    /// if not `(uid_t)-1`. tokio reads `SO_PEERCRED` (Linux) / `LOCAL_PEERCRED` +
    /// `LOCAL_PEERPID` (macOS, BSDs) for us.
    fn peer_credentials(&self) -> Result<crate::ffi::PeerCredentials> {
        let inner = self.cell.live()?;
        match &inner.socket {
            #[cfg(unix)]
            Socket::Unix(s) => {
                let cred = s.peer_cred().map_err(op("getsockopt(SO_PEERCRED)"))?;
                let pid = cred.pid().filter(|pid| *pid > 0);
                Ok(crate::ffi::PeerCredentials {
                    has_pid: pid.is_some(),
                    pid: pid.unwrap_or(0),
                    has_uid: cred.uid() != u32::MAX,
                    uid: cred.uid(),
                })
            }
            Socket::Tcp(_) => Err(KjIoError::other(
                "peer credentials",
                "not a Unix-domain socket",
            )),
        }
    }

    /// The underlying raw OS socket handle, widened to `i64`: a Unix fd
    /// (`kj::AsyncIoStream::getFd()`) or a win32 `SOCKET` (`getWin32Handle()`).
    fn raw_handle(&self) -> Result<i64> {
        #[cfg(unix)]
        {
            use std::os::fd::AsRawFd;
            self.with_borrowed_fd(|fd| i64::from(fd.as_raw_fd()))
        }
        #[cfg(windows)]
        {
            use std::os::windows::io::AsRawSocket;
            // A live SOCKET fits in i64 (Windows handles fit in 32 bits); the bridge carries
            // its bits verbatim.
            #[allow(clippy::cast_possible_wrap)]
            self.with_borrowed_socket(|sock| sock.as_raw_socket() as i64)
        }
    }

    /// Recovers whichever native tokio object this is, as a [`crate::serve::ServeIo`]
    /// (the unwrap fast path of [`crate::serve_kj_stream`]). `None` if hollow or busy.
    pub(crate) fn into_serve_io(self) -> Option<crate::serve::ServeIo> {
        match self.take_inner()?.socket {
            Socket::Tcp(stream) => Some(crate::serve::ServeIo::Tcp(stream)),
            #[cfg(unix)]
            Socket::Unix(stream) => Some(crate::serve::ServeIo::Unix(stream)),
        }
    }

    /// Moves the native stream out into a fresh handle, leaving this one hollow. Fails if the
    /// wrapper is already hollow, or if any I/O operation is in flight (see the module docs):
    /// that is the checked replacement for the old "no I/O promises may be outstanding" caller
    /// contract.
    fn take(&self) -> Result<Box<Self>> {
        let mut slot = self.cell.inner.try_borrow_mut().map_err(|_| {
            KjIoError::other(
                "kj_rs_io",
                "cannot unwrap a stream while I/O operations are in flight on it",
            )
        })?;
        let inner = slot.take().ok_or_else(|| {
            KjIoError::other("kj_rs_io", "stream was already unwrapped (hollow wrapper)")
        })?;
        Ok(Box::new(Self {
            cell: Rc::new(StreamCell {
                inner: RefCell::new(Some(inner)),
            }),
        }))
    }
}

// ======================================================================================
// Bridge entry points (see ffi.rs; `stream_try_read` lives there because it receives the raw
// buffer pointer). None of these captures the `&TokioStream`: see the module docs.

pub fn stream_write<'b>(
    stream: &TokioStream,
    buf: &'b [u8],
) -> impl Future<Output = Result<()>> + use<'b> {
    stream.write_all(buf)
}

pub fn stream_write_pieces<'b>(
    stream: &TokioStream,
    pieces: &'b crate::ffi::KjPieces,
) -> impl Future<Output = Result<()>> + use<'b> {
    stream.write_all_pieces(pieces)
}

pub fn stream_when_write_disconnected(
    stream: &TokioStream,
) -> impl Future<Output = Result<()>> + use<> {
    stream.when_write_disconnected()
}

pub fn stream_shutdown_write(stream: &TokioStream) -> Result<()> {
    stream.shutdown_write()
}

pub fn stream_abort_read(stream: &TokioStream) -> Result<()> {
    stream.abort_read()
}

/// Non-throwing variant for `kj::AsyncIoStream::getFd()`/`getWin32Handle()`, which return a
/// `kj::Maybe`: -1 when the wrapper is hollow, so C++ does not have to use exception catching
/// as control flow.
pub fn stream_try_raw_handle(stream: &TokioStream) -> i64 {
    stream.raw_handle().unwrap_or(-1)
}

pub fn stream_local_addr(stream: &TokioStream) -> Result<Vec<u8>> {
    stream.local_addr_bytes()
}

pub fn stream_peer_addr(stream: &TokioStream) -> Result<Vec<u8>> {
    stream.peer_addr_bytes()
}

pub fn stream_peer_credentials(stream: &TokioStream) -> Result<crate::ffi::PeerCredentials> {
    stream.peer_credentials()
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
// (async-io.c++) routes win32 wrapInputFd/wrapOutputFd through the socket path and ffi.rs
// never constructs these types there. The same ownership model as TokioStream applies: an
// operation owns a share of the registration, so destroying the wrapper mid-read is safe.

#[cfg(unix)]
type FdIo = tokio::io::unix::AsyncFd<std::os::fd::OwnedFd>;

/// A readable fd behind `kj::AsyncInputStream`.
pub struct TokioInputFd {
    #[cfg(unix)]
    inner: Rc<FdIo>,
}

/// A writable fd behind `kj::AsyncOutputStream`.
pub struct TokioOutputFd {
    #[cfg(unix)]
    inner: Rc<FdIo>,
}

#[cfg(unix)]
fn register_fd(fd: std::os::fd::OwnedFd, interest: Interest) -> Result<Rc<FdIo>> {
    crate::runtime::require_loop_runtime()?;
    Ok(Rc::new(
        tokio::io::unix::AsyncFd::with_interest(fd, interest).map_err(op("wrapFd"))?,
    ))
}

#[cfg(unix)]
impl TokioInputFd {
    /// Registers an owned, non-blocking readable fd with the loop runtime's I/O driver.
    pub(crate) fn new(fd: std::os::fd::OwnedFd) -> Result<Self> {
        Ok(Self {
            inner: register_fd(fd, Interest::READABLE)?,
        })
    }

    /// KJ `tryRead` semantics over the fd (see [`StreamCell::try_read_min`]); `buf` may be
    /// uninitialized.
    pub(crate) fn try_read_min<'b>(
        &self,
        buf: &'b mut [MaybeUninit<u8>],
        min_bytes: usize,
    ) -> impl Future<Output = Result<usize>> + use<'b> {
        use std::os::fd::AsFd;
        let io = Rc::clone(&self.inner);
        async move {
            let min_bytes = min_bytes.min(buf.len());
            let mut total = 0;
            while total < buf.len() {
                let mut guard = io.ready(Interest::READABLE).await.map_err(op("poll()"))?;
                match guard.try_io(|inner| {
                    crate::ffi::read_uninit(inner.get_ref().as_fd(), &mut buf[total..])
                }) {
                    Ok(Ok(0)) => break, // EOF
                    Ok(Ok(n)) => {
                        total += n;
                        if total >= min_bytes {
                            break;
                        }
                    }
                    Ok(Err(e)) if e.kind() == std::io::ErrorKind::Interrupted => {}
                    Ok(Err(e)) => return Err(op("read()")(e)),
                    Err(_would_block) => {
                        if total >= min_bytes {
                            break;
                        }
                    }
                }
            }
            Ok(total)
        }
    }
}

#[cfg(unix)]
impl TokioOutputFd {
    /// Registers an owned, non-blocking writable fd with the loop runtime's I/O driver.
    pub(crate) fn new(fd: std::os::fd::OwnedFd) -> Result<Self> {
        Ok(Self {
            inner: register_fd(fd, Interest::WRITABLE)?,
        })
    }

    /// Write-all semantics over the fd.
    fn write_all<'b>(&self, buf: &'b [u8]) -> impl Future<Output = Result<()>> + use<'b> {
        let io = Rc::clone(&self.inner);
        async move { Self::write_all_on(&io, buf).await }
    }

    /// Write-all over a share of the registration (the body of [`TokioOutputFd::write_all`]
    /// and of the multi-piece write).
    async fn write_all_on(io: &FdIo, buf: &[u8]) -> Result<()> {
        use std::os::fd::AsFd;
        let mut written = 0;
        while written < buf.len() {
            let mut guard = io.ready(Interest::WRITABLE).await.map_err(op("poll()"))?;
            match guard
                .try_io(|inner| crate::ffi::write_fd(inner.get_ref().as_fd(), &buf[written..]))
            {
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
    }
}

/// Every piece in order, write-all, as one operation owning its share of the fd.
pub fn output_fd_write_pieces<'b>(
    stream: &TokioOutputFd,
    pieces: &'b crate::ffi::KjPieces,
) -> impl Future<Output = Result<()>> + use<'b> {
    #[cfg(unix)]
    {
        let io = Rc::clone(&stream.inner);
        async move {
            let count = crate::ffi::kj_pieces_count(pieces);
            for index in 0..count {
                let piece = crate::ffi::kj_piece(pieces, index);
                if !piece.is_empty() {
                    TokioOutputFd::write_all_on(&io, piece).await?;
                }
            }
            Ok(())
        }
    }
    #[cfg(windows)]
    {
        let _ = (stream, pieces);
        std::future::ready(Err(KjIoError::other(
            "write()",
            "kj-rs-io's pipe-fd tier is unix only",
        )))
    }
}

pub fn output_fd_write<'b>(
    stream: &TokioOutputFd,
    buf: &'b [u8],
) -> impl Future<Output = Result<()>> + use<'b> {
    #[cfg(unix)]
    {
        stream.write_all(buf)
    }
    #[cfg(windows)]
    {
        let _ = (stream, buf);
        std::future::ready(Err(KjIoError::other(
            "write()",
            "kj-rs-io's pipe-fd tier is unix only",
        )))
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

    // `TokioStream` is a KJ-loop-thread handle (an Rc whose in-flight tracking lives in a
    // RefCell, see the module docs): neither Send nor Sync. The native socket it yields is Send.
    assert_not_impl_any!(TokioStream: Send, Sync);
    assert_impl_all!(TcpStream: Send);

    /// A connected localhost TCP pair as tokio streams registered with `port`'s runtime.
    // Takes the port only to make the caller prove one exists: the thread must be inside the
    // port's runtime context for `TcpStream::from_std` to find the I/O driver.
    fn connected_pair(_port: &kj_rs_tokio::TokioPort) -> (TokioStream, std::net::TcpStream) {
        let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let client = std::net::TcpStream::connect(listener.local_addr().unwrap()).unwrap();
        let (server, _) = listener.accept().unwrap();
        server.set_nonblocking(true).unwrap();
        // No `enter()`: the thread is inside the port's runtime context for the port's life.
        (
            TokioStream::from_tcp(TcpStream::from_std(server).unwrap()),
            client,
        )
    }

    fn poll_once<T>(fut: &mut std::pin::Pin<Box<impl Future<Output = T>>>) -> std::task::Poll<T> {
        let mut cx = Context::from_waker(Waker::noop());
        fut.as_mut().poll(&mut cx)
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
            taken.cell.live().is_ok(),
            "the taken wrapper holds the live stream"
        );

        // The original is hollow now.
        let err = |r: Result<()>| match r {
            Ok(()) => panic!("expected an error from a hollow wrapper"),
            Err(e) => KjError::from(e).description().to_owned(),
        };
        assert!(stream.cell.live().is_err());
        assert!(err(stream.shutdown_write()).contains("hollow"));
        assert!(err(stream.abort_read()).contains("hollow"));
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
        // interest and returns Pending while holding its borrow of the native stream.
        let mut buf = [MaybeUninit::<u8>::uninit(); 8];
        let mut read = Box::pin(stream.try_read_min(&mut buf, 1));
        assert!(poll_once(&mut read).is_pending());

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

    /// The ownership model of the module docs: a pending operation owns a share of the socket,
    /// so dropping the handle (the C++ wrapper's Box) while the read is in flight neither
    /// dangles nor closes the socket; the read is cancelled cleanly afterwards.
    #[test]
    fn dropping_the_handle_with_a_read_in_flight_is_safe() {
        use std::io::Read;
        let port = kj_rs_tokio::TokioPort::new();
        let (stream, client) = connected_pair(&port);

        let mut buf = [MaybeUninit::<u8>::uninit(); 8];
        let mut read = Box::pin(stream.try_read_min(&mut buf, 1));
        assert!(poll_once(&mut read).is_pending());
        assert_eq!(Rc::strong_count(&stream.cell), 2, "the read owns a share");

        drop(stream);
        // The socket is still open: the peer does not see EOF (a non-blocking read blocks).
        client.set_nonblocking(true).unwrap();
        let mut probe = [0u8; 1];
        assert_eq!(
            (&client).read(&mut probe).map_err(|e| e.kind()).err(),
            Some(std::io::ErrorKind::WouldBlock),
            "peer must not observe a close while the read owns the socket"
        );
        assert!(poll_once(&mut read).is_pending());
        drop(read); // releases the last share: now the socket closes
        let n = (&client).read(&mut probe);
        assert!(
            matches!(n, Ok(0)) || n.is_err(),
            "peer observes the close: {n:?}"
        );
    }

    #[test]
    fn concurrent_read_and_write_both_in_flight_share_the_borrow() {
        // The load-bearing invariant of the RefCell design: shared borrows stack, so a read and
        // a write can be in flight at once (kj's one-read + one-write contract), and `take()`
        // fails while EITHER is alive, succeeding only once BOTH are dropped.
        let port = kj_rs_tokio::TokioPort::new();
        let (stream, _client) = connected_pair(&port);

        let mut buf = [MaybeUninit::<u8>::uninit(); 8];
        let mut read = Box::pin(stream.try_read_min(&mut buf, 1));
        // 1 MiB: larger than the socket buffer, so the write cannot drain in one go and the
        // future stays pending, holding its borrow.
        let payload = vec![0u8; 1024 * 1024];
        let mut write = Box::pin(stream.write_all(&payload));
        assert!(poll_once(&mut read).is_pending());
        assert!(poll_once(&mut write).is_pending());
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
        // The original is hollow; recovering a ServeIo yields None.
        assert!(stream.into_serve_io().is_none());
    }

    #[cfg(unix)]
    #[test]
    fn write_disconnected_wait_counts_as_in_flight() {
        let port = kj_rs_tokio::TokioPort::new();
        let (stream, _client) = connected_pair(&port);
        let mut wait = Box::pin(stream.when_write_disconnected());
        assert!(poll_once(&mut wait).is_pending());
        assert!(
            stream.take().is_err(),
            "whenWriteDisconnected holds the stream too"
        );
        drop(wait);
        assert!(stream.take().is_ok());
    }

    /// The hangup watcher's `dup(2)` is per stream, not per call: the first
    /// `whenWriteDisconnected` creates it, and every later wait -- concurrent or not -- reuses
    /// the same descriptor and registration.
    #[cfg(unix)]
    #[test]
    fn write_disconnected_dups_the_socket_once_per_stream() {
        use std::os::fd::AsRawFd;
        let port = kj_rs_tokio::TokioPort::new();
        let (stream, _client) = connected_pair(&port);
        let watch_fd = || {
            stream
                .cell
                .live()
                .unwrap()
                .hangup_watch
                .get()
                .map(|afd| afd.get_ref().as_raw_fd())
        };
        assert_eq!(watch_fd(), None, "no dup before the first wait");
        let mut first = Box::pin(stream.when_write_disconnected());
        assert!(poll_once(&mut first).is_pending());
        let dup = watch_fd().expect("the first wait created the dup");
        assert_ne!(
            i64::from(dup),
            stream.raw_handle().unwrap(),
            "it is a dup, not the socket"
        );
        let mut second = Box::pin(stream.when_write_disconnected());
        assert!(poll_once(&mut second).is_pending());
        assert_eq!(watch_fd(), Some(dup), "a concurrent wait reuses it");
        drop(first);
        drop(second);
        let mut third = Box::pin(stream.when_write_disconnected());
        assert!(poll_once(&mut third).is_pending());
        assert_eq!(watch_fd(), Some(dup), "a later wait reuses it too");
    }
}
