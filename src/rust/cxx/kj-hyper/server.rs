//! One accepted connection served by hyper, each request dispatched to a [`Handler`] with
//! kj-typed arguments.
//!
//! A request's handler call runs alongside the connection rather than inside hyper's service
//! future: hyper needs the response head to go on, while the call goes on for as long as the
//! response body (or an upgraded WebSocket or tunnel) does. Calls are cancelled when the
//! connection ends without an upgrade, as kj's `HttpServer` cancels the service call of a peer
//! that hangs up.
//!
//! Failures are answered the way kj's default `HttpServerErrorHandler` answers them: a plain-text
//! 500 (503 for OVERLOADED, 501 for UNIMPLEMENTED) with the exception's description when no
//! response was sent, nothing at all for DISCONNECTED, and a dropped connection for a failure
//! after the head went out. A handler that wants other behavior handles its own errors.

use std::cell::Cell;
use std::cell::RefCell;
use std::future::Future;
use std::pin::Pin;
use std::pin::pin;
use std::rc::Rc;
use std::task::Poll;
use std::time::Duration;

use cxx::KjError;
use cxx::KjExceptionType;
use futures::FutureExt;
use futures::StreamExt;
use futures::future::LocalBoxFuture;
use futures::stream::FuturesUnordered;
use hyper::body::Incoming;
use hyper::server::conn::http1;
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

use crate::body::BodyAbort;
pub use crate::body::BodySink;
use crate::body::ChannelBody;
use crate::body::Head;
use crate::body::HeaderBlock;
use crate::body::RustBody;
use crate::ffi::ConnectResponse;
use crate::ffi::HttpHeaders;
use crate::ffi::WebSocketCompression;
use crate::ffi::WebSocketErrorHandler;
use crate::io::RustIo;
pub use crate::io::Upgrading;

type Response = http::Response<ChannelBody>;

fn failed(what: impl Into<String>) -> KjError {
    KjError::new(KjExceptionType::Failed, what.into())
}

/// What every connection is served with. Shared by the connections of a listener.
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
    /// One request. The response is sent through `response`; a call that returns without
    /// sending one, or fails, is answered as the module docs describe.
    async fn request<'a>(
        &'a self,
        method: Method,
        url: &'a [u8],
        headers: HeadersRef<'a>,
        body: Pin<&'a mut AsyncInputStream>,
        response: ServiceResponse<'a>,
    ) -> crate::Result<()>;

    /// One CONNECT request, answered through `connect`, in Rust or in C++.
    async fn connect<'a>(
        &'a self,
        host: &'a [u8],
        headers: HeadersRef<'a>,
        connect: Connect,
    ) -> crate::Result<()>;
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

struct Connection<'t> {
    table: &'t HeaderTable,
    settings: Rc<ServerSettings>,
    handler: Rc<dyn Handler + 't>,
    /// A response upgraded the connection: hyper hands the transport over and is done, while a
    /// call keeps using it.
    upgraded: Rc<Cell<bool>>,
}

/// The handler calls in flight (see the module docs). Owned by [`serve_connection`]'s future,
/// not by the [`Connection`] the calls themselves hold, so dropping the future drops the calls.
type Calls<'t> = Rc<RefCell<FuturesUnordered<LocalBoxFuture<'t, ()>>>>;

