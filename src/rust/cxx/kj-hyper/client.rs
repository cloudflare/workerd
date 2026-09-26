//! A pooled HTTP/1.1 client as a `kj::http::Service`, so C++ (a `WorkerInterface` for an
//! external service, say) makes outbound requests through it.
//!
//! The pool, keep-alive, idle eviction, upgrades and retries of stale connections are
//! `hyper-util`'s legacy client's; this module supplies the connector and the kj adaptation:
//! request bodies pumped from kj streams, responses (bodies, `WebSocket`s, tunnels) written back
//! into the kj `Response`. Connection tasks run on the current tokio runtime, so the transports a
//! dialer produces must be `Send`.
//!
//! Dropping a client request drops its connection (hyper's rule); the request body pump ends
//! with the exchange, as kj's adapter's does.

use std::error::Error;
use std::fmt::Display;
use std::future::Future;
use std::future::poll_fn;
use std::io;
use std::net::SocketAddr;
use std::pin::Pin;
use std::rc::Rc;
use std::str::from_utf8;
use std::sync::Arc;
use std::task::Context;
use std::task::Poll;
use std::time::Duration;

use cxx::KjError;
use cxx::KjExceptionType;
use futures::FutureExt;
use futures::TryFutureExt;
use futures::future::Either;
use futures::future::Shared;
use futures::future::select;
use http::header::SEC_WEBSOCKET_ACCEPT;
use http::header::UPGRADE;
use http::response::Parts;
use http::uri::Authority;
use http::uri::Scheme;
use hyper::body::Incoming;
use hyper::ext::ReasonPhrase;
use hyper::upgrade;
use hyper::upgrade::OnUpgrade;
use hyper_util::client::legacy;
use hyper_util::client::legacy::connect::Connected;
use hyper_util::client::legacy::connect::Connection;
use hyper_util::rt::TokioExecutor;
use hyper_util::rt::TokioIo;
use kj::http::ConnectResponse;
use kj::http::ConnectSettings;
use kj::http::HeaderId;
use kj::http::HeaderTable;
use kj::http::Headers;
use kj::http::HeadersRef;
use kj::http::Method;
use kj::http::Service;
use kj::http::ServiceResponse;
use kj::io::AsyncInputStream;
use kj::io::AsyncIoStream;
use kj_rs::KjMaybe;
use kj_rs::KjOwn;
use tokio::io::AsyncRead;
use tokio::io::AsyncWrite;
use tokio::io::ReadBuf;
use tokio::net::TcpStream;
#[cfg(unix)]
use tokio::net::UnixStream;
use tokio::net::lookup_host;

use crate::Result;
use crate::body::BodyAbort;
use crate::body::BodySink;
use crate::body::Builtin;
use crate::body::ChannelBody;
use crate::body::Head;
use crate::body::HeaderBlock;
use crate::body::MAX_HEADERS;
use crate::body::Protocol;
use crate::body::channel;
use crate::ffi::TlsStarterCallback;
use crate::ffi::WebSocketCompression;
use crate::ffi::WebSocketErrorHandler;
use crate::ffi::new_client_websocket;
use crate::ffi::pump_tunnel;
use crate::ffi::pump_websockets;
use crate::ffi::tls_starter_set;
use crate::ffi::websocket_agreement;
use crate::ffi::websocket_offer;
use crate::handshake::accept_key;
use crate::handshake::client_key;
use crate::io::Hangup;
use crate::io::into_kj_stream_with;
use crate::io::io_kj_error;
use crate::starttls::StartTlsIo;
use crate::tls;

fn failed(what: impl Display) -> KjError {
    KjError::new(KjExceptionType::Failed, what.to_string())
}

fn disconnected(what: impl Display) -> KjError {
    KjError::new(KjExceptionType::Disconnected, what.to_string())
}

