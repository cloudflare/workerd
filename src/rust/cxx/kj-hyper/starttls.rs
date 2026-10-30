//! A transport that starts as plain TCP and becomes TLS when kj's `TlsStarterCallback` is invoked
//! (opportunistic TLS: a worker's `socket.startTls()`). The client's CONNECT tunnel upgrades as
//! the client; a TCP listener's connection, offered to a `connect()` handler, as the server.
//!
//! The kj stream C++ pumps ([`StartTlsIo`]) and the starter handed to kj ([`TlsStarter`]) share
//! one [`Transport`]. The handshake is driven by whichever task polls it: the starter awaiting
//! it, or a read parked on the stream when it began (kj allows one). Once it settles, every
//! parked task is woken and I/O continues over TLS.

use std::cell::Cell;
use std::cell::RefCell;
use std::fmt::Display;
use std::future::Future;
use std::future::poll_fn;
use std::io;
use std::mem;
use std::pin::Pin;
use std::rc::Rc;
use std::sync::Arc;
use std::task::Context;
use std::task::Poll;
use std::task::Waker;

use cxx::KjError;
use cxx::KjExceptionType;
use kj_rs::KjOwn;
use tokio::io::AsyncRead;
use tokio::io::AsyncWrite;
use tokio::io::ReadBuf;

use crate::Result;
use crate::client::Dialed;
use crate::ffi::TlsStarterCallback;
use crate::ffi::new_tls_starter;
use crate::io::BoxIo;
use crate::io::Io;
use crate::tls;

type Handshake = Pin<Box<dyn Future<Output = Result<BoxIo>>>>;

enum Transport {
    /// The dial has not finished; only a starter invoked early observes this.
    Dialing,
    Plain(BoxIo),
    Handshaking(Handshake),
    Tls(BoxIo),
    /// The handshake failed; every operation reports why.
    Failed(String),
}

impl Transport {
    /// The plain connection, leaving the dial-in-progress state behind.
    fn take_plain(&mut self) -> Option<BoxIo> {
        match mem::replace(self, Self::Dialing) {
            Self::Plain(stream) => Some(stream),
            other => {
                *self = other;
                None
            }
        }
    }
}

/// Which end of the handshake this transport takes.
enum Role {
    /// Verifies the server as the name the starter is given, or as `host` (the connect host) when
    /// it names none.
    Client {
        config: Arc<rustls::ClientConfig>,
        host: String,
    },
    /// Presents the configured certificate. The name the starter is given is for verifying a
    /// server, so it is ignored.
    Server(Arc<rustls::ServerConfig>),
}

impl Role {
    fn handshake(&self, stream: BoxIo, name: &str) -> Handshake {
        match self {
            Self::Client { config, host } => {
                let config = Arc::clone(config);
                let name = if name.is_empty() { host } else { name }.to_owned();
                Box::pin(async move {
                    let stream = tls::connect(stream, config, &name).await?;
                    Ok(Box::new(stream) as BoxIo)
                })
            }
            Self::Server(config) => {
                let config = Arc::clone(config);
                Box::pin(async move {
                    let stream = tls::accept(stream, config).await?;
                    Ok(Box::new(stream) as BoxIo)
                })
            }
        }
    }
}

/// The state the kj stream ([`StartTlsIo`]) and the starter kj's callback holds
/// ([`TlsStarter`]) share, on one thread. Each cell is borrowed only inside one poll or call:
/// `started` is a flag read and set in one step, so a `Cell`; the rest are not `Copy`, so
/// `RefCell`s.
struct Shared {
    transport: RefCell<Transport>,
    role: Role,
    started: Cell<bool>,
    /// Tasks parked on a state change: the dial finishing or the handshake settling.
    waiters: RefCell<Vec<Waker>>,
}

impl Shared {
    fn park(&self, cx: &Context<'_>) {
        let mut waiters = self.waiters.borrow_mut();
        if !waiters.iter().any(|w| w.will_wake(cx.waker())) {
            waiters.push(cx.waker().clone());
        }
    }

    fn wake_all(&self) {
        let waiters = mem::take(&mut *self.waiters.borrow_mut());
        waiters.into_iter().for_each(Waker::wake);
    }

    /// Drives a handshake in progress; ready once the transport is not handshaking.
    fn poll_settled(&self, cx: &mut Context<'_>) -> Poll<()> {
        let mut transport = self.transport.borrow_mut();
        let Transport::Handshaking(handshake) = &mut *transport else {
            return Poll::Ready(());
        };
        let Poll::Ready(result) = handshake.as_mut().poll(cx) else {
            drop(transport);
            self.park(cx);
            return Poll::Pending;
        };
        *transport = match result {
            Ok(tls) => Transport::Tls(tls),
            Err(e) => Transport::Failed(e.description().to_owned()),
        };
        drop(transport);
        self.wake_all();
        Poll::Ready(())
    }

