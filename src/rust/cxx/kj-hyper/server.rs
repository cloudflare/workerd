//! One accepted connection served by hyper, each request dispatched to a [`Handler`] with
//! kj-typed arguments.
//!
//! A request's handler call runs alongside the connection rather than inside hyper's service
//! future: hyper needs the response head to go on, while the call goes on for as long as the
//! response body (or an upgraded WebSocket or tunnel) does, and past its response, as kj's
//! `HttpServer` awaits a whole service call. Without an upgrade, calls are cancelled when the
//! transport hangs up or the connection fails, as kj's `HttpServer` cancels the service call of a
//! peer that hangs up.
//!
//! A call that fails before responding is answered with a bare `500 Internal Server Error`: the
//! status text as the body and `Connection: close`, and the connection closes after it. The text
//! of an exception never reaches a client, and logging it is the handler's business. A failure
//! after the response head went out aborts the body, so hyper drops the connection instead of
//! framing a truncated message as complete, as does a body stream the handler aborts
//! (`abortWrite()`) before it drops it, which would otherwise end the body until the failure
//! comes; DISCONNECTED gets no answer. A call that returns without responding gets kj's
//! plain-text 500.
//!
//! Responses are written as kj writes them ([`crate::body::Head`]): `Connection: close` and the
//! body's framing first, then the application's headers in kj's order.
//!
//! # A connection and its handler on different threads
//!
//! [`serve_connection_remote`] runs the connection (hyper: parsing, framing, the transport) on
//! the calling task, which may belong to a runtime of several threads, and gives each request to
//! its caller as a [`RemoteRequest`], which is `Send`. The thread that has the handler calls
//! [`RemoteRequest::handle`], and the request is answered by the code that answers a local one:
//! everything above holds for it. What crosses threads is hyper's own request (the head, the
//! body, which that thread reads from hyper as the handler asks for it, and hyper's hand-over of
//! the transport for an upgrade) and, on the way back, the response head and a body that
//! holds a few chunks at most ([`crate::body::ChannelBody`]), so both bodies are bounded by
//! what their reader takes. Three things follow from the threads. A response body of unknown
//! length ends when its handler call has returned, not when its sink is dropped, since the
//! connection could otherwise see the sink of a failing response dropped before the failure
//! and frame the body as complete. And the `100 Continue` of a request with `Expect` is sent
//! when the handler reads the body only if the response head has not gone out by then: a
//! handler that sends its head first and reads afterwards gets none, where on one thread the
//! read is seen first. And the handler's thread reads the request body, and an upgraded
//! transport, paced to its own event port (`kj_rs_tokio::PortPace`): the connection's thread has
//! the next bytes ready whenever it is ahead, and a read that never has to wait would keep KJ
//! from everything else on the handler's thread. An upgraded transport is used by the handler's
//! thread directly, while it stays registered with the runtime that accepted it.
//!
//! # Request parsing
//!
//! Request parsing is hyper's. Framing is strict per RFC 9112: a request with both
//! `Content-Length` and `Transfer-Encoding` is read as chunked, reaches the handler without its
//! `Content-Length`, and closes the connection; an invalid `Content-Length` or differing
//! duplicates, whitespace before a header's colon, a folded header line, `Transfer-Encoding` on
//! HTTP/1.0 and a `Transfer-Encoding` that does not end in `chunked` are refused
//! (`Transfer-Encoding: gzip, chunked` is read as chunked), and an empty line before the request
//! line is accepted. kj differed on each of these. Absolute-form targets and `OPTIONS *` reach
//! the handler as written, as under kj.
//!
//! A request hyper cannot parse never reaches the [`Handler`]: hyper answers it itself with `400`
//! (`414` for a target past 65534 bytes, `431` for a head past hyper's buffer limit or
//! `MAX_HEADERS`), `Content-Type: text/plain`, `Connection: close` and hyper's own description of
//! the error as the body ("invalid HTTP header parsed"; a request line naming HEAD gets the same
//! head and no body), as RFC 9110 section 15.5 asks and kj did; upstream hyper sends no body
//! (patches/rust/crates/hyper/parse-error-explanation.patch).
//!
//! An HTTP/1.1 request with `Expect: 100-continue` gets hyper's `100 Continue` when the handler
//! first reads its body, so a client waits for the handler before sending the body. A handler
//! that responds without reading the body sends no `100 Continue`, and unless the body has
//! already arrived, the connection closes after the response. kj ignored `Expect`.