/// A request's failure: the I/O error behind it when there is one, else DISCONNECTED (hyper
/// reports a connection that went away in several shapes).
fn client_error(error: &(dyn Error + 'static)) -> KjError {
    let mut source = Some(error);
    while let Some(e) = source {
        if let Some(io) = e.downcast_ref::<io::Error>() {
            return io_kj_error(io);
        }
        source = e.source();
    }
    disconnected(error)
}

/// What every request is made with.
pub struct ClientSettings {
    /// How long an idle connection stays in the pool (kj's `idleTimeout`).
    pub idle_timeout: Duration,
    /// How a WebSocket request's permessage-deflate is offered and agreed.
    pub websocket_compression: WebSocketCompression,
    /// Turns a WebSocket protocol error into the exception `receive()` fails with; kj's default
    /// when `None`.
    pub websocket_errors: Option<KjOwn<WebSocketErrorHandler>>,
}

impl Default for ClientSettings {
    fn default() -> Self {
        Self {
            idle_timeout: Duration::from_secs(5),
            websocket_compression: WebSocketCompression::NONE,
            websocket_errors: None,
        }
    }
}

// =======================================================================================
// Connector

/// What a connection's peer is asked for: a path (origin-form request targets), or, of an HTTP
/// proxy, a whole URL (absolute-form).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Peer {
    Origin,
    Proxy,
}

/// A dialed transport: any `Send` tokio stream, and the signal of its peer going away.
///
/// The kj streams made of the connection (an upgrade, a tunnel) resolve
/// `whenWriteDisconnected()` from the signal. A socket's comes with it (`Dialed::from`).
pub struct Dialed {
    io: Box<dyn Io + Send>,
    peer: Peer,
    hangup: Shared<Hangup>,
}

trait Io: AsyncRead + AsyncWrite + Unpin {}
impl<T: AsyncRead + AsyncWrite + Unpin + ?Sized> Io for T {}

impl Dialed {
    /// A transport and the signal of its peer going away.
    pub fn with_hangup(
        io: impl AsyncRead + AsyncWrite + Unpin + Send + 'static,
        hangup: Hangup,
    ) -> Self {
        Self {
            io: Box::new(io),
            peer: Peer::Origin,
            hangup: hangup.shared(),
        }
    }

    /// TLS over the connection, which keeps its hang-up signal.
    ///
    /// # Errors
    ///
    /// A failed handshake.
    pub async fn tls(
        self,
        config: Arc<rustls::ClientConfig>,
        server_name: &str,
    ) -> io::Result<Self> {
        let (peer, hangup) = (self.peer, self.hangup.clone());
        let stream = tls::connect(self, config, server_name)
            .await
            .map_err(|e| io::Error::other(e.description().to_owned()))?;
        Ok(Self {
            io: Box::new(stream),
            peer,
            hangup,
        })
    }

    /// The connection itself as a `kj::AsyncIoStream`.
    #[must_use]
    pub fn into_kj(self) -> KjOwn<AsyncIoStream> {
        let hangup = self.hangup.clone();
        into_kj_stream_with(self, Some(hangup))
    }
}

impl From<TcpStream> for Dialed {
    fn from(socket: TcpStream) -> Self {
        let hangup = kj_rs_io::when_write_disconnected(&socket);
        Self::with_hangup(socket, hangup.err_into().boxed())
    }
}

#[cfg(unix)]
impl From<UnixStream> for Dialed {
    fn from(socket: UnixStream) -> Self {
        let hangup = kj_rs_io::when_write_disconnected(&socket);
        Self::with_hangup(socket, hangup.err_into().boxed())
    }
}

impl AsyncRead for Dialed {
    fn poll_read(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &mut ReadBuf<'_>,
    ) -> Poll<io::Result<()>> {
        Pin::new(&mut *self.get_mut().io).poll_read(cx, buf)
    }
}

impl AsyncWrite for Dialed {
    fn poll_write(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &[u8],
    ) -> Poll<io::Result<usize>> {
        Pin::new(&mut *self.get_mut().io).poll_write(cx, buf)
    }