/// Serves HTTP/1.1 on `io` until the peer is done with the connection (or `shutdown` says so)
/// and every handler call has finished.
///
/// The table, the settings and whatever the handler borrows outlive the returned future; the
/// handler is called once per request, concurrently where the protocol allows.
///
/// # Errors
///
/// An I/O failure of the transport itself. A peer that disconnects or speaks bad HTTP ends the
/// connection quietly.
pub async fn serve_connection<'t, IO>(
    io: IO,
    table: &'t HeaderTable,
    settings: Rc<ServerSettings>,
    handler: Rc<dyn Handler + 't>,
    shutdown: &Shutdown,
) -> crate::Result<()>
where
    IO: AsyncRead + AsyncWrite + Unpin + Send + 'static,
{
    let conn = Rc::new(Connection {
        table,
        settings,
        handler,
        upgraded: Rc::default(),
    });
    let calls: Calls<'_> = Rc::default();
    let service = {
        let conn = Rc::clone(&conn);
        let calls = Rc::clone(&calls);
        hyper::service::service_fn(move |request| {
            dispatch(Rc::clone(&conn), Rc::clone(&calls), request).boxed_local()
        })
    };
    let mut builder = http1::Builder::new();
    builder
        .timer(TokioTimer::new())
        .header_read_timeout(conn.settings.header_timeout)
        // A client may shut down its side once its request is sent.
        .half_close(true)
        .title_case_headers(true)
        .preserve_header_case(true)
        .max_headers(crate::body::MAX_HEADERS)
        .auto_date_header(false);
    let mut serving = pin!(
        builder
            .serve_connection(TokioIo::new(io), service)
            .with_upgrades()
    );
    let mut drain = shutdown.subscribe();
    let mut drained = pin!(async move {
        match drain.as_mut() {
            Some(drain) => drop(drain.wait_for(|draining| *draining).await),
            None => std::future::pending().await,
        }
    });
    let mut draining = false;
    let mut served: Option<hyper::Result<()>> = None;
    std::future::poll_fn(|cx| {
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
            while calls.borrow_mut().poll_next_unpin(cx) == Poll::Ready(Some(())) {
                progressed = true;
            }
            let before = calls.borrow().len();
            if served.is_none()
                && let Poll::Ready(result) = serving.as_mut().poll(cx)
            {
                served = Some(result);
                progressed = true;
                if !conn.upgraded.get() {
                    // The connection is gone: nothing a call does can reach the peer.
                    calls.borrow_mut().clear();
                }
            }
            if calls.borrow().len() != before {
                progressed = true;
            }
            if !progressed {
                break;
            }
        }
        if served.is_some() && calls.borrow().is_empty() {
            Poll::Ready(())
        } else {
            Poll::Pending
        }
    })
    .await;
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

/// The I/O error behind a hyper error, if that is what it is.
fn transport_error(error: &hyper::Error) -> Option<KjError> {
    let mut source = std::error::Error::source(error);
    while let Some(e) = source {
        if let Some(io) = e.downcast_ref::<std::io::Error>() {
            return Some(crate::io::io_kj_error(io));
        }
        source = e.source();
    }
    None
}

/// hyper's service: starts the handler call alongside the connection and waits for the response
/// head it sends. A call that ends without one closes the connection.
async fn dispatch<'t>(
    conn: Rc<Connection<'t>>,
    calls: Calls<'t>,
    mut request: http::Request<Incoming>,
) -> Result<Response, std::io::Error> {
    let may_upgrade = request.method() == http::Method::CONNECT
        || request.headers().contains_key(http::header::UPGRADE);
    let upgrade = may_upgrade.then(|| hyper::upgrade::on(&mut request));
    let (parts, body) = request.into_parts();
    let (head_tx, head_rx) = oneshot::channel();
    let sender = Rc::new(ResponseSender {
        head: RefCell::new(Some(head_tx)),
        head_request: parts.method == http::Method::HEAD,
        websocket_key: parts.headers.get(http::header::SEC_WEBSOCKET_KEY).cloned(),
        upgrade: RefCell::new(upgrade),
        upgraded: Rc::clone(&conn.upgraded),
        body_abort: RefCell::new(None),
        sent: Cell::new(false),
    });
    let call: LocalBoxFuture<'t, ()> = Box::pin(handle(conn, parts, body, sender));
    calls.borrow_mut().push(call);
    head_rx
        .await
        .map_err(|_| std::io::Error::other("the handler sent no response"))
}