use std::cell::Cell;
use std::cell::RefCell;
use std::error::Error;
use std::fmt;
use std::future::Future;
use std::future::pending;
use std::future::poll_fn;
use std::io;
use std::pin::Pin;
use std::pin::pin;
use std::rc::Rc;
use std::sync::Arc;
use std::sync::Mutex;
use std::sync::MutexGuard;
use std::sync::PoisonError;
use std::sync::atomic::AtomicBool;
use std::sync::atomic::Ordering;
use std::task::Poll;
use std::time::Duration;

use cxx::KjError;
use cxx::KjExceptionType;
use futures::FutureExt;
use futures::StreamExt;
use futures::future::BoxFuture;
use futures::future::LocalBoxFuture;
use futures::future::Shared;
use futures::stream::FuturesUnordered;
use http::header::SEC_WEBSOCKET_KEY;
use http::header::UPGRADE;
use http::request::Parts;
use http::uri::Authority;
use hyper::body::Incoming;
use hyper::ext::ReasonPhrase;
use hyper::server::conn::http1;
use hyper::service::service_fn;
use hyper::upgrade;
use hyper::upgrade::OnUpgrade;
use hyper_util::rt::TokioIo;
use hyper_util::rt::TokioTimer;
use kj::http::HeaderTable;
use kj::http::HeadersRef;
use kj::http::Method;
use kj::http::ServiceResponse;
use kj::io::AsyncInputStream;
use kj::io::AsyncIoStream;
use kj_rs::KjMaybe;
use kj_rs::KjOwn;
use tokio::io::AsyncRead;
use tokio::io::AsyncWrite;
use tokio::sync::oneshot;
use tokio::sync::watch;

use crate::Result;
use crate::body::BodyAbort;
pub use crate::body::BodySink;
use crate::body::Builtin;
use crate::body::ChannelBody;
use crate::body::Head;
use crate::body::HeaderBlock;
use crate::body::MAX_HEADERS;
use crate::body::Protocol;
use crate::body::RustBody;
use crate::body::channel_of;
use crate::ffi::ConnectResponse;
use crate::ffi::HttpHeaders;
use crate::ffi::WebSocketCompression;
use crate::ffi::WebSocketErrorHandler;
use crate::ffi::new_body_stream;
use crate::ffi::new_connect_response;
use crate::ffi::new_server_response;
use crate::ffi::parse_method;
use crate::handshake::accept_key;
use crate::io::Hangup;
use crate::io::RustIo;
pub use crate::io::Upgrading;
use crate::io::io_kj_error;

type Response = http::Response<ChannelBody>;

fn failed(what: impl Into<String>) -> KjError {
    KjError::new(KjExceptionType::Failed, what.into())
}

/// A server's settings, as kj's `HttpServerSettings`: one value shared by every connection a
/// listener serves.
pub struct ServerSettings {
    /// How long a request head may take to arrive (kj's `headerTimeout`), on tokio's clock.
    pub header_timeout: Duration,
    /// How `acceptWebSocket()` negotiates permessage-deflate.
    pub websocket_compression: WebSocketCompression,
    /// Turns a WebSocket protocol error into the exception `receive()` fails with; kj's default
    /// when `None`.
    pub websocket_errors: Option<KjOwn<WebSocketErrorHandler>>,
}

impl Default for ServerSettings {
    fn default() -> Self {
        Self {
            header_timeout: Duration::from_secs(15),
            websocket_compression: WebSocketCompression::NONE,
            websocket_errors: None,
        }
    }
}

impl ServerSettings {
    fn websocket_errors(&self) -> KjMaybe<&WebSocketErrorHandler> {
        self.websocket_errors.as_deref().into()
    }
}

/// The application behind a connection.
///
/// The arguments are the ones `kj::http::Service` takes, so an implementation can forward them
/// straight to a C++ `WorkerInterface`; a connection's peer identity is whatever the caller of
/// [`serve_connection`] built its handler with.
#[async_trait::async_trait(?Send)]
pub trait Handler {
    /// One request. The response is sent through `response`; how a call that sends none or
    /// fails is answered is in the module docs.
    async fn request<'a>(
        &'a self,
        method: Method,
        url: &'a [u8],
        headers: HeadersRef<'a>,
        body: Pin<&'a mut AsyncInputStream>,
        response: ServiceResponse<'a>,
    ) -> Result<()>;

    /// One CONNECT request, answered through `connect`, in Rust or in C++.
    async fn connect<'a>(
        &'a self,
        host: &'a [u8],
        headers: HeadersRef<'a>,
        connect: Connect,
    ) -> Result<()>;
}

/// Graceful shutdown for every connection served with it: idle connections close, in-flight
/// requests finish and their connection closes after the response.
#[derive(Default)]
pub struct Shutdown(Option<watch::Sender<bool>>);

impl Shutdown {
    #[must_use]
    pub fn new() -> Self {
        Self(Some(watch::channel(false).0))
    }

