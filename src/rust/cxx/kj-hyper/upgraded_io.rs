//! Shared plumbing for hyper connection upgrades (WebSocket sessions and CONNECT tunnels): a
//! lazily-ready byte stream over `hyper::upgrade::Upgraded`, pollable from the KJ event loop.
//!
//! # Runtime interplay
//!
//! The underlying socket is registered with the KJ thread's loop runtime (it was created by
//! the accept loop or the client dial, both tasks on that runtime), and after the upgrade all
//! I/O on it is driven by *KJ-side* futures: kj-rs's waker machinery forwards tokio reactor
//! wakes into KJ events, exactly like the request/response body bridging elsewhere in this
//! crate. Everything here is single-threaded (`Rc`/`RefCell`); interior-mutability borrows are
//! scoped to individual `poll` calls, so concurrent read/write/abort operations (all `&self`,
//! all polled by the same KJ event loop) never overlap borrows.

use std::cell::Cell;
use std::cell::RefCell;
use std::future::Future;
use std::pin::Pin;
use std::rc::Rc;
use std::task::Context;
use std::task::Poll;
use std::task::Waker;

use cxx::KjError;
use cxx::KjExceptionType;
use hyper::upgrade::Upgraded;
use hyper_util::rt::TokioIo;
use tokio::io::AsyncRead;
use tokio::io::AsyncWrite;
use tokio::io::ReadBuf;

/// A one-shot, latched event usable by many waiters (single-threaded).
#[derive(Default)]
pub struct LatchedEvent {
    fired: Cell<bool>,
    wakers: RefCell<Vec<Waker>>,
}

impl LatchedEvent {
    pub fn fire(&self) {
        if !self.fired.replace(true) {
            for waker in self.wakers.borrow_mut().drain(..) {
                waker.wake();
            }
        }
    }

    /// Resolves once (and as soon as) the event has fired.
    pub async fn wait(&self) {
        std::future::poll_fn(|cx| {
            if self.fired.get() {
                Poll::Ready(())
            } else {
                let mut wakers = self.wakers.borrow_mut();
                if !wakers.iter().any(|w| w.will_wake(cx.waker())) {
                    wakers.push(cx.waker().clone());
                }
                Poll::Pending
            }
        })
        .await;
    }
}

/// A repeating pulse event: wakes everyone currently waiting, without latching.
#[derive(Default)]
pub struct PulseEvent {
    wakers: RefCell<Vec<Waker>>,
}

impl PulseEvent {
    pub fn pulse(&self) {
        for waker in self.wakers.borrow_mut().drain(..) {
            waker.wake();
        }
    }

    /// Registers interest in the next pulse. Standard usage: check your condition, and if it
    /// does not hold, await this (re-checking the condition after every pulse).
    pub async fn next_pulse(&self) {
        let mut registered = false;
        std::future::poll_fn(|cx| {
            if registered {
                return Poll::Ready(());
            }
            registered = true;
            let mut wakers = self.wakers.borrow_mut();
            if !wakers.iter().any(|w| w.will_wake(cx.waker())) {
                wakers.push(cx.waker().clone());
            }
            Poll::Pending
        })
        .await;
    }
}

enum IoState {
    /// The upgrade response head has not necessarily gone out yet; hyper hands over the raw
    /// connection through this future once it has. (Boxed because `OnUpgrade` is not
    /// guaranteed to be `Unpin`.)
    Pending(Pin<Box<hyper::upgrade::OnUpgrade>>),
    Ready(TokioIo<Upgraded>),
    /// The upgrade will never happen (e.g. the CONNECT was rejected), or it failed.
    Failed(KjExceptionType, String),
}

/// The lazily-upgraded byte stream shared by every handle onto one upgraded connection.
pub struct SharedIo {
    state: RefCell<IoState>,
    /// Fires when the connection is observed to be broken (peer reset / local abort), or when
    /// the peer disappears entirely (EOF where the protocol expected more). Backs
    /// `kj::WebSocket::whenAborted()` / `kj::AsyncOutputStream::whenWriteDisconnected()`.
    /// Divergence from kj: kj can detect a peer reset proactively through epoll/kqueue error
    /// events; this signal only fires once some read or write actually observes the failure.
    aborted: LatchedEvent,
    /// `shutdownWrite()` was requested; applied immediately if ready, else once the upgrade
    /// completes.
    write_shutdown: Cell<bool>,
    write_shutdown_sent: Cell<bool>,
}