/// One handler call, and the default answer to how it ended (module docs).
async fn handle(
    conn: Rc<Connection<'_>>,
    parts: http::request::Parts,
    body: Incoming,
    sender: Rc<ResponseSender>,
) {
    let headers = match HeaderBlock::new(&parts.headers, &parts.extensions).to_kj(conn.table) {
        Ok(headers) => headers,
        Err(e) => {
            let _ = sender.send_error(400, b"Bad Request", format!("ERROR: {}", e.description()));
            return;
        }
    };
    let result = if parts.method == http::Method::CONNECT {
        let Some(upgrade) = sender.upgrade.borrow_mut().take() else {
            return;
        };
        let host = parts
            .uri
            .authority()
            .map_or("", http::uri::Authority::as_str)
            .to_owned();
        let connect = Connect {
            sender: Rc::clone(&sender),
            upgrade,
        };
        conn.handler
            .connect(host.as_bytes(), HeadersRef::from(&*headers), connect)
            .await
    } else {
        let mut method = Method::GET;
        if !crate::ffi::parse_method(parts.method.as_str().as_bytes(), &mut method) {
            let _ = sender.send_error(
                501,
                b"Not Implemented",
                "ERROR: Unrecognized request method.",
            );
            return;
        }
        let url = parts.uri.to_string();
        let mut body = crate::ffi::new_body_stream(Box::new(RustBody::new(body)));
        let mut response = crate::ffi::new_server_response(
            Box::new(ServerResponse(Rc::clone(&sender))),
            method,
            &headers,
            conn.settings.websocket_compression,
            conn.settings.websocket_errors(),
        );
        conn.handler
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
        Ok(()) if !sender.sent.get() => {
            let _ = sender.send_error(
                500,
                b"Internal Server Error",
                "ERROR: The HttpService did not generate a response.",
            );
        }
        Ok(()) => {}
        // As kj: no response, just a closed connection, and nothing worth logging.
        Err(e) if e.exception_type() == KjExceptionType::Disconnected => sender.abort(),
        Err(e) if !sender.sent.get() => {
            let (status, text, what) = match e.exception_type() {
                KjExceptionType::Overloaded => (
                    503,
                    &b"Service Unavailable"[..],
                    "The server is temporarily unable to handle your request",
                ),
                KjExceptionType::Unimplemented => (
                    501,
                    &b"Not Implemented"[..],
                    "The server does not implement this operation",
                ),
                _ => (
                    500,
                    &b"Internal Server Error"[..],
                    "The server threw an exception",
                ),
            };
            let _ = sender.send_error(
                status,
                text,
                format!("ERROR: {what}. Details:\n\n{}", e.description()),
            );
        }
        Err(_) => sender.abort(),
    }
}

/// The response side of one request, shared by the kj `Response` or `ConnectResponse` handed to
/// the handler and the call itself.
struct ResponseSender {
    head: RefCell<Option<oneshot::Sender<Response>>>,
    head_request: bool,
    websocket_key: Option<http::HeaderValue>,
    /// For a request that may upgrade the connection: hyper's hand-over of the transport.
    upgrade: RefCell<Option<hyper::upgrade::OnUpgrade>>,
    upgraded: Rc<Cell<bool>>,
    /// Fails the response body a `send()` opened, for a failure after the head went out.
    body_abort: RefCell<Option<BodyAbort>>,
    sent: Cell<bool>,
}

impl ResponseSender {
    /// The transport this request upgrades the connection to, once hyper hands it over.
    fn take_upgrade(&self) -> crate::Result<Upgrading> {
        self.upgrade
            .borrow_mut()
            .take()
            .map(Upgrading::new)
            .ok_or_else(|| failed("this request does not upgrade the connection"))
    }