    pub fn shutdown(&self) {
        if let Some(tx) = &self.0 {
            tx.send_replace(true);
        }
    }

    fn subscribe(&self) -> Option<watch::Receiver<bool>> {
        self.0.as_ref().map(watch::Sender::subscribe)
    }
}

/// What the requests of a local connection share: the application they are for.
struct Connection<'t> {
    table: &'t HeaderTable,
    settings: Rc<ServerSettings>,
    handler: Rc<dyn Handler + 't>,
}

/// What a connection shares with the response side of each of its requests. The two may be on
/// different threads ([`serve_connection_remote`]).
#[derive(Clone)]
struct Link {
    /// Set by the response that upgrades the connection: hyper hands the transport over and is
    /// done, while a call keeps using it. A request's [`ResponseSender`] sets it and the serve
    /// loop reads it.
    upgraded: Arc<AtomicBool>,
    /// The transport's hang-up signal, which an upgraded transport keeps.
    hangup: Shared<Hangup>,
    /// True once the server drains: the connection closes after its next response.
    draining: Option<watch::Receiver<bool>>,
    /// How many chunks of a response body may wait for hyper before a write waits.
    body_chunks: usize,
    /// The requests are handled on other threads than the connection's.
    remote: bool,
}

/// The way back to a request's connection: where its response head goes, and hyper's hand-over
/// of the transport for a request that may upgrade the connection.
struct Responder {
    head: oneshot::Sender<Response>,
    upgrade: Option<OnUpgrade>,
    link: Link,
}

/// The handler calls in flight (see the module docs). Owned by the serve loop's future, so
/// dropping that future drops the calls. hyper's service pushes to it and the serve loop polls
/// it, each locking it only for that; the lock is never contended, since both happen inside the
/// serve loop's poll.
type Calls<F> = Arc<Mutex<FuturesUnordered<F>>>;

fn lock<F>(calls: &Calls<F>) -> MutexGuard<'_, FuturesUnordered<F>> {
    calls.lock().unwrap_or_else(PoisonError::into_inner)
}

/// Serves HTTP/1.1 on `io` until the peer is done with the connection (or `shutdown` says so)
/// and every handler call has finished.
///
/// `hangup` is the transport's: an upgrade hands the handler a kj stream whose
/// `whenWriteDisconnected()` (and so the `whenAborted()` of a `kj::WebSocket` over it) resolves
/// when it does, which is how kj learns of a peer that goes away while nothing is read.
///
/// The table, the settings and whatever the handler borrows outlive the returned future; the
/// handler is called once per request, concurrently where the protocol allows.
///
/// # Errors
///
/// An I/O failure of the transport itself, or of watching it for a hang-up. A peer that
/// disconnects or speaks bad HTTP ends the connection quietly.
pub async fn serve_connection<'t, IO>(
    io: IO,
    hangup: Hangup,
    table: &'t HeaderTable,
    settings: Rc<ServerSettings>,
    handler: Rc<dyn Handler + 't>,
    shutdown: &Shutdown,
) -> Result<()>
where
    IO: AsyncRead + AsyncWrite + Unpin + Send + 'static,
{
    let header_timeout = settings.header_timeout;
    let conn = Rc::new(Connection {
        table,
        settings,
        handler,
    });
    let start = move |parts: Parts, body: Incoming, responder: Responder| {
        let conn = Rc::clone(&conn);
        let call: LocalBoxFuture<'t, ()> = Box::pin(async move {
            let sender = Rc::new(ResponseSender::new(responder, &parts));
            handle(
                conn.table,
                &conn.settings,
                &*conn.handler,
                parts,
                body,
                sender,
            )
            .await;
        });
        call
    };
    serve(io, hangup, header_timeout, shutdown, None, start).await
}

/// One request of a connection that [`serve_connection_remote`] serves, on its way to the thread
/// that handles it.
///
/// It is the request as hyper parsed it: the head, the body, which is read from the connection
/// as the handler reads it, and the way back for the response. Dropping it unhandled closes the
/// connection without an answer.
pub struct RemoteRequest {
    parts: Parts,
    body: Incoming,
    responder: Responder,
}

impl RemoteRequest {
    /// The request's target, as the client sent it.
    #[must_use]
    pub const fn uri(&self) -> &http::Uri {
        &self.parts.uri
    }

