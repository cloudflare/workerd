//! kj streams as tokio streams, and tokio streams as kj streams.

use std::cell::Cell;
use std::cell::RefCell;
use std::future::Future;
use std::pin::Pin;
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

use crate::ffi::AsyncIoStream;
use crate::ffi::KjStreamReadHalf;
use crate::ffi::KjStreamWriteHalf;
use crate::ffi::OwnerThread;

/// Any tokio byte stream. `Send` because hyper's upgrades require it.
pub trait Io: AsyncRead + AsyncWrite + Send + Unpin {}
impl<T: AsyncRead + AsyncWrite + Send + Unpin + ?Sized> Io for T {}

pub type BoxIo = Box<dyn Io>;

/// kj's `whenWriteDisconnected` for a connection: resolves once the peer can no longer receive.
/// Shared by everything built on the connection (TLS, upgrades) and independent of its lifetime.
pub type Hangup = futures::future::Shared<futures::future::BoxFuture<'static, ()>>;

pub fn hangup(watch: impl Future<Output = ()> + Send + 'static) -> Hangup {
    futures::FutureExt::shared(futures::FutureExt::boxed(watch))
}

/// A connection whose peer is never observed to go away.
pub fn never() -> Hangup {
    hangup(std::future::pending())
}

/// A kj stream as a tokio stream: its tokio socket where there is one to take (a kj-rs-io
/// socket, or a `RustIo` nothing is using), else the kj stream driven directly.
///
/// `lent`: the caller keeps using its stream (a non-owning `kj::Own`), so it is never taken
/// apart.
pub fn kj_to_tokio(stream: KjOwn<AsyncIoStream>, lent: bool) -> (BoxIo, Hangup) {
    if !lent {
        if crate::ffi::is_releasable_rust_io(stream.as_ref()) {
            let io = *crate::ffi::release_rust_io(stream);
            return (io.io.into_inner(), io.hangup);
        }
        if crate::ffi::is_tokio_stream(stream.as_ref()) {
            match crate::ffi::release_tokio_stream(stream).into_socket() {
                Ok(socket) => {
                    let watch = {
                        let handle = kj_rs_tokio::current_handle();
                        let _runtime = handle.as_ref().map(tokio::runtime::Handle::enter);
                        socket.hangup().map_or_else(|_| never(), hangup)
                    };
                    let io: BoxIo = match socket {
                        kj_rs_io::Socket::Tcp(socket) => {
                            let _ = socket.set_nodelay(true);
                            Box::new(socket)
                        }
                        #[cfg(unix)]
                        kj_rs_io::Socket::Unix(socket) => Box::new(socket),
                    };
                    return (io, watch);
                }
                // An operation still holds the socket: drive the re-wrapped stream instead.
                Err((native, _)) => {
                    return KjIo::open(crate::ffi::wrap_tokio_stream(native));
                }
            }
        }
    }
    KjIo::open(stream)
}

// =======================================================================================
// KjIo

type ReadOp =
    Pin<Box<dyn Future<Output = (KjStreamReadHalf, Vec<u8>, Result<usize, KjException>)>>>;
type WriteOp = Pin<Box<dyn Future<Output = (KjStreamWriteHalf, Result<(), KjException>)>>>;

enum Reader {
    Idle(KjStreamReadHalf),
    Reading(ReadOp),
    Buffered(KjStreamReadHalf, Vec<u8>, usize),
    Done,
}

enum Writer {
    Idle(KjStreamWriteHalf),
    Writing(WriteOp, usize),
    Done,
}

/// A foreign kj stream as a tokio stream, polling its bridged kj promises directly (one per
/// direction). A write completes once the kj stream has taken the bytes; a `Pending` write owns
/// a copy and the caller retries with the same buffer.
struct KjIo(OwnerThread<(Reader, Writer)>);

impl KjIo {
    fn open(stream: KjOwn<AsyncIoStream>) -> (BoxIo, Hangup) {
        let (mut read, write) = crate::ffi::split_kj_stream(stream);
        let watch = hangup(OwnerThread::new(Box::pin(read.when_write_disconnected())));
        let io = Self(OwnerThread::new((Reader::Idle(read), Writer::Idle(write))));
        (Box::new(io), watch)
    }

