//! Serving a `kj::AsyncIoStream` natively: the entry points hyper's server and stream-tier
//! client use to turn an owned kj stream into a tokio-side `AsyncRead + AsyncWrite`.
//!
//! Two tiers:
//!
//! - **Unwrap (native path).** A kj-rs-io stream gives up its tokio socket outright: the C++
//!   wrapper is consumed (`releaseTokioStream`, kj-stream.h) and `TokioStream::into_socket`
//!   takes the socket, or hands the stream back re-wrapped if an operation still holds a share
//!   of it. Zero copies, zero FFI crossings per read. A stream that is Rust underneath its kj
//!   wrapper already -- a `RustStream` (`rust_stream.rs`), e.g. the TLS streams of
//!   `tls-network.c++` -- is taken back out the same way when nothing still holds it.
//! - **Direct (foreign streams).** Anything else -- in-memory pipes, promised streams, tunnel
//!   streams, byte-transforming wrappers -- becomes a [`KjIo`], which polls the stream's own
//!   bridged `tryRead`/`write` promises inside `poll_read`/`poll_write`. A write completes when
//!   the kj stream has taken the bytes; dropping the `KjIo` destroys the stream and cancels
//!   whatever it had in flight -- kj's stream semantics, with no copy between. Never a foreign
//!   stream's fd: kj wrappers forward `getFd()` to their transport socket, so a TLS-shaped
//!   wrapper's fd carries ciphertext, not the stream's bytes.
//!
//! ```text
//! serve_kj_stream(KjOwn<AsyncIoStream>)  -> Result<ServeIo, TakeSocketError>
//!     |-- native path: wrapper consumed -> ServeIo::Tcp/Unix, or ServeIo::Rust
//!     `-- direct path: ServeIo::Kj, owning the KjOwn
//! take_kj_socket(KjOwn<AsyncIoStream>)   -> Result<ServeIo, TakeSocketError>   (kj-rs-io sockets only)
//! serve_lent_kj_stream(KjOwn<...>)       -> ServeIo::Kj                          (never unwraps)
//! ```
//!
//! **Where the result may be driven.** Every `ServeIo` is `Send`, because hyper's upgrade path
//! requires its transport to be, but it only progresses on the KJ event-loop thread that created
//! it: a native socket is registered with that thread's loop runtime, and a [`KjIo`] refuses to
//! touch its kj stream from any other thread (it fails the operation instead, and leaks the
//! stream if dropped elsewhere; see `OwnerThread` in ffi.rs).
//!
//! **Contracts.** As with destroying any kj stream, no I/O promise may be outstanding on it when
//! ownership is handed over. For kj-rs-io streams the unwrap *detects* that (the wrapper tracks
//! in-flight operations) and hands the stream back untouched ([`TakeSocketError`]); for foreign
//! streams it remains kj's own documented contract.
//!
//! **Lent streams.** A caller that only lends a stream (a non-owning `kj::Own`) keeps using its
//! wrapper afterwards, so such streams go through [`serve_lent_kj_stream`], which never takes
//! them apart.

use std::future::Future;
use std::pin::Pin;
use std::task::Context;
use std::task::Poll;

use cxx::KjError;
use cxx::KjException;
use cxx::KjExceptionType;
use kj_rs::KjOwn;
use tokio::io::AsyncRead;
use tokio::io::AsyncWrite;
use tokio::io::ReadBuf;
use tokio::net::TcpStream;
#[cfg(unix)]
use tokio::net::UnixStream;

use crate::ffi::AsyncIoStream;
use crate::ffi::KjStreamReadHalf;
use crate::ffi::KjStreamWatchHalf;
use crate::ffi::KjStreamWriteHalf;
use crate::ffi::OwnerThread;
use crate::ffi::is_releasable_rust_stream;
use crate::ffi::is_tokio_stream;
use crate::ffi::release_rust_stream;
use crate::ffi::release_tokio_stream;
use crate::ffi::split_kj_stream;
use crate::ffi::wrap_tokio_stream;
use crate::rust_stream::AsyncIo;
use crate::rust_stream::BoxedIo;
use crate::rust_stream::WriteDisconnect;

/// Read chunk size for a [`KjIo`]'s kj reads.
const READ_CHUNK: usize = 8192;

