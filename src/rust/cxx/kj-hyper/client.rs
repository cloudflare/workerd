//! A pooled HTTP/1.1 client as a `kj::http::Service`, so C++ (a `WorkerInterface` for an
//! external service, say) makes outbound requests through it.
//!
//! The pool, keep-alive, idle eviction, upgrades and retries of stale connections are
//! `hyper-util`'s legacy client's; this module supplies the connector and the kj adaptation:
//! request bodies pumped from kj streams, responses (bodies, `WebSocket`s, tunnels) written back
//! into the kj `Response`. Connection tasks run on the current tokio runtime, so the transports a
//! dialer produces must be `Send`.

use std::future::Future;
use std::net::SocketAddr;
use std::pin::Pin;
use std::rc::Rc;
use std::sync::Arc;
use std::task::Context;
use std::task::Poll;
use std::time::Duration;

use cxx::KjError;
use cxx::KjExceptionType;
use futures::future::Either;
use hyper::body::Incoming;
use hyper_util::client::legacy;
use hyper_util::client::legacy::connect::Connected;
use hyper_util::client::legacy::connect::Connection;
use hyper_util::rt::TokioExecutor;
use hyper_util::rt::TokioIo;
use kj::http::ConnectResponse;
use kj::http::ConnectSettings;
use kj::http::HeaderTable;
use kj::http::Headers;
use kj::http::HeadersRef;
use kj::http::Method;
use kj::http::ServiceResponse;
use kj::io::AsyncInputStream;
use kj::io::AsyncIoStream;
use kj_rs::KjMaybe;
use kj_rs::KjOwn;
use tokio::io::AsyncRead;
use tokio::io::AsyncWrite;
use tokio::io::ReadBuf;

use crate::body::ChannelBody;
use crate::body::Head;
use crate::body::HeaderBlock;
use crate::ffi::TlsStarterCallback;
use crate::ffi::WebSocketCompression;
use crate::ffi::WebSocketErrorHandler;
use crate::starttls::StartTlsIo;

fn failed(what: impl std::fmt::Display) -> KjError {
    KjError::new(KjExceptionType::Failed, what.to_string())
}

fn disconnected(what: impl std::fmt::Display) -> KjError {
    KjError::new(KjExceptionType::Disconnected, what.to_string())
}