    /// Calls `handler` with the request, on the calling thread, and answers the connection as
    /// [`serve_connection`] answers its own (module docs): the response the handler sends, the
    /// upgraded transport it takes, or the answer of a call that sends none.
    ///
    /// Resolves when the call has finished. Dropping the future cancels the call; a response
    /// whose head went out is then cut short.
    pub async fn handle(
        self,
        table: &HeaderTable,
        settings: &ServerSettings,
        handler: &dyn Handler,
    ) {
        let Self {
            parts,
            body,
            responder,
        } = self;
        let sender = Rc::new(ResponseSender::new(responder, &parts));
        // A call dropped after its head went out must not leave a body hyper would frame as
        // complete.
        let mut unfinished = AbortOnDrop(Some(Rc::clone(&sender)));
        handle(table, settings, handler, parts, body, sender).await;
        unfinished.0 = None;
    }
}

/// Fails the response body of a call that is dropped before it finished.
struct AbortOnDrop(Option<Rc<ResponseSender>>);

impl Drop for AbortOnDrop {
    fn drop(&mut self) {
        if let Some(sender) = &self.0 {
            sender.abort();
        }
    }
}

/// [`serve_connection`] for a server whose handlers are on other threads.
///
/// hyper serves `io` on the calling task, which may be one of a runtime with several threads,
/// and each request goes to `dispatch` as a [`RemoteRequest`] for another thread to
/// [`RemoteRequest::handle`].
///
/// The future `dispatch` returns stands for the handler call: it resolves once the call has
/// finished, wherever it ran, and dropping it cancels the call. It is dropped when the peer
/// hangs up or the connection fails, as [`serve_connection`] cancels its calls then, and
/// otherwise awaited before the connection is done.
///
/// # Errors
///
/// As [`serve_connection`].
pub async fn serve_connection_remote<IO, D>(
    io: IO,
    hangup: Hangup,
    header_timeout: Duration,
    shutdown: &Shutdown,
    dispatch: D,
) -> Result<()>
where
    IO: AsyncRead + AsyncWrite + Unpin + Send + 'static,
    D: Fn(RemoteRequest) -> BoxFuture<'static, ()>,
{
    let start = move |parts: Parts, body: Incoming, responder: Responder| {
        dispatch(RemoteRequest {
            parts,
            body,
            responder,
        })
    };
    let chunks = Some(REMOTE_BODY_CHUNKS);
    serve(io, hangup, header_timeout, shutdown, chunks, start).await
}

/// How many chunks of a response body a handler on another thread may have written ahead of
/// hyper. With one, every chunk would wait for the connection's thread to take the one before,
/// two thread wake-ups per chunk; a few let the writer run ahead while that thread writes.
const REMOTE_BODY_CHUNKS: usize = 4;

/// hyper's settings for a served connection.
fn http1(header_timeout: Duration) -> http1::Builder {
    let mut builder = http1::Builder::new();
    builder
        .timer(TokioTimer::new())
        .header_read_timeout(header_timeout)
        // A client may shut down its side once its request is sent.
        .half_close(true)
        .title_case_headers(true)
        .preserve_header_case(true)
        .max_headers(MAX_HEADERS)
        .auto_date_header(false);
    builder
}

