//! A Rust byte stream handed to kj as a `kj::AsyncIoStream`, and taken back natively.
//!
//! [`RustStream`] owns any tokio `AsyncRead + AsyncWrite` (today: a TLS stream over a served
//! kj stream, see `tls.rs`) and is wrapped by the C++ `RustAsyncIoStream` (hyper-server-ffi.c++)
//! for kj consumers: the kj `tryRead`/`write`/`shutdownWrite` calls come in over the bridge as
//! the methods below. When a hyper connection or client is later built over that kj stream,
//! `serve.rs` recognizes the wrapper, releases this object and takes the tokio stream out of it
//! ([`RustStream::into_parts`]) -- so a TLS connection over a TCP socket reaches hyper as a plain
//! tokio TLS stream, with neither a pump nor a bridge crossing per read.
//!
//! **The pump.** When the stream underneath was a foreign kj stream (an in-memory pipe, a
//! promised stream), `serve.rs` bridged it with a [`StreamPump`] that must be polled on the KJ
//! thread. It rides along here and is polled whenever an operation on this stream is polled --
//! before, so kj-side bytes are visible to the read, and after, so a write the operation just
//! offered starts into the kj stream at once. A consumer taking the parts gets the pump back and
//! drives it itself (hyper's serve loop does).

use std::cell::Cell;
use std::cell::RefCell;
use std::pin::Pin;
use std::task::Context;
use std::task::Poll;

use cxx::KjError;
use cxx::KjExceptionType;
use tokio::io::AsyncRead;
use tokio::io::AsyncWrite;
use tokio::io::ReadBuf;

use crate::serve::StreamPump;

/// Any tokio byte stream. `Send` because hyper's upgrade machinery requires its transport to
/// be; it is only ever polled from the one KJ loop thread.
pub trait AsyncIo: AsyncRead + AsyncWrite + Send {}
impl<T: AsyncRead + AsyncWrite + Send + ?Sized> AsyncIo for T {}

/// The stream type this handle carries.
pub type BoxedIo = Pin<Box<dyn AsyncIo>>;

/// See the module docs.
pub struct RustStream {
    io: RefCell<BoxedIo>,
    pump: RefCell<Option<StreamPump>>,
    /// The pump settled with this error (the kj stream failed); reported in place of the
    /// consumer-side failure it caused.
    pump_error: RefCell<Option<KjError>>,
    read_aborted: Cell<bool>,
}

impl RustStream {
    pub fn new(io: impl AsyncRead + AsyncWrite + Send + 'static, pump: Option<StreamPump>) -> Self {
        Self {
            io: RefCell::new(Box::pin(io)),
            pump: RefCell::new(pump),
            pump_error: RefCell::new(None),
            read_aborted: Cell::new(false),
        }
    }

    /// Takes the stream and its pump (if any) out: the native path of `serve.rs`. The caller
    /// must drive the pump on the KJ thread until it settles or is dropped.
    pub fn into_parts(self: Box<Self>) -> (BoxedIo, Option<StreamPump>) {
        let Self { io, pump, .. } = *self;
        (io.into_inner(), pump.into_inner())
    }

    /// Corresponds to `kj::AsyncInputStream::tryRead(buffer, min_bytes, buffer.len())`.
    pub async fn read(&self, buffer: &mut [u8], min_bytes: usize) -> Result<usize, KjError> {
        if self.read_aborted.get() {
            return Err(KjError::new(
                KjExceptionType::Disconnected,
                "read end of the stream was aborted".to_owned(),
            ));
        }
        let min_bytes = min_bytes.min(buffer.len());
        let mut total = 0;
        while total < min_bytes {
            let n = std::future::poll_fn(|cx| {
                self.poll_io(cx, "read()", |io, cx| {
                    let mut buf = ReadBuf::new(&mut buffer[total..]);
                    io.poll_read(cx, &mut buf).map_ok(|()| buf.filled().len())
                })
            })
            .await?;
            if n == 0 {
                break; // EOF: fewer than min_bytes signals EOF to kj.
            }
            total += n;
        }
        Ok(total)
    }

    /// Corresponds to `kj::AsyncOutputStream::write()` (write-all semantics).
    pub async fn write(&self, buffer: &[u8]) -> Result<(), KjError> {
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
    /// holds (a TLS close_notify) and shuts the write side down. Async because that flush is;
    /// the C++ wrapper runs it as a detached task, as kj's own TLS stream does.
    pub async fn shutdown_write(&self) -> Result<(), KjError> {
        std::future::poll_fn(|cx| {
            self.poll_io(cx, "shutdownWrite()", |io, cx| io.poll_shutdown(cx))
        })
        .await
    }

    /// Corresponds to `kj::AsyncIoStream::abortRead()`: future reads fail.
    pub fn abort_read(&self) {
        self.read_aborted.set(true);
    }

    /// Polls the pump around one poll of the stream (see the module docs).
    fn poll_io<T>(
        &self,
        cx: &mut Context<'_>,
        context: &str,
        op: impl FnOnce(Pin<&mut dyn AsyncIo>, &mut Context<'_>) -> Poll<std::io::Result<T>>,
    ) -> Poll<Result<T, KjError>> {
        self.poll_pump(cx);
        let result = op(self.io.borrow_mut().as_mut(), cx);
        self.poll_pump(cx);
        match result {
            Poll::Pending => Poll::Pending,
            Poll::Ready(Ok(value)) => Poll::Ready(Ok(value)),
            Poll::Ready(Err(e)) => Poll::Ready(Err(match self.pump_error.borrow_mut().take() {
                Some(pump_error) => pump_error,
                None => kj_error_for_stream_io(context, &e),
            })),
        }
    }

    fn poll_pump(&self, cx: &mut Context<'_>) {
        let mut pump = self.pump.borrow_mut();
        if let Some(future) = pump.as_mut()
            && let Poll::Ready(result) = future.as_mut().poll(cx)
        {
            *pump = None;
            if let Err(error) = result {
                *self.pump_error.borrow_mut() = Some(error);
            }
        }
    }
}

/// Renders a stream I/O failure as a `kj::Exception`. tokio-rustls surfaces TLS failures as
/// `io::Error`s wrapping a `rustls::Error`; those keep kj's certificate-verification wording
/// (see `tls.rs`), everything else maps by error kind.
pub fn kj_error_for_stream_io(context: &str, e: &std::io::Error) -> KjError {
    if let Some(inner) = e.get_ref()
        && let Some(tls_error) = inner.downcast_ref::<rustls::Error>()
    {
        return crate::tls::kj_error_for_rustls_error(tls_error);
    }
    crate::client::kj_error_for_io(context, e)
}
