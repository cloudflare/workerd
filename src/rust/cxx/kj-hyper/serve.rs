//! Serving a `kj::AsyncIoStream` natively: the entry points hyper's server and stream-tier
//! client use to turn an owned kj stream into a tokio-side `AsyncRead + AsyncWrite`.
//!
//! Two tiers:
//!
//! - **Unwrap (native path).** A kj-rs-io stream gives up its tokio socket outright
//!   (`kj_rs_io::TokioStream::into_socket` via the `unwrapTokioStream` hook, kj-rs-io/bridge.h);
//!   the hollow C++ wrapper is destroyed before the entry point returns. Zero copies, zero FFI
//!   crossings per read.
//! - **Duplex pump (foreign streams).** Anything else -- in-memory pipes, promised streams, TLS
//!   and other byte-transforming wrappers -- is bridged through a `tokio::io::duplex` by a pump
//!   future that owns the stream and moves bytes through bridged `kj::io` promises. Never a
//!   foreign stream's fd: kj wrappers forward `getFd()` to their transport socket, so a
//!   TLS-shaped wrapper's fd carries ciphertext, not the stream's bytes.
//!
//! ```text
//! serve_kj_stream(KjOwn<AsyncIoStream>) -> Result<ServedKjStream, TakeSocketError>
//!     |-- native path: unwrap -> ServeIo::Tcp/Unix, hollow wrapper destroyed
//!     `-- pump path:   ServeIo::Duplex (consumer end) + StreamPump (!Send) owning the KjOwn
//! take_kj_socket(KjOwn<AsyncIoStream>)  -> Result<ServeIo, TakeSocketError>   (native path only)
//! ```
//!
//! **Where the result may be driven.** A native `ServeIo` is a tokio socket registered with the
//! loop runtime that created it (the thread's `TokioEventPort` runtime); it may be handed to a connection
//! task on another thread, but it only progresses while that originating KJ loop turns tokio's
//! driver, and once that runtime is gone its next operation fails with an I/O error rather than
//! hanging. It is independent of the *KJ stream* it came from, not of the loop. The pump path's
//! `ServeIo::Duplex` may likewise be consumed on another thread; the [`StreamPump`] itself is not
//! `Send` -- it awaits bridged `kj::Promise`s and must be polled on the KJ event-loop thread that
//! owns the stream (kj-rs's cross-thread wake sink carries the consumer's notifications back).
//!
//! **Contracts.** As with destroying any kj stream, no I/O promise may be outstanding on it when
//! ownership is handed over. For kj-rs-io streams the unwrap *detects* that (the wrapper tracks
//! in-flight operations) and hands the stream back untouched ([`TakeSocketError`]); for foreign
//! streams it remains KJ's own documented contract. Dropping the pump drops the kj-side duplex
//! end and destroys the owned stream (abort-on-drop); a consumer dropping its `ServeIo::Duplex`
//! without `shutdown()` becomes `shutdownWrite()` on the kj stream, after the bytes it already
//! wrote have flushed (`close(2)` semantics).

use std::future::Future;
use std::pin::Pin;
use std::task::Context;
use std::task::Poll;

use cxx::KjError;
use cxx::KjException;
use cxx::KjExceptionType;
use kj_rs::KjOwn;
use tokio::io::AsyncRead;
use tokio::io::AsyncReadExt;
use tokio::io::AsyncWrite;
use tokio::io::AsyncWriteExt;
use tokio::io::DuplexStream;
use tokio::io::ReadBuf;
use tokio::net::TcpStream;
#[cfg(unix)]
use tokio::net::UnixStream;
use tokio::sync::watch;

use crate::ffi::AsyncIoStream;
use crate::ffi::is_tokio_stream;
use crate::ffi::split_kj_stream;
use crate::ffi::unwrap_tokio_stream;

/// Read chunk size for the pump fallback.
const PUMP_BUF: usize = 8192;
/// In-memory buffer per direction of the pump's duplex (how far the two sides may run ahead
/// of each other before backpressure).
const DUPLEX_CAPACITY: usize = 4 * PUMP_BUF;

