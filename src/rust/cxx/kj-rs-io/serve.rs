//! The native-serve entry points: give a Rust server (any tokio consumer) the
//! best-available tokio-side byte stream for an owned `kj::AsyncIoStream`.
//!
//! Two paths, two entry points: (1) kj-rs-io-originated streams give up their native tokio
//! socket (the hollow wrapper is destroyed) -- the only path [`take_kj_socket`] takes;
//! (2) [`serve_kj_stream`] bridges any other stream through an in-memory duplex plus a pump
//! future that owns it. Ownership arrives as a [`KjOwn`], so both entry points are safe
//! functions: there is no keep-it-alive caller contract, and the stream is destroyed by the
//! path that consumed it. (An fd-dup tier for foreign fd-backed streams used to exist; it had no
//! consumer -- under the rust backend every fd-backed stream is a kj-rs-io stream -- and rested
//! on a caller-asserted "the fd carries this stream's bytes" contract the type system cannot
//! check, so it was dropped until a consumer needs it.)
//!
//! # Where the result may be driven
//!
//! Every [`ServeIo`] is `Send` and may be handed to a connection task on another thread, but
//! moving a tokio socket does not move its I/O-driver registration: the native variants
//! (`Tcp`/`Unix`) stay registered with the loop runtime that created them and make progress only
//! while that KJ loop is parked in its port (or otherwise turning tokio's driver). They are
//! independent of the *KJ stream* they came from -- the wrapper is destroyed -- not of the
//! originating loop. If that loop's runtime is torn down first, tokio reports the socket's
//! next operation as an I/O error ("reactor gone"), it does not hang. A consumer that must
//! outlive or run independently of that loop needs the OS socket itself (`std::os::fd::OwnedFd`
//! via `into_std`) re-registered on its own runtime. The `Duplex` variant's peer end lives
//! inside the pump, which is polled on the originating KJ loop; its wakes cross threads through
//! the kj-rs waker bridge, and dropping the consumer's end ends the pump (see below).
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
//! - Dropping the *consumer's* end (without `shutdown()`) ends the pump too: the consumer end
//!   carries a drop signal the kj->consumer direction races against, so an idle peer cannot
//!   keep the pump, the kj stream and the connection alive for a consumer that is gone. A
//!   `shutdown()` by a still-reading consumer is the ordinary half-close and keeps that
//!   direction running.

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
use crate::ffi::is_tokio_stream;
use crate::ffi::split_kj_stream;
use crate::ffi::unwrap_tokio_stream;

/// Read chunk size for the pump fallback.
const PUMP_BUF: usize = 8192;

/// In-memory buffer per direction of the pump's duplex (how far the two sides may run ahead
/// of each other before backpressure).
pub const DUPLEX_CAPACITY: usize = 4 * PUMP_BUF;

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
/// Implements `AsyncRead + AsyncWrite`, so it drops into any tokio consumer. See the module
/// docs ("Where the result may be driven") for the reactor affinity every variant keeps.
pub enum ServeIo {
    Tcp(TcpStream),
    #[cfg(unix)]
    Unix(UnixStream),
    Duplex(PumpedStream),
}