    fn poll_flush(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<io::Result<()>> {
        Pin::new(&mut *self.get_mut().io).poll_flush(cx)
    }

    fn poll_shutdown(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<io::Result<()>> {
        Pin::new(&mut *self.get_mut().io).poll_shutdown(cx)
    }
}

impl Connection for Dialed {
    fn connected(&self) -> Connected {
        // hyper-util sends absolute-form request targets on a connection to a proxy, and puts
        // the signal in the extensions of every response the connection carries.
        Connected::new()
            .proxy(self.peer == Peer::Proxy)
            .extra(self.hangup.clone())
    }
}

type DialFuture = Pin<Box<dyn Future<Output = io::Result<Dialed>> + Send>>;
type Dialer = dyn Fn() -> DialFuture + Send + Sync;
type Connect = dyn Fn(String, u16) -> DialFuture + Send + Sync;

/// Where the pool's connections come from.
#[derive(Clone)]
enum Connector {
    /// One peer, however its dialer reaches it; the request URL is sent as given.
    Fixed(Arc<Dialer>, Peer),
    /// The authority each request's URL names, over TCP, with TLS for `https`.
    Internet(Arc<Internet>),
}

struct Internet {
    tls: Option<Arc<rustls::ClientConfig>>,
    /// A connection to a host and port.
    connect: Box<Connect>,
}

impl Internet {
    /// A connection to `host`, with a TLS handshake when `tls`.
    async fn dial(&self, host: &str, port: u16, tls: bool) -> io::Result<Dialed> {
        let stream = (self.connect)(host.to_owned(), port).await?;
        if !tls {
            return Ok(stream);
        }
        let config = self
            .tls
            .clone()
            .ok_or_else(|| io::Error::other("this client has no TLS configuration"))?;
        stream.tls(config, host).await
    }
}

/// The usual `connect` of [`Client::internet`]: resolves `host` and connects to the first address
/// that `allow` admits and that accepts.
///
/// When none accepts, the error is the last connection failure, else `PermissionDenied` if
/// `allow` refused an address, else `NotFound`.
pub async fn connect_allowed(
    host: &str,
    port: u16,
    allow: impl Fn(&SocketAddr) -> bool,
) -> io::Result<TcpStream> {
    let mut denied = false;
    let mut last = None;
    for addr in lookup_host((host, port)).await? {
        if !allow(&addr) {
            denied = true;
            continue;
        }
        match TcpStream::connect(addr).await {
            Ok(stream) => {
                let _ = stream.set_nodelay(true);
                return Ok(stream);
            }
            Err(e) => last = Some(e),
        }
    }
    Err(last.unwrap_or_else(|| {
        if denied {
            io::Error::new(
                io::ErrorKind::PermissionDenied,
                format!("connecting to {host}:{port} is not allowed"),
            )
        } else {
            io::Error::new(
                io::ErrorKind::NotFound,
                format!("{host} did not resolve to any address"),
            )
        }
    }))
}

impl tower_service::Service<http::Uri> for Connector {
    type Response = TokioIo<Dialed>;
    type Error = io::Error;
    type Future = Pin<Box<dyn Future<Output = io::Result<TokioIo<Dialed>>> + Send>>;

    fn poll_ready(&mut self, _cx: &mut Context<'_>) -> Poll<io::Result<()>> {
        Poll::Ready(Ok(()))
    }

    fn call(&mut self, uri: http::Uri) -> Self::Future {
        match self {
            Self::Fixed(dial, peer) => {
                let dialing = dial();
                let peer = *peer;
                Box::pin(async move {
                    let dialed = dialing.await?;
                    Ok(TokioIo::new(Dialed { peer, ..dialed }))
                })
            }
            Self::Internet(internet) => {
                let internet = Arc::clone(internet);
                Box::pin(async move {
                    let tls = uri.scheme() == Some(&Scheme::HTTPS);
                    let host = uri
                        .host()
                        .ok_or_else(|| io::Error::other("the request URL names no host"))?;
                    let port = uri.port_u16().unwrap_or(if tls { 443 } else { 80 });
                    internet
                        .dial(host.trim_matches(['[', ']']), port, tls)
                        .await
                        .map(TokioIo::new)
                })
            }
        }
    }
}

// =======================================================================================
// Client

/// The authority an origin peer's requests are pooled under; it never goes on the wire.
const FIXED_AUTHORITY: &str = "fixed.invalid";

/// See the module docs. Requests are made through `kj::http::Service`; clone it to share the
/// pool.
#[derive(Clone)]
pub struct Client<'t> {
    inner: legacy::Client<Connector, ChannelBody>,
    connector: Connector,
    table: &'t HeaderTable,
    settings: Rc<ClientSettings>,
}

impl<'t> Client<'t> {
    fn build(table: &'t HeaderTable, settings: ClientSettings, connector: Connector) -> Self {
        let inner = legacy::Client::builder(TokioExecutor::new())
            .pool_idle_timeout(settings.idle_timeout)
            .http1_title_case_headers(true)
            .http1_preserve_header_case(true)
            .http1_max_headers(MAX_HEADERS)
            // The `Host` header is the application's (kj sends what it is given); the "internet"
            // client sets it from the URL itself, as kj's does.
            .set_host(false)
            .build(connector.clone());
        Self {
            inner,
            connector,
            table,
            settings: Rc::new(settings),
        }
    }

