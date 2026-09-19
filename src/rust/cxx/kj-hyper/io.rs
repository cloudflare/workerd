//! kj streams as tokio streams, and tokio streams as kj streams.

use std::cell::Cell;
use std::cell::RefCell;
use std::future::Future;
use std::pin::Pin;
use std::rc::Rc;
use std::sync::Arc;
use std::sync::Mutex;
use std::task::Context;
use std::task::Poll;
use std::task::Wake;
use std::task::Waker;

use bytes::Buf;
use bytes::Bytes;
use cxx::KjError;
use cxx::KjException;
use cxx::KjExceptionType;
use futures::FutureExt;
use futures::future::LocalBoxFuture;
use futures::future::Shared;
use kj_rs::KjOwn;
use tokio::io::AsyncRead;
use tokio::io::AsyncWrite;
use tokio::io::ReadBuf;
use tokio::sync::oneshot;

use crate::ffi::AsyncIoStream;
use crate::ffi::KjStreamReadHalf;
use crate::ffi::KjStreamWriteHalf;

/// Any tokio byte stream.
pub trait Io: AsyncRead + AsyncWrite + Unpin {}
impl<T: AsyncRead + AsyncWrite + Unpin + ?Sized> Io for T {}

pub type BoxIo = Box<dyn Io>;

// =======================================================================================
// Hangup

type Observation = LocalBoxFuture<'static, crate::Result<()>>;

/// kj's `whenWriteDisconnected` for a connection: resolves once the peer can no longer receive, or
/// fails as the observation does. Clones share one watch, owned by the connection's I/O object
/// ([`HangupWatch`]), so a clone (in a response an application keeps, say) holds none of the
/// watch's resources; once the connection is gone nothing is observed, and clones never resolve.
#[derive(Clone)]
pub struct Hangup(Option<Shared<Observation>>);

impl Hangup {
    /// A connection whose peer is never observed to go away.
    #[must_use]
    pub fn never() -> Self {
        Self(None)
    }
}

impl Future for Hangup {
    type Output = crate::Result<()>;

    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<crate::Result<()>> {
        match &mut self.get_mut().0 {
            Some(watch) => watch.poll_unpin(cx),
            None => Poll::Pending,
        }
    }
}

/// The owning end of a [`Hangup`], held by the connection's I/O object: dropping it drops the
/// observation (a descriptor `dup`, or a kj promise on the stream).
pub struct HangupWatch(Rc<RefCell<Option<Observation>>>);

impl HangupWatch {
    pub fn new(observe: impl Future<Output = crate::Result<()>> + 'static) -> (Self, Hangup) {
        let observation = Rc::new(RefCell::new(Some(observe.boxed_local())));
        let shared = Rc::downgrade(&observation);
        let hangup = std::future::poll_fn(move |cx| {
            let Some(observation) = shared.upgrade() else {
                return Poll::Pending;
            };
            let mut observation = observation.borrow_mut();
            match observation.as_mut() {
                Some(future) => future.poll_unpin(cx),
                None => Poll::Pending,
            }
        });
        let hangup: Observation = hangup.boxed_local();
        (Self(observation), Hangup(Some(hangup.shared())))
    }
}

impl Drop for HangupWatch {
    fn drop(&mut self) {
        // Released outside the borrow: dropping a kj promise may run code.
        let observation = self.0.borrow_mut().take();
        drop(observation);
    }
}

/// A connection's I/O with its hangup watch, which goes when the connection does.
struct Watched<T> {
    io: T,
    _watch: HangupWatch,
}

impl<T: AsyncRead + Unpin> AsyncRead for Watched<T> {
    fn poll_read(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &mut ReadBuf<'_>,
    ) -> Poll<std::io::Result<()>> {
        Pin::new(&mut self.get_mut().io).poll_read(cx, buf)
    }
}

impl<T: AsyncWrite + Unpin> AsyncWrite for Watched<T> {
    fn poll_write(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &[u8],
    ) -> Poll<std::io::Result<usize>> {
        Pin::new(&mut self.get_mut().io).poll_write(cx, buf)
    }

