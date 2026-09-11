//! Serving a `kj::AsyncIoStream` natively: the entry points hyper's server and stream-tier
//! client use to turn an owned kj stream into a tokio-side `AsyncRead + AsyncWrite`.
//!
//! Two tiers:
//!
//! - **Unwrap (native path).** A kj-rs-io stream gives up its tokio socket outright: the C++
//!   wrapper is consumed (`releaseTokioStream`, kj-stream.h) and `TokioStream::into_socket`
//!   takes the socket, or hands the stream back re-wrapped if an operation still holds a share
//!   of it. Zero copies, zero FFI crossings per read.
//! - **Pump (foreign streams).** Anything else -- in-memory pipes, promised streams, TLS and
//!   other byte-transforming wrappers -- is bridged by a pump future that owns the stream and
//!   moves bytes through bridged `kj::io` promises into a rendezvous channel with kj-pipe
//!   semantics (a consumer write completes when the kj stream has taken it; dropping the
//!   consumer cancels what the peer never took). Never a foreign stream's fd: kj wrappers
//!   forward `getFd()` to their transport socket, so a TLS-shaped wrapper's fd carries
//!   ciphertext, not the stream's bytes.
//!
//! ```text
//! serve_kj_stream(KjOwn<AsyncIoStream>) -> Result<ServedKjStream, TakeSocketError>
//!     |-- native path: wrapper consumed -> ServeIo::Tcp/Unix
//!     `-- pump path:   ServeIo::Duplex (consumer end of the rendezvous) + StreamPump (!Send) owning the KjOwn
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
//! streams it remains KJ's own documented contract. Dropping the pump destroys the owned stream
//! (abort-on-drop) and fails the consumer's outstanding operations; a consumer dropping its
//! `ServeIo::Duplex` ends the pump and destroys the stream, discarding a write the kj peer had
//! not taken -- destroying a kj::WebSocket over a kj pipe does the same.

use std::future::Future;
use std::pin::Pin;
use std::sync::Arc;
use std::sync::Mutex;
use std::task::Context;
use std::task::Poll;
use std::task::Waker;

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
use crate::ffi::is_tokio_stream;
use crate::ffi::release_tokio_stream;
use crate::ffi::split_kj_stream;
use crate::ffi::wrap_tokio_stream;

/// Read chunk size for the pump fallback.
const PUMP_BUF: usize = 8192;

/// The tokio-side byte stream for a served kj stream: a native socket or the consumer end of
/// the pump's rendezvous channel.
///
/// Implements `AsyncRead + AsyncWrite`, so it drops into any tokio consumer. See the module
/// docs ("Where the result may be driven") for the reactor affinity every variant keeps.
pub enum ServeIo {
    Tcp(TcpStream),
    #[cfg(unix)]
    Unix(UnixStream),
    Duplex(PumpedStream),
}

// =======================================================================================
// The pump's rendezvous channel.
//
// A kj stream write is a rendezvous: it completes when the peer has taken the bytes, and
// destroying the stream cancels a write still in flight. The pump keeps those semantics for
// its consumer instead of buffering between them: the consumer's `poll_write` completes only
// once the pump has written the bytes into the kj stream, and dropping the consumer discards
// whatever it had offered but the kj peer never took -- exactly what destroying a
// kj::WebSocket over a kj pipe does. The read direction is the mirror image: the pump reads a
// chunk from the kj stream and hands it over; the consumer takes it at its own pace.
//
// One slot per direction, guarded by a plain mutex: the consumer may run on another thread
// (see the module docs), the pump always runs on the KJ thread.

/// The consumer's pending write (consumer -> kj direction).
enum Outbound {
    /// Nothing offered.
    Idle,
    /// Bytes the consumer offered and the pump has not yet written into the kj stream.
    Offered(Vec<u8>),
    /// The pump wrote this many bytes; the consumer's next `poll_write` completes with it. The
    /// caller retries with the same buffer (tokio's write loops do), so the count applies to
    /// it; `poll_write` never reports more than the retried buffer holds.
    Delivered(usize),
}

