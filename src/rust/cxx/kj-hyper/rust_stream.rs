//! A Rust byte stream handed to kj as a `kj::AsyncIoStream`, and taken back natively.
//!
//! [`RustStream`] owns a tokio byte stream -- today the TLS streams of workerd's rustls
//! `SecureNetworkWrapper` (`tls.rs`) -- and is wrapped by the C++ `RustAsyncIoStream`
//! (hyper-server-ffi.c++) for kj consumers: kj's `tryRead`/`write`/`shutdownWrite`/... come in
//! over the bridge as the methods below. When a hyper connection or client is later built over
//! that kj stream, `serve.rs` takes the tokio stream back out ([`RustStream::into_io`]), so a TLS
//! connection over a TCP socket reaches hyper as a plain tokio TLS stream, with no bridge
//! crossing per read.
//!
//! **Taking it back.** The C++ wrapper hands the object over only when [`RustStream::can_release`]
//! says so: no kj operation still holds `&self` (a pending read, write, shutdown flush or
//! `whenWriteDisconnected`), and the caller is on the thread that owns the stream. Otherwise the
//! stream stays wrapped and `serve.rs` drives it like any foreign kj stream -- the same rule
//! kj-rs-io's `TokioStream::into_socket` enforces with its shares.

use std::cell::Cell;
use std::cell::OnceCell;
use std::cell::RefCell;
use std::pin::Pin;
use std::task::Context;
use std::task::Poll;
use std::task::Waker;

use cxx::KjError;
use cxx::KjExceptionType;
use futures::FutureExt;
use futures::future::LocalBoxFuture;
use futures::future::Shared;
use tokio::io::AsyncRead;
use tokio::io::AsyncWrite;
use tokio::io::ReadBuf;

use crate::ffi::KjStreamWatchHalf;
use crate::serve::KjIoError;

/// How a stream reports that new writes are doomed ([`AsyncIo::write_disconnect`]).
pub enum WriteDisconnect {
    /// The transport cannot tell (kj permits a never-resolving `whenWriteDisconnected`).
    Never,
    /// A socket: watch a duplicate of its descriptor for a hangup, as kj-rs-io does.
    #[cfg(unix)]
    Fd(std::os::fd::OwnedFd),
    /// A foreign kj stream: its own `whenWriteDisconnected`.
    Kj(KjStreamWatchHalf),
}

/// A tokio byte stream a [`RustStream`] can carry.
///
/// What kj's stream interface needs beyond `AsyncRead + AsyncWrite`. `Send` because hyper's
/// upgrade machinery requires its transport to be; it is only ever polled from the one KJ loop
/// thread.
pub trait AsyncIo: AsyncRead + AsyncWrite + Send {
    /// The source of `whenWriteDisconnected`, for the transport underneath. Called at most once
    /// per stream.
    ///
    /// # Errors
    ///
    /// Duplicating the transport's descriptor failed, or the stream is off its owning thread.
    fn write_disconnect(self: Pin<&mut Self>) -> std::io::Result<WriteDisconnect>;

    /// `TCP_NODELAY` on the transport underneath, where there is one.
    ///
    /// # Errors
    ///
    /// The `setsockopt` failure.
    fn set_nodelay(&self, nodelay: bool) -> std::io::Result<()>;
}

/// The stream type this handle carries.
pub type BoxedIo = Pin<Box<dyn AsyncIo>>;

type Disconnected = Shared<LocalBoxFuture<'static, Result<(), KjError>>>;

/// See the module docs.
pub struct RustStream {
    io: RefCell<BoxedIo>,
    /// kj operations currently holding `&self` (see [`OpGuard`]).
    in_flight: Cell<usize>,
    read_aborted: Cell<bool>,
    /// The waker of a read parked on the stream, woken by `abort_read()`.
    read_waker: RefCell<Option<Waker>>,
    /// The thread owning the stream: its transport belongs to that thread's loop.
    owner: std::thread::ThreadId,
    /// The transport's `whenWriteDisconnected`, shared by every caller once the first asks.
    disconnected: OnceCell<Disconnected>,
}

