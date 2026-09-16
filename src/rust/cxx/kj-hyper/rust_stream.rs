//! A Rust byte stream handed to kj as a `kj::AsyncIoStream`, and taken back natively.
//!
//! [`RustStream`] owns a tokio byte stream -- today the TLS streams of workerd's rustls
//! `SecureNetworkWrapper` (`tls.rs`) -- and is wrapped by the C++ `RustAsyncIoStream`
//! (hyper-server-ffi.c++) for kj consumers: kj's `tryRead`/`write`/`shutdownWrite`/... come in
//! over the bridge as the methods below. When a hyper connection or client is later built over
//! that kj stream, `serve.rs` takes the tokio stream back out ([`RustStream::into_parts`]), so a
//! TLS connection over a TCP socket reaches hyper as a plain tokio TLS stream, with neither a
//! pump nor a bridge crossing per read.
//!
//! **Taking it back.** The C++ wrapper hands the object over only when [`RustStream::can_release`]
//! says so: no kj operation still holds `&self` (a pending read, write, shutdown flush or
//! `whenWriteDisconnected`), and the caller is on the thread that owns the stream. Otherwise the
//! stream stays wrapped and `serve.rs` pumps it like any foreign kj stream -- the same rule
//! kj-rs-io's `TokioStream::into_socket` enforces with its shares.
//!
//! **The pump.** When the TLS layer sits over a foreign kj stream (an in-memory pipe, kj's
//! promised stream), `serve.rs` bridged that stream with a [`StreamPump`], which must be polled
//! on the KJ thread. The C++ wrapper drives it as its own kj promise ([`RustStream::drive`], like
//! `HyperHttpClient`'s pump task); operations only poll the stream, whose channel wakes them as
//! the pump makes progress. A consumer taking the parts gets the pump back and drives it itself.

use std::cell::Cell;
use std::cell::OnceCell;
use std::cell::RefCell;
use std::future::Future;
use std::pin::Pin;
use std::task::Context;
use std::task::Poll;
use std::task::Waker;

use cxx::KjError;
use cxx::KjExceptionType;
use tokio::io::AsyncRead;
use tokio::io::AsyncWrite;
use tokio::io::ReadBuf;

use crate::serve::StreamPump;

/// How a stream reports that new writes are doomed ([`AsyncIo::write_disconnect`]).
pub enum WriteDisconnect {
    /// The transport cannot tell (kj permits a never-resolving `whenWriteDisconnected`).
    Never,
    /// A socket: watch a duplicate of its descriptor for a hangup, as kj-rs-io does.
    #[cfg(unix)]
    Fd(std::os::fd::OwnedFd),
    /// A pumped foreign kj stream: forwarded to its own `whenWriteDisconnected` by the pump.
    Pumped(Pin<Box<dyn Future<Output = ()> + Send>>),
}

/// A tokio byte stream a [`RustStream`] can carry.
///
/// What kj's stream interface needs beyond `AsyncRead + AsyncWrite`. `Send` because hyper's
/// upgrade machinery requires its transport to be; it is only ever polled from the one KJ loop
/// thread.
pub trait AsyncIo: AsyncRead + AsyncWrite + Send {
    /// The source of `whenWriteDisconnected`, for the transport underneath.
    ///
    /// # Errors
    ///
    /// Duplicating the transport's descriptor failed.
    fn write_disconnect(&self) -> std::io::Result<WriteDisconnect>;

    /// `TCP_NODELAY` on the transport underneath, where there is one.
    ///
    /// # Errors
    ///
    /// The `setsockopt` failure.
    fn set_nodelay(&self, nodelay: bool) -> std::io::Result<()>;
}

/// The stream type this handle carries.
pub type BoxedIo = Pin<Box<dyn AsyncIo>>;

/// See the module docs.
pub struct RustStream {
    io: RefCell<BoxedIo>,
    pump: RefCell<Option<StreamPump>>,
    /// The pump settled with this error (the kj stream failed); reported in place of the
    /// consumer-side failure it caused.
    pump_error: RefCell<Option<KjError>>,
    /// kj operations currently holding `&self` (see [`OpGuard`]).
    in_flight: Cell<usize>,
    read_aborted: Cell<bool>,
    /// The waker of a read parked on the stream, woken by `abort_read()`.
    read_waker: RefCell<Option<Waker>>,
    /// The thread owning the stream: the pump and the tokio registrations belong to its loop.
    owner: std::thread::ThreadId,
    /// The hangup watch, registered by the first `when_write_disconnected()` on a socket.
    #[cfg(unix)]
    hangup: OnceCell<tokio::io::unix::AsyncFd<std::os::fd::OwnedFd>>,
}

/// Counts a kj operation holding `&self`, so the stream is not taken out from under it.
struct OpGuard<'a>(&'a Cell<usize>);