    /// The starter's work: begins the handshake once the dial is done, then awaits it.
    fn poll_start(&self, cx: &mut Context<'_>, name: &str) -> Poll<Result<()>> {
        let mut transport = self.transport.borrow_mut();
        if matches!(*transport, Transport::Dialing) {
            drop(transport);
            self.park(cx);
            return Poll::Pending;
        }
        if let Some(stream) = transport.take_plain() {
            *transport = Transport::Handshaking(self.role.handshake(stream, name));
        }
        drop(transport);
        std::task::ready!(self.poll_settled(cx));
        match &*self.transport.borrow() {
            Transport::Tls(_) => Poll::Ready(Ok(())),
            Transport::Failed(why) => Poll::Ready(Err(failed(why))),
            _ => {
                self.park(cx);
                Poll::Pending
            }
        }
    }
}

fn failed(what: impl Display) -> KjError {
    KjError::new(KjExceptionType::Failed, what.to_string())
}

/// The upgradable transport, as the kj stream reads and writes it.
pub struct StartTlsIo(Rc<Shared>);

impl StartTlsIo {
    fn with(transport: Transport, role: Role) -> Self {
        Self(Rc::new(Shared {
            transport: RefCell::new(transport),
            role,
            started: Cell::new(false),
            waiters: RefCell::new(Vec::new()),
        }))
    }

    /// A client transport awaiting its dial, upgradable to TLS with `config` to `host` (or the
    /// name the starter is given).
    #[must_use]
    pub fn new(config: Arc<rustls::ClientConfig>, host: String) -> Self {
        Self::with(Transport::Dialing, Role::Client { config, host })
    }

    /// An accepted connection served in plaintext, and the `kj::TlsStarterCallback` that
    /// upgrades it to TLS as the server with `config` (a `connect()` handler's
    /// `socket.startTls()`). The callback is for a connect's `HttpConnectSettings`.
    pub fn server(
        io: impl AsyncRead + AsyncWrite + Unpin + 'static,
        config: Arc<rustls::ServerConfig>,
    ) -> (Self, KjOwn<TlsStarterCallback>) {
        let io = Self::with(Transport::Plain(Box::new(io)), Role::Server(config));
        let starter = new_tls_starter(Box::new(io.starter()));
        (io, starter)
    }

    /// The starter kj's `TlsStarterCallback` invokes.
    #[must_use]
    pub fn starter(&self) -> TlsStarter {
        TlsStarter(Rc::clone(&self.0))
    }

    /// The dialed connection; I/O and a starter invoked early proceed from here.
    pub fn dialed(&self, stream: Dialed) {
        *self.0.transport.borrow_mut() = Transport::Plain(Box::new(stream));
        self.0.wake_all();
    }

    fn poll_io<R>(
        &self,
        cx: &mut Context<'_>,
        op: impl FnOnce(Pin<&mut (dyn Io + 'static)>, &mut Context<'_>) -> Poll<io::Result<R>>,
    ) -> Poll<io::Result<R>> {
        std::task::ready!(self.0.poll_settled(cx));
        let mut transport = self.0.transport.borrow_mut();
        match &mut *transport {
            Transport::Plain(stream) | Transport::Tls(stream) => op(Pin::new(stream.as_mut()), cx),
            Transport::Failed(why) => Poll::Ready(Err(io::Error::other(why.clone()))),
            Transport::Dialing | Transport::Handshaking(_) => {
                drop(transport);
                self.0.park(cx);
                Poll::Pending
            }
        }
    }
}

impl AsyncRead for StartTlsIo {
    fn poll_read(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &mut ReadBuf<'_>,
    ) -> Poll<io::Result<()>> {
        match self.poll_io(cx, |io, cx| io.poll_read(cx, buf)) {
            // A peer that closes without `close_notify`: kj's upgraded stream reports it as the
            // end of the stream (its connections that begin with TLS report DISCONNECTED), and
            // workers upgraded by `startTls()` rely on that.
            Poll::Ready(Err(e)) if e.kind() == io::ErrorKind::UnexpectedEof => Poll::Ready(Ok(())),
            result => result,
        }
    }
}

impl AsyncWrite for StartTlsIo {
    fn poll_write(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &[u8],
    ) -> Poll<io::Result<usize>> {
        self.poll_io(cx, |io, cx| io.poll_write(cx, buf))
    }

    fn poll_flush(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<io::Result<()>> {
        self.poll_io(cx, AsyncWrite::poll_flush)
    }

    fn poll_shutdown(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<io::Result<()>> {
        self.poll_io(cx, AsyncWrite::poll_shutdown)
    }
}

/// kj's `TlsStarterCallback` for a [`StartTlsIo`] (bridged in ffi.rs).
pub struct TlsStarter(Rc<Shared>);

impl TlsStarter {
    /// Upgrades the transport to TLS. A client verifies the server as `expected_server_hostname`
    /// (the connect host when empty). Resolves once the handshake is complete; kj permits writes
    /// again from then on.
    pub fn start(
        &self,
        expected_server_hostname: &[u8],
    ) -> impl Future<Output = Result<()>> + use<> {
        let shared = Rc::clone(&self.0);
        let name = String::from_utf8(expected_server_hostname.to_vec());
        async move {
            if shared.started.replace(true) {
                return Err(failed("TLS was already started on this connection"));
            }
            let name = name.map_err(failed)?;
            poll_fn(|cx| shared.poll_start(cx, &name)).await
        }
    }
}
