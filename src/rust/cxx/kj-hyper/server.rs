//! One accepted connection served by hyper, dispatching to a C++ `kj::HttpService`.
//!
//! Upgrades are taken over by hand: once hyper has written a 101 (or accepted a CONNECT) and
//! finished HTTP on the connection, the serve loop takes the transport back from hyper and hands
//! it to the WebSocket or tunnel waiting for it.

use std::cell::Cell;
use std::cell::RefCell;
use std::future::Future;
use std::pin::Pin;
use std::pin::pin;
use std::rc::Rc;
use std::task::Poll;

use cxx::KjError;
use cxx::KjExceptionType;
use futures::FutureExt;
use futures::StreamExt;
use futures::future::LocalBoxFuture;
use futures::stream::FuturesUnordered;
use hyper::body::Incoming;
use hyper::server::conn::http1;
use hyper_util::rt::TokioIo;
use kj_rs::KjMaybe;
use tokio::sync::oneshot;
use tokio::sync::watch;

use crate::body::BodySink;
use crate::body::ChannelBody;
use crate::body::Head;
use crate::body::HeaderBlock;
use crate::body::RustBody;
use crate::ffi::HttpDispatcher;
use crate::ffi::HttpHeaders;
use crate::ffi::WsCompression;
use crate::io::BoxIo;
use crate::io::Hangup;
use crate::io::KjIoError;
use crate::io::RustIo;
use crate::io::UpgradeTx;
use crate::io::Upgraded;
use crate::io::rewound;
use crate::ws::Role;
use crate::ws::RustWebSocket;

type Response = http::Response<ChannelBody>;
type Calls<'a> = Rc<RefCell<FuturesUnordered<LocalBoxFuture<'a, ()>>>>;
/// Where the connection's transport goes once hyper is done with it, set once a response upgrades
/// the connection.
type UpgradeSlot = Rc<RefCell<Option<UpgradeTx>>>;

fn failed(what: impl Into<String>) -> KjError {
    KjError::new(KjExceptionType::Failed, what.into())
}

/// See the module docs.
pub struct HyperConnection {
    io: RefCell<Option<BoxIo>>,
    hangup: Hangup,
    header_timeout: std::time::Duration,
    drain: watch::Sender<bool>,
}

impl HyperConnection {
    pub fn new(io: BoxIo, hangup: Hangup, header_timeout: std::time::Duration) -> Self {
        Self {
            io: RefCell::new(Some(io)),
            hangup,
            header_timeout,
            drain: watch::channel(false).0,
        }
    }

    fn builder(&self) -> http1::Builder {
        let mut builder = http1::Builder::new();
        builder
            // kj's headerTimeout, which hyper applies to each request head it waits for.
            .timer(hyper_util::rt::TokioTimer::new())
            .header_read_timeout(self.header_timeout)
            // A client may shut down its side once its request is sent.
            .half_close(true)
            .title_case_headers(true)
            .preserve_header_case(true)
            .max_headers(crate::body::MAX_HEADERS)
            .auto_date_header(false);
        builder
    }

    /// Graceful shutdown: an idle connection closes, an in-flight request finishes first.
    pub fn shutdown(&self) {
        self.drain.send_replace(true);
    }

