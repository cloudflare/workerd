//! tokio streams as kj streams, and the transport adapters the server and client share.

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

use cxx::KjError;
use cxx::KjExceptionType;
use futures::FutureExt;
use hyper_util::rt::TokioIo;
use kj_rs::KjOwn;
use tokio::io::AsyncRead;
use tokio::io::AsyncWrite;
use tokio::io::ReadBuf;

use crate::ffi::AsyncIoStream;

/// Any tokio byte stream.
pub trait Io: AsyncRead + AsyncWrite + Unpin {}
impl<T: AsyncRead + AsyncWrite + Unpin + ?Sized> Io for T {}

pub type BoxIo = Box<dyn Io>;

// =======================================================================================
// Streams handed to C++

/// `io` as a `kj::AsyncIoStream`, for C++ to drive.
///
/// Each kj operation owns a share of the stream, so an operation in flight outlives the
/// `kj::Own` if C++ drops it first. `whenWriteDisconnected()` on the result never resolves (kj's default for a stream that has no
/// way to observe its peer); reads and writes fail once the peer is gone.
pub fn into_kj_stream(io: impl Io + 'static) -> KjOwn<AsyncIoStream> {
    crate::ffi::new_rust_io_stream(Box::new(RustIo::new(io)))
}

/// A tokio socket as a `kj::AsyncIoStream` through kj-rs-io's native stream.
///
/// That stream implements all of kj's socket operations (`whenWriteDisconnected`,
/// `getpeername`, ...), and C++ can hand it back to Rust (`TokioAsyncIoStream::release()` then
/// `TokioStream::into_socket()`).
///
/// # Errors
///
/// Off the loop thread (kj-rs-io's ownership rule).
pub fn socket_into_kj_stream(socket: kj_rs_io::Socket) -> crate::Result<KjOwn<AsyncIoStream>> {
    let stream = kj_rs_io::TokioStream::new(socket)?;
    Ok(crate::ffi::wrap_tokio_stream(Box::new(stream)))
}

/// [`socket_into_kj_stream`] for a TCP socket.
///
/// # Errors
///
/// As for [`socket_into_kj_stream`].
pub fn tcp_into_kj_stream(stream: tokio::net::TcpStream) -> crate::Result<KjOwn<AsyncIoStream>> {
    socket_into_kj_stream(kj_rs_io::Socket::Tcp(stream))
}

// =======================================================================================
// RustIo

struct IoState {
    io: RefCell<BoxIo>,
    read_aborted: Cell<bool>,
    /// Wakes a read parked on the stream when `abortRead()` is called.
    read_waker: RefCell<Option<Waker>>,
}

/// A tokio stream behind a `kj::AsyncIoStream` (CONNECT tunnels). kj reads and writes it from
/// different promises, so it is wrapped in [`SharedWakers`].
pub struct RustIo(Rc<IoState>);

