//! Tokio-backed `kj::AsyncIoStream` backend.
//!
//! # Operations own their state
//!
//! A [`TokioStream`] is a handle to an `Arc`-shared [`Inner`] holding the native socket. Every
//! bridged operation clones that `Arc` into the future it returns and never touches the
//! `&TokioStream` again: the entry points are `fn -> impl Future + use<..>`, capturing no borrow
//! of the handle. So the C++ wrapper that owns the handle may be destroyed while a read is
//! pending -- a KJ contract violation, but one `kj::AsyncIoStream` consumers can commit -- and
//! nothing dangles: the socket lives until the last operation settles or is cancelled. The
//! handle is `Send + Sync` by type (lib.rs, "Threads").
//!
//! # When the syscall happens: tokio's semantics, on purpose
//!
//! KJ's `AsyncStreamFd` issues `read(2)` / `write(2)` synchronously inside the call and only
//! waits for readiness on `EAGAIN`, so a KJ caller may drop a write's promise on the spot and
//! the bytes are still out. tokio works the other way round: `try_read` / `try_write` issue a
//! syscall only when the driver has already reported readiness for the descriptor and return
//! `WouldBlock` otherwise, and a future does nothing until it is polled. **This crate
//! deliberately keeps tokio's semantics rather than reproducing KJ's.** The long-term direction
//! is to move workerd's I/O onto tokio, so kj-rs-io is written as the tokio program it will be
//! part of, not as an emulation of KJ over tokio; reproducing the KJ behavior meant bypassing
//! tokio's readiness model with direct socket2 / nix / libc syscalls, which is exactly the kind
//! of hand-written I/O this crate is meant to retire. The observable difference is narrow: a
//! write whose promise is dropped without ever being awaited, as the *first* operation on a
//! descriptor tokio has not yet seen ready, is not sent. workerd's full test suite under the
//! Rust backend has no such case (verified 2026-09-13); the C++ adapters still start every
//! returned promise inside the call (async-io.c++ `started()`), so an operation that is kept
//! but never awaited completes as the loop turns, and on a descriptor the driver has already
//! seen ready the first poll does issue the syscall.
//!
//! # Read buffers
//!
//! KJ callers may hand `tryRead` uninitialized storage, so reads take `&mut [MaybeUninit<u8>]`
//! (ffi.rs builds it from the raw pointer that crosses the bridge); such a slice is a
//! `bytes::BufMut`, which tokio's `try_read_buf` fills without reading it. [`Socket`] dispatches
//! the two socket families so KJ's `tryRead` semantics live in one read loop and one write loop:
//! on `WouldBlock` wait for readiness (whatever `minBytes` is -- KJ's `tryReadInternal` does
//! too), return at EOF or once `minBytes` is satisfied.
//!
//! # whenWriteDisconnected costs a descriptor
//!
//! KJ's `whenWriteDisconnected` watches the socket for `EPOLLHUP` / `EPOLLERR` on the same fd
//! its reads and writes use. tokio has one readiness registration per socket, and waiting on it
//! for a hangup means clearing its write readiness on every spurious wake, which would park a
//! concurrent writer for good (`try_write` consults that cached readiness before it issues a
//! syscall). So the watch registers a `dup(2)` of the socket, lazily, once per stream. kj-http
//! calls `whenWriteDisconnected` on every served connection, so under this backend a workerd
//! process holding N connections holds about 2N descriptors; that is the price of early
//! client-disconnect detection (KJ permits a never-resolving promise, and the Windows arm returns
//! one), accepted here so workerd keeps cancelling work for clients that went away. Size the fd
//! limit accordingly.

use std::future::Future;
use std::io::IoSlice;
use std::mem::MaybeUninit;
use std::sync::Arc;

use cxx::KjError;
use tokio::io::Interest;
use tokio::net::TcpStream;
#[cfg(unix)]
use tokio::net::UnixStream;

use crate::error::KjIoError;
use crate::error::Result;
use crate::error::op;
use crate::ffi::SocketAddress;