    /// A client of one peer, every connection dialed by `dial` (a socket, or a [`Dialed`]: TLS,
    /// an in-process pipe...). An origin is asked for paths, sent as given; an HTTP proxy for
    /// absolute URLs, sent whole. `table` builds the response headers.
    pub fn new<F, Fut, IO>(
        table: &'t HeaderTable,
        settings: ClientSettings,
        peer: Peer,
        dial: F,
    ) -> Self
    where
        F: Fn() -> Fut + Send + Sync + 'static,
        Fut: Future<Output = io::Result<IO>> + Send + 'static,
        IO: Into<Dialed>,
    {
        let dialer: Arc<Dialer> = Arc::new(move || {
            let dialing = dial();
            Box::pin(async move { dialing.await.map(Into::into) })
        });
        Self::build(table, settings, Connector::Fixed(dialer, peer))
    }

    /// A client of whatever authority a request's (absolute) URL names, over the connection
    /// `connect` makes to its host and port (TCP through [`connect_allowed`], usually), with
    /// `tls` for `https` URLs.
    pub fn internet<F, Fut, IO>(
        table: &'t HeaderTable,
        settings: ClientSettings,
        tls: Option<Arc<rustls::ClientConfig>>,
        connect: F,
    ) -> Self
    where
        F: Fn(String, u16) -> Fut + Send + Sync + 'static,
        Fut: Future<Output = io::Result<IO>> + Send + 'static,
        IO: Into<Dialed>,
    {
        let connect: Box<Connect> = Box::new(move |host, port| {
            let connecting = connect(host, port);
            Box::pin(async move { connecting.await.map(Into::into) })
        });
        let internet = Arc::new(Internet { tls, connect });
        Self::build(table, settings, Connector::Internet(internet))
    }

    fn websocket_errors(&self) -> KjMaybe<&WebSocketErrorHandler> {
        self.settings.websocket_errors.as_deref().into()
    }

    /// The request URI and, for the "internet" client, the `Host` kj's client sets from it.
    fn target(&self, url: &[u8]) -> Result<(http::Uri, Option<http::HeaderValue>)> {
        let url = from_utf8(url).map_err(failed)?;
        match self.connector {
            Connector::Fixed(_, Peer::Origin) => {
                let uri = format!("http://{FIXED_AUTHORITY}{url}")
                    .parse()
                    .map_err(failed)?;
                Ok((uri, None))
            }
            Connector::Fixed(_, Peer::Proxy) => {
                let uri: http::Uri = url.parse().map_err(failed)?;
                if uri.scheme().is_none() || uri.authority().is_none() {
                    return Err(failed(format!("a proxy is asked for absolute URLs: {url}")));
                }
                Ok((uri, None))
            }
            Connector::Internet(_) => {
                let uri: http::Uri = url.parse().map_err(failed)?;
                let host = uri
                    .authority()
                    .ok_or_else(|| failed("the request URL names no host"))?
                    .as_str();
                let host = http::HeaderValue::from_str(host).map_err(failed)?;
                Ok((uri, Some(host)))
            }
        }
    }