/// The serve loop of a connection: hyper on `io`, and the handler calls `start` makes of its
/// requests, wherever they run.
async fn serve<IO, S, F>(
    io: IO,
    hangup: Hangup,
    header_timeout: Duration,
    shutdown: &Shutdown,
    remote_body_chunks: Option<usize>,
    start: S,
) -> Result<()>
where
    IO: AsyncRead + AsyncWrite + Unpin + Send + 'static,
    S: Fn(Parts, Incoming, Responder) -> F,
    F: Future<Output = ()> + Unpin,
{
    let hangup = hangup.shared();
    let link = Link {
        upgraded: Arc::default(),
        hangup: hangup.clone(),
        draining: shutdown.subscribe(),
        body_chunks: remote_body_chunks.unwrap_or(1),
        remote: remote_body_chunks.is_some(),
    };
    let calls: Calls<F> = Arc::default();
    let service = {
        let link = link.clone();
        let calls = Arc::clone(&calls);
        service_fn(move |mut request: http::Request<Incoming>| {
            let may_upgrade = request.method() == http::Method::CONNECT
                || request.headers().contains_key(UPGRADE);
            let upgrade = may_upgrade.then(|| upgrade::on(&mut request));
            let (parts, body) = request.into_parts();
            let (head, sent) = oneshot::channel();
            let responder = Responder {
                head,
                upgrade,
                link: link.clone(),
            };
            // The call starts alongside the connection; hyper waits for the response head it
            // sends. A call that ends without one closes the connection.
            lock(&calls).push(start(parts, body, responder));
            let remote = link.remote;
            async move { sent.await.map_err(|_| Unanswered::new(remote)) }
        })
    };
    let mut serving = pin!(
        http1(header_timeout)
            .serve_connection(TokioIo::new(io), service)
            .with_upgrades()
    );
    let mut drain = link.draining.clone();
    let mut drained = pin!(async move {
        match drain.as_mut() {
            Some(drain) => drop(drain.wait_for(|draining| *draining).await),
            None => pending().await,
        }
    });
    let upgraded = || link.upgraded.load(Ordering::Acquire);
    let mut hangup = pin!(hangup);
    let mut draining = false;
    let mut served: Option<hyper::Result<()>> = None;
    poll_fn(|cx| {
        // Only while hyper serves: hyper shuts down its side once it is done, so a hang-up after
        // that is the close it started, not a peer going away mid-call.
        if served.is_none()
            && !upgraded()
            && let Poll::Ready(hung_up) = hangup.as_mut().poll(cx)
        {
            // A failure to watch the transport fails the serve, as under kj's `exclusiveJoin`.
            hung_up?;
            // The peer is gone: nothing a call does can reach it.
            served = Some(Ok(()));
            lock(&calls).clear();
        }
        if !draining && drained.as_mut().poll(cx).is_ready() {
            draining = true;
            if served.is_none() {
                serving.as_mut().graceful_shutdown();
            }
        }
        // One fixed point per wake: polling the connection may start a handler call (hyper runs
        // the service), which then gets its first poll in this same turn.
        loop {
            let mut progressed = false;
            while lock(&calls).poll_next_unpin(cx) == Poll::Ready(Some(())) {
                progressed = true;
            }
            let before = lock(&calls).len();
            if served.is_none()
                && let Poll::Ready(result) = serving.as_mut().poll(cx)
            {
                if result.is_err() && !upgraded() {
                    // The connection failed: nothing a call does can reach the peer.
                    lock(&calls).clear();
                }
                served = Some(result);
                progressed = true;
            }
            if lock(&calls).len() != before {
                progressed = true;
            }
            if !progressed {
                break;
            }
        }
        if served.is_some() && lock(&calls).is_empty() {
            Poll::Ready(Result::<()>::Ok(()))
        } else {
            Poll::Pending
        }
    })
    .await?;
    // A failure of the transport itself is the serve's; protocol errors and disconnects end the
    // connection quietly.
    match served {
        Some(Err(error)) => match transport_error(&error) {
            Some(e) if e.exception_type() != KjExceptionType::Disconnected => Err(e),
            _ => Ok(()),
        },
        _ => Ok(()),
    }
}

/// The failure of a request whose handler call ended, or was dropped, without a response:
/// hyper closes the connection.
#[derive(Debug)]
enum Unanswered {
    /// By a call on the connection's own thread, whose handler failed as disconnected before it
    /// sent a head: the serve's failure, an I/O error as far as [`transport_error`] is
    /// concerned.
    Local(io::Error),
    /// By a call on another thread, which also ends unanswered when that thread drops the
    /// request it was handed, as it does when it stops: that closes the connection and is
    /// nobody's failure.
    Remote,
}

impl Unanswered {
    const WHAT: &str = "the handler sent no response";

    fn new(remote: bool) -> Self {
        if remote {
            Self::Remote
        } else {
            Self::Local(io::Error::other(Self::WHAT))
        }
    }
}

impl fmt::Display for Unanswered {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(Self::WHAT)
    }
}

impl Error for Unanswered {
    fn source(&self) -> Option<&(dyn Error + 'static)> {
        match self {
            Self::Local(error) => Some(error),
            Self::Remote => None,
        }
    }
}

/// The I/O error behind a hyper error, if that is what it is.
fn transport_error(error: &hyper::Error) -> Option<KjError> {
    let mut source = Error::source(error);
    while let Some(e) = source {
        if let Some(io) = e.downcast_ref::<io::Error>() {
            return Some(io_kj_error(io));
        }
        source = e.source();
    }
    None
}

