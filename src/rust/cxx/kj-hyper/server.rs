//! One accepted connection served by hyper, dispatching to a C++ `kj::HttpService`.

use std::cell::Cell;
use std::cell::RefCell;
use std::future::Future;
use std::pin::pin;
use std::rc::Rc;
use std::task::Poll;

use futures::FutureExt;
use futures::StreamExt;
use futures::future::LocalBoxFuture;
use futures::stream::FuturesUnordered;
use hyper::body::Incoming;
use hyper::server::conn::http1;
use hyper::upgrade::OnUpgrade;
use hyper_util::rt::TokioIo;
use kj::KjError;
use kj::KjExceptionType;
use kj_rs::KjMaybe;
use tokio::sync::oneshot;
use tokio::sync::watch;

use crate::body::BodySink;
use crate::body::ChannelBody;
use crate::body::Head;
use crate::body::RustBody;
use crate::body::from_header_map;
use crate::ffi::HeaderTablePtr;
use crate::ffi::HttpHeaders;
use crate::ffi::ServicePtr;
use crate::ffi::WsCompression;
use crate::ffi::WsRejection;
use crate::io::BoxIo;
use crate::io::Hangup;
use crate::io::RustIo;
use crate::io::upgraded;
use crate::ws::Role;
use crate::ws::RustWebSocket;

type Response = http::Response<ChannelBody>;
type Calls = Rc<RefCell<FuturesUnordered<LocalBoxFuture<'static, ()>>>>;

/// See the module docs.
pub struct HyperConnection {
    table: HeaderTablePtr,
    service: ServicePtr,
    io: RefCell<Option<BoxIo>>,
    hangup: Hangup,
    header_timeout: std::time::Duration,
    drain: watch::Sender<bool>,
}

impl HyperConnection {
    pub fn new(
        table: HeaderTablePtr,
        service: ServicePtr,
        io: BoxIo,
        hangup: Hangup,
        header_timeout: std::time::Duration,
    ) -> Self {
        Self {
            table,
            service,
            io: RefCell::new(Some(io)),
            hangup,
            header_timeout,
            drain: watch::channel(false).0,
        }
    }

    /// Graceful shutdown: an idle connection closes, an in-flight request finishes first.
    pub fn shutdown(&self) {
        self.drain.send_replace(true);
    }

    /// Serves the connection until it closes and every service call has finished.
    pub async fn serve(&self) -> kj::Result<()> {
        let io = self.io.borrow_mut().take().ok_or_else(|| {
            KjError::new(KjExceptionType::Failed, "serve() called twice".to_owned())
        })?;
        let calls: Calls = Rc::default();
        let (table, service) = (self.table, self.service);
        let dispatch = {
            let calls = calls.clone();
            let drain = self.drain.subscribe();
            let hangup = self.hangup.clone();
            hyper::service::service_fn(move |request| {
                dispatch(
                    table,
                    service,
                    calls.clone(),
                    drain.clone(),
                    hangup.clone(),
                    request,
                )
            })
        };
        let mut conn = pin!(
            http1::Builder::new()
                // kj's headerTimeout, which hyper applies to each request head it waits for.
                .timer(hyper_util::rt::TokioTimer::new())
                .header_read_timeout(self.header_timeout)
                .title_case_headers(true)
                .max_headers(crate::body::MAX_HEADERS)
                .auto_date_header(false)
                .serve_connection(TokioIo::new(io), dispatch)
                .with_upgrades()
        );
        let handle = kj_rs_tokio::current_handle();
        let mut drain = self.drain.subscribe();
        let mut drained = pin!(async move {
            let _ = drain.wait_for(|draining| *draining).await;
        });
        let mut hangup = self.hangup.clone();
        let mut draining = false;
        let mut conn_done = false;
        std::future::poll_fn(|cx| {
            let _runtime = handle.as_ref().map(tokio::runtime::Handle::enter);
            // As kj's HttpServer: a peer that disconnects cancels the connection, service calls
            // (an accepted WebSocket's or tunnel's included) with it.
            if hangup.poll_unpin(cx).is_ready() {
                return Poll::Ready(());
            }
            if !draining && drained.as_mut().poll(cx).is_ready() {
                draining = true;
                conn.as_mut().graceful_shutdown();
            }
            loop {
                let mut progressed = false;
                while calls.borrow_mut().poll_next_unpin(cx) == Poll::Ready(Some(())) {
                    progressed = true;
                }
                // Polling the connection may start a service call (hyper runs service_fn): a new
                // call is progress too, so it gets its first poll in this same turn.
                let before = calls.borrow().len();
                if !conn_done && conn.as_mut().poll(cx).is_ready() {
                    conn_done = true;
                    progressed = true;
                }
                if calls.borrow().len() != before {
                    progressed = true;
                }
                if !progressed {
                    break;
                }
            }
            if conn_done && calls.borrow().is_empty() {
                Poll::Ready(())
            } else {
                Poll::Pending
            }
        })
        .await;
        Ok(())
    }
}