    fn send_head(
        &self,
        status: u32,
        status_text: &[u8],
        head: Head,
        body: ChannelBody,
    ) -> crate::Result<()> {
        let mut parts = http::Response::new(()).into_parts().0;
        parts.status = u16::try_from(status)
            .ok()
            .and_then(|s| http::StatusCode::from_u16(s).ok())
            .ok_or_else(|| failed(format!("invalid status {status}")))?;
        if parts.status.canonical_reason().map(str::as_bytes) != Some(status_text)
            && let Ok(reason) =
                hyper::ext::ReasonPhrase::try_from(bytes::Bytes::copy_from_slice(status_text))
        {
            parts.extensions.insert(reason);
        }
        head.apply(&mut parts.headers, &mut parts.extensions);
        let tx = self
            .head
            .borrow_mut()
            .take()
            .ok_or_else(|| failed("response already sent"))?;
        self.sent.set(true);
        let _ = tx.send(http::Response::from_parts(parts, body));
        Ok(())
    }

    fn send(
        &self,
        status: u32,
        status_text: &[u8],
        headers: &HttpHeaders,
        length: Option<u64>,
    ) -> crate::Result<Box<BodySink>> {
        let mut head = Head::new(headers);
        if self.head_request {
            // As kj: a response to HEAD has no body. The application's Content-Length (or
            // Transfer-Encoding) describes the representation, and stands; otherwise a non-zero
            // expected size is sent as the length.
            let describes_body = head.contains(&http::header::CONTENT_LENGTH)
                || head.contains(&http::header::TRANSFER_ENCODING);
            if !describes_body {
                head = head.with_length(length.filter(|&n| n > 0));
            }
            self.send_head(status, status_text, head, ChannelBody::empty())?;
            return Ok(Box::new(BodySink::discarding()));
        }
        let (sink, abort, body) = crate::body::channel(length);
        self.send_head(status, status_text, head.with_length(length), body)?;
        *self.body_abort.borrow_mut() = Some(abort);
        Ok(Box::new(sink))
    }

    /// A plain-text response that closes the connection once sent.
    fn send_error(
        &self,
        status: u32,
        status_text: &[u8],
        message: impl Into<String>,
    ) -> crate::Result<()> {
        let message = message.into();
        let mut head = Head::empty();
        head.set(
            http::header::CONNECTION,
            "Connection",
            http::HeaderValue::from_static("close"),
        );
        head.set(
            http::header::CONTENT_TYPE,
            "Content-Type",
            http::HeaderValue::from_static("text/plain"),
        );
        if status == 426 {
            head.set(
                http::header::SEC_WEBSOCKET_VERSION,
                "Sec-WebSocket-Version",
                http::HeaderValue::from_static("13"),
            );
        }
        let body = if self.head_request {
            ChannelBody::empty()
        } else {
            ChannelBody::full(bytes::Bytes::from(message.clone()))
        };
        let head = head.with_length(Some(message.len() as u64));
        self.send_head(status, status_text, head, body)
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
    ) -> crate::Result<Box<BodySink>> {
        self.0.send(status, status_text, headers, length.into())
    }

    /// kj's answer to a bad WebSocket handshake: a plain-text error closing the connection.
    pub fn send_error(&self, status: u32, status_text: &[u8], message: &[u8]) -> crate::Result<()> {
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
    ) -> crate::Result<Box<RustIo>> {
        let key = self
            .0
            .websocket_key
            .as_ref()
            .ok_or_else(|| failed("not a WebSocket upgrade request"))?;
        let io = self.0.take_upgrade()?;
        let mut head = Head::new(headers);
        head.remove(&http::header::SEC_WEBSOCKET_EXTENSIONS);
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
        if let Ok(accept) =
            http::HeaderValue::from_str(&crate::handshake::accept_key(key.as_bytes()))
        {
            head.set(
                http::header::SEC_WEBSOCKET_ACCEPT,
                "Sec-WebSocket-Accept",
                accept,
            );
        }
        if !extensions.is_empty() {
            head.set(
                http::header::SEC_WEBSOCKET_EXTENSIONS,
                "Sec-WebSocket-Extensions",
                http::HeaderValue::from_bytes(extensions)
                    .map_err(|e| failed(format!("invalid extensions: {e}")))?,
            );
        }
        self.0
            .send_head(101, b"Switching Protocols", head, ChannelBody::empty())?;
        self.0.upgraded.set(true);
        Ok(Box::new(RustIo::new(io)))
    }
}