/// One handler call, and the answer when it sends none (module docs).
async fn handle(
    table: &HeaderTable,
    settings: &ServerSettings,
    handler: &dyn Handler,
    parts: Parts,
    body: Incoming,
    sender: Rc<ResponseSender>,
) {
    let headers = match HeaderBlock::new(&parts.headers, &parts.extensions).to_kj(table) {
        Ok(headers) => headers,
        Err(e) => {
            let _ = sender.send_error(400, b"Bad Request", format!("ERROR: {}", e.description()));
            return;
        }
    };
    let result = if parts.method == http::Method::CONNECT {
        let Ok(tunnel) = sender.take_upgrade() else {
            return;
        };
        let host = parts
            .uri
            .authority()
            .map_or("", Authority::as_str)
            .to_owned();
        let connect = Connect {
            sender: Rc::clone(&sender),
            tunnel,
        };
        handler
            .connect(host.as_bytes(), HeadersRef::from(&*headers), connect)
            .await
    } else {
        let mut method = Method::GET;
        if !parse_method(parts.method.as_str().as_bytes(), &mut method) {
            let _ = sender.send_error(
                501,
                b"Not Implemented",
                "ERROR: Unrecognized request method.",
            );
            return;
        }
        let url = parts.uri.to_string();
        let body = if sender.link.remote {
            RustBody::paced(body)
        } else {
            RustBody::new(body)
        };
        let mut body = new_body_stream(Box::new(body));
        let mut response = new_server_response(
            Box::new(ServerResponse(Rc::clone(&sender))),
            method,
            &headers,
            settings.websocket_compression,
            settings.websocket_errors(),
        );
        handler
            .request(
                method,
                url.as_bytes(),
                HeadersRef::from(&*headers),
                body.as_mut(),
                ServiceResponse::from(response.as_mut()),
            )
            .await
    };
    match result {
        Ok(()) if sender.sent() => sender.finish(),
        Ok(()) => {
            let _ = sender.send_error(
                500,
                b"Internal Server Error",
                "ERROR: The HttpService did not generate a response.",
            );
        }
        // The head went out already, or the peer is gone (as kj: no response).
        Err(e) if sender.sent() || e.exception_type() == KjExceptionType::Disconnected => {
            sender.abort();
        }
        Err(_) => {
            let text = b"Internal Server Error";
            let body = bytes::Bytes::from_static(text);
            let _ = sender.send_closing(500, text, Head::empty(), body);
        }
    }
}

/// The response side of one request, shared by the kj `Response` or `ConnectResponse` handed to
/// the handler and the call itself.
///
/// C++ calls the kj objects through `&self`, so the state they change is in cells, each taken or
/// set within one method and never borrowed across an await or a call out: the head goes out
/// once, the upgrade is taken once, and `closing` is set before the refusal it marks is sent.
struct ResponseSender {
    /// Until the head goes out.
    head: RefCell<Option<oneshot::Sender<Response>>>,
    head_request: bool,
    websocket_key: Option<http::HeaderValue>,
    /// For a request that may upgrade the connection: hyper's hand-over of the transport.
    upgrade: RefCell<Option<OnUpgrade>>,
    /// Fails the response body a `send()` opened, for a failure after the head went out.
    body_abort: RefCell<Option<BodyAbort>>,
    /// The connection closes after this response (kj's `closeAfterSend`), which says so.
    closing: Cell<bool>,
    /// The connection's: its upgrade flag, its hang-up signal, and whether the server drains (a
    /// response sent while it does says that the connection closes).
    link: Link,
}

impl ResponseSender {
    /// The response side of the request with the head `parts`.
    fn new(responder: Responder, parts: &Parts) -> Self {
        Self {
            head: RefCell::new(Some(responder.head)),
            head_request: parts.method == http::Method::HEAD,
            websocket_key: parts.headers.get(SEC_WEBSOCKET_KEY).cloned(),
            upgrade: RefCell::new(responder.upgrade),
            body_abort: RefCell::new(None),
            closing: Cell::new(false),
            link: responder.link,
        }
    }

    /// Sends the head of a response that upgrades the connection. The connection learns that it
    /// is upgraded no later than it gets the head, wherever it is served.
    fn send_upgrade(&self, status: u32, status_text: &[u8], head: Head) -> Result<()> {
        if self.sent() {
            return Err(failed("response already sent"));
        }
        self.link.upgraded.store(true, Ordering::Release);
        let sent = self.send_head(status, status_text, head, ChannelBody::empty());
        if sent.is_err() {
            // No head went out, so nobody acted on the flag: the connection is hyper's still,
            // and ends with its peer.
            self.link.upgraded.store(false, Ordering::Release);
        }
        sent
    }

    /// The head went out.
    fn sent(&self) -> bool {
        self.head.borrow().is_none()
    }

    /// The transport this request upgrades the connection to, once hyper hands it over.
    fn take_upgrade(&self) -> Result<Upgrading> {
        self.upgrade
            .borrow_mut()
            .take()
            .map(|upgrade| {
                let io = Upgrading::new(upgrade, self.link.hangup.clone());
                if self.link.remote { io.paced() } else { io }
            })
            .ok_or_else(|| failed("this request does not upgrade the connection"))
    }

    /// Says `Connection: close` when the connection closes after this response: the response
    /// refuses a CONNECT, or the server is draining.
    fn announce_close(&self, head: &mut Head) {
        if self.closing.get() || self.link.draining.as_ref().is_some_and(|d| *d.borrow()) {
            head.set(Builtin::Connection, http::HeaderValue::from_static("close"));
        }
    }