/// Checks a request against the handshake a server must accept (RFC 6455 section 4.2.1).
fn websocket_rejection<B>(request: &http::Request<B>) -> WsRejection {
    let headers = request.headers();
    let is = |name, expected: &str| {
        headers.get(name).is_some_and(|v: &http::HeaderValue| {
            v.as_bytes().eq_ignore_ascii_case(expected.as_bytes())
        })
    };
    if !is(http::header::UPGRADE, "websocket") {
        WsRejection::NOT_UPGRADE
    } else if request.method() != http::Method::GET {
        WsRejection::NOT_GET
    } else if !is(http::header::SEC_WEBSOCKET_VERSION, "13") {
        WsRejection::UNSUPPORTED_VERSION
    } else if !headers.contains_key(http::header::SEC_WEBSOCKET_KEY) {
        WsRejection::MISSING_KEY
    } else {
        WsRejection::NONE
    }
}

/// hyper's `service_fn`: starts the service call alongside the connection and waits for the
/// response head it sends.
async fn dispatch(
    table: HeaderTablePtr,
    service: ServicePtr,
    calls: Calls,
    drain: watch::Receiver<bool>,
    hangup: Hangup,
    mut request: http::Request<Incoming>,
) -> Result<Response, std::io::Error> {
    let (head_tx, head_rx) = oneshot::channel();
    let websocket_rejection = websocket_rejection(&request);
    let sender = Rc::new(ResponseSender {
        head: RefCell::new(Some(head_tx)),
        upgrade: RefCell::new(Some(hyper::upgrade::on(&mut request))),
        websocket_key: request
            .headers()
            .get(http::header::SEC_WEBSOCKET_KEY)
            .cloned(),
        websocket_extensions: request
            .headers()
            .get(http::header::SEC_WEBSOCKET_EXTENSIONS)
            .cloned(),
        websocket_rejection,
        close: Cell::new(false),
        drain,
        hangup,
    });
    let (parts, body) = request.into_parts();
    let headers = from_header_map(table.get(), &parts.headers);
    let call: LocalBoxFuture<'static, ()> = if parts.method == http::Method::CONNECT {
        let host = parts
            .uri
            .authority()
            .map(ToString::to_string)
            .unwrap_or_default();
        let upgrade = sender.take_upgrade();
        let responder = Box::new(ConnectResponder(sender.clone()));
        Box::pin(async move {
            let upgrade = match upgrade {
                Ok(upgrade) => upgrade,
                Err(error) => return sender.finish(Some(error)),
            };
            let io = RustIo::new(upgraded(upgrade), sender.hangup.clone());
            let mut tunnel = crate::ffi::new_rust_io_stream(Box::new(io));
            let mut response = crate::ffi::new_connect_response(responder);
            let result = crate::ffi::service_connect(
                service.get(),
                host.as_bytes(),
                headers.as_ref().as_ffi(),
                tunnel.as_mut(),
                response.as_mut(),
            )
            .await;
            sender.finish(result.err().map(KjError::from));
        })
    } else {
        let url = parts.uri.to_string();
        let method = parts.method.as_str().to_owned();
        let response_sender = Box::new(ServerResponse(sender.clone()));
        Box::pin(async move {
            let mut body = crate::ffi::new_body_stream(Box::new(RustBody::new(body)));
            let mut response = crate::ffi::new_server_response(response_sender);
            let result = crate::ffi::service_request(
                service.get(),
                method.as_bytes(),
                url.as_bytes(),
                headers.as_ref().as_ffi(),
                body.as_mut(),
                response.as_mut(),
            )
            .await;
            sender.finish(result.err().map(KjError::from));
        })
    };
    calls.borrow_mut().push(call);
    // No response at all: hyper closes the connection.
    head_rx
        .await
        .map_err(|_| std::io::Error::other("the service sent no response"))
}

/// A 500 carrying the failure of a service call that never answered.
fn error_response(error: &KjError) -> Response {
    let text = error.description();
    let (sink, body) = crate::body::channel(Some(text.len() as u64));
    let _ = sink.try_send(text.as_bytes());
    let mut response = http::Response::new(body);
    *response.status_mut() = http::StatusCode::INTERNAL_SERVER_ERROR;
    response
}

/// The response side of one request, shared by the kj `Response` or `ConnectResponse` handed to
/// the service and the service call itself.
struct ResponseSender {
    head: RefCell<Option<oneshot::Sender<Response>>>,
    upgrade: RefCell<Option<OnUpgrade>>,
    websocket_key: Option<http::HeaderValue>,
    websocket_extensions: Option<http::HeaderValue>,
    websocket_rejection: WsRejection,
    close: Cell<bool>,
    drain: watch::Receiver<bool>,
    hangup: Hangup,
}

impl ResponseSender {
    fn take_upgrade(&self) -> kj::Result<OnUpgrade> {
        self.upgrade.borrow_mut().take().ok_or_else(|| {
            KjError::new(
                KjExceptionType::Failed,
                "the connection was already upgraded".to_owned(),
            )
        })
    }