/// The tokio-side byte stream for a served kj stream. See the module docs.
pub enum ServeIo {
    Tcp(TcpStream),
    #[cfg(unix)]
    Unix(UnixStream),
    /// A stream that was already Rust underneath its kj wrapper (a TLS stream, see
    /// `rust_stream.rs`), taken back out of it.
    Rust(BoxedIo),
    /// A foreign kj stream, driven directly.
    Kj(KjIo),
}

// =======================================================================================
// KjIo: a foreign kj stream as a tokio stream.

type ReadOp =
    Pin<Box<dyn Future<Output = (KjStreamReadHalf, Vec<u8>, Result<usize, KjException>)>>>;
type WriteOp = Pin<Box<dyn Future<Output = (KjStreamWriteHalf, Result<(), KjException>)>>>;

/// The read direction: idle, a kj read in flight, bytes read but not yet taken, or done (EOF or
/// failure: later reads report EOF).
enum Reader {
    Idle(KjStreamReadHalf),
    Reading(ReadOp),
    Buffered {
        half: KjStreamReadHalf,
        chunk: Vec<u8>,
        taken: usize,
    },
    Done,
}

/// The write direction: idle, a kj write in flight (for `len` bytes of the caller's buffer), or
/// done (shut down or failed: later writes fail).
enum Writer {
    Idle(KjStreamWriteHalf),
    Writing { op: WriteOp, len: usize },
    Done,
}

struct KjIoState {
    reader: Reader,
    writer: Writer,
    /// Taken by the first `whenWriteDisconnected` (see `RustStream`).
    watch: Option<KjStreamWatchHalf>,
}

/// A foreign kj stream as a tokio `AsyncRead + AsyncWrite`.
///
/// Each direction polls the stream's bridged kj promise directly. kj allows one read and one
/// write in flight, which is exactly one op per direction here.
///
/// Writes follow kj's semantics, not a buffer's: `poll_write` completes only once the kj stream
/// has taken the bytes. A `Pending` write owns a copy of the buffer, and the caller retries it
/// with the same buffer (tokio's write loops and hyper do); the completed count applies to that
/// retry and never exceeds it.
pub struct KjIo {
    state: OwnerThread<KjIoState>,
}

impl KjIo {
    fn new(stream: KjOwn<AsyncIoStream>) -> Self {
        let (read, write, watch) = split_kj_stream(stream);
        Self {
            state: OwnerThread::new(KjIoState {
                reader: Reader::Idle(read),
                writer: Writer::Idle(write),
                watch: Some(watch),
            }),
        }
    }

    fn state(&mut self) -> std::io::Result<&mut KjIoState> {
        self.state.get_mut().ok_or_else(|| {
            std::io::Error::other("a kj stream was used off the thread owning its event loop")
        })
    }

    /// The stream's `whenWriteDisconnected`, for the first caller.
    fn take_watch(&mut self) -> std::io::Result<Option<KjStreamWatchHalf>> {
        Ok(self.state()?.watch.take())
    }
}

/// Carries a kj exception through an `io::Error`, so a consumer that maps I/O errors back to kj
/// (`rust_stream::kj_error_for_stream_io`) keeps its type and text.
#[derive(Debug)]
pub struct KjIoError(pub KjError);

impl std::fmt::Display for KjIoError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(self.0.description())
    }
}

impl std::error::Error for KjIoError {}

fn io_error(exception: KjException) -> std::io::Error {
    std::io::Error::other(KjIoError(KjError::from(exception)))
}

/// Whether a bridged kj exception is peer-teardown-shaped: a read treats it as EOF, a write as a
/// broken pipe.
fn is_disconnected(exception: &KjException) -> bool {
    exception.r#type() == KjExceptionType::Disconnected
}