    fn request_of(
        method: http::Method,
        uri: http::Uri,
        head: Head,
        body: ChannelBody,
    ) -> http::Request<ChannelBody> {
        let mut parts = http::Request::new(()).into_parts().0;
        parts.method = method;
        parts.uri = uri;
        head.apply(&mut parts.headers, &mut parts.extensions);
        http::Request::from_parts(parts, body)
    }

    async fn send(&self, request: http::Request<ChannelBody>) -> Result<http::Response<Incoming>> {
        self.inner
            .request(request)
            .await
            .map_err(|e| client_error(&e))
    }

    /// Writes a response's head and body into the kj response.
    async fn relay(
        &self,
        response: http::Response<Incoming>,
        into: ServiceResponse<'_>,
    ) -> Result<()> {
        let (parts, mut body) = response.into_parts();
        let headers = HeaderBlock::new(&parts.headers, &parts.extensions).to_kj(self.table)?;
        let length = http_body::Body::size_hint(&body).exact();
        let mut out = into.send(
            u32::from(parts.status.as_u16()),
            &reason(&parts),
            HeadersRef::from(&*headers),
            length,
        )?;
        while let Some(frame) = next_frame(&mut body).await {
            let frame = frame.map_err(|e| disconnected(format!("HTTP body: {e}")))?;
            if let Ok(data) = frame.into_data() {
                out.write(&data).await?;
            }
        }
        Ok(())
    }

    /// A plain-text response the client answers with itself (a failed WebSocket handshake, as
    /// kj's default `HttpClientErrorHandler` answers it).
    async fn answer(
        &self,
        into: ServiceResponse<'_>,
        status: u32,
        status_text: &str,
        message: &str,
    ) -> Result<()> {
        let headers = Headers::new(self.table);
        let mut out = into.send(status, status_text, &headers, Some(message.len() as u64))?;
        out.write(message.as_bytes()).await
    }