    fn poll_write_vectored(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        bufs: &[std::io::IoSlice<'_>],
    ) -> Poll<std::io::Result<usize>> {
        Pin::new(&mut self.get_mut().io).poll_write_vectored(cx, bufs)
    }

    fn is_write_vectored(&self) -> bool {
        self.io.is_write_vectored()
    }

    fn poll_flush(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<std::io::Result<()>> {
        Pin::new(&mut self.get_mut().io).poll_flush(cx)
    }

    fn poll_shutdown(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<std::io::Result<()>> {
        Pin::new(&mut self.get_mut().io).poll_shutdown(cx)
    }
}

/// A kj stream the caller hands over, as a tokio stream: its tokio socket where there is one to
/// take (a kj-rs-io socket, or a `RustIo` nothing is using), else the kj stream driven directly.
pub fn kj_to_tokio(stream: KjOwn<AsyncIoStream>) -> (BoxIo, Hangup) {
    if crate::ffi::is_releasable_rust_io(stream.as_ref()) {
        return (*crate::ffi::release_rust_io(stream)).into_parts();
    }
    if crate::ffi::is_tokio_stream(stream.as_ref()) {
        return match crate::ffi::release_tokio_stream(stream).into_socket() {
            Ok(socket) => {
                let (watch, hangup) = match socket.hangup() {
                    Ok(observe) => {
                        HangupWatch::new(observe.map(|r| r.map_err(|e| io_kj_error(&e))))
                    }
                    Err(e) => {
                        let error = io_kj_error(&e);
                        HangupWatch::new(async move { Err(error) })
                    }
                };
                let io: BoxIo = match socket {
                    kj_rs_io::Socket::Tcp(socket) => {
                        let _ = socket.set_nodelay(true);
                        Box::new(Watched {
                            io: socket,
                            _watch: watch,
                        })
                    }
                    #[cfg(unix)]
                    kj_rs_io::Socket::Unix(socket) => Box::new(Watched {
                        io: socket,
                        _watch: watch,
                    }),
                };
                (io, hangup)
            }
            // An operation still holds the socket: drive the re-wrapped stream instead.
            Err((native, _)) => KjIo::open(crate::ffi::wrap_tokio_stream(native)),
        };
    }
    KjIo::open(stream)
}

/// A kj stream the caller keeps using (a non-owning `kj::Own`), as a tokio stream: driven
/// directly, never taken apart.
pub fn drive_kj_stream(stream: KjOwn<AsyncIoStream>) -> (BoxIo, Hangup) {
    KjIo::open(stream)
}

// =======================================================================================
// KjIo

const READ_CHUNK: usize = 8192;

type ReadOp =
    Pin<Box<dyn Future<Output = (KjStreamReadHalf, Vec<u8>, Result<usize, KjException>)>>>;
type WriteOp = Pin<Box<dyn Future<Output = (KjStreamWriteHalf, Result<(), KjException>)>>>;

enum Reader {
    /// The buffer is kept between reads.
    Idle(KjStreamReadHalf, Vec<u8>),
    Reading(ReadOp),
    Buffered(KjStreamReadHalf, Vec<u8>, usize),
    Eof,
    /// The stream failed: every later read fails the same way.
    Failed(KjError),
}

enum Writer {
    Idle(KjStreamWriteHalf),
    Writing(WriteOp),
    Done,
}

/// A foreign kj stream as a tokio stream, polling its bridged kj promises directly (one per
/// direction). A write is accepted once the kj write has started; `poll_flush` (and the next
/// write) waits for it to finish, as a buffered tokio writer does.
struct KjIo {
    reader: Reader,
    writer: Writer,
    _watch: HangupWatch,
}

impl KjIo {
    fn open(stream: KjOwn<AsyncIoStream>) -> (BoxIo, Hangup) {
        let (mut read, write) = crate::ffi::split_kj_stream(stream);
        let (watch, hangup) = HangupWatch::new(read.when_write_disconnected());
        let io = Self {
            reader: Reader::Idle(read, Vec::new()),
            writer: Writer::Idle(write),
            _watch: watch,
        };
        (Box::new(io), hangup)
    }

    /// Drives a write in flight to completion.
    fn poll_written(writer: &mut Writer, cx: &mut Context<'_>) -> Poll<std::io::Result<()>> {
        match std::mem::replace(writer, Writer::Done) {
            Writer::Writing(mut op) => match op.as_mut().poll(cx) {
                Poll::Pending => {
                    *writer = Writer::Writing(op);
                    Poll::Pending
                }
                Poll::Ready((half, Ok(()))) => {
                    *writer = Writer::Idle(half);
                    Poll::Ready(Ok(()))
                }
                Poll::Ready((_, Err(e))) => Poll::Ready(Err(io_error(e))),
            },
            Writer::Idle(half) => {
                *writer = Writer::Idle(half);
                Poll::Ready(Ok(()))
            }
            Writer::Done => Poll::Ready(Err(std::io::ErrorKind::BrokenPipe.into())),
        }
    }
}

fn io_error(e: KjException) -> std::io::Error {
    KjIoError::into_io(KjError::from(e))
}

/// A kj exception carried through `std::io::Error` (and the hyper errors wrapping it), so the
/// caller gets the original exception back.
#[derive(Debug)]
pub struct KjIoError(KjError);

impl KjIoError {
    pub fn into_io(error: KjError) -> std::io::Error {
        let kind = if error.exception_type() == KjExceptionType::Disconnected {
            std::io::ErrorKind::BrokenPipe
        } else {
            std::io::ErrorKind::Other
        };
        std::io::Error::new(kind, Self(error))
    }

    /// The kj exception somewhere in `error`'s chain, if any.
    pub fn find(error: &(dyn std::error::Error + 'static)) -> Option<KjError> {
        let mut next = Some(error);
        while let Some(error) = next {
            let carried = error.downcast_ref::<Self>().or_else(|| {
                error
                    .downcast_ref::<std::io::Error>()
                    .and_then(std::io::Error::get_ref)
                    .and_then(|inner| inner.downcast_ref::<Self>())
            });
            if let Some(carried) = carried {
                return Some(carried.0.clone());
            }
            next = error.source();
        }
        None
    }
}

impl std::fmt::Display for KjIoError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(self.0.description())
    }
}

impl std::error::Error for KjIoError {}

impl AsyncRead for KjIo {
    fn poll_read(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &mut ReadBuf<'_>,
    ) -> Poll<std::io::Result<()>> {
        let reader = &mut self.get_mut().reader;
        loop {
            match std::mem::replace(reader, Reader::Eof) {
                Reader::Eof => return Poll::Ready(Ok(())),
                Reader::Failed(error) => {
                    *reader = Reader::Failed(error.clone());
                    return Poll::Ready(Err(KjIoError::into_io(error)));
                }
                Reader::Buffered(half, chunk, taken) => {
                    let n = (chunk.len() - taken).min(buf.remaining());
                    buf.put_slice(&chunk[taken..taken + n]);
                    *reader = if taken + n == chunk.len() {
                        Reader::Idle(half, chunk)
                    } else {
                        Reader::Buffered(half, chunk, taken + n)
                    };
                    return Poll::Ready(Ok(()));
                }
                Reader::Idle(mut half, mut chunk) => {
                    chunk.resize(READ_CHUNK, 0);
                    *reader = Reader::Reading(Box::pin(async move {
                        let result = half.try_read(&mut chunk, 1).await;
                        (half, chunk, result)
                    }));
                }
                Reader::Reading(mut op) => match op.as_mut().poll(cx) {
                    Poll::Pending => {
                        *reader = Reader::Reading(op);
                        return Poll::Pending;
                    }
                    Poll::Ready((half, mut chunk, Ok(n))) if n > 0 => {
                        chunk.truncate(n);
                        *reader = Reader::Buffered(half, chunk, 0);
                    }
                    Poll::Ready((_, _, Ok(_))) => return Poll::Ready(Ok(())),
                    Poll::Ready((_, _, Err(e))) => {
                        let error = KjError::from(e);
                        *reader = Reader::Failed(error.clone());
                        return Poll::Ready(Err(KjIoError::into_io(error)));
                    }
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
        let writer = &mut self.get_mut().writer;
        std::task::ready!(Self::poll_written(writer, cx))?;
        let Writer::Idle(mut half) = std::mem::replace(writer, Writer::Done) else {
            return Poll::Ready(Err(std::io::ErrorKind::BrokenPipe.into()));
        };
        let bytes = buf.to_vec();
        let mut op: WriteOp = Box::pin(async move {
            let result = half.write(&bytes).await;
            (half, result)
        });
        // Started here: the bytes are the kj stream's now.
        match op.as_mut().poll(cx) {
            Poll::Pending => *writer = Writer::Writing(op),
            Poll::Ready((half, Ok(()))) => *writer = Writer::Idle(half),
            Poll::Ready((_, Err(e))) => return Poll::Ready(Err(io_error(e))),
        }
        Poll::Ready(Ok(buf.len()))
    }

    fn poll_flush(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<std::io::Result<()>> {
        Self::poll_written(&mut self.get_mut().writer, cx)
    }

    fn poll_shutdown(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<std::io::Result<()>> {
        let writer = &mut self.get_mut().writer;
        if matches!(writer, Writer::Done) {
            return Poll::Ready(Ok(()));
        }
        std::task::ready!(Self::poll_written(writer, cx))?;
        match std::mem::replace(writer, Writer::Done) {
            Writer::Idle(mut half) => Poll::Ready(half.shutdown_write().map_err(io_error)),
            Writer::Writing(_) | Writer::Done => Poll::Ready(Ok(())),
        }
    }
}

// =======================================================================================
// RustIo

struct IoState {
    io: RefCell<BoxIo>,
    hangup: Hangup,
    read_aborted: Cell<bool>,
    /// Wakes a read parked on the stream when `abortRead()` is called.
    read_waker: RefCell<Option<Waker>>,
}

/// A tokio stream handed to kj as a `kj::AsyncIoStream` (TLS streams, CONNECT tunnels). Each
/// operation owns a share of the stream, so it may outlive the handle. hyper takes the stream back
/// out ([`kj_to_tokio`]) when no kj operation is using it.
pub struct RustIo(Rc<IoState>);

impl RustIo {
    pub fn new(io: impl Io + 'static, hangup: Hangup) -> Self {
        Self(Rc::new(IoState {
            io: RefCell::new(Box::new(io)),
            hangup,
            read_aborted: Cell::new(false),
            read_waker: RefCell::new(None),
        }))
    }

    /// kj's `whenWriteDisconnected`.
    pub fn when_write_disconnected(&self) -> Hangup {
        self.0.hangup.clone()
    }

    /// Whether no operation holds the stream.
    #[must_use]
    pub fn can_release(&self) -> bool {
        Rc::strong_count(&self.0) == 1
    }

    /// The stream and its hangup. An operation still holding the stream keeps it, and the caller
    /// gets a stream that fails.
    pub fn into_parts(self) -> (BoxIo, Hangup) {
        match Rc::try_unwrap(self.0) {
            Ok(state) => (state.io.into_inner(), state.hangup),
            Err(state) => (Box::new(Unavailable), state.hangup.clone()),
        }
    }

    /// kj's `tryRead`: reads until `min_bytes` or EOF.
    pub fn read<'b>(
        &self,
        mut buf: ReadBuf<'b>,
        min_bytes: usize,
    ) -> impl Future<Output = crate::Result<usize>> + use<'b> {
        let state = self.0.clone();
        async move {
            let min_bytes = min_bytes.min(buf.capacity());
            while buf.filled().len() < min_bytes && !state.read_aborted.get() {
                let before = buf.filled().len();
                std::future::poll_fn(|cx| {
                    if state.read_aborted.get() {
                        return Poll::Ready(Ok(()));
                    }
                    *state.read_waker.borrow_mut() = Some(cx.waker().clone());
                    Pin::new(&mut **state.io.borrow_mut()).poll_read(cx, &mut buf)
                })
                .await
                .map_err(|e| io_kj_error(&e))?;
                if buf.filled().len() == before {
                    break;
                }
            }
            if state.read_aborted.get() {
                return Err(KjError::new(
                    KjExceptionType::Disconnected,
                    "abortRead() called".to_owned(),
                ));
            }
            Ok(buf.filled().len())
        }
    }

    /// Resolves once the peer can receive every byte: a TLS stream may hold written bytes until
    /// flushed.
    pub fn write<'b>(&self, data: &'b [u8]) -> impl Future<Output = crate::Result<()>> + use<'b> {
        let state = self.0.clone();
        async move {
            let mut written = 0;
            while written < data.len() {
                written += std::future::poll_fn(|cx| {
                    Pin::new(&mut **state.io.borrow_mut()).poll_write(cx, &data[written..])
                })
                .await
                .map_err(|e| io_kj_error(&e))?;
            }
            std::future::poll_fn(|cx| Pin::new(&mut **state.io.borrow_mut()).poll_flush(cx))
                .await
                .map_err(|e| io_kj_error(&e))
        }
    }

    pub fn shutdown_write(&self) -> impl Future<Output = crate::Result<()>> + use<> {
        let state = self.0.clone();
        async move {
            std::future::poll_fn(|cx| Pin::new(&mut **state.io.borrow_mut()).poll_shutdown(cx))
                .await
                .map_err(|e| io_kj_error(&e))
        }
    }

    pub fn abort_read(&self) {
        self.0.read_aborted.set(true);
        if let Some(waker) = self.0.read_waker.borrow_mut().take() {
            waker.wake();
        }
    }
}

/// The stream a released `RustIo` leaves while an operation still holds its transport.
struct Unavailable;

fn unavailable() -> std::io::Error {
    std::io::Error::new(
        std::io::ErrorKind::NotConnected,
        "the stream was taken while an operation was using it",
    )
}

impl AsyncRead for Unavailable {
    fn poll_read(
        self: Pin<&mut Self>,
        _cx: &mut Context<'_>,
        _buf: &mut ReadBuf<'_>,
    ) -> Poll<std::io::Result<()>> {
        Poll::Ready(Err(unavailable()))
    }
}

impl AsyncWrite for Unavailable {
    fn poll_write(
        self: Pin<&mut Self>,
        _cx: &mut Context<'_>,
        _buf: &[u8],
    ) -> Poll<std::io::Result<usize>> {
        Poll::Ready(Err(unavailable()))
    }

    fn poll_flush(self: Pin<&mut Self>, _cx: &mut Context<'_>) -> Poll<std::io::Result<()>> {
        Poll::Ready(Err(unavailable()))
    }

    fn poll_shutdown(self: Pin<&mut Self>, _cx: &mut Context<'_>) -> Poll<std::io::Result<()>> {
        Poll::Ready(Err(unavailable()))
    }
}

/// An I/O failure as a kj exception: peer-teardown kinds (a TLS peer closing without
/// `close_notify` among them) are DISCONNECTED.
pub fn io_kj_error(e: &std::io::Error) -> KjError {
    use std::io::ErrorKind;
    if let Some(error) = KjIoError::find(e) {
        return error;
    }
    let kind = match e.kind() {
        ErrorKind::ConnectionReset
        | ErrorKind::ConnectionAborted
        | ErrorKind::BrokenPipe
        | ErrorKind::NotConnected
        | ErrorKind::UnexpectedEof => KjExceptionType::Disconnected,
        _ => KjExceptionType::Failed,
    };
    KjError::new(kind, e.to_string())
}

// =======================================================================================
// SharedWakers

#[derive(Default)]
struct Wakers {
    read: Mutex<Option<Waker>>,
    write: Mutex<Option<Waker>>,
}

fn take(slot: &Mutex<Option<Waker>>) -> Option<Waker> {
    slot.lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
        .take()
}

impl Wake for Wakers {
    fn wake(self: Arc<Self>) {
        self.wake_by_ref();
    }

    fn wake_by_ref(self: &Arc<Self>) {
        for waker in [take(&self.read), take(&self.write)].into_iter().flatten() {
            waker.wake();
        }
    }
}

/// A stream whose reads and writes may each drive the other direction of the transport (TLS:
/// rustls writes while reading and reads while writing), read and written by different tasks.
/// The transport keeps one waker per direction, so each task could replace the other's; every
/// poll here registers both tasks, so neither wake-up is lost.
pub struct SharedWakers<T> {
    io: T,
    wakers: Arc<Wakers>,
    waker: Waker,
}

impl<T> SharedWakers<T> {
    pub fn new(io: T) -> Self {
        let wakers = Arc::new(Wakers::default());
        Self {
            io,
            waker: Waker::from(wakers.clone()),
            wakers,
        }
    }
}

impl<T: Unpin> SharedWakers<T> {
    fn poll_with<R>(
        &mut self,
        cx: &Context<'_>,
        read: bool,
        poll: impl FnOnce(Pin<&mut T>, &mut Context<'_>) -> Poll<R>,
    ) -> Poll<R> {
        let slot = if read {
            &self.wakers.read
        } else {
            &self.wakers.write
        };
        *slot
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner) = Some(cx.waker().clone());
        poll(
            Pin::new(&mut self.io),
            &mut Context::from_waker(&self.waker),
        )
    }
}

impl<T: AsyncRead + Unpin> AsyncRead for SharedWakers<T> {
    fn poll_read(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &mut ReadBuf<'_>,
    ) -> Poll<std::io::Result<()>> {
        self.get_mut()
            .poll_with(cx, true, |io, cx| io.poll_read(cx, buf))
    }
}

impl<T: AsyncWrite + Unpin> AsyncWrite for SharedWakers<T> {
    fn poll_write(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &[u8],
    ) -> Poll<std::io::Result<usize>> {
        self.get_mut()
            .poll_with(cx, false, |io, cx| io.poll_write(cx, buf))
    }

    fn poll_flush(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<std::io::Result<()>> {
        self.get_mut().poll_with(cx, false, AsyncWrite::poll_flush)
    }

    fn poll_shutdown(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<std::io::Result<()>> {
        self.get_mut()
            .poll_with(cx, false, AsyncWrite::poll_shutdown)
    }
}

// =======================================================================================
// Upgrades

/// A connection's transport after an HTTP upgrade: what hyper had already read, then the rest.
struct Rewind {
    prefix: Bytes,
    io: BoxIo,
}

impl AsyncRead for Rewind {
    fn poll_read(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &mut ReadBuf<'_>,
    ) -> Poll<std::io::Result<()>> {
        let this = self.get_mut();
        if this.prefix.is_empty() {
            return Pin::new(&mut *this.io).poll_read(cx, buf);
        }
        let n = this.prefix.len().min(buf.remaining());
        buf.put_slice(&this.prefix[..n]);
        this.prefix.advance(n);
        Poll::Ready(Ok(()))
    }
}

impl AsyncWrite for Rewind {
    fn poll_write(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &[u8],
    ) -> Poll<std::io::Result<usize>> {
        Pin::new(&mut *self.get_mut().io).poll_write(cx, buf)
    }

    fn poll_write_vectored(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        bufs: &[std::io::IoSlice<'_>],
    ) -> Poll<std::io::Result<usize>> {
        Pin::new(&mut *self.get_mut().io).poll_write_vectored(cx, bufs)
    }

    fn is_write_vectored(&self) -> bool {
        self.io.is_write_vectored()
    }

    fn poll_flush(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<std::io::Result<()>> {
        Pin::new(&mut *self.get_mut().io).poll_flush(cx)
    }

    fn poll_shutdown(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<std::io::Result<()>> {
        Pin::new(&mut *self.get_mut().io).poll_shutdown(cx)
    }
}

/// A connection's transport once hyper has finished HTTP on it, with the bytes it read ahead.
#[must_use]
pub fn rewound(io: BoxIo, read_ahead: Bytes) -> BoxIo {
    if read_ahead.is_empty() {
        io
    } else {
        Box::new(Rewind {
            prefix: read_ahead,
            io,
        })
    }
}

/// Where an upgraded connection's transport is delivered.
pub type UpgradeTx = oneshot::Sender<BoxIo>;

/// A transport still being handed over (an upgraded connection hyper is finishing): I/O waits for
/// it. A read and a write may both be waiting, so the hand-over wakes every waiter.
pub struct Upgraded {
    state: UpgradeState,
    waiters: Vec<Waker>,
}

enum UpgradeState {
    Pending(oneshot::Receiver<BoxIo>),
    Ready(BoxIo),
    Failed,
}

impl Upgraded {
    #[must_use]
    pub fn new(rx: oneshot::Receiver<BoxIo>) -> Self {
        Self {
            state: UpgradeState::Pending(rx),
            waiters: Vec::new(),
        }
    }

    fn ready(&mut self, cx: &mut Context<'_>) -> Poll<std::io::Result<&mut BoxIo>> {
        if let UpgradeState::Pending(rx) = &mut self.state {
            let Poll::Ready(result) = rx.poll_unpin(cx) else {
                if !self.waiters.iter().any(|w| w.will_wake(cx.waker())) {
                    self.waiters.push(cx.waker().clone());
                }
                return Poll::Pending;
            };
            self.state = match result {
                Ok(io) => UpgradeState::Ready(io),
                Err(_) => UpgradeState::Failed,
            };
            self.waiters.drain(..).for_each(Waker::wake);
        }
        match &mut self.state {
            UpgradeState::Ready(io) => Poll::Ready(Ok(io)),
            UpgradeState::Failed => Poll::Ready(Err(std::io::Error::new(
                std::io::ErrorKind::NotConnected,
                "the connection was not upgraded",
            ))),
            UpgradeState::Pending(_) => Poll::Pending,
        }
    }
}

impl AsyncRead for Upgraded {
    fn poll_read(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &mut ReadBuf<'_>,
    ) -> Poll<std::io::Result<()>> {
        let io = std::task::ready!(self.get_mut().ready(cx))?;
        Pin::new(&mut **io).poll_read(cx, buf)
    }
}

impl AsyncWrite for Upgraded {
    fn poll_write(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &[u8],
    ) -> Poll<std::io::Result<usize>> {
        let io = std::task::ready!(self.get_mut().ready(cx))?;
        Pin::new(&mut **io).poll_write(cx, buf)
    }

    fn poll_flush(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<std::io::Result<()>> {
        let io = std::task::ready!(self.get_mut().ready(cx))?;
        Pin::new(&mut **io).poll_flush(cx)
    }

    fn poll_shutdown(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<std::io::Result<()>> {
        let io = std::task::ready!(self.get_mut().ready(cx))?;
        Pin::new(&mut **io).poll_shutdown(cx)
    }
}

#[cfg(test)]
mod tests {
    use tokio::io::AsyncReadExt;

    use super::*;

    #[test]
    fn a_write_completes_only_once_flushed() {
        futures::executor::block_on(async {
            let (ours, mut peer) = tokio::io::duplex(1 << 16);
            // A transport that holds what it is written until flushed, as a TLS stream may.
            let io = RustIo::new(tokio::io::BufWriter::new(ours), Hangup::never());
            io.write(b"hello").await.unwrap();
            let mut buf = [0; 5];
            peer.read_exact(&mut buf).await.unwrap();
            assert_eq!(&buf, b"hello");
        });
    }

    #[test]
    fn an_operation_owns_its_share_of_the_stream() {
        futures::executor::block_on(async {
            let (ours, mut peer) = tokio::io::duplex(1 << 16);
            let io = RustIo::new(ours, Hangup::never());
            let write = io.write(b"hi");
            drop(io);
            write.await.unwrap();
            let mut buf = [0; 2];
            peer.read_exact(&mut buf).await.unwrap();
            assert_eq!(&buf, b"hi");
        });
    }

    #[test]
    fn a_hangup_clone_holds_nothing_once_the_watch_is_dropped() {
        futures::executor::block_on(async {
            let held = Rc::new(());
            let observed = held.clone();
            let (watch, hangup) = HangupWatch::new(async move {
                std::future::pending::<()>().await;
                drop(observed);
                Ok(())
            });
            let mut clone = Box::pin(hangup.clone());
            assert!(futures::poll!(clone.as_mut()).is_pending());
            assert_eq!(Rc::strong_count(&held), 2);
            drop(watch);
            assert_eq!(Rc::strong_count(&held), 1);
            assert!(futures::poll!(clone.as_mut()).is_pending());
        });
    }

    #[test]
    fn a_failed_observation_fails_the_hangup() {
        futures::executor::block_on(async {
            let (_watch, hangup) = HangupWatch::new(async {
                Err(KjError::new(KjExceptionType::Failed, "observed".to_owned()))
            });
            assert!(hangup.await.is_err());
        });
    }

    #[test]
    fn a_rewound_transport_replays_what_was_read_ahead() {
        futures::executor::block_on(async {
            let (ours, mut peer) = tokio::io::duplex(1 << 16);
            tokio::io::AsyncWriteExt::write_all(&mut peer, b" world")
                .await
                .unwrap();
            let mut io = rewound(Box::new(ours), Bytes::from_static(b"hello"));
            let mut buf = [0; 11];
            io.read_exact(&mut buf).await.unwrap();
            assert_eq!(&buf, b"hello world");
        });
    }

    /// A transport with one waker slot for both directions, as a TLS stream whose reads write
    /// and whose writes read has in effect.
    struct OneSlotIo(Rc<RefCell<Option<Waker>>>);

    impl AsyncRead for OneSlotIo {
        fn poll_read(
            self: Pin<&mut Self>,
            cx: &mut Context<'_>,
            _buf: &mut ReadBuf<'_>,
        ) -> Poll<std::io::Result<()>> {
            *self.0.borrow_mut() = Some(cx.waker().clone());
            Poll::Pending
        }
    }

    impl AsyncWrite for OneSlotIo {
        fn poll_write(
            self: Pin<&mut Self>,
            cx: &mut Context<'_>,
            _buf: &[u8],
        ) -> Poll<std::io::Result<usize>> {
            *self.0.borrow_mut() = Some(cx.waker().clone());
            Poll::Pending
        }

        fn poll_flush(self: Pin<&mut Self>, _cx: &mut Context<'_>) -> Poll<std::io::Result<()>> {
            Poll::Ready(Ok(()))
        }

        fn poll_shutdown(self: Pin<&mut Self>, _cx: &mut Context<'_>) -> Poll<std::io::Result<()>> {
            Poll::Ready(Ok(()))
        }
    }

    #[derive(Default)]
    struct Flag(std::sync::atomic::AtomicBool);

    impl Wake for Flag {
        fn wake(self: Arc<Self>) {
            self.0.store(true, std::sync::atomic::Ordering::SeqCst);
        }
    }

    #[test]
    fn a_wake_for_either_direction_reaches_both_tasks() {
        let slot = Rc::default();
        let mut io = SharedWakers::new(OneSlotIo(Rc::clone(&slot)));
        let (reader, writer) = (Arc::new(Flag::default()), Arc::new(Flag::default()));
        let mut buf = [0; 1];
        let mut read_buf = ReadBuf::new(&mut buf);
        let reader_waker = Waker::from(reader.clone());
        let writer_waker = Waker::from(writer.clone());
        assert!(
            Pin::new(&mut io)
                .poll_read(&mut Context::from_waker(&reader_waker), &mut read_buf)
                .is_pending()
        );
        assert!(
            Pin::new(&mut io)
                .poll_write(&mut Context::from_waker(&writer_waker), b"x")
                .is_pending()
        );
        // The transport kept only the writer's registration; waking it reaches the reader too.
        slot.borrow_mut().take().unwrap().wake();
        assert!(reader.0.load(std::sync::atomic::Ordering::SeqCst));
        assert!(writer.0.load(std::sync::atomic::Ordering::SeqCst));
    }
}
