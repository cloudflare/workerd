//! The native-serve entry points: give a Rust server (any tokio consumer) the
//! best-available tokio-side byte stream for an owned `kj::AsyncIoStream`.
//!
//! Three tiers, two entry points:
//! (1) kj-rs-io-originated streams give up their native tokio socket (the hollow
//! wrapper is destroyed); (2) [`take_kj_socket`] only — a foreign fd-backed stream's fd is
//! duplicated into a fresh tokio socket; (3) [`serve_kj_stream`] only — any other foreign
//! stream is bridged through an in-memory duplex plus a pump future that owns it. Ownership
//! arrives as a [`KjOwn`], so both entry points are safe functions: there is no
//! keep-it-alive caller contract, and the stream is destroyed by the tier that consumed it.
//! See the two entry points' docs for the remaining semantics, in particular why the fd tier
//! is caller-asserted and never automatic.
//!
//! # Pump semantics (matching the hand-built pumps this subsumes)
//!
//! - Bidirectional; each direction ends independently.
//! - Half-close propagates both ways: kj-side EOF shuts down the duplex write half (the tokio
//!   consumer reads EOF); the consumer shutting down (or dropping) its duplex end results in
//!   `shutdownWrite()` on the kj stream.
//! - Peer-teardown-shaped kj failures (DISCONNECTED reads/writes) are treated as normal EOF,
//!   not errors — abrupt client disconnects are normal server load.
//! - Dropping the pump future cancels the in-flight bridged kj promises synchronously, drops
//!   the kj-side duplex end (the tokio consumer observes EOF), and destroys the owned kj
//!   stream — the peer observes teardown, not a zombie half-open connection (abort-on-drop).

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

use crate::ffi::KjAsyncIoStream;
#[cfg(unix)]
use crate::ffi::dup_raw_fd;
#[cfg(unix)]
use crate::ffi::kj_stream_get_handle;
use crate::ffi::split_kj_stream;
use crate::ffi::unwrap_tokio_stream;

/// Read chunk size for the pump fallback.
const PUMP_BUF: usize = 8192;

/// In-memory buffer per direction of the pump's duplex (how far the two sides may run ahead
/// of each other before backpressure).
pub(crate) const DUPLEX_CAPACITY: usize = 4 * PUMP_BUF;

/// Which transport path [`serve_kj_stream`] produced (perf observability: `Pumped` costs FFI
/// promise round-trips per buffer, `Native` costs none).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ServePath {
    /// A native tokio socket (unwrap fast path).
    Native,
    /// An in-memory duplex fed by the FFI stream pump.
    Pumped,
}

impl std::fmt::Display for ServePath {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(match self {
            Self::Native => "native",
            Self::Pumped => "pumped",
        })
    }
}

/// The tokio-side byte stream for a served kj stream: a native socket or the consumer end of
/// the pump's duplex.
///
/// Implements `AsyncRead + AsyncWrite`, so it drops into any tokio consumer, on any runtime
/// thread -- see [`ServedKjStream::io`] for the (performance, not soundness) note on driving
/// `Duplex` off the KJ event-loop thread.
pub enum ServeIo {
    Tcp(TcpStream),
    #[cfg(unix)]
    Unix(UnixStream),
    Duplex(DuplexStream),
}

impl ServeIo {
    /// Which transport path this stream is on (see [`ServePath`]).
    #[must_use]
    pub fn path(&self) -> ServePath {
        match self {
            Self::Tcp(_) => ServePath::Native,
            #[cfg(unix)]
            Self::Unix(_) => ServePath::Native,
            Self::Duplex(_) => ServePath::Pumped,
        }
    }

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
    /// The tokio-side stream.
    ///
    /// Thread affinity: every variant may be handed to a connection task on any runtime thread.
    /// The NATIVE variants (`Tcp`/`Unix`) stay registered with the I/O driver that created them
    /// (for kj-rs-io streams, this thread's KJ-loop runtime) and wake their consumer via
    /// tokio's own task waker. The [`ServeIo::Duplex`] variant's peer end lives inside `pump`,
    /// which is polled as a bridged future, so the waker parked in the duplex's internal waker
    /// slots is a `kj_rs` `FutureWakerCell` clone -- atomically refcounted and honoring the full
    /// `Waker: Send + Sync` contract: a read/write/drop of the duplex from another thread wakes
    /// the pump through the cell's cross-thread fulfiller. That hop is a bit more expensive
    /// than a same-thread wake, so co-locating a `Duplex` consumer with the KJ event-loop
    /// thread is a performance recommendation (check [`ServedKjStream::path`]), not a
    /// soundness requirement.
    pub io: ServeIo,
    /// Present iff `io` is [`ServeIo::Duplex`]: the pump that actually moves the bytes, owning
    /// the kj stream it bridges. The caller must poll it on the KJ event-loop thread until it
    /// settles or is dropped; dropping it destroys the stream (see the module docs).
    pub pump: Option<StreamPump>,
}