/// Counts a kj operation holding `&self`, so the stream is not taken out from under it.
struct OpGuard<'a>(&'a Cell<usize>);

impl Drop for OpGuard<'_> {
    fn drop(&mut self) {
        self.0.set(self.0.get() - 1);
    }
}

impl RustStream {
    pub fn new(io: impl AsyncIo + 'static) -> Self {
        Self {
            io: RefCell::new(Box::pin(io)),
            in_flight: Cell::new(0),
            read_aborted: Cell::new(false),
            read_waker: RefCell::new(None),
            owner: std::thread::current().id(),
            disconnected: OnceCell::new(),
        }
    }

    /// Whether the stream may be taken out ([`RustStream::into_io`]): no kj operation holds it
    /// and the caller is on the owning thread.
    #[must_use]
    pub fn can_release(&self) -> bool {
        self.in_flight.get() == 0 && std::thread::current().id() == self.owner
    }

    /// Takes the stream out: the native path of `serve.rs`, taken only after
    /// [`RustStream::can_release`].
    pub fn into_io(self) -> BoxedIo {
        debug_assert!(self.can_release(), "RustStream taken while in use");
        self.io.into_inner()
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
        let disconnected = if let Some(disconnected) = self.disconnected.get() {
            disconnected.clone()
        } else {
            let source = self
                .io
                .borrow_mut()
                .as_mut()
                .write_disconnect()
                .map_err(|e| kj_error_for_stream_io("whenWriteDisconnected", &e))?;
            self.disconnected
                .get_or_init(|| disconnect_future(source))
                .clone()
        };
        disconnected.await
    }

    /// Polls the stream for one operation, mapping its failure to kj.
    fn poll_io<T>(
        &self,
        cx: &mut Context<'_>,
        context: &str,
        poll: impl FnOnce(Pin<&mut dyn AsyncIo>, &mut Context<'_>) -> Poll<std::io::Result<T>>,
    ) -> Poll<Result<T, KjError>> {
        poll(self.io.borrow_mut().as_mut(), cx).map_err(|e| kj_error_for_stream_io(context, &e))
    }
}

fn read_aborted() -> KjError {
    KjError::new(
        KjExceptionType::Disconnected,
        "abortRead() has been called".to_owned(),
    )
}

/// The one `whenWriteDisconnected` future a stream shares among its callers.
fn disconnect_future(source: WriteDisconnect) -> Disconnected {
    match source {
        WriteDisconnect::Never => std::future::pending().boxed_local().shared(),
        #[cfg(unix)]
        WriteDisconnect::Fd(fd) => async move {
            // Plain writability is cleared so the wait sleeps until a state change; a half-close
            // does not count (kj-rs-io's `when_write_disconnected` rule).
            let watch = tokio::io::unix::AsyncFd::with_interest(fd, tokio::io::Interest::WRITABLE)
                .map_err(|e| kj_error_for_stream_io("whenWriteDisconnected", &e))?;
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
        .boxed_local()
        .shared(),
        // Either outcome means writes are doomed or the stream cannot tell any more.
        WriteDisconnect::Kj(mut watch) => async move {
            let _ = watch.when_write_disconnected().await;
            Ok(())
        }
        .boxed_local()
        .shared(),
    }
}

/// Renders a stream I/O failure as a `kj::Exception`.
///
/// A kj exception carried through the transport (a foreign kj stream underneath) comes back as
/// itself. tokio-rustls surfaces TLS failures as `io::Error`s wrapping a `rustls::Error`; those
/// keep kj's certificate-verification wording (see `tls.rs`). Everything else maps by kind.
pub fn kj_error_for_stream_io(context: &str, e: &std::io::Error) -> KjError {
    if let Some(inner) = e.get_ref() {
        if let Some(KjIoError(error)) = inner.downcast_ref::<KjIoError>() {
            return error.clone();
        }
        if let Some(tls_error) = inner.downcast_ref::<rustls::Error>() {
            return crate::tls::kj_error_for_rustls_error(tls_error);
        }
    }
    crate::client::kj_error_for_io(context, e)
}