impl RustIo {
    pub fn new(io: impl Io + 'static) -> Self {
        Self(Rc::new(IoState {
            io: RefCell::new(Box::new(SharedWakers::new(io))),
            read_aborted: Cell::new(false),
            read_waker: RefCell::new(None),
        }))
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
                let n = std::future::poll_fn(|cx| {
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

/// An I/O failure as a kj exception: the kinds kj classifies as DISCONNECTED (a peer gone or
/// unreachable, a connection refused, a TLS peer closing without `close_notify`) are
/// DISCONNECTED.
pub fn io_kj_error(e: &std::io::Error) -> KjError {
    use std::io::ErrorKind;
    let kind = match e.kind() {
        ErrorKind::ConnectionReset
        | ErrorKind::ConnectionAborted
        | ErrorKind::ConnectionRefused
        | ErrorKind::HostUnreachable
        | ErrorKind::NetworkUnreachable
        | ErrorKind::NetworkDown
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

/// A connection's transport after an HTTP upgrade, usable before hyper has finished handing it
/// over.
///
/// I/O waits for the hand-over. A read and a write may both be waiting (a `WebSocket`'s two
/// halves), so the hand-over wakes every waiter.
pub struct Upgrading {
    state: UpgradeState,
    waiters: Vec<Waker>,
}

enum UpgradeState {
    Pending(hyper::upgrade::OnUpgrade),
    Ready(TokioIo<hyper::upgrade::Upgraded>),
    Failed(String),
}

impl Upgrading {
    #[must_use]
    pub fn new(upgrade: hyper::upgrade::OnUpgrade) -> Self {
        Self {
            state: UpgradeState::Pending(upgrade),
            waiters: Vec::new(),
        }
    }

    fn ready(
        &mut self,
        cx: &mut Context<'_>,
    ) -> Poll<std::io::Result<&mut TokioIo<hyper::upgrade::Upgraded>>> {
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
            UpgradeState::Failed(why) => Poll::Ready(Err(std::io::Error::new(
                std::io::ErrorKind::NotConnected,
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
    ) -> Poll<std::io::Result<()>> {
        let io = std::task::ready!(self.get_mut().ready(cx))?;
        Pin::new(io).poll_read(cx, buf)
    }
}

impl AsyncWrite for Upgrading {
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

#[cfg(test)]
mod tests {
    use tokio::io::AsyncReadExt;

    use super::*;

    #[test]
    fn a_write_completes_only_once_flushed() {
        futures::executor::block_on(async {
            let (ours, mut peer) = tokio::io::duplex(1 << 16);
            // A transport that holds what it is written until flushed, as a TLS stream may.
            let io = RustIo::new(tokio::io::BufWriter::new(ours));
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
            let io = RustIo::new(ours);
            let write = io.write(b"hi");
            drop(io);
            write.await.unwrap();
            let mut buf = [0; 2];
            peer.read_exact(&mut buf).await.unwrap();
            assert_eq!(&buf, b"hi");
        });
    }

    /// A transport that takes nothing: `poll_write` reports zero bytes, as a closed one does.
    struct ClosedIo;

    impl AsyncRead for ClosedIo {
        fn poll_read(
            self: Pin<&mut Self>,
            _cx: &mut Context<'_>,
            _buf: &mut ReadBuf<'_>,
        ) -> Poll<std::io::Result<()>> {
            Poll::Ready(Ok(()))
        }
    }

    impl AsyncWrite for ClosedIo {
        fn poll_write(
            self: Pin<&mut Self>,
            _cx: &mut Context<'_>,
            _buf: &[u8],
        ) -> Poll<std::io::Result<usize>> {
            Poll::Ready(Ok(0))
        }

        fn poll_flush(self: Pin<&mut Self>, _cx: &mut Context<'_>) -> Poll<std::io::Result<()>> {
            Poll::Ready(Ok(()))
        }

        fn poll_shutdown(self: Pin<&mut Self>, _cx: &mut Context<'_>) -> Poll<std::io::Result<()>> {
            Poll::Ready(Ok(()))
        }
    }

    #[test]
    fn a_write_the_transport_takes_nothing_of_fails_as_disconnected() {
        futures::executor::block_on(async {
            let io = RustIo::new(ClosedIo);
            let error = io.write(b"hello").await.unwrap_err();
            assert_eq!(error.exception_type(), KjExceptionType::Disconnected);
        });
    }

    #[test]
    fn abort_read_ends_a_parked_read() {
        futures::executor::block_on(async {
            let (ours, _peer) = tokio::io::duplex(16);
            let io = RustIo::new(ours);
            let mut buf = [0; 4];
            let mut read = Box::pin(io.read(ReadBuf::new(&mut buf), 1));
            assert!(futures::poll!(read.as_mut()).is_pending());
            io.abort_read();
            let error = read.await.unwrap_err();
            assert_eq!(error.exception_type(), KjExceptionType::Disconnected);
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