    /// A WebSocket request: kj's client handshake and checks, then the two sockets pumped into
    /// each other (kj's client adapter).
    async fn websocket(
        &self,
        uri: http::Uri,
        mut head: Head,
        headers: HeadersRef<'_>,
        into: ServiceResponse<'_>,
    ) -> Result<()> {
        let mode = self.settings.websocket_compression;
        let offer = websocket_offer(headers.as_ffi(), mode);
        let key = client_key();
        // As kj: the handshake's connection-level headers are the client's own, so no framing
        // of a body goes with it, whatever the application's headers said.
        head.claim(Protocol::WebSocket);
        head.set(
            Builtin::Connection,
            http::HeaderValue::from_static("Upgrade"),
        );
        head.set(
            Builtin::Upgrade,
            http::HeaderValue::from_static("websocket"),
        );
        head.set(
            Builtin::SecWebSocketKey,
            http::HeaderValue::from_str(&key).map_err(failed)?,
        );
        head.set(
            Builtin::SecWebSocketVersion,
            http::HeaderValue::from_static("13"),
        );
        if !offer.extensions.is_empty() {
            head.set(
                Builtin::SecWebSocketExtensions,
                http::HeaderValue::from_str(&offer.extensions).map_err(failed)?,
            );
        }
        let request = Self::request_of(http::Method::GET, uri, head, ChannelBody::empty());
        let response = self.send(request).await?;
        if response.status() != http::StatusCode::SWITCHING_PROTOCOLS {
            return self.relay(response, into).await;
        }
        if let Some(what) = handshake_error(response.headers(), &key) {
            return self.answer(into, 502, "Bad Gateway", &what).await;
        }
        let (parts, _) = response.into_parts();
        let hangup = parts.extensions.get::<Shared<Hangup>>().cloned();
        let response_headers =
            HeaderBlock::new(&parts.headers, &parts.extensions).to_kj(self.table)?;
        let upgrade = parts
            .extensions
            .get::<OnUpgrade>()
            .is_some()
            .then(|| upgrade::on(http::Response::from_parts(parts, ())));
        let Some(upgrade) = upgrade else {
            return self
                .answer(into, 502, "Bad Gateway", "the connection was not upgraded")
                .await;
        };
        let agreed = match websocket_agreement(&offer, &response_headers, mode) {
            Ok(agreed) => agreed,
            Err(e) => return self.answer(into, 502, "Bad Gateway", &e.to_string()).await,
        };
        let upgraded = upgrade.await.map_err(|e| client_error(&e))?;
        let stream = into_kj_stream_with(TokioIo::new(upgraded), hangup);
        let ours = new_client_websocket(stream, &agreed, self.websocket_errors());
        let theirs = into.accept_websocket(HeadersRef::from(&*response_headers))?;
        pump_websockets(ours, theirs).await
    }
}

async fn next_frame(
    body: &mut Incoming,
) -> Option<Result<http_body::Frame<bytes::Bytes>, hyper::Error>> {
    poll_fn(|cx| http_body::Body::poll_frame(Pin::new(&mut *body), cx)).await
}

/// The reason phrase a response came with, or the status's canonical one.
fn reason(parts: &Parts) -> String {
    parts
        .extensions
        .get::<ReasonPhrase>()
        .map(|r| String::from_utf8_lossy(r.as_bytes()).into_owned())
        .or_else(|| parts.status.canonical_reason().map(str::to_owned))
        .unwrap_or_default()
}

/// kj's checks of a server's WebSocket handshake, with kj's messages.
fn handshake_error(headers: &http::HeaderMap, key: &str) -> Option<String> {
    let upgrade = headers.get(UPGRADE).map(http::HeaderValue::as_bytes);
    if !upgrade.is_some_and(|u| u.eq_ignore_ascii_case(b"websocket")) {
        return Some(match upgrade {
            Some(actual) => format!(
                "Server failed WebSocket handshake: incorrect Upgrade header: expected \
                 'websocket', got '{}'.",
                String::from_utf8_lossy(actual)
            ),
            None => "Server failed WebSocket handshake: missing Upgrade header.".to_owned(),
        });
    }
    let expected = accept_key(key.as_bytes());
    match headers.get(SEC_WEBSOCKET_ACCEPT) {
        Some(actual) if actual.as_bytes() == expected.as_bytes() => None,
        Some(actual) => Some(format!(
            "Server failed WebSocket handshake: incorrect Sec-WebSocket-Accept header: expected \
             '{expected}', got '{}'.",
            String::from_utf8_lossy(actual.as_bytes())
        )),
        None => Some(
            "Server failed WebSocket handshake: missing Sec-WebSocket-Accept header.".to_owned(),
        ),
    }
}

/// Whether kj considers the request a WebSocket handshake (`kj::HttpHeaders::isWebSocket()`).
fn is_websocket(headers: HeadersRef<'_>) -> bool {
    headers
        .get(HeaderId::UPGRADE)
        .is_some_and(|u| u.eq_ignore_ascii_case(b"websocket"))
}

/// Pumps a kj request body into a hyper body until it ends; a failed read fails the request.
async fn pump_body(mut body: Pin<&mut AsyncInputStream>, sink: BodySink, abort: BodyAbort) {
    let mut buf = vec![0; 8192];
    loop {
        match body.as_mut().try_read(&mut buf, 1).await {
            Ok(0) => return,
            Ok(n) => {
                if sink.write(&buf[..n]).await.is_err() {
                    return;
                }
            }
            Err(_) => {
                abort.abort();
                return;
            }
        }
    }
}

#[async_trait::async_trait(?Send)]
impl Service for Client<'_> {
    async fn request<'a>(
        &'a mut self,
        method: Method,
        url: &'a [u8],
        headers: HeadersRef<'a>,
        mut request_body: Pin<&'a mut AsyncInputStream>,
        response: ServiceResponse<'a>,
    ) -> Result<()> {
        let (uri, host) = self.target(url)?;
        let mut head = Head::new(headers.as_ffi());
        if let Some(host) = host {
            head.set(Builtin::Host, host);
        }
        if is_websocket(headers) {
            return self.websocket(uri, head, headers, response).await;
        }
        head.claim(Protocol::Message);
        let method = http::Method::from_bytes(format!("{method:?}").as_bytes()).map_err(failed)?;
        let length = request_body.as_mut().try_get_length();
        // As kj: an empty GET or HEAD carries no Content-Length. One of unknown length is not
        // framed either: hyper sends it without a body.
        let framed = !(matches!(method, http::Method::GET | http::Method::HEAD)
            && matches!(length, None | Some(0)));
        if framed {
            head.frame(length);
        }
        let (sink, abort, body) = channel(length.filter(|_| framed));
        let request = Self::request_of(method, uri, head, body);
        // The body is pumped for as long as the exchange lasts: a server that answers before
        // reading all of it ends the pump with the exchange, as kj's client adapter does.
        let exchange = std::pin::pin!(async {
            let received = self.send(request).await?;
            self.relay(received, response).await
        });
        let pump = std::pin::pin!(pump_body(request_body, sink, abort));
        match select(exchange, pump).await {
            Either::Left((result, _)) => result,
            Either::Right(((), exchange)) => exchange.await,
        }
    }

    fn connect<'a, 'b>(
        &'a mut self,
        host: &'a [u8],
        headers: HeadersRef<'a>,
        connection: Pin<&'a mut AsyncIoStream>,
        response: ConnectResponse<'a>,
        settings: ConnectSettings<'a>,
    ) -> Pin<Box<dyn Future<Output = Result<()>> + 'b>>
    where
        'a: 'b,
        Self: 'b,
    {
        // The starter is an output of the call itself (kj fills it before returning), so it is
        // installed here rather than when the future first runs.
        let use_tls = settings.use_tls;
        let starter: Option<Pin<&mut TlsStarterCallback>> = settings.tls_starter.into();
        let upgrade = match (&self.connector, starter) {
            (Connector::Internet(internet), Some(starter)) if !use_tls => {
                internet.tls.clone().and_then(|config| {
                    let (host, _) = host_port(from_utf8(host).ok()?).ok()?;
                    let io = StartTlsIo::new(config, host);
                    tls_starter_set(starter, Box::new(io.starter()));
                    Some(io)
                })
            }
            _ => None,
        };
        Box::pin(self.connect_with(host, headers, connection, response, use_tls, upgrade))
    }
}