impl ServedKjStream {
    /// Which transport path was taken (see [`ServePath`]).
    #[must_use]
    pub fn path(&self) -> ServePath {
        self.io.path()
    }
}

// =======================================================================================
// Entry points

/// [`take_kj_socket`]'s error: the failure, plus the untouched stream handed back so the
/// caller can fall back to [`serve_kj_stream`]'s pump tier (or destroy it).
pub struct TakeSocketError {
    /// The stream `take_kj_socket` consumed, returned untouched.
    pub stream: KjOwn<KjAsyncIoStream>,
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

/// Drops the handed-back stream (on the current — KJ event-loop — thread) and keeps the error.
impl From<TakeSocketError> for KjError {
    fn from(e: TakeSocketError) -> Self {
        e.error
    }
}

/// Tier-1 unwrap: if the owned stream is kj-rs-io-originated, moves its native tokio socket
/// out (leaving the C++ wrapper hollow) and returns it. `None` for a foreign stream.
fn unwrap_native(stream: &mut KjOwn<KjAsyncIoStream>) -> Option<ServeIo> {
    unwrap_tokio_stream(stream.as_mut())
        .ok()
        .and_then(|native| native.into_serve_io())
}

/// Takes the stream's socket natively (tiers 1 + 2).
///
/// Tier 1 moves the native tokio object out of a kj-rs-io stream; tier 2 — unix only —
/// duplicates the stream's OS fd (`F_DUPFD_CLOEXEC`, forced non-blocking) into a fresh tokio
/// socket. Either way the result is an owned, KJ-independent [`ServeIo`] (never
/// [`ServeIo::Duplex`]) and the consumed kj stream is destroyed before returning.
///
/// The handle tier (tier 2) is `cfg(unix)`: under the all-rust mode on Windows every
/// socket-backed stream originates in kj-rs-io, so tier 1 always applies and a windows dup arm
/// would be dead code (see `dup_raw_fd`'s docs for the analysis and the
/// `BorrowedSocket::try_clone_to_owned` escape hatch should that ever change).
///
/// The caller asserts the stream is a **plain stream socket**: if it exposes an fd, that fd
/// carries the stream's own bytes. Byte-transforming wrappers (TLS — `kj::TlsConnection`
/// forwards `getFd()` to the ciphertext transport socket) violate this and must go through
/// [`serve_kj_stream`] instead. As with destroying any kj stream, no I/O promises may be
/// outstanding on it when ownership is handed over (for kj-rs-io streams the unwrap detects
/// and rejects that; for foreign streams it remains KJ's own contract).
///
/// # Errors
///
/// Errors when the stream is neither kj-rs-io native nor fd-backed (in-memory pipes, promised
/// streams, non-Unix platforms' foreign streams): such transports can only be served through
/// [`serve_kj_stream`]'s pump tier — the error hands the stream back for exactly that
/// fallback. Also errors if the dup or tokio registration fails.
pub fn take_kj_socket(
    stream: KjOwn<KjAsyncIoStream>,
) -> std::result::Result<ServeIo, TakeSocketError> {
    let mut stream = stream;
    if let Some(io) = unwrap_native(&mut stream) {
        // Tier 1: the wrapper is hollow; destroy it now.
        drop(stream);
        return Ok(io);
    }
    #[cfg(unix)]
    {
        let handle = kj_stream_get_handle(&stream);
        if handle >= 0 {
            // A unix fd is a non-negative int, widened losslessly to i64 by the bridge.
            #[expect(clippy::cast_possible_truncation)]
            let fd = handle as i32;
            let result = dup_raw_fd(fd)
                .map_err(KjError::from)
                .and_then(|owned| crate::net::serve_io_from_owned_fd(owned).map_err(KjError::from));
            return match result {
                Ok(io) => {
                    // Tier 2: the dup is independent; the original stream (and its fd) can go.
                    drop(stream);
                    Ok(io)
                }
                Err(error) => Err(TakeSocketError { stream, error }),
            };
        }
    }
    Err(TakeSocketError {
        stream,
        error: KjError::new(
            KjExceptionType::Failed,
            "cannot take the stream's socket natively: not a kj-rs-io stream and no underlying \
             OS fd (foreign non-socket transport; serve it through serve_kj_stream's pump instead)"
                .to_owned(),
        ),
    })
}

/// Yields the best-available tokio-side stream for the owned `stream`.
///
/// That is the native tokio object when `stream` originated in kj-rs-io, else an in-memory
/// duplex bridged by a pump future that owns the stream. Never extracts a foreign stream's fd
/// — kj wrappers forward `getFd()` to their transport socket, so a byte-transforming wrapper's
/// fd carries the wrong bytes (TLS ciphertext); callers that can assert a plain socket should
/// prefer [`take_kj_socket`].
///
/// On the native path the hollow wrapper is destroyed before returning; on the pump path the
/// stream lives inside the pump and is destroyed when the pump settles or is dropped. The pump
/// must only be polled from the KJ event-loop thread owning the stream. As with destroying any
/// kj stream, no I/O promises may be outstanding on it when ownership is handed over (for
/// kj-rs-io streams the unwrap detects and rejects that; for foreign streams it remains KJ's
/// own contract).
#[must_use]
pub fn serve_kj_stream(stream: KjOwn<KjAsyncIoStream>) -> ServedKjStream {
    let mut stream = stream;
    if let Some(io) = unwrap_native(&mut stream) {
        // Native path: the wrapper is hollow; destroy it now.
        drop(stream);
        return ServedKjStream { io, pump: None };
    }

    // Foreign stream (or, pathologically, an already-hollow wrapper, whose pump reads will
    // surface the "already unwrapped" error): bridge through a duplex pump owning the stream.
    let (consumer_end, kj_end) = tokio::io::duplex(DUPLEX_CAPACITY);
    let pump = Box::pin(pump_kj_stream(stream, kj_end));
    ServedKjStream {
        io: ServeIo::Duplex(consumer_end),
        pump: Some(pump),
    }
}

/// Whether a bridged kj exception is peer-teardown-shaped (treated as EOF by the pump).
fn is_disconnected(exception: &KjException) -> bool {
    exception.r#type() == KjExceptionType::Disconnected
}

/// The duplex pump: bridges the owned `stream` (via bridged `kj::io` promises, on the calling
/// KJ thread) to `kj_end`, the pump-side end of the consumer's duplex. Owns the stream: it is
/// destroyed when this future settles or is dropped.
///
/// Unsafe-free and compiler-checked end to end: the stream is split into typed read/write
/// halves ([`split_kj_stream`]), so the borrow checker enforces kj's stream contract — at most
/// one read and one write in flight, nothing else touching the stream while the halves live —
/// and proves the owner outlives every in-flight bridged promise.
pub(crate) async fn pump_kj_stream(
    mut stream: KjOwn<KjAsyncIoStream>,
    kj_end: DuplexStream,
) -> Result<(), KjError> {
    let (mut rd, mut wr) = split_kj_stream(&mut stream);
    let (mut from_consumer, mut to_consumer) = tokio::io::split(kj_end);

    // kj stream -> consumer. Ends (shutting down the duplex write half, i.e. EOF to the
    // consumer) at kj-side EOF — or when the peer disconnects abruptly (DISCONNECTED read
    // failures are normal client behavior, treated as EOF).
    let kj_to_consumer = async {
        let mut buf = vec![0u8; PUMP_BUF];
        loop {
            let n = match rd.try_read(&mut buf, 1).await {
                Ok(n) => n,
                Err(e) if is_disconnected(&e) => 0,
                Err(e) => return Err(KjError::from(e)),
            };
            if n == 0 {
                let _ = to_consumer.shutdown().await;
                return Ok::<(), KjError>(());
            }
            if to_consumer.write_all(&buf[..n]).await.is_err() {
                // The consumer dropped its duplex end: the connection was abandoned
                // deliberately; nothing more to deliver in this direction.
                return Ok(());
            }
        }
    };

    // consumer -> kj stream. Ends (with a kj-side shutdownWrite) when the consumer shuts
    // down or drops its end — or, without an error, when the kj peer already went away (a
    // DISCONNECTED write failure: the consumer's remaining output has nowhere to go).
    let consumer_to_kj = async {
        let mut buf = vec![0u8; PUMP_BUF];
        loop {
            let n = match from_consumer.read(&mut buf).await {
                // Duplex reads only fail if the consumer end vanished ungracefully; either
                // way this direction is over.
                Ok(0) | Err(_) => 0,
                Ok(n) => n,
            };
            if n == 0 {
                // The consumer is done writing: half-close the kj side. A peer that already
                // vanished (DISCONNECTED) is the same outcome for this direction.
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

#[cfg(test)]
mod send_guards {
    use static_assertions::assert_impl_all;
    use static_assertions::assert_not_impl_any;

    use super::*;

    // The pump awaits bridged kj::Promises and owns a KjOwn: it must be polled on the KJ
    // event-loop thread, which the type system enforces only while this stays true.
    assert_not_impl_any!(StreamPump: Send, Sync);

    // The consumer-side stream may be handed to a connection task on any runtime thread
    // (native variants: tokio's own wakers; Duplex: the thread-safe kj-rs waker cell).
    assert_impl_all!(ServeIo: Send);

    // Hands a KjOwn back to the caller: a KJ object, single-loop, never Send.
    assert_not_impl_any!(TakeSocketError: Send, Sync);
}