/// The consumer end of a pumped stream: a `DuplexStream` plus the drop signal that ends the
/// pump when this end goes away (see the module docs). Behaves exactly like the `DuplexStream`.
pub struct PumpedStream {
    inner: DuplexStream,
    /// Dropped with `self`; the pump's `Receiver` then resolves and its kj->consumer direction
    /// stops waiting on the peer.
    _alive: tokio::sync::oneshot::Sender<()>,
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
    /// The tokio-side stream. May be handed to a connection task on another thread; see the
    /// module docs for what stays bound to the originating loop.
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

/// A native-serving error, plus the untouched stream handed back to the caller.
///
/// A foreign-stream error from [`take_kj_socket`] can fall back to [`serve_kj_stream`]'s pump
/// tier. A native extraction error ("in flight") means an operation is still using the stream;
/// the caller may cancel it and retry, or simply drop this error: dropping the stream with the
/// operation pending is memory-safe (the operation owns its share of the socket, see lib.rs
/// "Ownership of in-flight operations"), the pending promise then settles or is cancelled on
/// its own schedule.
pub struct TakeSocketError {
    /// The stream consumed by the native-serving entry point, returned untouched.
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
/// Safe even with an operation in flight on the stream (see the type docs).
impl From<TakeSocketError> for KjError {
    fn from(e: TakeSocketError) -> Self {
        e.error
    }
}

/// Tier-1 unwrap: if the owned stream is kj-rs-io-originated, moves its native tokio socket
/// out (leaving the C++ wrapper hollow) and returns it. `Ok(None)` for a foreign stream. A
/// checked extraction failure (an operation in flight, or an already-hollow wrapper) is an
/// `Err`, kept distinct so callers do not reuse or destroy the native wrapper while an
/// operation still borrows it.
fn unwrap_native(
    stream: &mut KjOwn<KjAsyncIoStream>,
) -> std::result::Result<Option<ServeIo>, KjError> {
    if !is_tokio_stream(stream.as_ref()) {
        return Ok(None);
    }

    let native = unwrap_tokio_stream(stream.as_mut()).map_err(KjError::from)?;
    native.into_serve_io().map(Some).ok_or_else(|| {
        KjError::new(
            KjExceptionType::Failed,
            "native unwrap returned a hollow stream".to_owned(),
        )
    })
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
/// other wrappers): such transports are served through [`serve_kj_stream`]'s pump — the error
/// hands the stream back for exactly that fallback. Also errors, keeping the stream, when it is
/// a kj-rs-io stream that cannot be unwrapped right now.
pub fn take_kj_socket(
    stream: KjOwn<KjAsyncIoStream>,
) -> std::result::Result<ServeIo, TakeSocketError> {
    let mut stream = stream;
    match unwrap_native(&mut stream) {
        Ok(Some(io)) => {
            // The wrapper is hollow; destroy it now.
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
///
/// # Errors
///
/// Returns the untouched stream when it is a kj-rs-io wrapper whose native socket cannot be
/// extracted (an I/O operation still borrows it, or it was already unwrapped), or when the
/// pump's end objects cannot be allocated.
pub fn serve_kj_stream(
    stream: KjOwn<KjAsyncIoStream>,
) -> std::result::Result<ServedKjStream, TakeSocketError> {
    let mut stream = stream;
    match unwrap_native(&mut stream) {
        Ok(Some(io)) => {
            // Native path: the wrapper is hollow; destroy it now.
            drop(stream);
            return Ok(ServedKjStream { io, pump: None });
        }
        Ok(None) => {}
        Err(error) => return Err(TakeSocketError { stream, error }),
    }

    // Foreign stream: bridge through a duplex pump owning the stream. (An already-hollow
    // kj-rs-io wrapper never gets here: `unwrap_native` reports it as an error above.)
    let (consumer_end, kj_end) = tokio::io::duplex(DUPLEX_CAPACITY);
    let (alive_tx, alive_rx) = tokio::sync::oneshot::channel();
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

/// The duplex pump: bridges the owned `stream` (via bridged `kj::io` promises, on the calling
/// KJ thread) to `kj_end`, the pump-side end of the consumer's duplex. Owns the stream: it is
/// destroyed when this future settles or is dropped.
///
/// The stream is split into typed read/write halves ([`split_kj_stream`]), each driving its own
/// C++ end object through an exclusive borrow, so the borrow checker enforces kj's stream
/// contract — at most one read and one write in flight, nothing else touching the stream while
/// the halves live — and the halves cannot outlive the owner they borrow.
pub async fn pump_kj_stream(
    mut stream: KjOwn<KjAsyncIoStream>,
    kj_end: DuplexStream,
    mut consumer_alive: tokio::sync::oneshot::Receiver<()>,
) -> Result<(), KjError> {
    let (mut rd, mut wr) = split_kj_stream(&mut stream).map_err(KjError::from)?;
    let (mut from_consumer, mut to_consumer) = tokio::io::split(kj_end);

    // kj stream -> consumer. Ends (shutting down the duplex write half, i.e. EOF to the
    // consumer) at kj-side EOF — or when the peer disconnects abruptly (DISCONNECTED read
    // failures are normal client behavior, treated as EOF) — or as soon as the consumer has
    // dropped its end: nothing is left to deliver to, and an idle peer must not keep this
    // direction (and with it the pump, the kj stream and the connection) alive.
    let kj_to_consumer = async {
        let mut buf = vec![0u8; PUMP_BUF];
        loop {
            let read = tokio::select! {
                // The Sender lives in the consumer's PumpedStream; its drop resolves this.
                _ = &mut consumer_alive => return Ok::<(), KjError>(()),
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
    assert_impl_all!(PumpedStream: Send);

    // Hands a KjOwn back to the caller: a KJ object, single-loop, never Send.
    assert_not_impl_any!(TakeSocketError: Send, Sync);
}