    fn send_head(
        &self,
        status: u32,
        status_text: &[u8],
        head: Head,
        body: ChannelBody,
    ) -> kj::Result<()> {
        let tx = self.head.borrow_mut().take().ok_or_else(|| {
            KjError::new(KjExceptionType::Failed, "response already sent".to_owned())
        })?;
        let (mut parts, ()) = http::Response::new(()).into_parts();
        parts.status = u16::try_from(status)
            .ok()
            .and_then(|s| http::StatusCode::from_u16(s).ok())
            .ok_or_else(|| {
                KjError::new(KjExceptionType::Failed, format!("invalid status {status}"))
            })?;
        if parts.status.canonical_reason().map(str::as_bytes) != Some(status_text)
            && let Ok(reason) =
                hyper::ext::ReasonPhrase::try_from(bytes::Bytes::copy_from_slice(status_text))
        {
            parts.extensions.insert(reason);
        }
        head.apply(&mut parts.headers, &mut parts.extensions);
        let _ = tx.send(http::Response::from_parts(parts, body));
        Ok(())
    }

    fn send(
        &self,
        status: u32,
        status_text: &[u8],
        headers: &HttpHeaders,
        length: Option<u64>,
    ) -> kj::Result<Box<BodySink>> {
        let (sink, body) = crate::body::channel(length);
        let mut head = Head::new(headers);
        // Written by us rather than hyper, so it leads the headers as kj's does.
        if self.close.get() || *self.drain.borrow() {
            head.set(
                http::header::CONNECTION,
                "Connection",
                http::HeaderValue::from_static("close"),
            );
        }
        self.send_head(status, status_text, head.with_length(length), body)?;
        Ok(Box::new(sink))
    }

    /// The service call finished. A failure it never answered becomes a 500; a call that settled
    /// without answering closes the connection (the error handler chose not to respond).
    fn finish(&self, error: Option<KjError>) {
        if let (Some(tx), Some(error)) = (self.head.borrow_mut().take(), error) {
            let _ = tx.send(error_response(&error));
        }
    }
}

/// `kj::HttpService::Response`.
pub struct ServerResponse(Rc<ResponseSender>);

impl ServerResponse {
    pub fn send(
        &self,
        status: u32,
        status_text: &[u8],
        headers: &HttpHeaders,
        length: KjMaybe<u64>,
    ) -> kj::Result<Box<BodySink>> {
        self.0.send(status, status_text, headers, length.into())
    }

    /// The connection closes once this response is sent (kj's server after an application error).
    pub fn close_after_send(&self) {
        self.0.close.set(true);
    }

    /// The client's `Sec-WebSocket-Extensions` offer, for C++ to negotiate against.
    pub fn websocket_extensions(&self) -> &[u8] {
        self.0
            .websocket_extensions
            .as_ref()
            .map_or(&[], http::HeaderValue::as_bytes)
    }

    pub fn websocket_rejection(&self) -> WsRejection {
        self.0.websocket_rejection
    }

    /// `extensions`: the agreed `Sec-WebSocket-Extensions` response, if any, and `compression`
    /// what it agreed.
    pub fn accept_websocket(
        &self,
        headers: &HttpHeaders,
        extensions: &[u8],
        compression: &WsCompression,
    ) -> kj::Result<Box<RustWebSocket>> {
        let key = self.0.websocket_key.as_ref().ok_or_else(|| {
            KjError::new(
                KjExceptionType::Failed,
                "not a WebSocket upgrade request".to_owned(),
            )
        })?;
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
        if let Ok(accept) = http::HeaderValue::from_str(&crate::ws::accept_key(key.as_bytes())) {
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
                http::HeaderValue::from_bytes(extensions).map_err(|e| {
                    KjError::new(KjExceptionType::Failed, format!("invalid extensions: {e}"))
                })?,
            );
        }
        self.0
            .send_head(101, b"Switching Protocols", head, ChannelBody::empty())?;
        let io = upgraded(self.0.take_upgrade()?);
        Ok(Box::new(RustWebSocket::new(
            Box::new(io),
            Role::Server,
            compression,
            self.0.hangup.clone(),
        )))
    }
}

/// `kj::HttpService::ConnectResponse`.
pub struct ConnectResponder(Rc<ResponseSender>);

impl ConnectResponder {
    pub fn accept(&self, status: u32, status_text: &[u8], headers: &HttpHeaders) -> kj::Result<()> {
        self.0.send_head(
            status,
            status_text,
            Head::new(headers),
            ChannelBody::empty(),
        )
    }

    pub fn reject(
        &self,
        status: u32,
        status_text: &[u8],
        headers: &HttpHeaders,
        length: KjMaybe<u64>,
    ) -> kj::Result<Box<BodySink>> {
        self.0.send(status, status_text, headers, length.into())
    }
}