/// A chunk the pump read from the kj stream (kj -> consumer direction).
struct Inbound {
    chunk: Vec<u8>,
    taken: usize,
}

struct ChannelState {
    outbound: Outbound,
    /// The consumer called `poll_shutdown`: once `outbound` drains, the pump shuts the kj
    /// stream's write side.
    consumer_shutdown: bool,
    /// The consumer end was dropped. Offered bytes are discarded; both directions end.
    consumer_dropped: bool,
    /// The pump failed writing to the kj stream; the consumer's writes fail with this.
    write_error: Option<std::io::Error>,
    inbound: Option<Inbound>,
    /// The kj stream reached EOF (or a disconnect): the consumer's reads return 0.
    read_closed: bool,
    /// The pump is gone (settled or dropped).
    pump_gone: bool,
    consumer_read_waker: Option<Waker>,
    consumer_write_waker: Option<Waker>,
    pump_waker: Option<Waker>,
}

struct Channel {
    state: Mutex<ChannelState>,
}

impl Channel {
    fn new() -> Arc<Self> {
        Arc::new(Self {
            state: Mutex::new(ChannelState {
                outbound: Outbound::Idle,
                consumer_shutdown: false,
                consumer_dropped: false,
                write_error: None,
                inbound: None,
                read_closed: false,
                pump_gone: false,
                consumer_read_waker: None,
                consumer_write_waker: None,
                pump_waker: None,
            }),
        })
    }

    fn lock(&self) -> std::sync::MutexGuard<'_, ChannelState> {
        self.state
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
    }

    // --- pump side (KJ thread) ---

    /// The consumer's next offered write, or `None` once there will be no more: the consumer
    /// shut down its write side (after draining) or dropped its end (discarding anything
    /// offered). The bool says which: `true` = dropped.
    fn next_write(self: &Arc<Self>) -> impl Future<Output = Option<(Vec<u8>, bool)>> + use<> {
        let chan = Arc::clone(self);
        std::future::poll_fn(move |cx| {
            let mut st = chan.lock();
            if st.consumer_dropped {
                st.outbound = Outbound::Idle;
                return Poll::Ready(None);
            }
            match std::mem::replace(&mut st.outbound, Outbound::Idle) {
                Outbound::Offered(bytes) => Poll::Ready(Some((bytes, false))),
                other => {
                    st.outbound = other;
                    if st.consumer_shutdown {
                        return Poll::Ready(None);
                    }
                    st.pump_waker = Some(cx.waker().clone());
                    Poll::Pending
                }
            }
        })
    }

    /// The pump wrote `len` offered bytes into the kj stream.
    fn complete_write(&self, len: usize) {
        let mut st = self.lock();
        st.outbound = Outbound::Delivered(len);
        if let Some(waker) = st.consumer_write_waker.take() {
            waker.wake();
        }
    }

    /// Writing to the kj stream failed; the consumer's writes fail from now on.
    fn fail_writes(&self, error: std::io::Error) {
        let mut st = self.lock();
        st.write_error = Some(error);
        if let Some(waker) = st.consumer_write_waker.take() {
            waker.wake();
        }
    }

    /// Whether the consumer shut its write side down (as opposed to dropping its end).
    fn consumer_shutdown(&self) -> bool {
        self.lock().consumer_shutdown
    }

    /// Hands a chunk read from the kj stream to the consumer; resolves once the consumer has
    /// taken all of it, or `false` if the consumer dropped its end first.
    fn deliver_read(self: &Arc<Self>, chunk: Vec<u8>) -> impl Future<Output = bool> + use<> {
        let chan = Arc::clone(self);
        let mut chunk = Some(chunk);
        std::future::poll_fn(move |cx| {
            let mut st = chan.lock();
            if st.consumer_dropped {
                return Poll::Ready(false);
            }
            if let Some(chunk) = chunk.take() {
                st.inbound = Some(Inbound { chunk, taken: 0 });
                if let Some(waker) = st.consumer_read_waker.take() {
                    waker.wake();
                }
            }
            // The consumer clears `inbound` when it takes the last byte.
            if st.inbound.is_some() {
                st.pump_waker = Some(cx.waker().clone());
                Poll::Pending
            } else {
                Poll::Ready(true)
            }
        })
    }

    /// The kj stream has no more data: the consumer's reads return EOF.
    fn close_read(&self) {
        let mut st = self.lock();
        st.read_closed = true;
        if let Some(waker) = st.consumer_read_waker.take() {
            waker.wake();
        }
    }

    /// Resolves once the consumer has dropped its end.
    fn consumer_gone(self: &Arc<Self>) -> impl Future<Output = ()> + use<> {
        let chan = Arc::clone(self);
        std::future::poll_fn(move |cx| {
            let mut st = chan.lock();
            if st.consumer_dropped {
                Poll::Ready(())
            } else {
                st.pump_waker = Some(cx.waker().clone());
                Poll::Pending
            }
        })
    }

    /// The pump is gone: the consumer's outstanding operations fail and its reads return EOF.
    fn pump_gone(&self) {
        let mut st = self.lock();
        st.pump_gone = true;
        st.read_closed = true;
        for waker in [
            st.consumer_read_waker.take(),
            st.consumer_write_waker.take(),
        ] {
            if let Some(waker) = waker {
                waker.wake();
            }
        }
    }
}