/// A connected tokio stream (TCP or Unix domain) behind a `kj::AsyncIoStream`. See the module
/// docs for its ownership model.
pub struct TokioStream {
    inner: Arc<Inner>,
}

/// The shared state behind a [`TokioStream`] and every operation in flight on it.
struct Inner {
    socket: Socket,
    /// The runtime `socket` is registered with (lib.rs, `ensure_owner_loop`).
    owner: tokio::runtime::Id,
    /// The `dup(2)` + I/O-driver registration behind `whenWriteDisconnected`, created on the
    /// first call and shared by every later one (kj-http calls it once per server connection;
    /// KJ itself forks one observation per stream). One extra fd per stream at most, not per
    /// call. See [`Inner::when_write_disconnected`] for why it is a separate registration at
    /// all.
    #[cfg(unix)]
    hangup_watch: std::sync::OnceLock<tokio::io::unix::AsyncFd<std::os::fd::OwnedFd>>,
    /// Set by `abortRead()`. A read parked on readiness is woken through `read_abort` and returns
    /// EOF; later reads return EOF without touching the socket. tokio issues `recv` only once the
    /// poller reports readability, and not every poller reports a *local* receive shutdown (AFD
    /// on Windows does not; epoll and kqueue do), so the stream records the abort itself rather
    /// than relying on `shutdown(2)` to surface as an event.
    read_aborted: std::sync::atomic::AtomicBool,
    read_abort: tokio::sync::Notify,
}

/// A registered tokio socket of either family.
///
/// Registered on the loop thread (lib.rs, "The tokio runtime"): by net.rs, or by a Rust server
/// handing its own socket to C++ as a `kj::AsyncIoStream` (kj-hyper).
pub enum Socket {
    Tcp(TcpStream),
    #[cfg(unix)]
    Unix(UnixStream),
}

impl Socket {
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