    fn send_head(
        &self,
        status: u32,
        status_text: &[u8],
        head: Head,
        body: ChannelBody,
    ) -> Result<()> {
        let mut parts = http::Response::new(()).into_parts().0;
        parts.status = u16::try_from(status)
            .ok()
            .and_then(|s| http::StatusCode::from_u16(s).ok())
            .ok_or_else(|| failed(format!("invalid status {status}")))?;
        if parts.status.canonical_reason().map(str::as_bytes) != Some(status_text)
            && let Ok(reason) = ReasonPhrase::try_from(bytes::Bytes::copy_from_slice(status_text))
        {
            parts.extensions.insert(reason);
        }
        head.apply(&mut parts.headers, &mut parts.extensions);
        let tx = self
            .head
            .borrow_mut()
            .take()
            .ok_or_else(|| failed("response already sent"))?;
        let _ = tx.send(http::Response::from_parts(parts, body));
        Ok(())
    }

    fn send(
        &self,
        status: u32,
        status_text: &[u8],
        headers: &HttpHeaders,
        length: Option<u64>,
    ) -> Result<BodySink> {
        // As kj: these statuses have no body, whatever the application meant to write.
        let length = if matches!(status, 204 | 205 | 304) {
            Some(0)
        } else {
            length
        };
        let mut head = Head::new(headers);
        // As kj: a response to HEAD has no body, and the application's own Content-Length or
        // Transfer-Encoding, describing the representation, stands.
        let described = self.head_request
            && (head.has(Builtin::ContentLength) || head.has(Builtin::TransferEncoding));
        if described {
            head.claim(Protocol::HeadResponse);
        } else {
            head.claim(Protocol::Message);
            match status {
                // No framing either.
                204 | 304 => {}
                // An empty representation is not announced to HEAD, except that a 205 always
                // spells its empty body out.
                _ if self.head_request && length == Some(0) && status != 205 => {}
                _ => head.frame(length),
            }
        }
        self.announce_close(&mut head);
        if self.head_request {
            self.send_head(status, status_text, head, ChannelBody::empty())?;
            return Ok(BodySink::discarding());
        }
        let (sink, abort, body) = channel_of(length, self.link.body_chunks, self.link.remote);
        self.send_head(status, status_text, head, body)?;
        *self.body_abort.borrow_mut() = Some(abort);
        Ok(sink)
    }

    /// A 2xx answer to a CONNECT, which has no body to frame: the connection is the tunnel once
    /// hyper has written it.
    fn accept_connect(&self, status: u32, status_text: &[u8], headers: &HttpHeaders) -> Result<()> {
        let mut head = Head::new(headers);
        head.claim(Protocol::Message);
        self.send_upgrade(status, status_text, head)
    }

    /// A plain-text response that closes the connection once sent.
    fn send_error(
        &self,
        status: u32,
        status_text: &[u8],
        message: impl Into<String>,
    ) -> Result<()> {
        let mut head = Head::empty();
        head.append(b"Content-Type", b"text/plain");
        if status == 426 {
            head.set(
                Builtin::SecWebSocketVersion,
                http::HeaderValue::from_static("13"),
            );
        }
        self.send_closing(status, status_text, head, message.into().into())
    }

    /// A response of exactly `body` that closes the connection once sent.
    fn send_closing(
        &self,
        status: u32,
        status_text: &[u8],
        mut head: Head,
        body: bytes::Bytes,
    ) -> Result<()> {
        head.set(Builtin::Connection, http::HeaderValue::from_static("close"));
        head.frame(Some(body.len() as u64));
        let body = if self.head_request {
            ChannelBody::empty()
        } else {
            ChannelBody::full(body)
        };
        self.send_head(status, status_text, head, body)
    }

    /// Says that the call ended well: the response body a `send()` opened, which a connection
    /// on another thread holds open until it knows, ends where its sink was dropped.
    fn finish(&self) {
        self.body_abort.borrow_mut().take();
    }

    /// Fails the response in flight, dropping the connection, for a failure after the head.
    fn abort(&self) {
        if let Some(abort) = self.body_abort.borrow_mut().take() {
            abort.abort();
        }
    }
}

/// `kj::HttpService::Response` (its Rust half; the C++ half is `ServerResponseImpl`).
pub struct ServerResponse(Rc<ResponseSender>);

impl ServerResponse {
    pub fn send(
        &self,
        status: u32,
        status_text: &[u8],
        headers: &HttpHeaders,
        length: KjMaybe<u64>,
    ) -> Result<Box<BodySink>> {
        self.0
            .send(status, status_text, headers, length.into())
            .map(Box::new)
    }

