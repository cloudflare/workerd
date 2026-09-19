//! hyper-util's pooling client (dialing through a C++ connector) and a single-connection client
//! over one kj stream.

use std::cell::RefCell;
use std::future::Future;
use std::pin::Pin;
use std::sync::Arc;
use std::task::Context;
use std::task::Poll;

use futures::future::LocalBoxFuture;
use hyper::body::Incoming;
use hyper::client::conn::http1;
use hyper::rt::ReadBufCursor;
use hyper_util::client::legacy::Client;
use hyper_util::client::legacy::connect::Connected;
use hyper_util::client::legacy::connect::Connection;
use hyper_util::rt::TokioIo;
use kj::KjError;
use kj::KjExceptionType;
use kj::http::Headers;
use kj_rs::KjMaybe;

use crate::body::BodySink;
use crate::body::ChannelBody;
use crate::body::Head;
use crate::body::RustBody;
use crate::body::from_header_map;
use crate::ffi::ConnectorPtr;
use crate::ffi::HeaderTablePtr;
use crate::ffi::HttpHeaders;
use crate::ffi::OwnerThread;
use crate::ffi::WsCompression;
use crate::io::BoxIo;
use crate::io::Hangup;
use crate::io::RustIo;
use crate::io::upgraded;
use crate::ws::Role;
use crate::ws::RustWebSocket;

fn failed(what: impl std::fmt::Display) -> KjError {
    KjError::new(KjExceptionType::Failed, what.to_string())
}

fn disconnected(what: impl std::fmt::Display) -> KjError {
    KjError::new(KjExceptionType::Disconnected, what.to_string())
}

/// A request's failure: the kj exception behind it (a failed dial or stream) when there is one.
fn client_error(error: impl std::error::Error + 'static) -> KjError {
    crate::io::KjIoError::find(&error).unwrap_or_else(|| disconnected(error))
}

/// The connection a response arrived on going away (kj's `whenWriteDisconnected`), carried in
/// the response's extensions for what it upgrades into.
#[derive(Clone)]
struct ConnectionHangup(Hangup);

type ResponseFuture = Pin<Box<dyn Future<Output = kj::Result<http::Response<Incoming>>>>>;
type Driver = Pin<Box<dyn Future<Output = ()>>>;

enum Upstream {
    Pooled {
        client: Client<KjConnector, ChannelBody>,
        proxy: bool,
        tasks: Arc<ClientTasks>,
        running: RefCell<futures::stream::FuturesUnordered<BoxTask>>,
    },
    Single {
        sender: RefCell<Option<http1::SendRequest<ChannelBody>>>,
        driver: RefCell<Option<Driver>>,
        /// A request is waiting for its response.
        busy: std::rc::Rc<std::cell::Cell<bool>>,
        hangup: Hangup,
    },
}

/// See the module docs.
pub struct HyperClient {
    table: HeaderTablePtr,
    upstream: Upstream,
}

impl HyperClient {
    /// A pooling client whose connections come from `connector`.
    /// `proxy`: requests go out in absolute form, as to an HTTP proxy.
    pub fn pooled(
        table: HeaderTablePtr,
        connector: ConnectorPtr,
        idle_timeout_ms: u64,
        proxy: bool,
    ) -> Self {
        let tasks = Arc::new(ClientTasks::default());
        let client = Client::builder(ClientExecutor(tasks.clone()))
            .pool_timer(hyper_util::rt::TokioTimer::new())
            .http1_title_case_headers(true)
            .http1_max_headers(crate::body::MAX_HEADERS)
            .pool_idle_timeout(std::time::Duration::from_millis(idle_timeout_ms))
            .build(KjConnector(Arc::new(connector), proxy));
        Self {
            table,
            upstream: Upstream::Pooled {
                client,
                proxy,
                tasks,
                running: RefCell::default(),
            },
        }
    }

    /// A client over one pre-connected stream. The handshake never suspends (no I/O), so it is
    /// completed here; the connection driver runs from [`HyperClient::drive`].
    pub fn single(table: HeaderTablePtr, io: BoxIo, hangup: Hangup) -> kj::Result<Self> {
        let (sender, connection) = futures::FutureExt::now_or_never(
            http1::Builder::new()
                .title_case_headers(true)
                .max_headers(crate::body::MAX_HEADERS)
                .handshake(TokioIo::new(io)),
        )
        .ok_or_else(|| failed("HTTP/1.1 handshake suspended"))?
        .map_err(failed)?;
        Ok(Self {
            table,
            upstream: Upstream::Single {
                sender: RefCell::new(Some(sender)),
                driver: RefCell::new(Some(Box::pin(async move {
                    let _ = connection.with_upgrades().await;
                }))),
                busy: std::rc::Rc::default(),
                hangup,
            },
        })
    }

