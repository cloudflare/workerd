//! tokio streams as kj streams, and the transport adapters the server and client share.

use std::cell::Cell;
use std::cell::RefCell;
use std::future::Future;
use std::future::pending;
use std::future::poll_fn;
use std::io;
use std::pin::Pin;
use std::rc::Rc;
use std::sync::Arc;
use std::sync::Mutex;
use std::sync::PoisonError;
use std::task::Context;
use std::task::Poll;
use std::task::Wake;
use std::task::Waker;

use cxx::KjError;
use cxx::KjExceptionType;
use futures::FutureExt;
use futures::future::BoxFuture;
use futures::future::Shared;
use hyper::upgrade::OnUpgrade;
use hyper::upgrade::Upgraded;
use hyper_util::rt::TokioIo;
use kj_rs::KjOwn;
use tokio::io::AsyncRead;
use tokio::io::AsyncWrite;
use tokio::io::ReadBuf;

use crate::Result;
use crate::ffi::AsyncIoStream;
use crate::ffi::new_rust_io_stream;

/// Any tokio byte stream.
pub trait Io: AsyncRead + AsyncWrite + Unpin {}
impl<T: AsyncRead + AsyncWrite + Unpin + ?Sized> Io for T {}

pub type BoxIo = Box<dyn Io>;

// =======================================================================================
// Streams handed to C++

/// `io` as a `kj::AsyncIoStream`, for C++ to drive.
///
/// Each kj operation owns a share of the stream, so an operation in flight outlives the
/// `kj::Own` if C++ drops it first. `whenWriteDisconnected()` on the result never resolves (kj's
/// default for a stream that has no way to observe its peer); reads and writes fail once the peer
/// is gone.
pub fn into_kj_stream(io: impl Io + 'static) -> KjOwn<AsyncIoStream> {
    into_kj_stream_with(io, None)
}

/// [`into_kj_stream`] whose `whenWriteDisconnected()` resolves when `hangup` does.
pub fn into_kj_stream_with(
    io: impl Io + 'static,
    hangup: Option<Shared<Hangup>>,
) -> KjOwn<AsyncIoStream> {
    new_rust_io_stream(Box::new(RustIo::with_hangup(io, hangup)))
}

// =======================================================================================
// RustIo

/// Resolves once a connection's peer is gone.
///
/// That is when `whenWriteDisconnected()` of kj's own stream over the transport would: a socket
/// hung up or failed (`kj_rs_io::when_write_disconnected`; a peer shutting down its side is not
/// that), an in-memory pipe's other end dropped; never on Windows, as under kj.
///
/// A transport comes with its `Hangup`. [`crate::server::serve_connection`] takes it as an
/// argument; a dialer returns a socket, which [`crate::client::Dialed`]'s `From` takes it of, or
/// a `Dialed` made with one ([`crate::client::Dialed::with_hangup`], kept across
/// [`crate::client::Dialed::tls`]). The kj streams made of the connection resolve
/// `whenWriteDisconnected()` from it, and so a `kj::WebSocket` its `whenAborted()`, which is how
/// kj learns of a peer that goes away while nothing is read: a served connection's WebSocket and
/// a CONNECT's [`crate::server::Connect::into_kj`] tunnel (the upgraded transport keeps the
/// signal), the client's `WebSocket`s, CONNECT tunnels and raw `connect()` tunnels (the pool
/// hands each response the connection's signal), [`crate::client::Dialed::into_kj`], and a
/// transport handed to C++ with its signal ([`into_kj_stream_with`]). One handed over with
/// [`into_kj_stream`] never resolves it (reads and writes fail once the peer is gone).
pub type Hangup = BoxFuture<'static, Result<()>>;

/// kj calls a [`RustIo`] through `&self` from promises that run at once (a read, a write,
/// `abortRead()`), on one thread, so its state is in cells, each borrowed only inside one poll or
/// call.
struct IoState {
    io: RefCell<BoxIo>,
    /// Shared, because kj asks more than once.
    hangup: Shared<Hangup>,
    read_aborted: Cell<bool>,
    /// Wakes a read parked on the stream when `abortRead()` is called.
    read_waker: RefCell<Option<Waker>>,
}