    /// kj's answer to a bad WebSocket handshake: a plain-text error closing the connection.
    pub fn send_error(&self, status: u32, status_text: &[u8], message: &[u8]) -> Result<()> {
        self.0
            .send_error(status, status_text, String::from_utf8_lossy(message))
    }

    /// Answers the handshake (C++ has checked it; `extensions` is the agreed
    /// `Sec-WebSocket-Extensions`, if any) and hands back the upgraded connection, which C++
    /// builds kj's `WebSocket` on.
    pub fn accept_websocket(
        &self,
        headers: &HttpHeaders,
        extensions: &[u8],
    ) -> Result<Box<RustIo>> {
        let key = self
            .0
            .websocket_key
            .as_ref()
            .ok_or_else(|| failed("not a WebSocket upgrade request"))?;
        let io = self.0.take_upgrade()?;
        let mut head = Head::new(headers);
        head.claim(Protocol::WebSocket);
        head.set(
            Builtin::Connection,
            http::HeaderValue::from_static("Upgrade"),
        );
        head.set(
            Builtin::Upgrade,
            http::HeaderValue::from_static("websocket"),
        );
        if let Ok(accept) = http::HeaderValue::from_str(&accept_key(key.as_bytes())) {
            head.set(Builtin::SecWebSocketAccept, accept);
        }
        if !extensions.is_empty() {
            head.set(
                Builtin::SecWebSocketExtensions,
                http::HeaderValue::from_bytes(extensions)
                    .map_err(|e| failed(format!("invalid extensions: {e}")))?,
            );
        }
        self.0.send_upgrade(101, b"Switching Protocols", head)?;
        Ok(Box::new(RustIo::from(io)))
    }
}

/// A CONNECT request's answer.
///
/// Accept it and take the tunnel in Rust, reject it, or hand both to a C++
/// `kj::HttpService::connect()` with [`Connect::into_kj`]. Answer before reading the tunnel:
/// bytes the client sent along with the request arrive only after the response head.
pub struct Connect {
    sender: Rc<ResponseSender>,
    tunnel: Upgrading,
}

impl Connect {
    /// Accepts with a 2xx status; the tunnel is the connection's transport once hyper has
    /// written the response.
    ///
    /// # Errors
    ///
    /// A status outside 2xx, or an answer already given.
    pub fn accept(
        self,
        status: u32,
        status_text: &str,
        headers: HeadersRef<'_>,
    ) -> Result<Upgrading> {
        if !(200..300).contains(&status) {
            return Err(failed("the statusCode must be 2xx for accept"));
        }
        self.sender
            .accept_connect(status, status_text.as_bytes(), headers.as_ffi())?;
        Ok(self.tunnel)
    }

    /// Rejects with a non-2xx status and a body written to the returned sink; the connection
    /// closes once the response is sent, as kj's server closes it.
    ///
    /// # Errors
    ///
    /// A 2xx status, or an answer already given.
    pub fn reject(
        self,
        status: u32,
        status_text: &str,
        headers: HeadersRef<'_>,
        length: Option<u64>,
    ) -> Result<BodySink> {
        if (200..300).contains(&status) {
            return Err(failed("the statusCode must not be 2xx for reject"));
        }
        self.sender.closing.set(true);
        self.sender
            .send(status, status_text.as_bytes(), headers.as_ffi(), length)
    }

    /// The tunnel and the response as kj takes them, for `kj::HttpService::connect()`. The
    /// tunnel fails if the response rejects the request.
    #[must_use]
    pub fn into_kj(self) -> (KjOwn<AsyncIoStream>, KjOwn<ConnectResponse>) {
        let tunnel = self.tunnel.into_kj();
        let response = new_connect_response(Box::new(ConnectResponder(self.sender)));
        (tunnel, response)
    }
}

/// `kj::HttpService::ConnectResponse` (its Rust half; the C++ half is `ConnectResponseImpl`).
pub struct ConnectResponder(Rc<ResponseSender>);

impl ConnectResponder {
    pub fn accept(&self, status: u32, status_text: &[u8], headers: &HttpHeaders) -> Result<()> {
        self.0.accept_connect(status, status_text, headers)
    }

    /// As kj's server: the connection closes once the refusal is sent.
    pub fn reject(
        &self,
        status: u32,
        status_text: &[u8],
        headers: &HttpHeaders,
        length: KjMaybe<u64>,
    ) -> Result<Box<BodySink>> {
        self.0.closing.set(true);
        self.0
            .send(status, status_text, headers, length.into())
            .map(Box::new)
    }
}

#[cfg(test)]
#[path = "server-test.rs"]
mod tests;