impl Drop for OpGuard<'_> {
    fn drop(&mut self) {
        self.0.set(self.0.get() - 1);
    }
}

impl RustStream {
    pub fn new(io: impl AsyncIo + 'static, pump: Option<StreamPump>) -> Self {
        Self::from_parts(Box::pin(io), pump)
    }

    /// The inverse of [`RustStream::into_parts`].
    #[must_use]
    pub fn from_parts(io: BoxedIo, pump: Option<StreamPump>) -> Self {
        Self {
            io: RefCell::new(io),
            pump: RefCell::new(pump),
            pump_error: RefCell::new(None),
            in_flight: Cell::new(0),
            read_aborted: Cell::new(false),
            read_waker: RefCell::new(None),
            owner: std::thread::current().id(),
            #[cfg(unix)]
            hangup: OnceCell::new(),
        }
    }

    /// Whether the stream may be taken out ([`RustStream::into_parts`]): no kj operation holds
    /// it and the caller is on the owning thread. The C++ wrapper cancels its pump driver
    /// before handing the stream over; the driver does not count.
    #[must_use]
    pub fn can_release(&self) -> bool {
        self.in_flight.get() == 0 && std::thread::current().id() == self.owner
    }

    /// Takes the stream and its pump (if any) out: the native path of `serve.rs`, taken only
    /// after [`RustStream::can_release`]. The caller drives the pump on the KJ thread.
    pub fn into_parts(self: Box<Self>) -> (BoxedIo, Option<StreamPump>) {
        debug_assert!(self.can_release(), "RustStream taken while in use");
        let Self { io, pump, .. } = *self;
        (io.into_inner(), pump.into_inner())
    }