    fn state(&mut self) -> std::io::Result<&mut (Reader, Writer)> {
        self.0
            .get_mut()
            .ok_or_else(|| std::io::Error::other("kj stream used off its event loop's thread"))
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
        let (reader, _) = self.get_mut().state()?;
        loop {
            match std::mem::replace(reader, Reader::Done) {
                Reader::Done => return Poll::Ready(Ok(())),
                Reader::Buffered(half, chunk, taken) => {
                    let n = (chunk.len() - taken).min(buf.remaining());
                    buf.put_slice(&chunk[taken..taken + n]);
                    *reader = if taken + n == chunk.len() {
                        Reader::Idle(half)
                    } else {
                        Reader::Buffered(half, chunk, taken + n)
                    };
                    return Poll::Ready(Ok(()));
                }
                Reader::Idle(mut half) => {
                    *reader = Reader::Reading(Box::pin(async move {
                        let mut chunk = vec![0u8; 8192];
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
        let (_, writer) = self.get_mut().state()?;
        loop {
            match std::mem::replace(writer, Writer::Done) {
                Writer::Done => return Poll::Ready(Err(std::io::ErrorKind::BrokenPipe.into())),
                Writer::Idle(mut half) => {
                    let bytes = buf.to_vec();
                    *writer = Writer::Writing(
                        Box::pin(async move {
                            let result = half.write(&bytes).await;
                            (half, result)
                        }),
                        buf.len(),
                    );
                }
                Writer::Writing(mut op, len) => match op.as_mut().poll(cx) {
                    Poll::Pending => {
                        *writer = Writer::Writing(op, len);
                        return Poll::Pending;
                    }
                    Poll::Ready((half, Ok(()))) => {
                        *writer = Writer::Idle(half);
                        return Poll::Ready(Ok(len.min(buf.len())));
                    }
                    Poll::Ready((_, Err(e))) => return Poll::Ready(Err(io_error(e))),
                },
            }
        }
    }

    fn poll_flush(self: Pin<&mut Self>, _cx: &mut Context<'_>) -> Poll<std::io::Result<()>> {
        Poll::Ready(Ok(()))
    }

    fn poll_shutdown(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<std::io::Result<()>> {
        let (_, writer) = self.get_mut().state()?;
        match std::mem::replace(writer, Writer::Done) {
            Writer::Writing(mut op, len) => match op.as_mut().poll(cx) {
                Poll::Pending => {
                    *writer = Writer::Writing(op, len);
                    Poll::Pending
                }
                Poll::Ready((mut half, Ok(()))) => {
                    Poll::Ready(half.shutdown_write().map_err(io_error))
                }
                Poll::Ready((_, Err(e))) => Poll::Ready(Err(io_error(e))),
            },
            Writer::Idle(mut half) => Poll::Ready(half.shutdown_write().map_err(io_error)),
            Writer::Done => Poll::Ready(Ok(())),
        }
    }
}

// =======================================================================================
// RustIo

/// A tokio stream handed to kj as a `kj::AsyncIoStream` (TLS streams, CONNECT tunnels). hyper
/// takes it back out ([`kj_to_tokio`]) when no kj operation is using it.
pub struct RustIo {
    pub(crate) io: RefCell<BoxIo>,
    hangup: Hangup,
    in_flight: Cell<usize>,
    read_aborted: Cell<bool>,
    /// Wakes a read parked on the stream when `abortRead()` is called.
    read_waker: RefCell<Option<Waker>>,
}

struct InFlight<'a>(&'a Cell<usize>);

impl Drop for InFlight<'_> {
    fn drop(&mut self) {
        self.0.set(self.0.get() - 1);
    }
}

impl RustIo {
    pub fn new(io: impl Io + 'static, hangup: Hangup) -> Self {
        Self {
            io: RefCell::new(Box::new(io)),
            hangup,
            in_flight: Cell::new(0),
            read_aborted: Cell::new(false),
            read_waker: RefCell::new(None),
        }
    }

    fn enter(&self) -> InFlight<'_> {
        self.in_flight.set(self.in_flight.get() + 1);
        InFlight(&self.in_flight)
    }

    /// kj's `whenWriteDisconnected`.
    pub fn when_write_disconnected(&self) -> Hangup {
        self.hangup.clone()
    }

    #[must_use]
    pub fn can_release(&self) -> bool {
        self.in_flight.get() == 0
    }

    pub async fn read(&self, buf: &mut ReadBuf<'_>, min_bytes: usize) -> kj::Result<usize> {
        let _op = self.enter();
        let min_bytes = min_bytes.min(buf.capacity());
        while buf.filled().len() < min_bytes && !self.read_aborted.get() {
            let before = buf.filled().len();
            match std::future::poll_fn(|cx| {
                if self.read_aborted.get() {
                    return Poll::Ready(Ok(()));
                }
                *self.read_waker.borrow_mut() = Some(cx.waker().clone());
                Pin::new(&mut **self.io.borrow_mut()).poll_read(cx, buf)
            })
            .await
            {
                // A TLS peer that closes without close_notify: an EOF, as kj's TLS reports it.
                Err(e) if e.kind() == std::io::ErrorKind::UnexpectedEof => break,
                result => result.map_err(|e| io_kj_error(&e))?,
            }
            if buf.filled().len() == before {
                break;
            }
        }
        if self.read_aborted.get() {
            return Err(KjError::new(
                KjExceptionType::Disconnected,
                "abortRead() called".to_owned(),
            ));
        }
        Ok(buf.filled().len())
    }

    pub async fn write(&self, data: &[u8]) -> kj::Result<()> {
        let _op = self.enter();
        let mut written = 0;
        while written < data.len() {
            written += std::future::poll_fn(|cx| {
                Pin::new(&mut **self.io.borrow_mut()).poll_write(cx, &data[written..])
            })
            .await
            .map_err(|e| io_kj_error(&e))?;
        }
        Ok(())
    }

    pub async fn shutdown_write(&self) -> kj::Result<()> {
        let _op = self.enter();
        std::future::poll_fn(|cx| Pin::new(&mut **self.io.borrow_mut()).poll_shutdown(cx))
            .await
            .map_err(|e| io_kj_error(&e))
    }

    pub fn abort_read(&self) {
        self.read_aborted.set(true);
        if let Some(waker) = self.read_waker.borrow_mut().take() {
            waker.wake();
        }
    }
}

/// An I/O failure as a kj exception: peer-teardown kinds are DISCONNECTED.
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
// LazyIo

type Handshake<T> = Pin<Box<dyn Future<Output = std::io::Result<T>> + Send>>;

/// A transport still being set up (an HTTP upgrade, a TLS server handshake): I/O waits for it.
/// A read and a write may both be waiting, so completion wakes every waiter.
pub struct LazyIo<T> {
    state: Lazy<T>,
    waiters: Vec<Waker>,
}

enum Lazy<T> {
    Pending(Handshake<T>),
    Ready(T),
    Failed(std::io::ErrorKind, String),
}

impl<T: Io> LazyIo<T> {
    pub fn new(setup: impl Future<Output = std::io::Result<T>> + Send + 'static) -> Self {
        Self {
            state: Lazy::Pending(Box::pin(setup)),
            waiters: Vec::new(),
        }
    }

    fn ready(&mut self, cx: &mut Context<'_>) -> Poll<std::io::Result<&mut T>> {
        if let Lazy::Pending(setup) = &mut self.state {
            let Poll::Ready(result) = setup.as_mut().poll(cx) else {
                if !self.waiters.iter().any(|w| w.will_wake(cx.waker())) {
                    self.waiters.push(cx.waker().clone());
                }
                return Poll::Pending;
            };
            self.state = match result {
                Ok(io) => Lazy::Ready(io),
                Err(e) => Lazy::Failed(e.kind(), e.to_string()),
            };
            self.waiters.drain(..).for_each(Waker::wake);
        }
        match &mut self.state {
            Lazy::Ready(io) => Poll::Ready(Ok(io)),
            Lazy::Failed(kind, message) => {
                Poll::Ready(Err(std::io::Error::new(*kind, message.clone())))
            }
            Lazy::Pending(_) => Poll::Pending,
        }
    }
}

impl<T: Io> AsyncRead for LazyIo<T> {
    fn poll_read(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &mut ReadBuf<'_>,
    ) -> Poll<std::io::Result<()>> {
        let io = std::task::ready!(self.get_mut().ready(cx))?;
        Pin::new(io).poll_read(cx, buf)
    }
}

impl<T: Io> AsyncWrite for LazyIo<T> {
    fn poll_write(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &[u8],
    ) -> Poll<std::io::Result<usize>> {
        let io = std::task::ready!(self.get_mut().ready(cx))?;
        Pin::new(io).poll_write(cx, buf)
    }

    fn poll_flush(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<std::io::Result<()>> {
        let io = std::task::ready!(self.get_mut().ready(cx))?;
        Pin::new(io).poll_flush(cx)
    }

    fn poll_shutdown(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<std::io::Result<()>> {
        let io = std::task::ready!(self.get_mut().ready(cx))?;
        Pin::new(io).poll_shutdown(cx)
    }
}

/// An upgraded HTTP connection, once hyper hands it over.
pub fn upgraded(
    on_upgrade: hyper::upgrade::OnUpgrade,
) -> LazyIo<hyper_util::rt::TokioIo<hyper::upgrade::Upgraded>> {
    LazyIo::new(async move {
        on_upgrade
            .await
            .map(hyper_util::rt::TokioIo::new)
            .map_err(|e| std::io::Error::new(std::io::ErrorKind::NotConnected, e))
    })
}