/// A CONNECT request's answer.
///
/// Accept it and take the tunnel in Rust, reject it, or hand both to a C++
/// `kj::HttpService::connect()` with [`Connect::into_kj`]. Answer before reading the tunnel:
/// bytes the client sent along with the request arrive only after the response head.
pub struct Connect {
    sender: Rc<ResponseSender>,
    upgrade: hyper::upgrade::OnUpgrade,
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
    ) -> crate::Result<Upgrading> {
        if !(200..300).contains(&status) {
            return Err(failed("the statusCode must be 2xx for accept"));
        }
        self.sender.send_head(
            status,
            status_text.as_bytes(),
            Head::new(headers.as_ffi()),
            ChannelBody::empty(),
        )?;
        self.sender.upgraded.set(true);
        Ok(Upgrading::new(self.upgrade))
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
    ) -> crate::Result<BodySink> {
        if (200..300).contains(&status) {
            return Err(failed("the statusCode must not be 2xx for reject"));
        }
        let mut head = Head::new(headers.as_ffi());
        head.set(
            http::header::CONNECTION,
            "Connection",
            http::HeaderValue::from_static("close"),
        );
        let (sink, abort, body) = crate::body::channel(length);
        self.sender.send_head(
            status,
            status_text.as_bytes(),
            head.with_length(length),
            body,
        )?;
        *self.sender.body_abort.borrow_mut() = Some(abort);
        Ok(sink)
    }

    /// The tunnel and the response as kj takes them, for `kj::HttpService::connect()`. The
    /// tunnel fails if the response rejects the request.
    #[must_use]
    pub fn into_kj(self) -> (KjOwn<AsyncIoStream>, KjOwn<ConnectResponse>) {
        let tunnel = crate::io::into_kj_stream(Upgrading::new(self.upgrade));
        let response = crate::ffi::new_connect_response(Box::new(ConnectResponder(self.sender)));
        (tunnel, response)
    }
}

/// `kj::HttpService::ConnectResponse` (its Rust half; the C++ half is `ConnectResponseImpl`).
pub struct ConnectResponder(Rc<ResponseSender>);

impl ConnectResponder {
    pub fn accept(
        &self,
        status: u32,
        status_text: &[u8],
        headers: &HttpHeaders,
    ) -> crate::Result<()> {
        self.0.send_head(
            status,
            status_text,
            Head::new(headers),
            ChannelBody::empty(),
        )?;
        self.0.upgraded.set(true);
        Ok(())
    }

    /// As kj's server: the connection closes once the refusal is sent.
    pub fn reject(
        &self,
        status: u32,
        status_text: &[u8],
        headers: &HttpHeaders,
        length: KjMaybe<u64>,
    ) -> crate::Result<Box<BodySink>> {
        let length: Option<u64> = length.into();
        let mut head = Head::new(headers);
        head.set(
            http::header::CONNECTION,
            "Connection",
            http::HeaderValue::from_static("close"),
        );
        let (sink, abort, body) = crate::body::channel(length);
        self.0
            .send_head(status, status_text, head.with_length(length), body)?;
        *self.0.body_abort.borrow_mut() = Some(abort);
        Ok(Box::new(sink))
    }
}

#[cfg(test)]
mod tests {
    use kj::http::HeaderId;
    use kj::http::Headers;
    use tokio::io::AsyncReadExt;
    use tokio::io::AsyncWriteExt;

    use super::*;