impl AsyncRead for KjIo {
    fn poll_read(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &mut ReadBuf<'_>,
    ) -> Poll<std::io::Result<()>> {
        let state = self.get_mut().state()?;
        loop {
            match std::mem::replace(&mut state.reader, Reader::Done) {
                Reader::Done => return Poll::Ready(Ok(())),
                Reader::Buffered { half, chunk, taken } => {
                    let n = (chunk.len() - taken).min(buf.remaining());
                    buf.put_slice(&chunk[taken..taken + n]);
                    let taken = taken + n;
                    state.reader = if taken == chunk.len() {
                        Reader::Idle(half)
                    } else {
                        Reader::Buffered { half, chunk, taken }
                    };
                    return Poll::Ready(Ok(()));
                }
                Reader::Idle(mut half) => {
                    state.reader = Reader::Reading(Box::pin(async move {
                        let mut chunk = vec![0u8; READ_CHUNK];
                        let result = half.try_read(&mut chunk, 1).await;
                        (half, chunk, result)
                    }));
                }
                Reader::Reading(mut op) => match op.as_mut().poll(cx) {
                    Poll::Pending => {
                        state.reader = Reader::Reading(op);
                        return Poll::Pending;
                    }
                    Poll::Ready((half, mut chunk, Ok(n))) if n > 0 => {
                        chunk.truncate(n);
                        state.reader = Reader::Buffered {
                            half,
                            chunk,
                            taken: 0,
                        };
                    }
                    // EOF, or a DISCONNECTED read (the reset-peer shape): end of input.
                    Poll::Ready((_, _, Ok(_))) => return Poll::Ready(Ok(())),
                    Poll::Ready((_, _, Err(e))) if is_disconnected(&e) => {
                        return Poll::Ready(Ok(()));
                    }
                    Poll::Ready((_, _, Err(e))) => return Poll::Ready(Err(io_error(e))),
                },
            }
        }
    }
}

impl AsyncWrite for KjIo {
    fn poll_write(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &[u8],
    ) -> Poll<std::io::Result<usize>> {
        let state = self.get_mut().state()?;
        loop {
            match std::mem::replace(&mut state.writer, Writer::Done) {
                Writer::Done => return Poll::Ready(Err(std::io::ErrorKind::BrokenPipe.into())),
                Writer::Idle(half) if buf.is_empty() => {
                    state.writer = Writer::Idle(half);
                    return Poll::Ready(Ok(0));
                }
                Writer::Idle(mut half) => {
                    let bytes = buf.to_vec();
                    let len = bytes.len();
                    state.writer = Writer::Writing {
                        op: Box::pin(async move {
                            let result = half.write(&bytes).await;
                            (half, result)
                        }),
                        len,
                    };
                }
                Writer::Writing { mut op, len } => match op.as_mut().poll(cx) {
                    Poll::Pending => {
                        state.writer = Writer::Writing { op, len };
                        return Poll::Pending;
                    }
                    Poll::Ready((half, Ok(()))) => {
                        state.writer = Writer::Idle(half);
                        return Poll::Ready(Ok(len.min(buf.len())));
                    }
                    Poll::Ready((_, Err(e))) if is_disconnected(&e) => {
                        return Poll::Ready(Err(std::io::ErrorKind::BrokenPipe.into()));
                    }
                    Poll::Ready((_, Err(e))) => return Poll::Ready(Err(io_error(e))),
                },
            }
        }
    }

    fn poll_flush(self: Pin<&mut Self>, _cx: &mut Context<'_>) -> Poll<std::io::Result<()>> {
        // A completed write is already in the kj stream.
        Poll::Ready(Ok(()))
    }