fn is_disconnect_kind(kind: std::io::ErrorKind) -> bool {
    use std::io::ErrorKind;
    matches!(
        kind,
        ErrorKind::ConnectionRefused
            | ErrorKind::ConnectionReset
            | ErrorKind::ConnectionAborted
            | ErrorKind::BrokenPipe
            | ErrorKind::NotConnected
            | ErrorKind::UnexpectedEof
    )
}

impl SharedIo {
    /// An already-upgraded stream (client side: the upgrade was awaited before construction).
    pub fn ready(upgraded: Upgraded) -> Rc<Self> {
        Rc::new(Self {
            state: RefCell::new(IoState::Ready(TokioIo::new(upgraded))),
            aborted: LatchedEvent::default(),
            write_shutdown: Cell::new(false),
            write_shutdown_sent: Cell::new(false),
        })
    }

    /// A stream that becomes ready once hyper has written the upgrade response head (server
    /// side: `acceptWebSocket()` / CONNECT `accept()` return before that happens).
    pub fn pending(on_upgrade: hyper::upgrade::OnUpgrade) -> Rc<Self> {
        Rc::new(Self {
            state: RefCell::new(IoState::Pending(Box::pin(on_upgrade))),
            aborted: LatchedEvent::default(),
            write_shutdown: Cell::new(false),
            write_shutdown_sent: Cell::new(false),
        })
    }

    /// Permanently fail the stream (e.g. the CONNECT tunnel was rejected). No effect once ready.
    pub fn fail(&self, exception_type: KjExceptionType, message: &str) {
        let mut state = self.state.borrow_mut();
        if matches!(&*state, IoState::Pending(_)) {
            *state = IoState::Failed(exception_type, message.to_owned());
        }
    }

    /// Classify and record an I/O failure: disconnect-class errors additionally fire the
    /// aborted signal (see the field docs).
    fn map_io_error(&self, context: &str, error: &std::io::Error) -> KjError {
        let exception_type = if is_disconnect_kind(error.kind()) {
            self.aborted.fire();
            KjExceptionType::Disconnected
        } else {
            KjExceptionType::Failed
        };
        KjError::new(exception_type, format!("{context}: {error}"))
    }

    /// Runs an I/O `operation` against the ready `TokioIo`, first driving the pending upgrade
    /// to completion if necessary. All `RefCell` borrows are contained within this single poll.
    fn poll_io<R>(
        &self,
        cx: &mut Context<'_>,
        context_str: &str,
        operation: impl FnOnce(
            Pin<&mut TokioIo<Upgraded>>,
            &mut Context<'_>,
        ) -> Poll<std::io::Result<R>>,
    ) -> Poll<Result<R, KjError>> {
        let mut state = self.state.borrow_mut();
        loop {
            match &mut *state {
                IoState::Pending(on_upgrade) => {
                    match on_upgrade.as_mut().poll(cx) {
                        Poll::Pending => return Poll::Pending,
                        Poll::Ready(Ok(upgraded)) => {
                            *state = IoState::Ready(TokioIo::new(upgraded));
                            // Loop to apply any deferred shutdown and run the operation.
                        }
                        Poll::Ready(Err(e)) => {
                            let message = format!("connection upgrade failed: {e}");
                            self.aborted.fire();
                            *state =
                                IoState::Failed(KjExceptionType::Disconnected, message.clone());
                            return Poll::Ready(Err(KjError::new(
                                KjExceptionType::Disconnected,
                                message,
                            )));
                        }
                    }
                }
                IoState::Ready(io) => {
                    if self.write_shutdown.get() && !self.write_shutdown_sent.get() {
                        // Deferred shutdownWrite(): best-effort, like a plain socket shutdown.
                        self.write_shutdown_sent.set(true);
                        let _ = Pin::new(&mut *io).poll_shutdown(cx);
                    }
                    return match operation(Pin::new(io), cx) {
                        Poll::Pending => Poll::Pending,
                        Poll::Ready(Ok(r)) => Poll::Ready(Ok(r)),
                        Poll::Ready(Err(e)) => Poll::Ready(Err(self.map_io_error(context_str, &e))),
                    };
                }
                IoState::Failed(exception_type, message) => {
                    return Poll::Ready(Err(KjError::new(*exception_type, message.clone())));
                }
            }
        }
    }