    /// Drives a single-connection client's connection; the C++ client owns this as a task, so
    /// the connection is cancelled with the client. Resolves at once for pooled clients.
    pub async fn drive(&self) {
        match &self.upstream {
            Upstream::Single { driver, .. } => {
                std::future::poll_fn(|cx| {
                    let mut driver = driver.borrow_mut();
                    match driver.as_mut() {
                        Some(future) => future.as_mut().poll(cx).map(|()| *driver = None),
                        None => Poll::Ready(()),
                    }
                })
                .await;
            }
            // hyper-util's background tasks (connection drivers, background connects) run here,
            // so they are cancelled with the client.
            Upstream::Pooled { tasks, running, .. } => {
                std::future::poll_fn(|cx| {
                    let mut running = running.borrow_mut();
                    running.extend(tasks.take(cx.waker()));
                    while futures::StreamExt::poll_next_unpin(&mut *running, cx)
                        == Poll::Ready(Some(()))
                    {}
                    Poll::<()>::Pending
                })
                .await;
            }
        }
    }

    fn send(&self, request: http::Request<ChannelBody>) -> kj::Result<ResponseFuture> {
        match &self.upstream {
            Upstream::Pooled { client, proxy, .. } => {
                let mut request = request;
                // As kj's network client: `Host` names where the request actually goes, whatever
                // the caller set.
                if !*proxy
                    && let Some(authority) = request.uri().authority()
                    && let Ok(host) = http::HeaderValue::from_str(authority.as_str())
                {
                    request.headers_mut().insert(http::header::HOST, host);
                }
                let response = client.request(request);
                Ok(Box::pin(
                    async move { response.await.map_err(client_error) },
                ))
            }
            Upstream::Single {
                sender,
                busy,
                hangup,
                ..
            } => {
                // As kj's client over one stream: no concurrent requests.
                if busy.get() {
                    return Err(failed(
                        "can't start a new request until the previous response has arrived",
                    ));
                }
                let mut sender = sender.borrow_mut();
                let sender = sender
                    .as_mut()
                    .ok_or_else(|| disconnected("connection closed"))?;
                let response = sender.send_request(request);
                busy.set(true);
                let busy = busy.clone();
                let hangup = ConnectionHangup(hangup.clone());
                Ok(Box::pin(async move {
                    let result = response.await.map_err(client_error);
                    busy.set(false);
                    result.map(|mut response| {
                        response.extensions_mut().insert(hangup);
                        response
                    })
                }))
            }
        }
    }

    fn build(
        method: &str,
        url: &str,
        head: Head,
        body: ChannelBody,
    ) -> kj::Result<http::Request<ChannelBody>> {
        let (mut parts, ()) = http::Request::new(()).into_parts();
        parts.method = http::Method::from_bytes(method.as_bytes()).map_err(failed)?;
        parts.uri = url.parse().map_err(failed)?;
        head.apply(&mut parts.headers, &mut parts.extensions);
        Ok(http::Request::from_parts(parts, body))
    }

    pub fn request(
        &self,
        method: &str,
        url: &str,
        headers: &HttpHeaders,
        length: KjMaybe<u64>,
    ) -> kj::Result<Box<ClientRequest>> {
        let length: Option<u64> = length.into();
        let (sink, body) = crate::body::channel(length);
        // As kj: an empty GET or HEAD carries no Content-Length.
        let declared = length.filter(|&n| n != 0 || !matches!(method, "GET" | "HEAD"));
        let head = Head::new(headers).with_length(declared);
        let request = Self::build(method, url, head, body)?;
        Ok(Box::new(ClientRequest {
            table: self.table,
            sink: Some(Box::new(sink)),
            response: Some(self.send(request)?),
            expected_accept: None,
        }))
    }