/// Runs `Channel::pump_gone` when the pump future is dropped or settles.
struct PumpGoneGuard(Arc<Channel>);

impl Drop for PumpGoneGuard {
    fn drop(&mut self) {
        self.0.pump_gone();
    }
}

/// The consumer end of a pumped stream (see the module docs and the channel above).
pub struct PumpedStream {
    chan: Arc<Channel>,
}

impl Drop for PumpedStream {
    fn drop(&mut self) {
        let mut st = self.chan.lock();
        st.consumer_dropped = true;
        st.outbound = Outbound::Idle;
        if let Some(waker) = st.pump_waker.take() {
            waker.wake();
        }
    }
}

impl AsyncRead for PumpedStream {
    fn poll_read(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &mut ReadBuf<'_>,
    ) -> Poll<std::io::Result<()>> {
        let mut st = self.chan.lock();
        if let Some(inbound) = &mut st.inbound {
            let remaining = &inbound.chunk[inbound.taken..];
            let n = remaining.len().min(buf.remaining());
            buf.put_slice(&remaining[..n]);
            inbound.taken += n;
            if inbound.taken == inbound.chunk.len() {
                // The chunk is consumed: clear it here, not when the pump next runs, so a
                // read polled before then waits for more rather than filling nothing (which
                // the consumer would take as EOF).
                st.inbound = None;
                if let Some(waker) = st.pump_waker.take() {
                    waker.wake();
                }
            }
            return Poll::Ready(Ok(()));
        }
        if st.read_closed {
            return Poll::Ready(Ok(()));
        }
        st.consumer_read_waker = Some(cx.waker().clone());
        Poll::Pending
    }
}

impl AsyncWrite for PumpedStream {
    fn poll_write(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &[u8],
    ) -> Poll<std::io::Result<usize>> {
        let mut st = self.chan.lock();
        // A write the pump already put into the kj stream completes even if the pump has since
        // gone away: the bytes were delivered.
        if let Outbound::Delivered(len) = st.outbound {
            st.outbound = Outbound::Idle;
            return Poll::Ready(Ok(len.min(buf.len())));
        }
        if let Some(error) = &st.write_error {
            return Poll::Ready(Err(std::io::Error::new(error.kind(), error.to_string())));
        }
        if st.pump_gone {
            return Poll::Ready(Err(std::io::ErrorKind::BrokenPipe.into()));
        }
        match std::mem::replace(&mut st.outbound, Outbound::Idle) {
            Outbound::Idle => {
                if buf.is_empty() {
                    return Poll::Ready(Ok(0));
                }
                st.outbound = Outbound::Offered(buf.to_vec());
                st.consumer_write_waker = Some(cx.waker().clone());
                if let Some(waker) = st.pump_waker.take() {
                    waker.wake();
                }
                Poll::Pending
            }
            Outbound::Offered(bytes) => {
                st.outbound = Outbound::Offered(bytes);
                st.consumer_write_waker = Some(cx.waker().clone());
                Poll::Pending
            }
            // Handled above.
            Outbound::Delivered(len) => Poll::Ready(Ok(len.min(buf.len()))),
        }
    }