/// A request's failure: the I/O error behind it when there is one, else DISCONNECTED (hyper
/// reports a connection that went away in several shapes).
fn client_error(error: &(dyn std::error::Error + 'static)) -> KjError {
    let mut source = Some(error);
    while let Some(e) = source {
        if let Some(io) = e.downcast_ref::<std::io::Error>() {
            return crate::io::io_kj_error(io);
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

/// A dialed transport: any `Send` tokio stream.
pub struct Dialed(Box<dyn Io + Send>);

trait Io: AsyncRead + AsyncWrite + Unpin {}
impl<T: AsyncRead + AsyncWrite + Unpin + ?Sized> Io for T {}

impl Dialed {
    pub fn new(io: impl AsyncRead + AsyncWrite + Unpin + Send + 'static) -> Self {
        Self(Box::new(io))
    }
}

impl AsyncRead for Dialed {
    fn poll_read(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &mut ReadBuf<'_>,
    ) -> Poll<std::io::Result<()>> {
        Pin::new(&mut *self.get_mut().0).poll_read(cx, buf)
    }
}

impl AsyncWrite for Dialed {
    fn poll_write(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &[u8],
    ) -> Poll<std::io::Result<usize>> {
        Pin::new(&mut *self.get_mut().0).poll_write(cx, buf)
    }

    fn poll_flush(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<std::io::Result<()>> {
        Pin::new(&mut *self.get_mut().0).poll_flush(cx)
    }

    fn poll_shutdown(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<std::io::Result<()>> {
        Pin::new(&mut *self.get_mut().0).poll_shutdown(cx)
    }
}

impl Connection for Dialed {
    fn connected(&self) -> Connected {
        Connected::new()
    }
}

type DialFuture = Pin<Box<dyn Future<Output = std::io::Result<Dialed>> + Send>>;
type Dialer = dyn Fn() -> DialFuture + Send + Sync;
type PeerFilter = dyn Fn(&SocketAddr) -> bool + Send + Sync;

/// Where the pool's connections come from.
#[derive(Clone)]
enum Connector {
    /// One peer, however its dialer reaches it; the request URL is the path sent.
    Fixed(Arc<Dialer>),
    /// The authority each request's URL names, over TCP, with TLS for `https`.
    Internet(Arc<Internet>),
}

struct Internet {
    tls: Option<Arc<rustls::ClientConfig>>,
    allow: Box<PeerFilter>,
}

impl Internet {
    /// Resolves `host` and connects to the first allowed address that accepts.
    async fn dial_tcp(&self, host: &str, port: u16) -> std::io::Result<tokio::net::TcpStream> {
        let mut denied = false;
        let mut last = None;
        for addr in tokio::net::lookup_host((host, port)).await? {
            if !(self.allow)(&addr) {
                denied = true;
                continue;
            }
            match tokio::net::TcpStream::connect(addr).await {
                Ok(stream) => {
                    let _ = stream.set_nodelay(true);
                    return Ok(stream);
                }
                Err(e) => last = Some(e),
            }
        }
        Err(last.unwrap_or_else(|| {
            if denied {
                std::io::Error::new(
                    std::io::ErrorKind::PermissionDenied,
                    format!("connecting to {host}:{port} is not allowed"),
                )
            } else {
                std::io::Error::new(
                    std::io::ErrorKind::NotFound,
                    format!("{host} did not resolve to any address"),
                )
            }
        }))
    }

    /// [`Self::dial_tcp`], with a TLS handshake to `host` when `tls`.
    async fn dial(&self, host: &str, port: u16, tls: bool) -> std::io::Result<Dialed> {
        let stream = self.dial_tcp(host, port).await?;
        if !tls {
            return Ok(Dialed::new(stream));
        }
        let config = self
            .tls
            .clone()
            .ok_or_else(|| std::io::Error::other("this client has no TLS configuration"))?;
        let stream = crate::tls::connect(stream, config, host)
            .await
            .map_err(|e| std::io::Error::other(e.description().to_owned()))?;
        Ok(Dialed::new(stream))
    }
}

impl tower_service::Service<http::Uri> for Connector {
    type Response = TokioIo<Dialed>;
    type Error = std::io::Error;
    type Future = Pin<Box<dyn Future<Output = std::io::Result<TokioIo<Dialed>>> + Send>>;

    fn poll_ready(&mut self, _cx: &mut Context<'_>) -> Poll<std::io::Result<()>> {
        Poll::Ready(Ok(()))
    }

    fn call(&mut self, uri: http::Uri) -> Self::Future {
        match self {
            Self::Fixed(dial) => {
                let dialing = dial();
                Box::pin(async move { dialing.await.map(TokioIo::new) })
            }
            Self::Internet(internet) => {
                let internet = Arc::clone(internet);
                Box::pin(async move {
                    let tls = uri.scheme() == Some(&http::uri::Scheme::HTTPS);
                    let host = uri
                        .host()
                        .ok_or_else(|| std::io::Error::other("the request URL names no host"))?;
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

/// The authority a fixed connector's requests are pooled under; it never goes on the wire.
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
            .http1_max_headers(crate::body::MAX_HEADERS)
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

    /// A client of one peer, every connection dialed by `dial` (plain TCP, TLS, a Unix socket,
    /// an in-process pipe...); request URLs are sent as given. `table` builds the response
    /// headers.
    pub fn new<F, Fut, IO>(table: &'t HeaderTable, settings: ClientSettings, dial: F) -> Self
    where
        F: Fn() -> Fut + Send + Sync + 'static,
        Fut: Future<Output = std::io::Result<IO>> + Send + 'static,
        IO: AsyncRead + AsyncWrite + Unpin + Send + 'static,
    {
        let dialer: Arc<Dialer> = Arc::new(move || {
            let dialing = dial();
            Box::pin(async move { dialing.await.map(Dialed::new) })
        });
        Self::build(table, settings, Connector::Fixed(dialer))
    }

    /// A client of whatever authority a request's (absolute) URL names, over TCP, with `tls`
    /// for `https` URLs; `allow` decides which resolved addresses may be connected to.
    pub fn internet(
        table: &'t HeaderTable,
        settings: ClientSettings,
        tls: Option<Arc<rustls::ClientConfig>>,
        allow: impl Fn(&SocketAddr) -> bool + Send + Sync + 'static,
    ) -> Self {
        let internet = Arc::new(Internet {
            tls,
            allow: Box::new(allow),
        });
        Self::build(table, settings, Connector::Internet(internet))
    }

    fn websocket_errors(&self) -> KjMaybe<&WebSocketErrorHandler> {
        self.settings.websocket_errors.as_deref().into()
    }

    /// The request URI and, for the "internet" client, the `Host` kj's client sets from it.
    fn target(&self, url: &[u8]) -> crate::Result<(http::Uri, Option<http::HeaderValue>)> {
        let url = std::str::from_utf8(url).map_err(failed)?;
        match self.connector {
            Connector::Fixed(_) => {
                let uri = format!("http://{FIXED_AUTHORITY}{url}")
                    .parse()
                    .map_err(failed)?;
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

    async fn send(
        &self,
        request: http::Request<ChannelBody>,
    ) -> crate::Result<http::Response<Incoming>> {
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
    ) -> crate::Result<()> {
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
    ) -> crate::Result<()> {
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
    ) -> crate::Result<()> {
        let mode = self.settings.websocket_compression;
        let offer = crate::ffi::websocket_offer(headers.as_ffi(), mode);
        let key = crate::handshake::client_key();
        head.remove(&http::header::SEC_WEBSOCKET_EXTENSIONS);
        // The handshake carries no body, so no framing of one goes with it, whatever the
        // application's headers said.
        head.remove(&http::header::CONTENT_LENGTH);
        head.remove(&http::header::TRANSFER_ENCODING);
        if !offer.extensions.is_empty() {
            head.set(
                http::header::SEC_WEBSOCKET_EXTENSIONS,
                "Sec-WebSocket-Extensions",
                http::HeaderValue::from_str(&offer.extensions).map_err(failed)?,
            );
        }
        head.set(
            http::header::CONNECTION,
            "Connection",
            http::HeaderValue::from_static("Upgrade"),
        );
        head.set(
            http::header::UPGRADE,
            "Upgrade",
            http::HeaderValue::from_static("websocket"),
        );
        head.set(
            http::header::SEC_WEBSOCKET_VERSION,
            "Sec-WebSocket-Version",
            http::HeaderValue::from_static("13"),
        );
        head.set(
            http::header::SEC_WEBSOCKET_KEY,
            "Sec-WebSocket-Key",
            http::HeaderValue::from_str(&key).map_err(failed)?,
        );
        let request = Self::request_of(http::Method::GET, uri, head, ChannelBody::empty());
        let response = self.send(request).await?;
        if response.status() != http::StatusCode::SWITCHING_PROTOCOLS {
            return self.relay(response, into).await;
        }
        if let Some(what) = handshake_error(response.headers(), &key) {
            return self.answer(into, 502, "Bad Gateway", &what).await;
        }
        let (parts, _) = response.into_parts();
        let response_headers =
            HeaderBlock::new(&parts.headers, &parts.extensions).to_kj(self.table)?;
        let upgrade = parts
            .extensions
            .get::<hyper::upgrade::OnUpgrade>()
            .is_some()
            .then(|| hyper::upgrade::on(http::Response::from_parts(parts, ())));
        let Some(upgrade) = upgrade else {
            return self
                .answer(into, 502, "Bad Gateway", "the connection was not upgraded")
                .await;
        };
        let agreed = match crate::ffi::websocket_agreement(&offer, &response_headers, mode) {
            Ok(agreed) => agreed,
            Err(e) => return self.answer(into, 502, "Bad Gateway", &e.to_string()).await,
        };
        let upgraded = upgrade.await.map_err(|e| client_error(&e))?;
        let stream = crate::io::into_kj_stream(TokioIo::new(upgraded));
        let ours = crate::ffi::new_client_websocket(stream, &agreed, self.websocket_errors());
        let theirs = into.accept_websocket(HeadersRef::from(&*response_headers))?;
        crate::ffi::pump_websockets(ours, theirs).await
    }
}

async fn next_frame(
    body: &mut Incoming,
) -> Option<Result<http_body::Frame<bytes::Bytes>, hyper::Error>> {
    std::future::poll_fn(|cx| http_body::Body::poll_frame(Pin::new(&mut *body), cx)).await
}

/// The reason phrase a response came with, or the status's canonical one.
fn reason(parts: &http::response::Parts) -> String {
    parts
        .extensions
        .get::<hyper::ext::ReasonPhrase>()
        .map(|r| String::from_utf8_lossy(r.as_bytes()).into_owned())
        .or_else(|| parts.status.canonical_reason().map(str::to_owned))
        .unwrap_or_default()
}

/// kj's checks of a server's WebSocket handshake, with kj's messages.
fn handshake_error(headers: &http::HeaderMap, key: &str) -> Option<String> {
    let upgrade = headers
        .get(http::header::UPGRADE)
        .map(http::HeaderValue::as_bytes);
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
    let expected = crate::handshake::accept_key(key.as_bytes());
    match headers.get(http::header::SEC_WEBSOCKET_ACCEPT) {
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
        .get(kj::http::HeaderId::UPGRADE)
        .is_some_and(|u| u.eq_ignore_ascii_case(b"websocket"))
}

/// Pumps a kj request body into a hyper body until it ends; a failed read fails the request.
async fn pump_body(
    mut body: Pin<&mut AsyncInputStream>,
    sink: crate::body::BodySink,
    abort: crate::body::BodyAbort,
) {
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
impl kj::http::Service for Client<'_> {
    async fn request<'a>(
        &'a mut self,
        method: Method,
        url: &'a [u8],
        headers: HeadersRef<'a>,
        mut request_body: Pin<&'a mut AsyncInputStream>,
        response: ServiceResponse<'a>,
    ) -> crate::Result<()> {
        let (uri, host) = self.target(url)?;
        let mut head = Head::new(headers.as_ffi());
        if let Some(host) = host {
            head.set(http::header::HOST, "Host", host);
        }
        if is_websocket(headers) {
            return self.websocket(uri, head, headers, response).await;
        }
        let method = http::Method::from_bytes(format!("{method:?}").as_bytes()).map_err(failed)?;
        let length = request_body.as_mut().try_get_length();
        // As kj: an empty GET or HEAD carries no Content-Length.
        let declared =
            length.filter(|&n| n != 0 || !matches!(method, http::Method::GET | http::Method::HEAD));
        let (sink, abort, body) = crate::body::channel(declared);
        let request = Self::request_of(method, uri, head.with_length(declared), body);
        // The body is pumped for as long as the exchange lasts: a server that answers before
        // reading all of it ends the pump with the exchange, as kj's client adapter does.
        let exchange = std::pin::pin!(async {
            let received = self.send(request).await?;
            self.relay(received, response).await
        });
        let pump = std::pin::pin!(pump_body(request_body, sink, abort));
        match futures::future::select(exchange, pump).await {
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
    ) -> Pin<Box<dyn Future<Output = crate::Result<()>> + 'b>>
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
                    let (host, _) = host_port(std::str::from_utf8(host).ok()?).ok()?;
                    let io = StartTlsIo::new(config, host);
                    crate::ffi::tls_starter_set(starter, Box::new(io.starter()));
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
    ) -> crate::Result<()> {
        if is_websocket(headers) {
            return Err(failed(
                "WebSocket upgrade headers are not permitted in a connect.",
            ));
        }
        let host_str = std::str::from_utf8(host).map_err(failed)?;
        match &self.connector {
            // As kj's network client: a plain connection to the host is the tunnel, TLS from the
            // start or from when the starter says.
            Connector::Internet(internet) => {
                let (host, port) = host_port(host_str)?;
                let io = |e: std::io::Error| crate::io::io_kj_error(&e);
                let tunnel = if use_tls {
                    let dialed = internet.dial(&host, port, true).await.map_err(io)?;
                    crate::io::into_kj_stream(dialed)
                } else {
                    let stream = internet.dial_tcp(&host, port).await.map_err(io)?;
                    match upgrade {
                        Some(upgradable) => {
                            upgradable.dialed(stream);
                            crate::io::into_kj_stream(upgradable)
                        }
                        None => crate::io::into_kj_stream(stream),
                    }
                };
                response.accept(200, "OK", &Headers::new(self.table))?;
                Ok(crate::ffi::pump_tunnel(connection.as_mut(), tunnel).await?)
            }
            // As kj's client over one address: an HTTP CONNECT to the peer.
            Connector::Fixed(_) => {
                if use_tls {
                    return Err(KjError::new(
                        KjExceptionType::Unimplemented,
                        "This HttpClient does not support TLS.".to_owned(),
                    ));
                }
                let uri: http::Uri = host_str.parse().map_err(failed)?;
                let request = Self::request_of(
                    http::Method::CONNECT,
                    uri,
                    Head::new(headers.as_ffi()),
                    ChannelBody::empty(),
                );
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
                let headers =
                    HeaderBlock::new(&parts.headers, &parts.extensions).to_kj(self.table)?;
                response.accept(
                    u32::from(parts.status.as_u16()),
                    &reason(&parts),
                    HeadersRef::from(&*headers),
                )?;
                let upgraded = hyper::upgrade::on(http::Response::from_parts(parts, ()))
                    .await
                    .map_err(|e| client_error(&e))?;
                let tunnel = crate::io::into_kj_stream(TokioIo::new(upgraded));
                Ok(crate::ffi::pump_tunnel(connection.as_mut(), tunnel).await?)
            }
        }
    }
}

/// The host and port a CONNECT names (`host:port`, brackets around an IPv6 host removed).
fn host_port(host: &str) -> crate::Result<(String, u16)> {
    let authority: http::uri::Authority = host.parse().map_err(failed)?;
    let port = authority
        .port_u16()
        .ok_or_else(|| failed(format!("CONNECT host has no port: {host}")))?;
    Ok((authority.host().trim_matches(['[', ']']).to_owned(), port))
}

#[cfg(test)]
mod tests {
    use tokio::io::AsyncReadExt;
    use tokio::io::AsyncWriteExt;

    use super::*;

    fn runtime() -> tokio::runtime::Runtime {
        tokio::runtime::Builder::new_current_thread()
            .enable_all()
            .build()
            .unwrap()
    }

    /// One HTTP/1.1 exchange over a raw stream, answered with `response`; returns the request.
    async fn peer(mut io: tokio::io::DuplexStream, response: &str) -> String {
        let mut request = Vec::new();
        while !request.ends_with(b"\r\n\r\n") {
            let mut byte = [0];
            io.read_exact(&mut byte).await.unwrap();
            request.push(byte[0]);
        }
        io.write_all(response.as_bytes()).await.unwrap();
        String::from_utf8(request).unwrap()
    }

    #[test]
    fn a_fixed_client_dials_through_its_dialer_and_sends_the_url_as_given() {
        let table = HeaderTable::builtin();
        runtime().block_on(async {
            let (ours, theirs) = tokio::io::duplex(1 << 16);
            let dialed = std::sync::Mutex::new(Some(ours));
            let client = Client::new(&table, ClientSettings::default(), move || {
                let io = dialed.lock().unwrap().take();
                async move { io.ok_or_else(|| std::io::Error::other("dialed twice")) }
            });
            let mut head = Head::empty();
            head.set(
                http::header::HOST,
                "Host",
                http::HeaderValue::from_static("example.com"),
            );
            let (uri, host) = client.target(b"/path?q=1").unwrap();
            assert!(host.is_none());
            let request = Client::request_of(http::Method::GET, uri, head, ChannelBody::empty());
            let server = tokio::spawn(peer(
                theirs,
                "HTTP/1.1 204 No Content\r\nX-Reply: yes\r\n\r\n",
            ));
            let response = client.send(request).await.unwrap();
            let request = server.await.unwrap();
            assert!(
                request.starts_with("GET /path?q=1 HTTP/1.1\r\n"),
                "{request}"
            );
            assert!(request.contains("\r\nHost: example.com\r\n"), "{request}");
            assert_eq!(response.status(), http::StatusCode::NO_CONTENT);
            let headers = HeaderBlock::new(response.headers(), response.extensions())
                .to_kj(&table)
                .unwrap();
            assert_eq!(
                HeadersRef::from(&*headers).get_by_name("x-reply"),
                Some(&b"yes"[..])
            );
        });
    }

    #[test]
    fn an_internet_client_sets_the_host_from_the_url() {
        let table = HeaderTable::builtin();
        let client = Client::internet(&table, ClientSettings::default(), None, |_| true);
        let (uri, host) = client.target(b"https://example.com:8443/p").unwrap();
        assert_eq!(uri.scheme_str(), Some("https"));
        assert_eq!(host.unwrap(), "example.com:8443");
        assert!(client.target(b"/relative").is_err());
    }

    #[test]
    fn a_denied_peer_is_not_connected() {
        runtime().block_on(async {
            let internet = Internet {
                tls: None,
                allow: Box::new(|_| false),
            };
            let Err(error) = internet.dial("127.0.0.1", 9, false).await else {
                panic!("a denied peer was connected");
            };
            assert_eq!(error.kind(), std::io::ErrorKind::PermissionDenied);
        });
    }
}