    /// `key`: the `Sec-WebSocket-Key`, from the client's entropy source; `extensions`: the
    /// `Sec-WebSocket-Extensions` offer to send, if any.
    pub fn open_websocket(
        &self,
        url: &str,
        headers: &HttpHeaders,
        key: &str,
        extensions: &[u8],
    ) -> kj::Result<Box<ClientRequest>> {
        let expected_accept = crate::ws::accept_key(key.as_bytes());
        let mut head = Head::new(headers);
        head.remove(&http::header::SEC_WEBSOCKET_EXTENSIONS);
        if !extensions.is_empty() {
            head.set(
                http::header::SEC_WEBSOCKET_EXTENSIONS,
                "Sec-WebSocket-Extensions",
                http::HeaderValue::from_bytes(extensions).map_err(failed)?,
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
            http::HeaderValue::try_from(key).map_err(failed)?,
        );
        let request = Self::build("GET", url, head, ChannelBody::empty())?;
        Ok(Box::new(ClientRequest {
            table: self.table,
            sink: None,
            response: Some(self.send(request)?),
            expected_accept: Some(expected_accept),
        }))
    }

    pub fn connect(&self, host: &str, headers: &HttpHeaders) -> kj::Result<Box<ClientRequest>> {
        let request = Self::build("CONNECT", host, Head::new(headers), ChannelBody::empty())?;
        Ok(Box::new(ClientRequest {
            table: self.table,
            sink: None,
            response: Some(self.send(request)?),
            expected_accept: None,
        }))
    }
}

/// A request in flight: its body sink (plain requests) and its response.
pub struct ClientRequest {
    table: HeaderTablePtr,
    sink: Option<Box<BodySink>>,
    response: Option<ResponseFuture>,
    /// For a WebSocket request: the `Sec-WebSocket-Accept` the server must answer with.
    expected_accept: Option<String>,
}

impl ClientRequest {
    pub fn take_body_sink(&mut self) -> kj::Result<Box<BodySink>> {
        self.sink.take().ok_or_else(|| failed("no request body"))
    }

    pub async fn response(&mut self) -> kj::Result<Box<ClientResponse>> {
        let response = self
            .response
            .take()
            .ok_or_else(|| failed("response already taken"))?
            .await?;
        let (mut parts, body) = response.into_parts();
        let upgrade = parts.extensions.remove::<hyper::upgrade::OnUpgrade>();
        let reason = parts.extensions.remove::<hyper::ext::ReasonPhrase>();
        let hangup = parts
            .extensions
            .remove::<ConnectionHangup>()
            .map_or_else(crate::io::never, |hangup| hangup.0);
        let handshake_error = self
            .expected_accept
            .as_ref()
            .filter(|_| parts.status == http::StatusCode::SWITCHING_PROTOCOLS)
            .and_then(|expected| websocket_handshake_error(&parts.headers, expected));
        Ok(Box::new(ClientResponse {
            status: parts.status,
            reason,
            hangup,
            handshake_error,
            headers: from_header_map(self.table.get(), &parts.headers),
            body: RefCell::new(Some(body)),
            upgrade: RefCell::new(upgrade),
        }))
    }
}

/// kj's checks of a server's WebSocket handshake, with kj's messages.
fn websocket_handshake_error(headers: &http::HeaderMap, expected_accept: &str) -> Option<String> {
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
    match headers.get(http::header::SEC_WEBSOCKET_ACCEPT) {
        Some(actual) if actual.as_bytes() == expected_accept.as_bytes() => None,
        Some(actual) => Some(format!(
            "Server failed WebSocket handshake: incorrect Sec-WebSocket-Accept header: expected \
             '{expected_accept}', got '{}'.",
            String::from_utf8_lossy(actual.as_bytes())
        )),
        None => Some("Server failed WebSocket handshake: missing Upgrade header.".to_owned()),
    }
}

/// A response: head, then exactly one of body, WebSocket or tunnel.
pub struct ClientResponse {
    status: http::StatusCode,
    reason: Option<hyper::ext::ReasonPhrase>,
    hangup: Hangup,
    /// A WebSocket response that fails kj's handshake checks.
    handshake_error: Option<String>,
    headers: Headers<'static>,
    body: RefCell<Option<Incoming>>,
    upgrade: RefCell<Option<hyper::upgrade::OnUpgrade>>,
}

impl ClientResponse {
    pub fn status_code(&self) -> u32 {
        u32::from(self.status.as_u16())
    }

    pub fn status_text(&self) -> &[u8] {
        self.reason.as_ref().map_or_else(
            || self.status.canonical_reason().unwrap_or("").as_bytes(),
            hyper::ext::ReasonPhrase::as_bytes,
        )
    }

    pub fn headers(&self) -> &HttpHeaders {
        self.headers.as_ref().as_ffi()
    }

    /// Why this WebSocket response fails the handshake, empty if it doesn't.
    pub fn websocket_handshake_error(&self) -> &str {
        self.handshake_error.as_deref().unwrap_or("")
    }