/// A tokio stream behind a `kj::AsyncIoStream` (CONNECT tunnels). kj reads and writes it from
/// different promises, so it is wrapped in [`SharedWakers`].
pub struct RustIo(Rc<IoState>);

impl RustIo {
    pub fn new(io: impl Io + 'static) -> Self {
        Self::with_hangup(io, None)
    }

    fn with_hangup(io: impl Io + 'static, hangup: Option<Shared<Hangup>>) -> Self {
        Self(Rc::new(IoState {
            io: RefCell::new(Box::new(SharedWakers::new(io))),
            hangup: hangup.unwrap_or_else(|| pending().boxed().shared()),
            read_aborted: Cell::new(false),
            read_waker: RefCell::new(None),
        }))
    }

    /// kj's `tryRead`: reads until `min_bytes` or EOF.
    pub fn read<'b>(
        &self,
        mut buf: ReadBuf<'b>,
        min_bytes: usize,
    ) -> impl Future<Output = Result<usize>> + use<'b> {
        let state = self.0.clone();
        async move {
            let min_bytes = min_bytes.min(buf.capacity());
            while buf.filled().len() < min_bytes && !state.read_aborted.get() {
                let before = buf.filled().len();
                poll_fn(|cx| {
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
    pub fn write<'b>(&self, data: &'b [u8]) -> impl Future<Output = Result<()>> + use<'b> {
        let state = self.0.clone();
        async move {
            let mut written = 0;
            while written < data.len() {
                let n = poll_fn(|cx| {
                    Pin::new(&mut **state.io.borrow_mut()).poll_write(cx, &data[written..])
                })
                .await
                .map_err(|e| io_kj_error(&e))?;
                // A transport that takes no bytes is one that takes no more (`poll_write`'s
                // contract); polling it again would never progress.
                if n == 0 {
                    return Err(KjError::new(
                        KjExceptionType::Disconnected,
                        "the transport accepts no more bytes".to_owned(),
                    ));
                }
                written += n;
            }
            poll_fn(|cx| Pin::new(&mut **state.io.borrow_mut()).poll_flush(cx))
                .await
                .map_err(|e| io_kj_error(&e))
        }
    }

    pub fn shutdown_write(&self) -> impl Future<Output = Result<()>> + use<> {
        let state = self.0.clone();
        async move {
            poll_fn(|cx| Pin::new(&mut **state.io.borrow_mut()).poll_shutdown(cx))
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

    /// Never resolves unless the stream was made with a [`Hangup`].
    pub fn when_write_disconnected(&self) -> Shared<Hangup> {
        self.0.hangup.clone()
    }
}

impl From<Upgrading> for RustIo {
    fn from(io: Upgrading) -> Self {
        let hangup = io.hangup.clone();
        Self::with_hangup(io, Some(hangup))
    }
}

/// An I/O failure as a kj exception of the type kj gives it (`kj_rs_io::exception_type`): a
/// peer gone or unreachable, a connection refused and a TLS peer closing without `close_notify`
/// are DISCONNECTED.
pub fn io_kj_error(e: &io::Error) -> KjError {
    KjError::new(kj_rs_io::exception_type(e), e.to_string())
}

// =======================================================================================
// SharedWakers

#[derive(Default)]
struct Wakers {
    read: Mutex<Option<Waker>>,
    write: Mutex<Option<Waker>>,
}

fn take(slot: &Mutex<Option<Waker>>) -> Option<Waker> {
    slot.lock().unwrap_or_else(PoisonError::into_inner).take()
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
        *slot.lock().unwrap_or_else(PoisonError::into_inner) = Some(cx.waker().clone());
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
    ) -> Poll<io::Result<()>> {
        self.get_mut()
            .poll_with(cx, true, |io, cx| io.poll_read(cx, buf))
    }
}

impl<T: AsyncWrite + Unpin> AsyncWrite for SharedWakers<T> {
    fn poll_write(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &[u8],
    ) -> Poll<io::Result<usize>> {
        self.get_mut()
            .poll_with(cx, false, |io, cx| io.poll_write(cx, buf))
    }

    fn poll_flush(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<io::Result<()>> {
        self.get_mut().poll_with(cx, false, AsyncWrite::poll_flush)
    }

    fn poll_shutdown(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<io::Result<()>> {
        self.get_mut()
            .poll_with(cx, false, AsyncWrite::poll_shutdown)
    }
}

// =======================================================================================
// Upgrades

/// A connection's transport after an HTTP upgrade, usable before hyper has finished handing it
/// over.
///
/// I/O waits for the hand-over. A read and a write may both be waiting (a `WebSocket`'s two
/// halves), so the hand-over wakes every waiter.
pub struct Upgrading {
    state: UpgradeState,
    waiters: Vec<Waker>,
    /// The connection's, which the transport keeps.
    hangup: Shared<Hangup>,
}

enum UpgradeState {
    Pending(OnUpgrade),
    Ready(TokioIo<Upgraded>),
    Failed(String),
}

impl Upgrading {
    /// The transport as a `kj::AsyncIoStream` whose `whenWriteDisconnected()` is the
    /// connection's hang-up.
    #[must_use]
    pub fn into_kj(self) -> KjOwn<AsyncIoStream> {
        new_rust_io_stream(Box::new(self.into()))
    }

    #[must_use]
    pub fn new(upgrade: OnUpgrade, hangup: Shared<Hangup>) -> Self {
        Self {
            state: UpgradeState::Pending(upgrade),
            waiters: Vec::new(),
            hangup,
        }
    }

    fn ready(&mut self, cx: &mut Context<'_>) -> Poll<io::Result<&mut TokioIo<Upgraded>>> {
        if let UpgradeState::Pending(upgrade) = &mut self.state {
            let Poll::Ready(result) = upgrade.poll_unpin(cx) else {
                if !self.waiters.iter().any(|w| w.will_wake(cx.waker())) {
                    self.waiters.push(cx.waker().clone());
                }
                return Poll::Pending;
            };
            self.state = match result {
                Ok(io) => UpgradeState::Ready(TokioIo::new(io)),
                Err(e) => UpgradeState::Failed(e.to_string()),
            };
            self.waiters.drain(..).for_each(Waker::wake);
        }
        match &mut self.state {
            UpgradeState::Ready(io) => Poll::Ready(Ok(io)),
            UpgradeState::Failed(why) => Poll::Ready(Err(io::Error::new(
                io::ErrorKind::NotConnected,
                format!("the connection was not upgraded: {why}"),
            ))),
            UpgradeState::Pending(_) => Poll::Pending,
        }
    }
}

impl AsyncRead for Upgrading {
    fn poll_read(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &mut ReadBuf<'_>,
    ) -> Poll<io::Result<()>> {
        let io = std::task::ready!(self.get_mut().ready(cx))?;
        Pin::new(io).poll_read(cx, buf)
    }
}

impl AsyncWrite for Upgrading {
    fn poll_write(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &[u8],
    ) -> Poll<io::Result<usize>> {
        let io = std::task::ready!(self.get_mut().ready(cx))?;
        Pin::new(io).poll_write(cx, buf)
    }

    fn poll_flush(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<io::Result<()>> {
        let io = std::task::ready!(self.get_mut().ready(cx))?;
        Pin::new(io).poll_flush(cx)
    }

    fn poll_shutdown(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<io::Result<()>> {
        let io = std::task::ready!(self.get_mut().ready(cx))?;
        Pin::new(io).poll_shutdown(cx)
    }
}

#[cfg(test)]
#[path = "io-test.rs"]
mod tests;