    /// Enters a kj operation: fails off the owning thread, else counts it until the guard drops.
    fn enter(&self) -> Result<OpGuard<'_>, KjError> {
        if std::thread::current().id() != self.owner {
            return Err(KjError::new(
                KjExceptionType::Failed,
                "a Rust-backed kj stream was used from a thread other than the one owning it"
                    .to_owned(),
            ));
        }
        self.in_flight.set(self.in_flight.get() + 1);
        Ok(OpGuard(&self.in_flight))
    }

    /// Drives the pump (if any) until it settles; resolves at once when there is none. The C++
    /// wrapper holds this as its own kj promise and cancels it before handing the stream over.
    pub async fn drive(&self) {
        std::future::poll_fn(|cx| {
            let mut pump = self.pump.borrow_mut();
            let Some(future) = pump.as_mut() else {
                return Poll::Ready(());
            };
            let Poll::Ready(result) = future.as_mut().poll(cx) else {
                return Poll::Pending;
            };
            *pump = None;
            if let Err(error) = result {
                *self.pump_error.borrow_mut() = Some(error);
            }
            Poll::Ready(())
        })
        .await;
    }

    /// Corresponds to `kj::AsyncInputStream::tryRead(buffer, min_bytes, len)`: fills `buf` (kj's
    /// storage, possibly uninitialized) until `min_bytes` are there or EOF.
    pub async fn read(&self, buf: &mut ReadBuf<'_>, min_bytes: usize) -> Result<usize, KjError> {
        let _op = self.enter()?;
        let min_bytes = min_bytes.min(buf.capacity());
        while buf.filled().len() < min_bytes {
            let before = buf.filled().len();
            std::future::poll_fn(|cx| {
                if self.read_aborted.get() {
                    return Poll::Ready(Err(read_aborted()));
                }
                match self.poll_io(cx, "read()", |io, cx| io.poll_read(cx, buf)) {
                    Poll::Pending => {
                        let mut waker = self.read_waker.borrow_mut();
                        if !waker.as_ref().is_some_and(|w| w.will_wake(cx.waker())) {
                            *waker = Some(cx.waker().clone());
                        }
                        Poll::Pending
                    }
                    ready @ Poll::Ready(_) => ready,
                }
            })
            .await?;
            if buf.filled().len() == before {
                break; // EOF: fewer than min_bytes signals EOF to kj.
            }
        }
        Ok(buf.filled().len())
    }

    /// Corresponds to `kj::AsyncOutputStream::write()` (write-all semantics).
    pub async fn write(&self, buffer: &[u8]) -> Result<(), KjError> {
        let _op = self.enter()?;
        let mut written = 0;
        while written < buffer.len() {
            let n = std::future::poll_fn(|cx| {
                self.poll_io(cx, "write()", |io, cx| {
                    io.poll_write(cx, &buffer[written..])
                })
            })
            .await?;
            if n == 0 {
                return Err(KjError::new(
                    KjExceptionType::Failed,
                    "write(): wrote zero bytes".to_owned(),
                ));
            }
            written += n;
        }
        Ok(())
    }

    /// Corresponds to `kj::AsyncIoStream::shutdownWrite()`: flushes what the stream still
    /// holds (a TLS `close_notify`) and shuts the write side down. Async because that flush is;
    /// the C++ wrapper runs it as a detached task and logs its failure, as kj's TLS stream does.
    pub async fn shutdown_write(&self) -> Result<(), KjError> {
        let _op = self.enter()?;
        // A closure, not `AsyncWrite::poll_shutdown`: the method path is not general over the
        // lifetime of the `Pin<&mut dyn AsyncIo>` that `poll_io` passes.
        #[expect(clippy::redundant_closure_for_method_calls)]
        std::future::poll_fn(|cx| {
            self.poll_io(cx, "shutdownWrite()", |io, cx| io.poll_shutdown(cx))
        })
        .await
    }

    /// Corresponds to `kj::AsyncIoStream::abortRead()`: a pending read and every later read fail.
    pub fn abort_read(&self) {
        self.read_aborted.set(true);
        let waker = self.read_waker.borrow_mut().take();
        if let Some(waker) = waker {
            waker.wake();
        }
    }

    /// Corresponds to `kj::AsyncOutputStream::whenWriteDisconnected()`, forwarded to the
    /// transport underneath, as kj's TLS stream forwards it.
    pub async fn when_write_disconnected(&self) -> Result<(), KjError> {
        let _op = self.enter()?;
        #[cfg(unix)]
        if let Some(watch) = self.hangup.get() {
            return wait_for_hangup(watch).await;
        }
        let source = self
            .io
            .borrow()
            .write_disconnect()
            .map_err(|e| kj_error_for_stream_io("whenWriteDisconnected", &e))?;
        match source {
            WriteDisconnect::Never => std::future::pending().await,
            #[cfg(unix)]
            WriteDisconnect::Fd(fd) => {
                let watch = match tokio::io::unix::AsyncFd::with_interest(
                    fd,
                    tokio::io::Interest::WRITABLE,
                ) {
                    Ok(watch) => watch,
                    Err(e) => return Err(kj_error_for_stream_io("whenWriteDisconnected", &e)),
                };
                // A concurrent first caller may have registered its own meanwhile; then this one
                // is dropped (closing its dup) and the stored one is shared.
                let watch = self.hangup.get_or_init(|| watch);
                wait_for_hangup(watch).await
            }
            WriteDisconnect::Pumped(disconnected) => {
                disconnected.await;
                Ok(())
            }
        }
    }

    /// Polls the stream for one operation, reporting a failure the pump caused in place of the
    /// consumer-side error it produced.
    fn poll_io<T>(
        &self,
        cx: &mut Context<'_>,
        context: &str,
        poll: impl FnOnce(Pin<&mut dyn AsyncIo>, &mut Context<'_>) -> Poll<std::io::Result<T>>,
    ) -> Poll<Result<T, KjError>> {
        match poll(self.io.borrow_mut().as_mut(), cx) {
            Poll::Pending => Poll::Pending,
            Poll::Ready(Ok(value)) => Poll::Ready(Ok(value)),
            Poll::Ready(Err(e)) => Poll::Ready(Err(match self.pump_error.borrow_mut().take() {
                Some(pump_error) => pump_error,
                None => kj_error_for_stream_io(context, &e),
            })),
        }
    }
}

fn read_aborted() -> KjError {
    KjError::new(
        KjExceptionType::Disconnected,
        "abortRead() has been called".to_owned(),
    )
}

/// Resolves once the watched descriptor reports write-closed (see kj-rs-io's
/// `when_write_disconnected`, whose rule this follows: plain writability is cleared so the wait
/// sleeps until a state change; a half-close does not count).
#[cfg(unix)]
async fn wait_for_hangup(
    watch: &tokio::io::unix::AsyncFd<std::os::fd::OwnedFd>,
) -> Result<(), KjError> {
    loop {
        let mut guard = watch
            .ready(tokio::io::Interest::WRITABLE)
            .await
            .map_err(|e| kj_error_for_stream_io("whenWriteDisconnected", &e))?;
        if guard.ready().is_write_closed() {
            return Ok(());
        }
        guard.clear_ready();
    }
}

/// Renders a stream I/O failure as a `kj::Exception`.
///
/// tokio-rustls surfaces TLS failures as `io::Error`s wrapping a `rustls::Error`; those keep kj's
/// certificate-verification wording (see `tls.rs`), everything else maps by error kind.
pub fn kj_error_for_stream_io(context: &str, e: &std::io::Error) -> KjError {
    if let Some(inner) = e.get_ref()
        && let Some(tls_error) = inner.downcast_ref::<rustls::Error>()
    {
        return crate::tls::kj_error_for_rustls_error(tls_error);
    }
    crate::client::kj_error_for_io(context, e)
}