    #[expect(
        clippy::unnecessary_box_returns,
        reason = "cxx passes opaque Rust types by Box"
    )]
    pub fn take_body(&self) -> Box<RustBody> {
        Box::new(
            self.body
                .borrow_mut()
                .take()
                .map_or_else(RustBody::empty, RustBody::new),
        )
    }

    fn take_upgraded(&self) -> kj::Result<BoxIo> {
        let upgrade = self
            .upgrade
            .borrow_mut()
            .take()
            .ok_or_else(|| failed("not an upgrade"))?;
        Ok(Box::new(upgraded(upgrade)))
    }

    pub fn take_websocket(&self, compression: &WsCompression) -> kj::Result<Box<RustWebSocket>> {
        Ok(Box::new(RustWebSocket::new(
            self.take_upgraded()?,
            Role::Client,
            compression,
            self.hangup.clone(),
        )))
    }

    pub fn take_tunnel(&self) -> kj::Result<Box<RustIo>> {
        Ok(Box::new(RustIo::new(
            self.take_upgraded()?,
            self.hangup.clone(),
        )))
    }
}

// =======================================================================================
// The pooling client's connector and executor

/// Dials through the C++ connector (a kj `Network` or `NetworkAddress`, so kj's peer filters and
/// TLS apply), then takes the stream natively.
#[derive(Clone)]
struct KjConnector(Arc<ConnectorPtr>, bool);

impl tower_service::Service<http::Uri> for KjConnector {
    type Response = Conn;
    type Error = std::io::Error;
    type Future = Pin<Box<dyn Future<Output = std::io::Result<Conn>> + Send>>;

    fn poll_ready(&mut self, _cx: &mut Context<'_>) -> Poll<std::io::Result<()>> {
        Poll::Ready(Ok(()))
    }

    fn call(&mut self, uri: http::Uri) -> Self::Future {
        let (connector, proxy) = (self.0.clone(), self.1);
        let authority = uri.authority().map(ToString::to_string).unwrap_or_default();
        let https = uri.scheme() == Some(&http::uri::Scheme::HTTPS);
        let dial: LocalBoxFuture<'static, std::io::Result<Conn>> = Box::pin(async move {
            let stream = crate::ffi::connect(connector.get(), authority.as_bytes(), https)
                .await
                .map_err(crate::io::KjIoError::into_io)?;
            let (io, hangup) = crate::io::kj_to_tokio(stream, false);
            Ok(Conn(TokioIo::new(io), proxy, ConnectionHangup(hangup)))
        });
        Box::pin(OwnerThread::new(dial))
    }
}

/// A dialed connection, as hyper-util's pool wants it.
struct Conn(TokioIo<BoxIo>, bool, ConnectionHangup);

impl Connection for Conn {
    fn connected(&self) -> Connected {
        Connected::new().proxy(self.1).extra(self.2.clone())
    }
}

impl hyper::rt::Read for Conn {
    fn poll_read(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: ReadBufCursor<'_>,
    ) -> Poll<std::io::Result<()>> {
        Pin::new(&mut self.get_mut().0).poll_read(cx, buf)
    }
}

impl hyper::rt::Write for Conn {
    fn poll_write(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &[u8],
    ) -> Poll<std::io::Result<usize>> {
        Pin::new(&mut self.get_mut().0).poll_write(cx, buf)
    }

    fn poll_flush(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<std::io::Result<()>> {
        Pin::new(&mut self.get_mut().0).poll_flush(cx)
    }

    fn poll_shutdown(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<std::io::Result<()>> {
        Pin::new(&mut self.get_mut().0).poll_shutdown(cx)
    }
}

type BoxTask = Pin<Box<dyn Future<Output = ()> + Send>>;

/// hyper-util's background tasks, queued for the client's driver.
#[derive(Default)]
struct ClientTasks {
    queue: std::sync::Mutex<(Vec<BoxTask>, Option<std::task::Waker>)>,
}

impl ClientTasks {
    fn lock(&self) -> std::sync::MutexGuard<'_, (Vec<BoxTask>, Option<std::task::Waker>)> {
        self.queue
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
    }

    fn take(&self, waker: &std::task::Waker) -> Vec<BoxTask> {
        let mut queue = self.lock();
        queue.1 = Some(waker.clone());
        std::mem::take(&mut queue.0)
    }
}

#[derive(Clone)]
struct ClientExecutor(Arc<ClientTasks>);

impl<F: Future<Output = ()> + Send + 'static> hyper::rt::Executor<F> for ClientExecutor {
    fn execute(&self, future: F) {
        let waker = {
            let mut queue = self.0.lock();
            queue.0.push(Box::pin(future));
            queue.1.take()
        };
        if let Some(waker) = waker {
            waker.wake();
        }
    }
}