    fn poll_shutdown(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<std::io::Result<()>> {
        let state = self.get_mut().state()?;
        match std::mem::replace(&mut state.writer, Writer::Done) {
            Writer::Done => Poll::Ready(Ok(())),
            // A write still in flight finishes first.
            Writer::Writing { mut op, len } => match op.as_mut().poll(cx) {
                Poll::Pending => {
                    state.writer = Writer::Writing { op, len };
                    Poll::Pending
                }
                Poll::Ready((mut half, Ok(()))) => Poll::Ready(shutdown(&mut half)),
                Poll::Ready((_, Err(e))) if is_disconnected(&e) => Poll::Ready(Ok(())),
                Poll::Ready((_, Err(e))) => Poll::Ready(Err(io_error(e))),
            },
            Writer::Idle(mut half) => Poll::Ready(shutdown(&mut half)),
        }
    }
}

/// `shutdownWrite()`; a DISCONNECTED failure (the peer is already gone) is not an error.
fn shutdown(half: &mut KjStreamWriteHalf) -> std::io::Result<()> {
    match half.shutdown_write() {
        Ok(()) => Ok(()),
        Err(e) if is_disconnected(&e) => Ok(()),
        Err(e) => Err(io_error(e)),
    }
}

// =======================================================================================
// ServeIo

impl ServeIo {
    /// Sets `TCP_NODELAY` on the underlying socket where applicable (a no-op for Unix-domain
    /// sockets and kj streams, which have no Nagle to disable).
    ///
    /// # Errors
    ///
    /// Returns the underlying `std::io::Error` if setting the socket option fails.
    pub fn set_nodelay(&self, nodelay: bool) -> std::io::Result<()> {
        match self {
            Self::Tcp(s) => s.set_nodelay(nodelay),
            #[cfg(unix)]
            Self::Unix(_) => Ok(()),
            Self::Rust(s) => s.set_nodelay(nodelay),
            Self::Kj(_) => Ok(()),
        }
    }
}

impl AsyncIo for ServeIo {
    fn write_disconnect(self: Pin<&mut Self>) -> std::io::Result<WriteDisconnect> {
        match self.get_mut() {
            #[cfg(unix)]
            Self::Tcp(s) => {
                use std::os::fd::AsFd;
                Ok(WriteDisconnect::Fd(s.as_fd().try_clone_to_owned()?))
            }
            #[cfg(windows)]
            Self::Tcp(_) => Ok(WriteDisconnect::Never),
            #[cfg(unix)]
            Self::Unix(s) => {
                use std::os::fd::AsFd;
                Ok(WriteDisconnect::Fd(s.as_fd().try_clone_to_owned()?))
            }
            Self::Rust(s) => s.as_mut().write_disconnect(),
            Self::Kj(s) => Ok(s
                .take_watch()?
                .map_or(WriteDisconnect::Never, WriteDisconnect::Kj)),
        }
    }

    fn set_nodelay(&self, nodelay: bool) -> std::io::Result<()> {
        Self::set_nodelay(self, nodelay)
    }
}

impl From<kj_rs_io::Socket> for ServeIo {
    fn from(socket: kj_rs_io::Socket) -> Self {
        match socket {
            kj_rs_io::Socket::Tcp(stream) => Self::Tcp(stream),
            #[cfg(unix)]
            kj_rs_io::Socket::Unix(stream) => Self::Unix(stream),
        }
    }
}

impl AsyncRead for ServeIo {
    fn poll_read(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &mut ReadBuf<'_>,
    ) -> Poll<std::io::Result<()>> {
        match self.get_mut() {
            Self::Tcp(s) => Pin::new(s).poll_read(cx, buf),
            #[cfg(unix)]
            Self::Unix(s) => Pin::new(s).poll_read(cx, buf),
            Self::Rust(s) => s.as_mut().poll_read(cx, buf),
            Self::Kj(s) => Pin::new(s).poll_read(cx, buf),
        }
    }
}

impl AsyncWrite for ServeIo {
    fn poll_write(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &[u8],
    ) -> Poll<std::io::Result<usize>> {
        match self.get_mut() {
            Self::Tcp(s) => Pin::new(s).poll_write(cx, buf),
            #[cfg(unix)]
            Self::Unix(s) => Pin::new(s).poll_write(cx, buf),
            Self::Rust(s) => s.as_mut().poll_write(cx, buf),
            Self::Kj(s) => Pin::new(s).poll_write(cx, buf),
        }
    }