    /// Read some bytes. Returns 0 at EOF.
    pub fn poll_read_some(
        &self,
        cx: &mut Context<'_>,
        buffer: &mut [u8],
    ) -> Poll<Result<usize, KjError>> {
        self.poll_io(cx, "read()", |io, cx| {
            let mut read_buf = ReadBuf::new(buffer);
            match io.poll_read(cx, &mut read_buf) {
                Poll::Pending => Poll::Pending,
                Poll::Ready(Ok(())) => Poll::Ready(Ok(read_buf.filled().len())),
                Poll::Ready(Err(e)) => Poll::Ready(Err(e)),
            }
        })
    }

    /// Write some bytes (single attempt once ready).
    pub fn poll_write_some(
        &self,
        cx: &mut Context<'_>,
        buffer: &[u8],
    ) -> Poll<Result<usize, KjError>> {
        self.poll_io(cx, "write()", |io, cx| io.poll_write(cx, buffer))
    }

    /// KJ `tryRead` semantics: read until at least `min_bytes` (or EOF), up to `buffer.len()`.
    pub async fn read_min(&self, buffer: &mut [u8], min_bytes: usize) -> Result<usize, KjError> {
        let min_bytes = min_bytes.min(buffer.len());
        let mut total = 0;
        while total < min_bytes {
            let n =
                std::future::poll_fn(|cx| self.poll_read_some(cx, &mut buffer[total..])).await?;
            if n == 0 {
                break; // EOF: fewer than min_bytes signals EOF to KJ.
            }
            total += n;
        }
        Ok(total)
    }

    /// Write-all semantics.
    pub async fn write_all(&self, buffer: &[u8]) -> Result<(), KjError> {
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

    /// `shutdown(SHUT_WR)` equivalent. Applied on the next poll if the upgrade is not ready
    /// yet. Safe to call multiple times.
    pub fn shutdown_write(&self) {
        self.write_shutdown.set(true);
        if let Ok(mut state) = self.state.try_borrow_mut()
            && let IoState::Ready(io) = &mut *state
            && !self.write_shutdown_sent.replace(true)
        {
            // TCP shutdown completes synchronously; poll once with a no-op waker.
            let mut cx = Context::from_waker(Waker::noop());
            let _ = Pin::new(io).poll_shutdown(&mut cx);
        }
    }

    /// Locally abort the connection: fires the aborted signal and shuts down the write end.
    pub fn abort(&self) {
        self.aborted.fire();
        self.shutdown_write();
    }

    pub fn aborted(&self) -> &LatchedEvent {
        &self.aborted
    }
}

// =======================================================================================
// CONNECT tunnels

/// The Rust side of a `kj::AsyncIoStream` over an upgraded CONNECT tunnel (wrapped by
/// `RustTunnelStream` in hyper-server-ffi.c++). Server-side tunnels start in the pending state
/// and become readable/writable once the service accepts and hyper hands over the connection;
/// client-side tunnels are constructed ready.
pub struct HyperTunnel {
    io: Rc<SharedIo>,
    read_aborted: Cell<bool>,
}

impl HyperTunnel {
    pub(crate) fn new(io: Rc<SharedIo>) -> Self {
        Self {
            io,
            read_aborted: Cell::new(false),
        }
    }

    /// Corresponds to `kj::AsyncInputStream::tryRead(buffer, min_bytes, buffer.len())`.
    pub async fn read(&self, buffer: &mut [u8], min_bytes: usize) -> Result<usize, KjError> {
        if self.read_aborted.get() {
            return Err(KjError::new(
                KjExceptionType::Disconnected,
                "read end of the tunnel was aborted".to_owned(),
            ));
        }
        self.io.read_min(buffer, min_bytes).await
    }

    /// Corresponds to `kj::AsyncOutputStream::write()`.
    pub async fn write(&self, buffer: &[u8]) -> Result<(), KjError> {
        self.io.write_all(buffer).await
    }

    /// Corresponds to `kj::AsyncIoStream::shutdownWrite()`. Deferred until the tunnel is
    /// established if called early.
    pub fn shutdown_write(&self) {
        self.io.shutdown_write();
    }

    /// Corresponds to `kj::AsyncIoStream::abortRead()`: future reads fail.
    pub fn abort_read(&self) {
        self.read_aborted.set(true);
    }

    /// Corresponds to `kj::AsyncOutputStream::whenWriteDisconnected()`. See `SharedIo::aborted`
    /// for the detection divergence from kj.
    pub async fn when_write_disconnected(&self) {
        self.io.aborted().wait().await;
    }
}