impl Client<'_> {
    async fn connect_with(
        &self,
        host: &[u8],
        headers: HeadersRef<'_>,
        mut connection: Pin<&mut AsyncIoStream>,
        response: ConnectResponse<'_>,
        use_tls: bool,
        upgrade: Option<StartTlsIo>,
    ) -> Result<()> {
        if is_websocket(headers) {
            return Err(failed(
                "WebSocket upgrade headers are not permitted in a connect.",
            ));
        }
        let host_str = from_utf8(host).map_err(failed)?;
        match &self.connector {
            // As kj's network client: a plain connection to the host is the tunnel, TLS from the
            // start or from when the starter says.
            Connector::Internet(internet) => {
                let (host, port) = host_port(host_str)?;
                let io = |e: io::Error| io_kj_error(&e);
                let tunnel = if use_tls {
                    let dialed = internet.dial(&host, port, true).await.map_err(io)?;
                    dialed.into_kj()
                } else {
                    let stream = (internet.connect)(host, port).await.map_err(io)?;
                    match upgrade {
                        Some(upgradable) => {
                            let hangup = stream.hangup.clone();
                            upgradable.dialed(stream);
                            into_kj_stream_with(upgradable, Some(hangup))
                        }
                        None => stream.into_kj(),
                    }
                };
                response.accept(200, "OK", &Headers::new(self.table))?;
                Ok(pump_tunnel(connection.as_mut(), tunnel).await?)
            }
            // As kj's client over one address: an HTTP CONNECT to the peer.
            Connector::Fixed(..) => {
                if use_tls {
                    return Err(KjError::new(
                        KjExceptionType::Unimplemented,
                        "This HttpClient does not support TLS.".to_owned(),
                    ));
                }
                let uri: http::Uri = host_str.parse().map_err(failed)?;
                let mut head = Head::new(headers.as_ffi());
                head.claim(Protocol::Message);
                let request =
                    Self::request_of(http::Method::CONNECT, uri, head, ChannelBody::empty());
                let received = self.send(request).await?;
                if !received.status().is_success() {
                    let (parts, mut body) = received.into_parts();
                    let headers =
                        HeaderBlock::new(&parts.headers, &parts.extensions).to_kj(self.table)?;
                    let mut out = response.reject(
                        u32::from(parts.status.as_u16()),
                        &reason(&parts),
                        HeadersRef::from(&*headers),
                        http_body::Body::size_hint(&body).exact(),
                    )?;
                    while let Some(frame) = next_frame(&mut body).await {
                        let frame = frame.map_err(|e| disconnected(format!("HTTP body: {e}")))?;
                        if let Ok(data) = frame.into_data() {
                            out.write(&data).await?;
                        }
                    }
                    return Ok(());
                }
                let (parts, _) = received.into_parts();
                let hangup = parts.extensions.get::<Shared<Hangup>>().cloned();
                let headers =
                    HeaderBlock::new(&parts.headers, &parts.extensions).to_kj(self.table)?;
                response.accept(
                    u32::from(parts.status.as_u16()),
                    &reason(&parts),
                    HeadersRef::from(&*headers),
                )?;
                let upgraded = upgrade::on(http::Response::from_parts(parts, ()))
                    .await
                    .map_err(|e| client_error(&e))?;
                let tunnel = into_kj_stream_with(TokioIo::new(upgraded), hangup);
                Ok(pump_tunnel(connection.as_mut(), tunnel).await?)
            }
        }
    }

    /// A tunnel to `host` through the peer of a client of one peer ([`Client::new`]): an HTTP
    /// CONNECT, which the peer must accept.
    ///
    /// # Errors
    ///
    /// A peer that cannot be reached or that refuses, and a client of no one peer.
    pub async fn tunnel(&self, host: &str) -> Result<KjOwn<AsyncIoStream>> {
        let Connector::Fixed(..) = self.connector else {
            return Err(failed("this client has no peer to tunnel through"));
        };
        let uri: http::Uri = host.parse().map_err(failed)?;
        let request = Self::request_of(
            http::Method::CONNECT,
            uri,
            Head::empty(),
            ChannelBody::empty(),
        );
        let received = self.send(request).await?;
        if !received.status().is_success() {
            let status = received.status();
            return Err(failed(format!("CONNECT to {host} failed: {status}")));
        }
        let hangup = received.extensions().get::<Shared<Hangup>>().cloned();
        let upgraded = upgrade::on(received).await.map_err(|e| client_error(&e))?;
        Ok(into_kj_stream_with(TokioIo::new(upgraded), hangup))
    }
}

/// The host and port a CONNECT names (`host:port`, brackets around an IPv6 host removed).
fn host_port(host: &str) -> Result<(String, u16)> {
    let authority: Authority = host.parse().map_err(failed)?;
    let port = authority
        .port_u16()
        .ok_or_else(|| failed(format!("CONNECT host has no port: {host}")))?;
    Ok((authority.host().trim_matches(['[', ']']).to_owned(), port))
}

#[cfg(test)]
#[path = "client-test.rs"]
mod tests;