    /// Serves the connection until it closes and every service call has finished. The dispatcher
    /// is borrowed for as long as the serve runs.
    pub async fn serve(&self, dispatcher: &HttpDispatcher) -> crate::Result<()> {
        let io = self
            .io
            .borrow_mut()
            .take()
            .ok_or_else(|| failed("serve() called twice"))?;
        let calls: Calls<'_> = Rc::default();
        let upgrade: UpgradeSlot = Rc::default();
        let service = {
            let calls = calls.clone();
            let upgrade = upgrade.clone();
            let drain = self.drain.subscribe();
            let hangup = self.hangup.clone();
            hyper::service::service_fn(move |request| {
                dispatch(
                    dispatcher,
                    calls.clone(),
                    upgrade.clone(),
                    drain.clone(),
                    hangup.clone(),
                    request,
                )
                .boxed_local()
            })
        };
        let mut conn = Some(self.builder().serve_connection(TokioIo::new(io), service));
        // The transport of a connection that ended without an upgrade, being shut down.
        let mut closing: Option<BoxIo> = None;
        let mut conn_error = None;
        let mut drain = self.drain.subscribe();
        let mut drained = pin!(async move {
            let _ = drain.wait_for(|draining| *draining).await;
        });
        let mut hangup = self.hangup.clone();
        let mut draining = false;
        let hung_up = std::future::poll_fn(|cx| {
            // As kj's HttpServer: a peer that disconnects cancels the connection, service calls
            // (an accepted WebSocket's or tunnel's included) with it.
            if let Poll::Ready(result) = hangup.poll_unpin(cx) {
                return Poll::Ready(Some(result));
            }
            if !draining && drained.as_mut().poll(cx).is_ready() {
                draining = true;
                if let Some(conn) = conn.as_mut() {
                    Pin::new(conn).graceful_shutdown();
                }
            }
            // One fixed point per wake: polling the connection may start a service call (hyper
            // runs service_fn), which then gets its first poll in this same turn.
            loop {
                let mut progressed = false;
                while calls.borrow_mut().poll_next_unpin(cx) == Poll::Ready(Some(())) {
                    progressed = true;
                }
                let before = calls.borrow().len();
                if let Some(serving) = conn.as_mut()
                    && let Poll::Ready(result) = serving.poll_without_shutdown(cx)
                {
                    progressed = true;
                    if let Some(serving) = conn.take() {
                        match result {
                            Ok(()) => {
                                let parts = serving.into_parts();
                                let io = rewound(parts.io.into_inner(), parts.read_buf);
                                match upgrade.borrow_mut().take() {
                                    Some(tx) => drop(tx.send(io)),
                                    None => closing = Some(io),
                                }
                            }
                            Err(error) => conn_error = Some(error),
                        }
                    }
                }
                if let Some(io) = closing.as_mut()
                    && Pin::new(&mut **io).poll_shutdown(cx).is_ready()
                {
                    closing = None;
                    progressed = true;
                }
                if calls.borrow().len() != before {
                    progressed = true;
                }
                if !progressed {
                    break;
                }
            }
            if conn.is_none() && closing.is_none() && calls.borrow().is_empty() {
                Poll::Ready(None)
            } else {
                Poll::Pending
            }
        })
        .await;
        if let Some(result) = hung_up {
            return result;
        }
        // A failure of the stream itself is the serve's (kj's listenHttp() throws on an I/O
        // error); protocol errors and disconnects end the connection quietly.
        match conn_error {
            Some(error) => match KjIoError::find(&error) {
                Some(e) if e.exception_type() != KjExceptionType::Disconnected => Err(e),
                _ => Ok(()),
            },
            None => Ok(()),
        }
    }
}

/// hyper's `service_fn`: starts the service call alongside the connection and waits for the
/// response head it sends. A call that ends without one closes the connection: C++ answers every
/// failure it can (kj's `HttpServerErrorHandler`).
async fn dispatch<'a>(
    dispatcher: &'a HttpDispatcher,
    calls: Calls<'a>,
    upgrade: UpgradeSlot,
    drain: watch::Receiver<bool>,
    hangup: Hangup,
    request: http::Request<Incoming>,
) -> Result<Response, std::io::Error> {
    let (head_tx, head_rx) = oneshot::channel();
    let (parts, body) = request.into_parts();
    let wants_upgrade =
        parts.method == http::Method::CONNECT || parts.headers.contains_key(http::header::UPGRADE);
    let (upgrade_tx, upgrade_rx) = if wants_upgrade {
        let (tx, rx) = oneshot::channel();
        (Some(tx), Some(rx))
    } else {
        (None, None)
    };
    let sender = Rc::new(ResponseSender {
        head: RefCell::new(Some(head_tx)),
        head_request: parts.method == http::Method::HEAD,
        upgrade_tx: RefCell::new(upgrade_tx),
        upgrade_rx: RefCell::new(upgrade_rx),
        upgrade,
        websocket_key: parts.headers.get(http::header::SEC_WEBSOCKET_KEY).cloned(),
        close: Cell::new(false),
        drain,
        hangup,
    });
    let headers = HeaderBlock::new(&parts.headers, &parts.extensions);
    let call: LocalBoxFuture<'a, ()> = if parts.method == http::Method::CONNECT {
        let host = parts
            .uri
            .authority()
            .map(ToString::to_string)
            .unwrap_or_default();
        Box::pin(async move {
            let Ok(upgraded) = sender.take_upgrade() else {
                return;
            };
            let tunnel = Box::new(RustIo::new(upgraded, sender.hangup.clone()));
            let responder = Box::new(ConnectResponder(sender));
            let _ = crate::ffi::dispatch_connect(
                dispatcher,
                host.as_bytes(),
                &headers,
                tunnel,
                responder,
            )
            .await;
        })
    } else {
        let url = parts.uri.to_string();
        let method = parts.method.as_str().to_owned();
        let body = Box::new(RustBody::new(body, None));
        let response = Box::new(ServerResponse(sender));
        Box::pin(async move {
            let _ = crate::ffi::dispatch_request(
                dispatcher,
                method.as_bytes(),
                url.as_bytes(),
                &headers,
                body,
                response,
            )
            .await;
        })
    };
    calls.borrow_mut().push(call);
    head_rx
        .await
        .map_err(|_| std::io::Error::other("the service sent no response"))
}