    fn runtime() -> tokio::runtime::Runtime {
        tokio::runtime::Builder::new_current_thread()
            .enable_all()
            .build()
            .unwrap()
    }

    fn table() -> KjOwn<HeaderTable> {
        HeaderTable::builtin()
    }

    /// Answers requests with an empty 200 naming the request, echoes on CONNECT tunnels, and
    /// fails `/fail`. Everything it does on the kj side is synchronous, so no kj event loop is
    /// needed.
    struct Echo<'t>(&'t HeaderTable);

    #[async_trait::async_trait(?Send)]
    impl Handler for Echo<'_> {
        async fn request<'a>(
            &'a self,
            method: Method,
            url: &'a [u8],
            headers: HeadersRef<'a>,
            _body: Pin<&'a mut AsyncInputStream>,
            response: ServiceResponse<'a>,
        ) -> crate::Result<()> {
            if url == b"/fail" {
                return Err(failed("the handler failed"));
            }
            assert_eq!(method, Method::GET);
            assert_eq!(headers.get(HeaderId::HOST), Some(&b"example.com"[..]));
            let mut sent = Headers::new(self.0);
            sent.set(HeaderId::CONTENT_TYPE, "text/plain");
            sent.set(HeaderId::LOCATION, std::str::from_utf8(url).unwrap());
            if headers.get(HeaderId::UPGRADE).is_some() {
                // The handshake answer: the WebSocket itself would need a kj event loop.
                drop(response.accept_websocket(&sent)?);
                return Ok(());
            }
            response.send(200, "OK", &sent, Some(0))?;
            Ok(())
        }

        async fn connect<'a>(
            &'a self,
            host: &'a [u8],
            _headers: HeadersRef<'a>,
            connect: Connect,
        ) -> crate::Result<()> {
            assert_eq!(host, b"example.com:443");
            let mut tunnel = connect.accept(200, "OK", (&Headers::new(self.0)).into())?;
            let mut buf = [0; 64];
            loop {
                let n = tunnel
                    .read(&mut buf)
                    .await
                    .map_err(|e| crate::io::io_kj_error(&e))?;
                if n == 0 {
                    return Ok(());
                }
                tunnel
                    .write_all(&buf[..n])
                    .await
                    .map_err(|e| crate::io::io_kj_error(&e))?;
            }
        }
    }

    /// Serves `request` and returns what the peer read until the server closed the connection.
    fn exchange(request: &[u8]) -> String {
        let table = table();
        runtime().block_on(async {
            let (ours, mut peer) = tokio::io::duplex(1 << 16);
            let shutdown = Shutdown::new();
            let served = serve_connection(
                ours,
                &table,
                Rc::new(ServerSettings::default()),
                Rc::new(Echo(&table)),
                &shutdown,
            );
            let client = async {
                peer.write_all(request).await.unwrap();
                peer.shutdown().await.unwrap();
                let mut response = Vec::new();
                peer.read_to_end(&mut response).await.unwrap();
                String::from_utf8(response).unwrap()
            };
            let (served, response) = futures::join!(served, client);
            served.unwrap();
            response
        })
    }

    #[test]
    fn a_request_reaches_the_handler_with_kj_types_and_its_response_is_written() {
        let response = exchange(b"GET /hello?x=1 HTTP/1.1\r\nHost: example.com\r\n\r\n");
        assert!(response.starts_with("HTTP/1.1 200 OK\r\n"), "{response}");
        // Header spellings are the application's, and Content-Length is kj's.
        assert!(
            response.contains("\r\nContent-Type: text/plain\r\n"),
            "{response}"
        );
        assert!(
            response.contains("\r\nLocation: /hello?x=1\r\n"),
            "{response}"
        );
        assert!(response.contains("\r\nContent-Length: 0\r\n"), "{response}");
    }

    #[test]
    fn a_failed_handler_is_answered_as_kj_answers_it() {
        let response = exchange(b"GET /fail HTTP/1.1\r\nHost: example.com\r\n\r\n");
        assert!(
            response.starts_with("HTTP/1.1 500 Internal Server Error\r\n"),
            "{response}"
        );
        assert!(response.contains("\r\nConnection: close\r\n"), "{response}");
        assert!(
            response
                .ends_with("ERROR: The server threw an exception. Details:\n\nthe handler failed"),
            "{response}"
        );
    }

    #[test]
    fn an_unknown_method_is_refused() {
        let response = exchange(b"BREW /pot HTTP/1.1\r\nHost: example.com\r\n\r\n");
        assert!(
            response.starts_with("HTTP/1.1 501 Not Implemented\r\n"),
            "{response}"
        );
    }

    #[test]
    fn a_websocket_handshake_is_answered_through_kj() {
        let response = exchange(
            b"GET /ws HTTP/1.1\r\nHost: example.com\r\nUpgrade: websocket\r\n\
              Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\n\
              Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n",
        );
        assert!(
            response.starts_with("HTTP/1.1 101 Switching Protocols\r\n"),
            "{response}"
        );
        assert!(
            response.contains("\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"),
            "{response}"
        );
    }

    #[test]
    fn a_bad_websocket_handshake_is_refused_as_kj_refuses_it() {
        let response = exchange(
            b"GET /ws HTTP/1.1\r\nHost: example.com\r\nUpgrade: websocket\r\n\
              Connection: Upgrade\r\nSec-WebSocket-Version: 12\r\n\r\n",
        );
        assert!(
            response.starts_with("HTTP/1.1 426 Upgrade Required\r\n"),
            "{response}"
        );
        assert!(
            response.contains("\r\nSec-WebSocket-Version: 13\r\n"),
            "{response}"
        );
        assert!(
            response.ends_with("ERROR: The requested WebSocket version is not supported."),
            "{response}"
        );
    }

    #[test]
    fn a_connect_is_answered_in_rust_and_the_tunnel_taken_over() {
        let table = table();
        runtime().block_on(async {
            let (ours, mut peer) = tokio::io::duplex(1 << 16);
            let shutdown = Shutdown::new();
            let served = serve_connection(
                ours,
                &table,
                Rc::new(ServerSettings::default()),
                Rc::new(Echo(&table)),
                &shutdown,
            );
            let client = async {
                peer.write_all(
                    b"CONNECT example.com:443 HTTP/1.1\r\nHost: example.com:443\r\n\r\n",
                )
                .await
                .unwrap();
                let mut head = Vec::new();
                while !head.ends_with(b"\r\n\r\n") {
                    let mut byte = [0];
                    peer.read_exact(&mut byte).await.unwrap();
                    head.push(byte[0]);
                }
                assert!(head.starts_with(b"HTTP/1.1 200 OK\r\n"));
                peer.write_all(b"ping").await.unwrap();
                let mut echoed = [0; 4];
                peer.read_exact(&mut echoed).await.unwrap();
                assert_eq!(&echoed, b"ping");
                peer.shutdown().await.unwrap();
                let mut rest = Vec::new();
                peer.read_to_end(&mut rest).await.unwrap();
                assert!(rest.is_empty());
            };
            let (served, ()) = futures::join!(served, client);
            served.unwrap();
        });
    }

    #[test]
    fn shutdown_closes_an_idle_connection() {
        let table = table();
        runtime().block_on(async {
            let (ours, mut peer) = tokio::io::duplex(1 << 16);
            let shutdown = Shutdown::new();
            let served = serve_connection(
                ours,
                &table,
                Rc::new(ServerSettings::default()),
                Rc::new(Echo(&table)),
                &shutdown,
            );
            let client = async {
                shutdown.shutdown();
                let mut rest = Vec::new();
                peer.read_to_end(&mut rest).await.unwrap();
                assert!(rest.is_empty());
            };
            let (served, ()) = futures::join!(served, client);
            served.unwrap();
        });
    }
}