    fn poll_flush(self: Pin<&mut Self>, _cx: &mut Context<'_>) -> Poll<std::io::Result<()>> {
        // Writes are rendezvous: a completed write is already in the kj stream.
        Poll::Ready(Ok(()))
    }

    fn poll_shutdown(self: Pin<&mut Self>, _cx: &mut Context<'_>) -> Poll<std::io::Result<()>> {
        let mut st = self.chan.lock();
        st.consumer_shutdown = true;
        if let Some(waker) = st.pump_waker.take() {
            waker.wake();
        }
        Poll::Ready(Ok(()))
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

/// Why [`unwrap_native`] handed the stream back.
enum NotTaken {
    /// Not a kj-rs-io stream: there is no socket to take. The pump path applies.
    Foreign,
    /// A kj-rs-io stream whose socket cannot be taken right now (an operation still owns a
    /// share of it, or the wrong loop): the stream comes back re-wrapped, untouched.
    Refused(KjError),
}

/// Tier-1 unwrap: if the owned stream is kj-rs-io-originated, consumes the wrapper and returns
/// its native tokio socket; otherwise hands the stream back, saying why.
fn unwrap_native(
    stream: KjOwn<AsyncIoStream>,
) -> Result<ServeIo, (KjOwn<AsyncIoStream>, NotTaken)> {
    if !is_tokio_stream(stream.as_ref()) {
        return Err((stream, NotTaken::Foreign));
    }
    match release_tokio_stream(stream).into_socket() {
        Ok(socket) => Ok(ServeIo::from(socket)),
        Err((native, error)) => Err((wrap_tokio_stream(native), NotTaken::Refused(error))),
    }
}

/// Takes the stream's native tokio socket: the unwrap path only.
///
/// Moves the native tokio object out of a kj-rs-io stream. The result is an owned [`ServeIo`]
/// (never [`ServeIo::Duplex`]) that no longer depends on the consumed kj stream, which is
/// gone before returning; it remains registered with this thread's loop runtime (see the
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
    match unwrap_native(stream) {
        Ok(io) => Ok(io),
        Err((stream, NotTaken::Foreign)) => Err(TakeSocketError {
            stream,
            error: KjError::new(
                KjExceptionType::Failed,
                "cannot take the stream's socket natively: not a kj-rs-io stream (serve it \
                 through serve_kj_stream's pump instead)"
                    .to_owned(),
            ),
        }),
        Err((stream, NotTaken::Refused(error))) => Err(TakeSocketError { stream, error }),
    }
}

/// Yields the best-available tokio-side stream for the owned `stream`.
///
/// That is the native tokio object when `stream` originated in kj-rs-io, else an in-memory
/// duplex bridged by a pump future that owns the stream. Never extracts a foreign stream's fd
/// (see the module docs); callers that can assert a plain socket should prefer
/// [`take_kj_socket`].
///
/// On the native path the wrapper is consumed before returning; on the pump path the
/// stream lives inside the pump and is destroyed when the pump settles or is dropped. The pump
/// must only be polled from the KJ event-loop thread owning the stream.
///
/// # Errors
///
/// Returns the stream when it is a kj-rs-io wrapper whose native socket cannot be extracted (an
/// I/O operation still owns a share of it), or when the pump's end objects cannot be allocated.
pub fn serve_kj_stream(stream: KjOwn<AsyncIoStream>) -> Result<ServedKjStream, TakeSocketError> {
    let stream = match unwrap_native(stream) {
        Ok(io) => return Ok(ServedKjStream { io, pump: None }),
        Err((stream, NotTaken::Foreign)) => stream,
        Err((stream, NotTaken::Refused(error))) => return Err(TakeSocketError { stream, error }),
    };
    let chan = Channel::new();
    let pump = Box::pin(pump_kj_stream(stream, Arc::clone(&chan)));
    Ok(ServedKjStream {
        io: ServeIo::Duplex(PumpedStream { chan }),
        pump: Some(pump),
    })
}

/// Whether a bridged kj exception is peer-teardown-shaped (treated as EOF by the pump).
fn is_disconnected(exception: &KjException) -> bool {
    exception.r#type() == KjExceptionType::Disconnected
}

/// The pump: bridges the owned `stream` (via bridged `kj::io` promises, on the calling KJ
/// thread) to the consumer end of `chan`. Owns the stream: it is destroyed when this future
/// settles or is dropped.
///
/// The stream is split into its read and write directions ([`split_kj_stream`]): two C++ end
/// objects sharing ownership of the stream, each driven through an exclusive borrow, so the
/// borrow checker enforces kj's stream contract -- at most one read and one write in flight --
/// and C++ ownership keeps the stream alive as long as either end exists.
async fn pump_kj_stream(stream: KjOwn<AsyncIoStream>, chan: Arc<Channel>) -> Result<(), KjError> {
    let _gone = PumpGoneGuard(Arc::clone(&chan));
    let (mut rd, mut wr) = split_kj_stream(stream).map_err(KjError::from)?;

    // kj -> consumer: read a chunk, hand it over, repeat. EOF (or a DISCONNECTED read, the
    // reset-peer shape) closes the consumer's read side; the consumer dropping its end ends
    // the direction (nobody is left to read, and the stream is about to be destroyed).
    let kj_to_consumer = async {
        let chan = &chan;
        let mut buf = vec![0u8; PUMP_BUF];
        loop {
            let read = tokio::select! {
                () = chan.consumer_gone() => return Ok::<(), KjError>(()),
                read = rd.try_read(&mut buf, 1) => read,
            };
            let n = match read {
                Ok(n) => n,
                Err(e) if is_disconnected(&e) => 0,
                Err(e) => return Err(KjError::from(e)),
            };
            if n == 0 {
                chan.close_read();
                return Ok::<(), KjError>(());
            }
            if !chan.deliver_read(buf[..n].to_vec()).await {
                return Ok(());
            }
        }
    };

    // consumer -> kj: each offered write goes into the kj stream and completes the consumer's
    // write once written -- a rendezvous, as with a kj pipe. A consumer shutdown becomes
    // shutdownWrite() after the last offered write; a consumer drop discards anything still
    // offered, as destroying a kj stream cancels its in-flight write.
    let consumer_to_kj = async {
        let chan = &chan;
        loop {
            match chan.next_write().await {
                // The consumer dropping its end mid-write cancels the kj write (dropping the
                // bridged promise), as destroying a kj stream cancels its in-flight write.
                Some((bytes, _)) => match tokio::select! {
                    () = chan.consumer_gone() => return Ok::<(), KjError>(()),
                    written = wr.write(&bytes) => written,
                } {
                    Ok(()) => chan.complete_write(bytes.len()),
                    Err(e) if is_disconnected(&e) => {
                        chan.fail_writes(std::io::ErrorKind::BrokenPipe.into());
                        return Ok::<(), KjError>(());
                    }
                    Err(e) => {
                        let error = KjError::from(e);
                        chan.fail_writes(std::io::Error::other(error.description().to_owned()));
                        return Err(error);
                    }
                },
                None => {
                    if chan.consumer_shutdown() {
                        match wr.shutdown_write() {
                            Ok(()) => {}
                            Err(e) if is_disconnected(&e) => {}
                            Err(e) => return Err(KjError::from(e)),
                        }
                    }
                    return Ok::<(), KjError>(());
                }
            }
        }
    };

    tokio::try_join!(kj_to_consumer, consumer_to_kj).map(|((), ())| ())
}