/// The tokio-side byte stream for a served kj stream: a native socket or the consumer end of
/// the pump's duplex.
///
/// Implements `AsyncRead + AsyncWrite`, so it drops into any tokio consumer. See the module
/// docs ("Where the result may be driven") for the reactor affinity every variant keeps.
pub enum ServeIo {
    Tcp(TcpStream),
    #[cfg(unix)]
    Unix(UnixStream),
    Duplex(PumpedStream),
}

/// The consumer end of a pumped stream: a `DuplexStream` whose drop the pump observes (see the
/// module docs). Otherwise behaves exactly like the `DuplexStream`.
pub struct PumpedStream {
    inner: DuplexStream,
    /// Dropped with `self`; the pump's receiver then sees the channel close.
    _alive: watch::Sender<()>,
}

impl AsyncRead for PumpedStream {
    fn poll_read(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &mut ReadBuf<'_>,
    ) -> Poll<std::io::Result<()>> {
        Pin::new(&mut self.get_mut().inner).poll_read(cx, buf)
    }
}

impl AsyncWrite for PumpedStream {
    fn poll_write(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &[u8],
    ) -> Poll<std::io::Result<usize>> {
        Pin::new(&mut self.get_mut().inner).poll_write(cx, buf)
    }

    fn poll_flush(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<std::io::Result<()>> {
        Pin::new(&mut self.get_mut().inner).poll_flush(cx)
    }

    fn poll_shutdown(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<std::io::Result<()>> {
        Pin::new(&mut self.get_mut().inner).poll_shutdown(cx)
    }

    fn poll_write_vectored(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        bufs: &[std::io::IoSlice<'_>],
    ) -> Poll<std::io::Result<usize>> {
        Pin::new(&mut self.get_mut().inner).poll_write_vectored(cx, bufs)
    }

    fn is_write_vectored(&self) -> bool {
        self.inner.is_write_vectored()
    }
}

impl ServeIo {
    /// Sets `TCP_NODELAY` on the underlying socket where applicable (a no-op for Unix-domain
    /// and duplex transports, which have no Nagle to disable).
    ///
    /// # Errors
    ///
    /// Returns the underlying `std::io::Error` if setting the socket option fails.
    pub fn set_nodelay(&self, nodelay: bool) -> std::io::Result<()> {
        match self {
            Self::Tcp(s) => s.set_nodelay(nodelay),
            #[cfg(unix)]
            Self::Unix(_) => Ok(()),
            Self::Duplex(_) => Ok(()),
        }
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
            Self::Duplex(s) => Pin::new(s).poll_read(cx, buf),
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
            Self::Duplex(s) => Pin::new(s).poll_write(cx, buf),
        }
    }

    fn poll_flush(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<std::io::Result<()>> {
        match self.get_mut() {
            Self::Tcp(s) => Pin::new(s).poll_flush(cx),
            #[cfg(unix)]
            Self::Unix(s) => Pin::new(s).poll_flush(cx),
            Self::Duplex(s) => Pin::new(s).poll_flush(cx),
        }
    }

    fn poll_shutdown(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<std::io::Result<()>> {
        match self.get_mut() {
            Self::Tcp(s) => Pin::new(s).poll_shutdown(cx),
            #[cfg(unix)]
            Self::Unix(s) => Pin::new(s).poll_shutdown(cx),
            Self::Duplex(s) => Pin::new(s).poll_shutdown(cx),
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
            Self::Duplex(s) => Pin::new(s).poll_write_vectored(cx, bufs),
        }
    }

    fn is_write_vectored(&self) -> bool {
        match self {
            Self::Tcp(s) => s.is_write_vectored(),
            #[cfg(unix)]
            Self::Unix(s) => s.is_write_vectored(),
            Self::Duplex(s) => s.is_write_vectored(),
        }
    }
}

/// The KJ-side pump future of the fallback path.
///
/// Not `Send`: it awaits bridged `kj::Promise`s and must be polled on the KJ event-loop thread
/// owning the stream. Resolves when both directions are done; dropping it aborts the
/// connection bridge (see the module docs).
pub type StreamPump = Pin<Box<dyn Future<Output = Result<(), KjError>>>>;

/// The result of [`serve_kj_stream`].
pub struct ServedKjStream {
    /// The tokio-side stream. May be handed to a connection task on another thread; see the
    /// module docs for what stays bound to the originating loop.
    pub io: ServeIo,
    /// Present iff `io` is [`ServeIo::Duplex`]: the pump that actually moves the bytes, owning
    /// the kj stream it bridges. The caller must poll it on the KJ event-loop thread until it
    /// settles or is dropped; dropping it destroys the stream (see the module docs).
    pub pump: Option<StreamPump>,
}

/// A native-serving error, plus the untouched stream handed back to the caller.
///
/// A foreign-stream error from [`take_kj_socket`] can fall back to [`serve_kj_stream`]'s pump
/// tier. A native extraction error ("in flight") means an operation is still using the stream;
/// the caller may cancel it and retry, or simply drop this error: dropping the stream with the
/// operation pending is memory-safe (the operation owns its share of the socket, see kj-rs-io's
/// stream.rs), the pending promise then settles or is cancelled on its own schedule.
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

/// Tier-1 unwrap: if the owned stream is kj-rs-io-originated, moves its native tokio socket
/// out (leaving the C++ wrapper hollow) and returns it. `Ok(None)` for a foreign stream. A
/// checked extraction failure (an operation in flight, or an already-hollow wrapper) is an
/// `Err`, kept distinct so callers do not reuse or destroy the native wrapper while an
/// operation still owns a share of it.
fn unwrap_native(stream: &mut KjOwn<AsyncIoStream>) -> Result<Option<ServeIo>, KjError> {
    if !is_tokio_stream(stream.as_ref()) {
        return Ok(None);
    }
    let native = unwrap_tokio_stream(stream.as_mut()).map_err(KjError::from)?;
    let socket = (*native).into_socket().map_err(KjError::from)?;
    Ok(Some(ServeIo::from(socket)))
}

/// Takes the stream's native tokio socket: the unwrap path only.
///
/// Moves the native tokio object out of a kj-rs-io stream. The result is an owned [`ServeIo`]
/// (never [`ServeIo::Duplex`]) that no longer depends on the consumed kj stream, which is
/// destroyed before returning; it remains registered with this thread's loop runtime (see the
/// module docs). No I/O promise may be in flight on the stream: the unwrap detects that and
/// hands the stream back (see [`TakeSocketError`]).
///
/// # Errors
///
/// Errors when the stream is not a kj-rs-io stream (in-memory pipes, promised streams, TLS or
/// other wrappers): such transports are served through [`serve_kj_stream`]'s pump -- the error
/// hands the stream back for exactly that fallback. Also errors, keeping the stream, when it is
/// a kj-rs-io stream that cannot be unwrapped right now.
pub fn take_kj_socket(stream: KjOwn<AsyncIoStream>) -> Result<ServeIo, TakeSocketError> {
    let mut stream = stream;
    match unwrap_native(&mut stream) {
        Ok(Some(io)) => {
            drop(stream);
            Ok(io)
        }
        Ok(None) => Err(TakeSocketError {
            stream,
            error: KjError::new(
                KjExceptionType::Failed,
                "cannot take the stream's socket natively: not a kj-rs-io stream (serve it \
                 through serve_kj_stream's pump instead)"
                    .to_owned(),
            ),
        }),
        Err(error) => Err(TakeSocketError { stream, error }),
    }
}

/// Yields the best-available tokio-side stream for the owned `stream`.
///
/// That is the native tokio object when `stream` originated in kj-rs-io, else an in-memory
/// duplex bridged by a pump future that owns the stream. Never extracts a foreign stream's fd
/// (see the module docs); callers that can assert a plain socket should prefer
/// [`take_kj_socket`].
///
/// On the native path the hollow wrapper is destroyed before returning; on the pump path the
/// stream lives inside the pump and is destroyed when the pump settles or is dropped. The pump
/// must only be polled from the KJ event-loop thread owning the stream.
///
/// # Errors
///
/// Returns the untouched stream when it is a kj-rs-io wrapper whose native socket cannot be
/// extracted (an I/O operation still owns a share of it, or it was already unwrapped), or when
/// the pump's end objects cannot be allocated.
pub fn serve_kj_stream(stream: KjOwn<AsyncIoStream>) -> Result<ServedKjStream, TakeSocketError> {
    let mut stream = stream;
    match unwrap_native(&mut stream) {
        Ok(Some(io)) => {
            drop(stream);
            return Ok(ServedKjStream { io, pump: None });
        }
        Ok(None) => {}
        Err(error) => return Err(TakeSocketError { stream, error }),
    }
    let (consumer_end, kj_end) = tokio::io::duplex(DUPLEX_CAPACITY);
    let (alive_tx, alive_rx) = watch::channel(());
    let pump = Box::pin(pump_kj_stream(stream, kj_end, alive_rx));
    Ok(ServedKjStream {
        io: ServeIo::Duplex(PumpedStream {
            inner: consumer_end,
            _alive: alive_tx,
        }),
        pump: Some(pump),
    })
}

/// Whether a bridged kj exception is peer-teardown-shaped (treated as EOF by the pump).
fn is_disconnected(exception: &KjException) -> bool {
    exception.r#type() == KjExceptionType::Disconnected
}

/// Resolves when the consumer's end has been dropped.
async fn consumer_gone(alive: &mut watch::Receiver<()>) {
    while alive.changed().await.is_ok() {}
}

/// The duplex pump: bridges the owned `stream` (via bridged `kj::io` promises, on the calling
/// KJ thread) to `kj_end`, the pump-side end of the consumer's duplex. Owns the stream: it is
/// destroyed when this future settles or is dropped.
///
/// The stream is split into its read and write directions ([`split_kj_stream`]): two C++ end
/// objects sharing ownership of the stream, each driven through an exclusive borrow, so the
/// borrow checker enforces kj's stream contract -- at most one read and one write in flight --
/// and C++ ownership keeps the stream alive as long as either end exists.
async fn pump_kj_stream(
    stream: KjOwn<AsyncIoStream>,
    kj_end: DuplexStream,
    mut alive: watch::Receiver<()>,
) -> Result<(), KjError> {
    let (mut rd, mut wr) = split_kj_stream(stream).map_err(KjError::from)?;
    let (mut from_consumer, mut to_consumer) = tokio::io::split(kj_end);

    // kj -> consumer: read from the kj stream, write into the duplex. EOF (or a DISCONNECTED
    // read, the reset-peer shape) half-closes the duplex; the consumer dropping its end ends
    // this direction (nobody is left to read).
    let kj_to_consumer = async {
        let mut buf = vec![0u8; PUMP_BUF];
        loop {
            let read = tokio::select! {
                () = consumer_gone(&mut alive) => return Ok::<(), KjError>(()),
                read = rd.try_read(&mut buf, 1) => read,
            };
            let n = match read {
                Ok(n) => n,
                Err(e) if is_disconnected(&e) => 0,
                Err(e) => return Err(KjError::from(e)),
            };
            if n == 0 {
                let _ = to_consumer.shutdown().await;
                return Ok::<(), KjError>(());
            }
            if to_consumer.write_all(&buf[..n]).await.is_err() {
                return Ok(());
            }
        }
    };

    // consumer -> kj: read from the duplex, write to the kj stream. The consumer's shutdown or
    // drop (EOF here) becomes shutdownWrite() on the kj stream after everything it wrote has
    // been flushed; a DISCONNECTED write is a clean end of the direction.
    let consumer_to_kj = async {
        let mut buf = vec![0u8; PUMP_BUF];
        loop {
            let n = match from_consumer.read(&mut buf).await {
                Ok(0) | Err(_) => 0,
                Ok(n) => n,
            };
            if n == 0 {
                match wr.shutdown_write() {
                    Ok(()) => {}
                    Err(e) if is_disconnected(&e) => {}
                    Err(e) => return Err(KjError::from(e)),
                }
                return Ok::<(), KjError>(());
            }
            match wr.write(&buf[..n]).await {
                Ok(()) => {}
                Err(e) if is_disconnected(&e) => return Ok(()),
                Err(e) => return Err(KjError::from(e)),
            }
        }
    };

    tokio::try_join!(kj_to_consumer, consumer_to_kj).map(|((), ())| ())
}