    fn poll_flush(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<std::io::Result<()>> {
        match self.get_mut() {
            Self::Tcp(s) => Pin::new(s).poll_flush(cx),
            #[cfg(unix)]
            Self::Unix(s) => Pin::new(s).poll_flush(cx),
            Self::Rust(s) => s.as_mut().poll_flush(cx),
            Self::Kj(s) => Pin::new(s).poll_flush(cx),
        }
    }

    fn poll_shutdown(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<std::io::Result<()>> {
        match self.get_mut() {
            Self::Tcp(s) => Pin::new(s).poll_shutdown(cx),
            #[cfg(unix)]
            Self::Unix(s) => Pin::new(s).poll_shutdown(cx),
            Self::Rust(s) => s.as_mut().poll_shutdown(cx),
            Self::Kj(s) => Pin::new(s).poll_shutdown(cx),
        }
    }

    fn poll_write_vectored(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        bufs: &[std::io::IoSlice<'_>],
    ) -> Poll<std::io::Result<usize>> {
        match self.get_mut() {
            Self::Tcp(s) => Pin::new(s).poll_write_vectored(cx, bufs),
            #[cfg(unix)]
            Self::Unix(s) => Pin::new(s).poll_write_vectored(cx, bufs),
            Self::Rust(s) => s.as_mut().poll_write_vectored(cx, bufs),
            Self::Kj(s) => Pin::new(s).poll_write_vectored(cx, bufs),
        }
    }

    fn is_write_vectored(&self) -> bool {
        match self {
            Self::Tcp(s) => s.is_write_vectored(),
            #[cfg(unix)]
            Self::Unix(s) => s.is_write_vectored(),
            Self::Rust(s) => s.is_write_vectored(),
            Self::Kj(s) => s.is_write_vectored(),
        }
    }
}

// =======================================================================================
// Entry points

/// A native-serving error, plus the untouched stream handed back to the caller.
///
/// A foreign-stream error from [`take_kj_socket`] can fall back to [`serve_kj_stream`]. A native
/// extraction error ("in flight") means an operation is still using the stream; the caller may
/// cancel it and retry, or simply drop this error: dropping the stream with the operation pending
/// is memory-safe (the operation owns its share of the socket, see kj-rs-io's stream.rs), the
/// pending promise then settles or is cancelled on its own schedule.
pub struct TakeSocketError {
    /// The stream consumed by the native-serving entry point, returned untouched.
    pub stream: KjOwn<AsyncIoStream>,
    /// Why the socket could not be taken natively.
    pub error: KjError,
}

impl std::fmt::Debug for TakeSocketError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("TakeSocketError")
            .field("error", &self.error)
            .finish_non_exhaustive()
    }
}

impl std::fmt::Display for TakeSocketError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{:?}", self.error)
    }
}

/// Drops the handed-back stream (on the current -- KJ event-loop -- thread) and keeps the
/// error. Safe even with an operation in flight on the stream (see the type docs).
impl From<TakeSocketError> for KjError {
    fn from(e: TakeSocketError) -> Self {
        e.error
    }
}

/// Takes the stream's native tokio socket: kj-rs-io streams only.
///
/// The result is an owned [`ServeIo::Tcp`]/[`ServeIo::Unix`] that no longer depends on the
/// consumed kj stream, which is gone before returning. No I/O promise may be in flight on the
/// stream: the unwrap detects that and hands the stream back (see [`TakeSocketError`]).
///
/// # Errors
///
/// Errors, handing the stream back, when it is not a kj-rs-io stream (serve it through
/// [`serve_kj_stream`] instead), or when it is one that cannot be unwrapped right now.
pub fn take_kj_socket(stream: KjOwn<AsyncIoStream>) -> Result<ServeIo, TakeSocketError> {
    if !is_tokio_stream(stream.as_ref()) {
        return Err(TakeSocketError {
            stream,
            error: KjError::new(
                KjExceptionType::Failed,
                "cannot take the stream's socket natively: not a kj-rs-io stream (serve it \
                 through serve_kj_stream instead)"
                    .to_owned(),
            ),
        });
    }
    match release_tokio_stream(stream).into_socket() {
        Ok(socket) => Ok(ServeIo::from(socket)),
        Err((native, error)) => Err(TakeSocketError {
            stream: wrap_tokio_stream(native),
            error,
        }),
    }
}

/// Yields the best-available tokio-side stream for the owned `stream`: its native tokio object
/// where there is one to take (see the module docs), else a [`KjIo`] owning it.
///
/// # Errors
///
/// Returns the stream when it is a kj-rs-io wrapper whose native socket cannot be extracted (an
/// I/O operation still owns a share of it).
pub fn serve_kj_stream(stream: KjOwn<AsyncIoStream>) -> Result<ServeIo, TakeSocketError> {
    // A Rust stream is taken only when nothing still holds it (see `RustStream::can_release`).
    if is_releasable_rust_stream(stream.as_ref()) {
        return Ok(ServeIo::Rust((*release_rust_stream(stream)).into_io()));
    }
    if is_tokio_stream(stream.as_ref()) {
        return take_kj_socket(stream);
    }
    Ok(ServeIo::Kj(KjIo::new(stream)))
}

/// Drives the stream directly ([`KjIo`]), never taking it apart.
///
/// For a stream the caller only lends (a non-owning `kj::Own`, e.g.
/// `kj::newHttpClient(table, stream&)`), whose wrapper must stay intact for the caller.
#[must_use]
pub fn serve_lent_kj_stream(stream: KjOwn<AsyncIoStream>) -> ServeIo {
    ServeIo::Kj(KjIo::new(stream))
}