/// The response side of one request, shared by the kj `Response` or `ConnectResponse` handed to
/// the service and the service call itself.
struct ResponseSender {
    head: RefCell<Option<oneshot::Sender<Response>>>,
    head_request: bool,
    /// For a request that may upgrade the connection: armed into `upgrade` by an upgrading
    /// response, and the transport's receiving end.
    upgrade_tx: RefCell<Option<UpgradeTx>>,
    upgrade_rx: RefCell<Option<oneshot::Receiver<BoxIo>>>,
    upgrade: UpgradeSlot,
    websocket_key: Option<http::HeaderValue>,
    close: Cell<bool>,
    drain: watch::Receiver<bool>,
    hangup: Hangup,
}

impl ResponseSender {
    /// The transport this request upgrades the connection to, once hyper hands it over.
    fn take_upgrade(&self) -> crate::Result<Upgraded> {
        self.upgrade_rx
            .borrow_mut()
            .take()
            .map(Upgraded::new)
            .ok_or_else(|| failed("this request does not upgrade the connection"))
    }

    /// The response sent upgrades the connection.
    fn arm_upgrade(&self) {
        if let Some(tx) = self.upgrade_tx.borrow_mut().take() {
            *self.upgrade.borrow_mut() = Some(tx);
        }
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
        // Written by us rather than hyper, so it leads the headers as kj's does.
        if self.close.get() || *self.drain.borrow() {
            head.set(
                http::header::CONNECTION,
                "Connection",
                http::HeaderValue::from_static("close"),
            );
        }
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
        let (sink, body) = crate::body::channel(length);
        self.send_head(status, status_text, head.with_length(length), body)?;
        Ok(Box::new(sink))
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
    ) -> crate::Result<Box<BodySink>> {
        self.0.send(status, status_text, headers, length.into())
    }

    /// The connection closes once this response is sent (kj's server after an application error).
    pub fn close_after_send(&self) {
        self.0.close.set(true);
    }

    /// `extensions`: the agreed `Sec-WebSocket-Extensions` response, if any, and `compression`
    /// what it agreed. C++ has checked the handshake.
    pub fn accept_websocket(
        &self,
        headers: &HttpHeaders,
        extensions: &[u8],
        compression: &WsCompression,
    ) -> crate::Result<Box<RustWebSocket>> {
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
                http::HeaderValue::from_bytes(extensions)
                    .map_err(|e| failed(format!("invalid extensions: {e}")))?,
            );
        }
        self.0
            .send_head(101, b"Switching Protocols", head, ChannelBody::empty())?;
        self.0.arm_upgrade();
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
        self.0.arm_upgrade();
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
        self.0.close.set(true);
        self.0.send(status, status_text, headers, length.into())
    }

    /// A plain response to the CONNECT request, for the error handler to answer a failure with.
    #[expect(
        clippy::unnecessary_box_returns,
        reason = "cxx passes opaque Rust types by Box"
    )]
    pub fn error_response(&self) -> Box<ServerResponse> {
        Box::new(ServerResponse(self.0.clone()))
    }
}