    /// Runs `f` on a `socket2::SockRef` borrowing the live socket: the socket-option surface
    /// (`shutdown` / `getsockname` / `getpeername`) on either platform.
    fn with_sock_ref<T>(&self, f: impl FnOnce(&socket2::SockRef<'_>) -> T) -> T {
        #[cfg(unix)]
        let sock = self.as_borrowed_fd();
        #[cfg(windows)]
        let sock = self.as_borrowed_socket();
        f(&socket2::SockRef::from(&sock))
    }

    async fn ready(&self, interest: Interest) -> std::io::Result<()> {
        match self {
            Self::Tcp(s) => s.ready(interest).await.map(|_| ()),
            #[cfg(unix)]
            Self::Unix(s) => s.ready(interest).await.map(|_| ()),
        }
    }

    fn try_read(&self, buf: &mut [MaybeUninit<u8>]) -> std::io::Result<usize> {
        // `&mut [MaybeUninit<u8>]` is a `bytes::BufMut`; `try_read_buf` fills it without
        // reading it and advances it past what was read.
        let mut buf = buf;
        match self {
            Self::Tcp(s) => s.try_read_buf(&mut buf),
            #[cfg(unix)]
            Self::Unix(s) => s.try_read_buf(&mut buf),
        }
    }

    async fn readable(&self) -> std::io::Result<()> {
        self.ready(Interest::READABLE).await
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

    async fn writable(&self) -> std::io::Result<()> {
        self.ready(Interest::WRITABLE).await
    }

    /// `getsockname(2)`, typed (ffi.rs `SocketAddress`).
    fn local_addr(&self) -> std::io::Result<SocketAddress> {
        match self {
            Self::Tcp(s) => s.local_addr().map(SocketAddress::from),
            #[cfg(unix)]
            Self::Unix(s) => s
                .local_addr()
                .map(|addr| SocketAddress::from(&std::os::unix::net::SocketAddr::from(addr))),
        }
    }

    /// `getpeername(2)`, typed.
    fn peer_addr(&self) -> std::io::Result<SocketAddress> {
        match self {
            Self::Tcp(s) => s.peer_addr().map(SocketAddress::from),
            #[cfg(unix)]
            Self::Unix(s) => s
                .peer_addr()
                .map(|addr| SocketAddress::from(&std::os::unix::net::SocketAddr::from(addr))),
        }
    }
}

/// A zero-byte result from a non-empty write: the peer is gone (`send` reports "would block" as
/// an error, never as `Ok(0)`). Classified DISCONNECTED, like `EPIPE`.
fn wrote_zero_bytes() -> std::io::Error {
    std::io::Error::new(
        std::io::ErrorKind::ConnectionReset,
        "wrote zero bytes (connection closed)",
    )
}

fn would_block(error: &std::io::Error) -> bool {
    error.kind() == std::io::ErrorKind::WouldBlock
}

fn interrupted(error: &std::io::Error) -> bool {
    error.kind() == std::io::ErrorKind::Interrupted
}

/// KJ `tryRead` semantics (`AsyncStreamFd::tryReadInternal`, kj/async-io-unix.c++) in tokio's
/// try-then-wait shape: on `WouldBlock` wait for readability and try again, whatever `min_bytes`
/// is; stop at EOF, or once at least `min_bytes` have arrived (so `min_bytes == 0` returns after
/// the first read that yields data), or when the buffer is full. `buf` may be uninitialized;
/// only the returned count's worth of leading bytes is initialized afterwards.
async fn read_min(inner: &Inner, buf: &mut [MaybeUninit<u8>], min_bytes: usize) -> Result<usize> {
    let min_bytes = min_bytes.min(buf.len());
    let mut total = 0;
    while total < buf.len() {
        if inner
            .read_aborted
            .load(std::sync::atomic::Ordering::Acquire)
        {
            break; // abortRead(): EOF, as KJ's shutdown(SHUT_RD) read would observe.
        }
        match inner.socket.try_read(&mut buf[total..]) {
            Ok(0) => break, // EOF: fewer than min_bytes tells KJ so.
            Ok(n) => {
                total += n;
                if total >= min_bytes {
                    break;
                }
            }
            Err(e) if would_block(&e) => inner.wait_readable().await?,
            Err(e) if interrupted(&e) => {}
            Err(e) => return Err(op("read()")(e)),
        }
    }
    Ok(total)
}

/// Write-all semantics, same shape as [`read_min`].
async fn write_all(inner: &Inner, buf: &[u8]) -> Result<()> {
    let mut written = 0;
    while written < buf.len() {
        match inner.socket.try_write(&buf[written..]) {
            Ok(0) => return Err(op("write()")(wrote_zero_bytes())),
            Ok(n) => written += n,
            Err(e) if would_block(&e) => inner.wait_writable().await?,
            Err(e) if interrupted(&e) => {}
            Err(e) => return Err(op("write()")(e)),
        }
    }
    Ok(())
}

/// Write-all semantics over several pieces, as one operation: `writev` until every piece is
/// fully written. `IoSlice::advance_slices` drops fully-written leading pieces and trims a
/// partially written one, so a short write resumes exactly where the kernel stopped. The
/// `IOV_MAX` batching KJ's `writeInternal` does is `std`'s job here: its unix `write_vectored`
/// clamps the iovec count to `sysconf(_SC_IOV_MAX)` (library/std/src/sys/fd/unix.rs,
/// `max_iov`), and winsock's `WSASend` has no such limit.
///
/// Empty pieces are dropped up front: a leading run of them would write zero bytes --
/// indistinguishable from a closed connection -- while data still waits further down.
async fn write_all_pieces(inner: &Inner, pieces: &crate::ffi::KjPieces) -> Result<()> {
    let count = crate::ffi::kj_pieces_count(pieces);
    let mut slices = Vec::with_capacity(count);
    for index in 0..count {
        // Unreachable by construction (index < count), but cxx makes the bounds check fallible.
        let piece = crate::ffi::kj_piece(pieces, index)
            .map_err(|e| KjIoError::verbatim(cxx::KjError::from(e).description()))?;
        if !piece.is_empty() {
            slices.push(IoSlice::new(piece));
        }
    }
    let mut bufs: &mut [IoSlice<'_>] = &mut slices;
    while !bufs.is_empty() {
        match inner.socket.try_write_vectored(bufs) {
            Ok(0) => return Err(op("writev()")(wrote_zero_bytes())),
            Ok(n) => IoSlice::advance_slices(&mut bufs, n),
            Err(e) if would_block(&e) => inner.wait_writable().await?,
            Err(e) if interrupted(&e) => {}
            Err(e) => return Err(op("writev()")(e)),
        }
    }
    Ok(())
}

impl Inner {
    /// Waits for readability or for `abortRead()`, whichever comes first; the caller re-checks
    /// `read_aborted` before reading. `Notified::enable` registers the waiter *before* the flag
    /// is consulted, so an abort landing between the two is not lost (`notify_waiters` wakes
    /// only registered waiters).
    async fn wait_readable(&self) -> Result<()> {
        crate::ensure_owner_loop(self.owner)?;
        let mut aborted = std::pin::pin!(self.read_abort.notified());
        aborted.as_mut().enable();
        if self.read_aborted.load(std::sync::atomic::Ordering::Acquire) {
            return Ok(());
        }
        let mut readable = std::pin::pin!(self.socket.readable());
        std::future::poll_fn(|cx| {
            if aborted.as_mut().poll(cx).is_ready() {
                return std::task::Poll::Ready(Ok(()));
            }
            readable.as_mut().poll(cx).map_err(op("poll()"))
        })
        .await
    }

    /// Waits for writability; the owner-loop check happens here, on the slow path only.
    async fn wait_writable(&self) -> Result<()> {
        crate::ensure_owner_loop(self.owner)?;
        self.socket.writable().await.map_err(op("poll()"))
    }

    /// Resolves once new writes are doomed to fail (peer reset / hangup observed).
    ///
    /// tokio has no direct primitive for this, so we register a *duplicate* of the socket fd
    /// with the I/O driver for WRITABLE interest and wait -- explicitly clearing plain-writable
    /// readiness -- until the OS reports write-closed (kqueue: `EV_EOF` on the write filter;
    /// epoll: `EPOLLHUP`, or `EPOLLERR`, which mio folds into write-closed). Like KJ's own
    /// implementation this does *not* fire on a mere half-close (peer FIN / `EPOLLRDHUP`): reads
    /// hitting EOF must not count as write-disconnect.
    ///
    /// Why a second fd rather than the socket's own registration: this waiter must clear the
    /// WRITABLE readiness to avoid spinning on an always-writable socket, and clearing it on the
    /// socket's own registration would park a concurrent writer's `writable()` on an edge that
    /// never comes. A `dup(2)` is its own registration, so clearing its readiness disturbs no
    /// one. The dup is created once per stream and shared by every call (concurrent waiters all
    /// want the same edge, and none of them writes through it); as the one registration made
    /// after construction it runs the same loop-thread check the constructors do.
    ///
    /// Windows behavior (see the arm below): a never-resolving future, which IS KJ parity --
    /// capnproto's win32 `whenWriteDisconnected` returns `NEVER_DONE` today (its
    /// `IOCTL_AFD_POLL` idea, the mechanism `select()` is built on, is only a TODO there).
    #[cfg(unix)]
    async fn when_write_disconnected(&self) -> Result<()> {
        let async_fd = if let Some(existing) = self.hangup_watch.get() {
            existing
        } else {
            // Borrow the live socket's fd and dup it, so no raw fd is ever materialized without
            // an owner. The dup shares the underlying open socket (and its O_NONBLOCK status),
            // but has its own registration with the I/O driver.
            let owned = self
                .socket
                .as_borrowed_fd()
                .try_clone_to_owned()
                .map_err(op("dup()"))?;
            crate::ensure_owner_loop(self.owner)?;
            let async_fd = tokio::io::unix::AsyncFd::with_interest(owned, Interest::WRITABLE)
                .map_err(op("whenWriteDisconnected"))?;
            // A concurrent first caller may have stored its own registration meanwhile; then
            // this one is dropped (closing its dup) and the stored one is shared.
            self.hangup_watch.get_or_init(|| async_fd)
        };
        loop {
            let mut guard = async_fd
                .ready(Interest::WRITABLE)
                .await
                .map_err(op("whenWriteDisconnected"))?;
            if guard.ready().is_write_closed() {
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
        // `pending()` infers the Result<()> return type, so there is no unreachable tail
        // (crate-level deny(clippy::unreachable)).
        std::future::pending().await
    }
}

impl TokioStream {
    /// Wraps a socket registered with the loop's runtime.
    ///
    /// # Errors
    ///
    /// Off the loop thread (lib.rs, "The tokio runtime").
    pub fn new(socket: Socket) -> Result<Self> {
        Ok(Self {
            inner: Arc::new(Inner {
                socket,
                owner: crate::current_loop_runtime_id()?,
                #[cfg(unix)]
                hangup_watch: std::sync::OnceLock::new(),
                read_aborted: std::sync::atomic::AtomicBool::new(false),
                read_abort: tokio::sync::Notify::new(),
            }),
        })
    }

    /// A share of the state, for an operation's future to own (see the module docs).
    fn shared(&self) -> Arc<Inner> {
        Arc::clone(&self.inner)
    }

    /// Recovers the native tokio socket, consuming the wrapper (the reverse of
    /// `TokioAsyncIoStream::release()` handing the stream back to Rust). The socket stays
    /// registered with the loop runtime that created it.
    ///
    /// Refuses -- handing the wrapper back untouched -- while any operation still holds a share
    /// of the state (an I/O future in flight, an outstanding `whenWriteDisconnected()`), or off
    /// the owning loop: that is the checked half of KJ's "no I/O in flight when handing over a
    /// stream" contract, detected here rather than assumed.
    ///
    /// # Errors
    ///
    /// The wrapper plus the reason, so the caller can keep using (or re-wrap) the stream.
    pub fn into_socket(self: Box<Self>) -> std::result::Result<Socket, (Box<Self>, KjError)> {
        if let Err(error) = crate::ensure_owner_loop(self.inner.owner) {
            return Err((self, error.into()));
        }
        let Self { inner } = *self;
        match Arc::try_unwrap(inner) {
            Ok(inner) => Ok(inner.socket),
            Err(inner) => Err((
                Box::new(Self { inner }),
                KjIoError::other(
                    "kj_rs_io",
                    "cannot take the stream's socket: an I/O operation is still in flight on it",
                )
                .into(),
            )),
        }
    }

    /// `kj::AsyncIoStream::tryRead` as a future owning its share of the stream. `buf` may be
    /// uninitialized; it is the caller's, valid until the future settles (KJ's contract).
    pub(crate) fn try_read_min<'b>(
        &self,
        buf: &'b mut [MaybeUninit<u8>],
        min_bytes: usize,
    ) -> impl Future<Output = Result<usize>> + use<'b> {
        let inner = self.shared();
        async move { read_min(&inner, buf, min_bytes).await }
    }

    /// `kj::AsyncIoStream::write` (write-all) as a future owning its share of the stream.
    pub(crate) fn write_all<'b>(
        &self,
        buf: &'b [u8],
    ) -> impl Future<Output = Result<()>> + use<'b> {
        let inner = self.shared();
        async move { write_all(&inner, buf).await }
    }

    /// `kj::AsyncIoStream::write(pieces)` as a future owning its share of the stream.
    pub(crate) fn write_all_pieces<'b>(
        &self,
        pieces: &'b crate::ffi::KjPieces,
    ) -> impl Future<Output = Result<()>> + use<'b> {
        let inner = self.shared();
        async move { write_all_pieces(&inner, pieces).await }
    }

    /// `kj::AsyncIoStream::whenWriteDisconnected` as a future owning its share of the stream.
    pub(crate) fn when_write_disconnected(&self) -> impl Future<Output = Result<()>> + use<> {
        let inner = self.shared();
        async move { inner.when_write_disconnected().await }
    }

    /// `shutdown(SHUT_WR)`: cleanly shut down the write end, keeping the read end open.
    /// `shutdown(2)` acts on the socket, not on a descriptor, so a `SockRef` borrow of the live
    /// socket is all it needs, identical on unix and windows.
    fn shutdown_write(&self) -> Result<()> {
        self.shared()
            .socket
            .with_sock_ref(|sock| sock.shutdown(std::net::Shutdown::Write))
            .map_err(op("shutdown(SHUT_WR)"))
    }

    /// `kj::AsyncIoStream::abortRead`: a pending read observes EOF and later reads return EOF
    /// (kj-http and the WebSocket abort path rely on this to terminate reads), on every platform
    /// -- the stream records the abort and wakes the parked read itself (see [`Inner`]) -- plus
    /// `shutdown(SHUT_RD)` as KJ's `AsyncStreamFd::abortRead` does, so the peer-facing effect is
    /// KJ's too. Errors surface as KJ's own `KJ_SYSCALL` would.
    fn abort_read(&self) -> Result<()> {
        let inner = self.shared();
        inner
            .read_aborted
            .store(true, std::sync::atomic::Ordering::Release);
        inner.read_abort.notify_waiters();
        inner
            .socket
            .with_sock_ref(|sock| sock.shutdown(std::net::Shutdown::Read))
            .map_err(op("shutdown(SHUT_RD)"))
    }

    /// Raw `struct sockaddr` bytes of the socket's locally-bound address (the `getsockname()`
    /// passthrough behind `kj::AsyncIoStream::getsockname`).
    fn local_addr(&self) -> Result<SocketAddress> {
        self.inner.socket.local_addr().map_err(op("getsockname()"))
    }

    /// Raw `struct sockaddr` bytes of the connected peer's address (the `getpeername()`
    /// passthrough behind `kj::AsyncIoStream::getpeername`).
    fn peer_addr(&self) -> Result<SocketAddress> {
        self.inner.socket.peer_addr().map_err(op("getpeername()"))
    }

    /// The connected Unix-domain peer's process credentials, with KJ's validity rules
    /// (`kj/async-io-unix.c++` `getIdentity`): a pid is reported only if positive, a uid only
    /// if not `(uid_t)-1`. tokio reads `SO_PEERCRED` (Linux) / `LOCAL_PEERCRED` +
    /// `LOCAL_PEERPID` (macOS, BSDs) for us.
    fn peer_credentials(&self) -> Result<crate::ffi::PeerCredentials> {
        match &self.shared().socket {
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
    fn raw_handle(&self) -> i64 {
        #[cfg(unix)]
        {
            use std::os::fd::AsRawFd;
            i64::from(self.inner.socket.as_borrowed_fd().as_raw_fd())
        }
        #[cfg(windows)]
        {
            use std::os::windows::io::AsRawSocket;
            // A live SOCKET fits in i64 (Windows handles fit in 32 bits); the bridge carries
            // its bits verbatim.
            #[allow(clippy::cast_possible_wrap)]
            (self.inner.socket.as_borrowed_socket().as_raw_socket() as i64)
        }
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

pub fn stream_raw_handle(stream: &TokioStream) -> i64 {
    stream.raw_handle()
}

pub fn stream_local_addr(stream: &TokioStream) -> Result<SocketAddress> {
    stream.local_addr()
}

pub fn stream_peer_addr(stream: &TokioStream) -> Result<SocketAddress> {
    stream.peer_addr()
}

pub fn stream_peer_credentials(stream: &TokioStream) -> Result<crate::ffi::PeerCredentials> {
    stream.peer_credentials()
}

#[cfg(test)]
mod tests {
    use std::io::Read;
    use std::task::Context;
    use std::task::Waker;

    use static_assertions::assert_impl_all;

    use super::*;

    // Send + Sync like every kj-rs-io handle (lib.rs asserts this at compile time).
    assert_impl_all!(TokioStream: Send, Sync);

    /// A connected localhost TCP pair: the server end as a kj-rs-io stream registered with
    /// `port`'s runtime, the client end as a std socket.
    // Takes the port only to make the caller prove one exists (registration needs it).
    fn connected_pair(_port: &kj_rs_tokio::TokioPort) -> (TokioStream, std::net::TcpStream) {
        let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let client = std::net::TcpStream::connect(listener.local_addr().unwrap()).unwrap();
        let (server, _) = listener.accept().unwrap();
        server.set_nonblocking(true).unwrap();
        let server = TcpStream::from_std(server).unwrap();
        (TokioStream::new(Socket::Tcp(server)).unwrap(), client)
    }

    fn poll_once<T>(fut: &mut std::pin::Pin<Box<impl Future<Output = T>>>) -> std::task::Poll<T> {
        let mut cx = Context::from_waker(Waker::noop());
        fut.as_mut().poll(&mut cx)
    }

    /// The ownership model of the module docs: a pending operation owns a share of the socket,
    /// so dropping the handle (the C++ wrapper's Box) while the read is in flight neither
    /// dangles nor closes the socket; the read is cancelled cleanly afterwards.
    #[test]
    fn dropping_the_handle_with_a_read_in_flight_is_safe() {
        let port = kj_rs_tokio::TokioPort::new();
        let (stream, client) = connected_pair(&port);

        let mut buf = [MaybeUninit::<u8>::uninit(); 8];
        let mut read = Box::pin(stream.try_read_min(&mut buf, 1));
        assert!(poll_once(&mut read).is_pending());
        assert_eq!(
            Arc::strong_count(&stream.inner),
            2,
            "the handle and the read own a share"
        );

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
        // The close reaches the peer asynchronously (loopback still hands the FIN over after
        // close(2) returns), so wait for it rather than sampling once: blocking with a timeout.
        client.set_nonblocking(false).unwrap();
        client
            .set_read_timeout(Some(std::time::Duration::from_secs(5)))
            .unwrap();
        let n = (&client).read(&mut probe);
        assert!(
            matches!(n, Ok(0))
                || matches!(&n, Err(e) if e.kind() == std::io::ErrorKind::ConnectionReset),
            "peer observes the close (EOF, or a reset if unread data was pending): {n:?}"
        );
    }

    #[test]
    fn concurrent_read_and_write_both_in_flight_share_the_socket() {
        // A read and a write can be in flight at once (kj's one-read + one-write contract),
        // each owning a share; the socket outlives the handle until both are gone.
        let port = kj_rs_tokio::TokioPort::new();
        let (stream, _client) = connected_pair(&port);

        let mut buf = [MaybeUninit::<u8>::uninit(); 8];
        let mut read = Box::pin(stream.try_read_min(&mut buf, 1));
        // 1 MiB: larger than the socket buffer, so the write cannot drain in one go and the
        // future stays pending, holding its share.
        let payload = vec![0u8; 1024 * 1024];
        let mut write = Box::pin(stream.write_all(&payload));
        assert!(poll_once(&mut read).is_pending());
        assert!(poll_once(&mut write).is_pending());
        assert_eq!(Arc::strong_count(&stream.inner), 3);
        drop(read);
        assert_eq!(Arc::strong_count(&stream.inner), 2);
        drop(write);
        assert_eq!(Arc::strong_count(&stream.inner), 1);
    }

    /// The hangup watcher's `dup(2)` is per stream, not per call: the first
    /// `whenWriteDisconnected` creates it, and every later wait -- concurrent or not -- reuses
    /// the same descriptor and registration.
    /// `abortRead()` ends a read parked on readiness and makes later reads EOF, without relying on
    /// the poller reporting the local shutdown (it does not on Windows).
    #[test]
    fn abort_read_ends_a_parked_read_and_later_reads_with_eof() {
        let port = kj_rs_tokio::TokioPort::new();
        let (stream, _client) = connected_pair(&port);
        let mut buf = [MaybeUninit::<u8>::uninit(); 8];
        let mut read = Box::pin(stream.try_read_min(&mut buf, 1));
        assert!(
            poll_once(&mut read).is_pending(),
            "nothing to read yet: parked"
        );
        stream.abort_read().unwrap();
        assert!(
            matches!(poll_once(&mut read), std::task::Poll::Ready(Ok(0))),
            "the parked read observes EOF"
        );
        drop(read);
        let mut later = Box::pin(stream.try_read_min(&mut buf, 1));
        assert!(matches!(
            poll_once(&mut later),
            std::task::Poll::Ready(Ok(0))
        ));
    }

    /// A stream carried to another loop thread is memory-safe, but its socket is registered with
    /// its creator's driver: the first wait there fails instead of parking forever.
    #[test]
    fn a_read_parked_on_a_different_port_is_refused() {
        let port = kj_rs_tokio::TokioPort::new();
        let (stream, _client) = connected_pair(&port);
        let err = std::thread::spawn(move || {
            let _other_port = kj_rs_tokio::TokioPort::new();
            let mut buf = [MaybeUninit::<u8>::uninit(); 8];
            let mut read = Box::pin(stream.try_read_min(&mut buf, 1));
            match poll_once(&mut read) {
                std::task::Poll::Ready(Err(e)) => cxx::KjError::from(e),
                _ => panic!("a read on a foreign port must fail at its first wait"),
            }
        })
        .join()
        .unwrap();
        assert!(err.description().contains("different TokioEventPort"));
    }

    #[cfg(unix)]
    #[test]
    fn write_disconnected_dups_the_socket_once_per_stream() {
        use std::os::fd::AsRawFd;
        let port = kj_rs_tokio::TokioPort::new();
        let (stream, _client) = connected_pair(&port);
        let watch_fd = || {
            stream
                .inner
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
            stream.raw_handle(),
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

    /// The lazy hangup-watch registration is the one registration that happens after
    /// construction; it goes through the same loop-thread check as the constructors, so a
    /// foreign runtime entered over the loop thread is an error, not a registration with a
    /// driver that never turns.
    #[cfg(unix)]
    #[test]
    fn write_disconnected_refuses_to_register_under_a_foreign_runtime() {
        let port = kj_rs_tokio::TokioPort::new();
        let (stream, _client) = connected_pair(&port);
        let auxiliary = tokio::runtime::Builder::new_current_thread()
            .build()
            .unwrap();
        let _guard = auxiliary.enter();
        let mut wait = Box::pin(stream.when_write_disconnected());
        let std::task::Poll::Ready(Err(e)) = poll_once(&mut wait) else {
            panic!("must fail under a foreign runtime");
        };
        assert!(
            cxx::KjError::from(e)
                .description()
                .contains("other than this thread's TokioEventPort runtime")
        );
    }

    #[test]
    fn into_socket_takes_the_socket_when_nothing_is_in_flight() {
        let port = kj_rs_tokio::TokioPort::new();
        let (stream, _client) = connected_pair(&port);
        assert!(Box::new(stream).into_socket().is_ok());
        drop(port);
    }

    #[test]
    fn into_socket_while_an_operation_is_in_flight_hands_the_stream_back() {
        let port = kj_rs_tokio::TokioPort::new();
        let (stream, _client) = connected_pair(&port);
        let mut buf = [MaybeUninit::<u8>::uninit(); 8];
        // The read borrows only the shared state, not the handle, so the handle can be moved.
        let mut read = Box::pin(stream.try_read_min(&mut buf, 1));
        assert!(poll_once(&mut read).is_pending());
        let Err((stream, error)) = Box::new(stream).into_socket() else {
            panic!("into_socket() succeeded with a read in flight")
        };
        assert!(error.description().contains("in flight"), "{error:?}");
        // The handed-back wrapper is the same stream: still sharing with the read.
        assert_eq!(Arc::strong_count(&stream.inner), 2);
        drop(read);
        assert!(stream.into_socket().is_ok());
        drop(port);
    }
}
