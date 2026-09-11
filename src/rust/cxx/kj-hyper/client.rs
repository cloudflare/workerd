//! The hyper-backed outbound HTTP client behind `kj::HttpService` semantics.
//!
//! # Runtime interplay
//!
//! Each `request()`/`open_websocket()`/`connect_tunnel()` call returns a Rust future that kj-rs
//! wraps into a `kj::Promise`, polled by the KJ event loop on the calling thread. The TCP dial
//! and the hyper connection task (socket I/O, keep-alive, upgrade handoff) are tasks on the
//! same thread's per-thread tokio runtime (`kj_rs_tokio`), which runs whenever the KJ loop
//! sleeps; the halves communicate through channels whose wakes are same-thread task wakes.
//! Bodies stream through bounded channels in both directions (request: capacity-1 chunk
//! channel; response: the pump + frame channel of [`HyperRequestBody::from_body`], see
//! translate.rs), so backpressure propagates end to end.
//!
//! # Cancellation
//!
//! Dropping the `kj::Promise` on the C++ side drops this future (kj-rs guarantees this), which
//! closes both body channels; the pump stops, the hyper connection task notices and closes the
//! TCP connection. A connection whose response was not fully consumed is never returned to the
//! keep-alive pool.
//!
//! # I/O-stall watchdog
//!
//! Every connection's transport is wrapped in `StallIo` and raced against
//! [`stall::client_stall_watchdog`]: outstanding socket I/O with zero progress for the steady
//! grace (60 s) drops the connection, so a silently-dead origin fails the in-flight request
//! instead of wedging it. Waiting for a response *head* is exempt (kj waits indefinitely for a
//! server to start responding); upgraded streams are exempt. See stall.rs and hyper-http.h.
//!
//! # WebSocket and CONNECT
//!
//! `open_websocket()` performs the RFC 6455 client handshake the way `kj::HttpClientImpl` does
//! (kj's exact error texts, MANUAL-mode extension-agreement parsing); `connect_tunnel()` issues
//! a CONNECT the way kj's `connect()` does (any 2xx accepts). Both yield their upgraded byte
//! stream through `hyper::upgrade`; the C++ `HyperHttpService` wrapper mirrors kj's
//! `HttpServiceAdapter` shape on top. One deliberate divergence: kj's `MANUAL_COMPRESSION` client
//! re-serializes (and filters) the application's `Sec-WebSocket-Extensions` offer; this client
//! sends the header bytes verbatim while still parsing them to validate the server's agreement
//! — byte-identical for every offer workerd generates.

use std::cell::RefCell;
use std::net::SocketAddr;
use std::net::ToSocketAddrs;
use std::pin::Pin;
use std::pin::pin;
use std::rc::Rc;
use std::sync::Arc;
use std::task::Context;
use std::task::Poll;

use bytes::Bytes;
use cxx::KjError;
use cxx::KjExceptionType;
use cxx::UniquePtr;
use futures::FutureExt;
use futures::select_biased;
use http_body_util::BodyExt;
use hyper::client::conn::http1;
use hyper_util::rt::TokioIo;
use kj::Result;
use kj::http::HeaderTable;
use kj::http::Headers;
use kj::http::HeadersRef;
use kj::http::Method;
use kj::http::ServiceResponse;
use kj::io::AsyncInputStream;
use kj_rs::KjMaybe;
use kj_rs_io::ServedKjStream;
use tokio::net::TcpStream;
use tokio::sync::mpsc;

use crate::ffi::HeaderTablePtr;
use crate::ffi::HyperPeerFilter;
use crate::ffi::SharedFilter;
use crate::stall;
use crate::stall::StallIo;
use crate::stall::WriteStallTracker;
use crate::tls::TlsParams;
use crate::translate::HyperRequestBody;
use crate::translate::kj_error_for_hyper;
use crate::upgraded_io::HyperTunnel;
use crate::upgraded_io::SharedIo;
use crate::ws;
use crate::ws::WsSession;
use crate::ws_ext;

/// Chunk size used when pumping the KJ request body into hyper.
const READ_CHUNK_SIZE: usize = 8192;

/// Buffer size used when draining the response-body frame channel toward the caller's
/// `kj::AsyncOutputStream`. Big enough to coalesce a full channel of hyper's typical
/// ~8–16 KiB data frames into one write (see `HyperRequestBody::read`'s coalescing), matching
/// the ≤64 KiB chunks workerd's own body pumps use.
const RESPONSE_READ_CHUNK_SIZE: usize = 64 * 1024;

/// Outbound HTTP/1.1 client for a single upstream `host:port`, implementing the
/// `kj::HttpService` shape on top of hyper with keep-alive connection reuse.
pub struct HyperClient {
    inner: Rc<Inner>,
}

struct Inner {
    /// The per-server `kj::HttpHeaderTable`; response header objects are allocated against it.
    ///
    /// Invariant: the creator of this client guarantees the table outlives it (the same
    /// contract as `kj::newHttpClient(timer, table, addr, ...)`). See [`HeaderTablePtr`].
    table: HeaderTablePtr,
    /// Where connections come from: a `host:port` this client dials itself, or one
    /// pre-connected kj stream (see [`Upstream`]).
    upstream: Upstream,
    /// Render WebSocket protocol errors the way workerd's `JsgifyWebSocketErrors` handler does
    /// (see `WsSession::jsgify_errors`).
    jsgify_websocket_errors: bool,
    /// Idle keep-alive connections available for reuse. Connections are checked for liveness
    /// (`is_closed()` + `ready()`) on checkout and only returned after a fully-consumed response.
    pool: RefCell<Vec<http1::SendRequest<BridgeBody>>>,
    /// The stream-tier byte pump (the transport for `Upstream::Stream` clients whose kj stream
    /// has no native socket), taken by [`HyperClient::drive_stream_pump`]. It is NOT spawned as
    /// a runtime task: the pump reads/writes the client's kj stream — which is only borrowed,
    /// with a lifetime the C++ creator scopes to the client object — and it holds bridged kj
    /// promises, so it must be owned and cancelled by the C++ client (a kj promise member),
    /// synchronously at client destruction, while the KJ event loop still exists. A detached
    /// task would be reaped only at port teardown: after the stream dies (use-after-free) and
    /// after the EventLoop's destructor (which then sees the pump's armed events still queued).
    stream_pump: RefCell<Option<kj_rs_io::StreamPump>>,
}

/// The client's connection source.
enum Upstream {
    /// A `host:port` upstream the client dials itself (one fresh TCP — possibly TLS-wrapped —
    /// connection whenever the pool has no reusable one).
    HostPort {
        host: String,
        port: u16,
        /// restrictPeers ACL applied to every connection this client dials, at the resolved IP
        /// (see `dial_filtered`). Re-applies, per connection, the filter kj enforces inside
        /// `connect()`.
        filter: Arc<SharedFilter>,
        /// When present, connections to `host:port` are wrapped in TLS (HTTPS upstream): the
        /// rustls handshake — SNI and certificate verification against
        /// `TlsParams::server_name` — runs right after the TCP dial, and hyper speaks HTTP/1.1
        /// over the TLS stream. Mirrors kj's `TlsContext::wrapAddress(addr,
        /// expectedServerHostname)` client shape.
        tls: Option<TlsParams>,
    },
    /// One pre-connected kj stream (the `kj::newHttpClient(table, stream)` single-connection
    /// client shape): the first connection takes it; once that connection dies the client is
    /// spent — later requests fail with DISCONNECTED, like kj's client whose stream hit EOF.
    /// The stream arrives pre-adapted by `kj_rs_io::serve_kj_stream` (native tokio socket, or
    /// duplex + pump for streams without one); the pump, when present, is spawned onto this
    /// thread's loop runtime when the connection is made.
    Stream(RefCell<Option<ServedKjStream>>),
}

impl Inner {
    /// The borrowed header table. Relies on the creator's guarantee that it outlives the client
    /// (and everything the client hands out); see [`HeaderTablePtr`].
    fn table(&self) -> &'static HeaderTable {
        self.table.get()
    }
}

impl HyperClient {
    /// Create a client for `host`:`port`.
    #[must_use]
    pub(crate) fn new(
        table: HeaderTablePtr,
        host: String,
        port: u16,
        jsgify_websocket_errors: bool,
        filter: UniquePtr<HyperPeerFilter>,
    ) -> Self {
        Self {
            inner: Rc::new(Inner {
                table,
                upstream: Upstream::HostPort {
                    host,
                    port,
                    filter: Arc::new(SharedFilter::new(filter)),
                    tls: None,
                },
                jsgify_websocket_errors,
                pool: RefCell::new(Vec::new()),
                stream_pump: RefCell::new(None),
            }),
        }
    }

    /// Create a client for an HTTPS upstream at `host`:`port`; see `Upstream::HostPort`.
    #[must_use]
    pub(crate) fn new_with_tls(
        table: HeaderTablePtr,
        host: String,
        port: u16,
        jsgify_websocket_errors: bool,
        tls: TlsParams,
        filter: UniquePtr<HyperPeerFilter>,
    ) -> Self {
        Self {
            inner: Rc::new(Inner {
                table,
                upstream: Upstream::HostPort {
                    host,
                    port,
                    filter: Arc::new(SharedFilter::new(filter)),
                    tls: Some(tls),
                },
                jsgify_websocket_errors,
                pool: RefCell::new(Vec::new()),
                stream_pump: RefCell::new(None),
            }),
        }
    }

    /// Create a single-connection client over one pre-connected kj stream (the
    /// `kj::newHttpClient(table, stream)` shape); see `Upstream::Stream`.
    #[must_use]
    pub(crate) fn new_with_stream(
        table: HeaderTablePtr,
        mut served: ServedKjStream,
        jsgify_websocket_errors: bool,
    ) -> Self {
        let pump = served.pump.take();
        Self {
            inner: Rc::new(Inner {
                table,
                upstream: Upstream::Stream(RefCell::new(Some(served))),
                jsgify_websocket_errors,
                pool: RefCell::new(Vec::new()),
                stream_pump: RefCell::new(pump),
            }),
        }
    }

    /// Drive the stream-tier byte pump, if this client has one (see `Inner::stream_pump`). The
    /// C++ client wrapper owns the returned promise as a member, so the pump is cancelled —
    /// and its bridged kj promises with it — exactly when the client is destroyed. Resolves
    /// immediately for clients without a pump (dialing clients, native-socket streams).
    pub(crate) async fn drive_stream_pump(&self) {
        let pump = self.inner.stream_pump.borrow_mut().take();
        if let Some(pump) = pump {
            // Failures surface to hyper as EOF/errors on the duplex transport; the pump's own
            // result needs no separate handling.
            let _ = pump.await;
        }
    }

    /// A new handle to the same client: the `Rc` inner (upstream, keep-alive pool, header
    /// table) is shared, so requests made through either handle reuse one another's
    /// connections. This is the composition seam that keeps the
    /// `kj::newHttpClient(HttpService&)` / `kj::newHttpService(HttpClient&)` fast paths inside
    /// Rust: the service and client shapes are two views of one client.
    #[must_use]
    pub(crate) fn clone_handle(&self) -> Self {
        Self {
            inner: self.inner.clone(),
        }
    }

    /// Whether this client can accept another request without failing DISCONNECTED (the analog
    /// of kj `HttpClientImpl::canReuse()`; see the ffi.rs doc comment). Senders return to the
    /// pool at response-head time and an upgrade consumes its sender outright, so by the time a
    /// pooling wrapper asks (after the response body/WebSocket has been dropped) the pool state
    /// is authoritative: a stream client is reusable iff its stream is still unconsumed or a
    /// pooled sender is still open. Closed senders are pruned as a side effect. Like kj, a
    /// connection the server closed while idle may still race in as reusable if hyper's
    /// connection task hasn't observed the close yet (inherent to HTTP/1.1 keep-alive).
    pub(crate) fn can_reuse(&self) -> bool {
        match &self.inner.upstream {
            Upstream::Stream(cell) => {
                if cell.borrow().is_some() {
                    return true; // Fresh: the stream hasn't been consumed yet.
                }
                let mut pool = self.inner.pool.borrow_mut();
                pool.retain(|sender| !sender.is_closed());
                !pool.is_empty()
            }
            Upstream::HostPort { .. } => true, // Reconnects internally; always reusable.
        }
    }

    /// Begin a plain request in the `kj::HttpClient::request()` shape: the head is translated
    /// and the body channel created synchronously (kj clients may destroy `url`/`headers` as
    /// soon as `request()` returns), and everything else — connection checkout, sending,
    /// response capture — happens in [`PendingHttpRequest::response`].
    ///
    /// Framing follows `kj::HttpClient::request()` exactly: `expected_body_size` alone decides
    /// (kj's client drops any application Content-Length/Transfer-Encoding via its
    /// connection-header overrides) — `Some(n)` becomes `Content-Length: n`, `None` becomes
    /// chunked, with the framing header serialized before the application headers as kj does.
    /// GET/HEAD follow kj's no-entity-body rule: no framing header (and a writer that rejects
    /// writes) for an explicit zero size AND for an unknown size, unless the caller set a
    /// Transfer-Encoding header — kj's pass-through signal that a GET really carries a body.
    pub(crate) fn start_request(
        &self,
        method: Method,
        url: &[u8],
        headers: HeadersRef<'_>,
        expected_body_size: Option<u64>,
    ) -> Result<PendingHttpRequest> {
        let method = translate_method(method)?;
        let uri = translate_url(url)?;
        let (mut header_map, framing, header_case) = translate_request_headers(headers)?;

        let is_get_like = method == http::Method::GET || method == http::Method::HEAD;
        let (body, sink) = match expected_body_size {
            Some(0) if is_get_like => {
                // GET/HEAD with an explicit empty body: no framing header at all (kj's rule),
                // and the returned writer rejects writes like kj's no-entity-body writer.
                (BridgeBody::Empty, ClientSinkKind::Null)
            }
            None if is_get_like && !framing.has_transfer_encoding => {
                // GET/HEAD with an unknown body size and no explicit Transfer-Encoding: kj
                // sends no entity-body at all (HttpClientImpl's rule; a chunked GET would make
                // a body-reading server wait forever for chunks the caller never writes).
                (BridgeBody::Empty, ClientSinkKind::Null)
            }
            Some(0) => {
                // kj (like Node) emits "Content-Length: 0" for body-bearing methods even when
                // the body is empty, ordered before the application headers. Carry a
                // zero-length channel whose sender is already gone (size_hint 0 but NOT
                // is_end_stream) so hyper preserves the header; see request_impl's twin case.
                let mut ordered = http::HeaderMap::with_capacity(header_map.len() + 1);
                ordered.append(
                    http::header::CONTENT_LENGTH,
                    http::HeaderValue::from_static("0"),
                );
                for (name, value) in &header_map {
                    ordered.append(name.clone(), value.clone());
                }
                header_map = ordered;
                let (_tx, rx) = mpsc::channel(1);
                (
                    BridgeBody::Channel {
                        rx,
                        length: Some(0),
                    },
                    ClientSinkKind::Exhausted,
                )
            }
            length => {
                // kj's serializeRequest writes the framing header BEFORE the application
                // headers; pre-insert it in that position so requests match kj byte for byte.
                let mut ordered = http::HeaderMap::with_capacity(header_map.len() + 1);
                match length {
                    Some(n) => {
                        ordered.append(http::header::CONTENT_LENGTH, http::HeaderValue::from(n));
                    }
                    None => {
                        ordered.append(
                            http::header::TRANSFER_ENCODING,
                            http::HeaderValue::from_static("chunked"),
                        );
                    }
                };
                for (name, value) in &header_map {
                    ordered.append(name.clone(), value.clone());
                }
                header_map = ordered;
                let (tx, rx) = mpsc::channel(1);
                (
                    BridgeBody::Channel { rx, length },
                    ClientSinkKind::Channel {
                        tx,
                        remaining: length,
                    },
                )
            }
        };

        let mut request = http::Request::new(body);
        *request.method_mut() = method.clone();
        *request.uri_mut() = uri;
        *request.headers_mut() = header_map;
        // Original header-name spellings; hyper's encoder writes them verbatim (title-casing
        // anything it adds itself, e.g. framing headers).
        request.extensions_mut().insert(header_case);

        Ok(PendingHttpRequest {
            inner: self.inner.clone(),
            method,
            request: RefCell::new(Some(request)),
            sink: RefCell::new(Some(RequestBodySink { kind: sink })),
        })
    }

    /// Bridge entry point for `request` (see ffi.rs); converts the raw ffi types into the safe
    /// wrappers.
    pub async fn request<'a>(
        &'a self,
        method: Method,
        url: &'a [u8],
        headers: &'a kj::http::ffi::HttpHeaders,
        request_body: Pin<&'a mut AsyncInputStream>,
        response: Pin<&'a mut kj::http::ffi::HttpServiceResponse>,
    ) -> Result<()> {
        self.request_impl(
            method,
            url,
            HeadersRef::from(headers),
            request_body,
            ServiceResponse::from(response),
        )
        .await
    }

    /// Bridge entry point for `open_websocket` (see ffi.rs).
    pub async fn open_websocket<'a>(
        &'a self,
        url: &'a [u8],
        headers: &'a kj::http::ffi::HttpHeaders,
    ) -> Result<Box<WsUpgradeOutcome>> {
        self.open_websocket_impl(url, HeadersRef::from(headers))
            .await
    }

    /// Bridge entry point for `connect_tunnel` (see ffi.rs).
    pub async fn connect_tunnel<'a>(
        &'a self,
        host: &'a [u8],
        headers: &'a kj::http::ffi::HttpHeaders,
    ) -> Result<Box<TunnelOutcome>> {
        self.connect_tunnel_impl(host, HeadersRef::from(headers))
            .await
    }

    /// Bridge entry point for `upgrade_request` (see ffi.rs).
    pub async fn upgrade_request<'a>(
        &'a self,
        method: Method,
        url: &'a [u8],
        headers: &'a kj::http::ffi::HttpHeaders,
        body: &'a [u8],
    ) -> Result<Box<TunnelOutcome>> {
        self.upgrade_request_impl(method, url, HeadersRef::from(headers), body)
            .await
    }

    /// Corresponds to `kj::HttpService::request()` for plain (non-upgrade) requests.
    #[expect(
        clippy::too_many_lines,
        reason = "single linear translation of a kj request into a hyper request and back; splitting it would scatter the tightly-coupled header/body/trailer handling"
    )]
    async fn request_impl<'a>(
        &'a self,
        method: Method,
        url: &'a [u8],
        headers: HeadersRef<'a>,
        mut request_body: Pin<&'a mut AsyncInputStream>,
        response: ServiceResponse<'a>,
    ) -> Result<()> {
        let inner = self.inner.clone();

        let method = translate_method(method)?;
        let uri = translate_url(url)?;
        let (mut header_map, framing, header_case) = translate_request_headers(headers)?;

        // Body framing follows kj-http's HttpClient: an exact size (from the stream itself or the
        // caller's Content-Length header) becomes Content-Length; unknown size becomes chunked.
        let body_length = request_body
            .as_mut()
            .try_get_length()
            .or(framing.content_length);

        let (body, body_tx) = if body_length == Some(0) {
            // Empty body. kj-http and Node emit "Content-Length: 0" for body-bearing methods (every
            // method except GET/HEAD) even when the body is empty, ordered before Host; hyper omits
            // framing entirely for an is_end_stream() Empty body. So for those methods prepend an
            // explicit "Content-Length: 0" (matching Node's header order) and carry a zero-length
            // Channel (size_hint 0 but NOT is_end_stream) so hyper preserves it. GET/HEAD carry no
            // Content-Length and stay unframed via Empty.
            if method != http::Method::GET && method != http::Method::HEAD {
                let mut ordered = http::HeaderMap::with_capacity(header_map.len() + 1);
                ordered.append(
                    http::header::CONTENT_LENGTH,
                    http::HeaderValue::from_static("0"),
                );
                for (name, value) in &header_map {
                    ordered.append(name.clone(), value.clone());
                }
                header_map = ordered;
                let (_tx, rx) = mpsc::channel(1);
                (
                    BridgeBody::Channel {
                        rx,
                        length: Some(0),
                    },
                    None,
                )
            } else {
                (BridgeBody::Empty, None)
            }
        } else {
            // kj's serializeRequest writes the framing header (Content-Length /
            // Transfer-Encoding) BEFORE the application headers; pre-insert it in that position
            // so requests match kj byte for byte (hyper sees it and frames accordingly).
            let mut ordered = http::HeaderMap::with_capacity(header_map.len() + 1);
            match body_length {
                Some(n) => ordered.append(http::header::CONTENT_LENGTH, http::HeaderValue::from(n)),
                None => ordered.append(
                    http::header::TRANSFER_ENCODING,
                    http::HeaderValue::from_static("chunked"),
                ),
            };
            for (name, value) in &header_map {
                ordered.append(name.clone(), value.clone());
            }
            header_map = ordered;
            let (tx, rx) = mpsc::channel(1);
            (
                BridgeBody::Channel {
                    rx,
                    length: body_length,
                },
                Some(tx),
            )
        };

        let mut request = http::Request::new(body);
        *request.method_mut() = method.clone();
        *request.uri_mut() = uri;
        *request.headers_mut() = header_map;
        // Original header-name spellings; hyper's encoder writes them verbatim (title-casing
        // anything it adds itself, e.g. framing headers).
        request.extensions_mut().insert(header_case);

        // --- Obtain a connection (reused keep-alive connection or a fresh dial).
        let mut sender = checkout_connection(&inner).await?;

        // The request-body pump runs concurrently with both waiting for the response head and
        // streaming the response body (HTTP/1.1 allows the server to respond early).
        let mut pump_fut = pin!(
            async move {
                match body_tx {
                    Some(tx) => pump_request_body(request_body, tx).await,
                    None => None,
                }
            }
            .fuse()
        );
        // The KJ exception from a failed request-body read, if any; preferred over hyper's
        // derived error so the caller sees the original failure.
        let mut pump_error: Option<KjError> = None;

        // --- Phase 1: send the request head and body until the response head arrives.
        let head_result = {
            let mut send_fut = pin!(sender.send_request(request).fuse());
            loop {
                select_biased! {
                    result = send_fut => break result,
                    error = pump_fut => pump_error = error,
                }
            }
        };
        let head = match head_result {
            Ok(head) => head,
            Err(e) => {
                return Err(
                    pump_error.unwrap_or_else(|| kj_error_for_hyper("send HTTP request", &e))
                );
            }
        };

        let (parts, incoming) = head.into_parts();
        let expected = expected_body_size(&parts, &method, &incoming);
        let table = inner.table();
        let mut body_out = send_response_head(response, table, &parts, expected)?;

        // --- Phase 2: stream the response body while finishing the request-body pump.
        //
        // `Incoming` is not polled here (a waker round-trip per HTTP frame): the pump task
        // feeds a bounded frame channel and this future drains it, coalescing buffered frames
        // into one write (see the module docs).
        let mut body_in = HyperRequestBody::from_body(
            incoming.map_err(|e| kj_error_for_hyper("read HTTP response body", &e)),
        );
        let mut stream_fut = pin!(
            async move {
                let mut buffer = vec![0u8; RESPONSE_READ_CHUNK_SIZE];
                loop {
                    let n = body_in.read(&mut buffer, 1).await?;
                    if n == 0 {
                        break Ok(());
                    }
                    body_out.write(&buffer[..n]).await?;
                }
            }
            .fuse()
        );
        let body_result: Result<()> = loop {
            select_biased! {
                result = stream_fut => break result,
                error = pump_fut => pump_error = error,
            }
        };
        if let Err(e) = body_result {
            return Err(pump_error.unwrap_or(e));
        }
        if let Some(e) = pump_error {
            return Err(e);
        }

        // --- Success: hand the connection back for keep-alive reuse. Pool correctness: the
        // frame channel only reports EOF after the pump polled `Incoming` to completion, so
        // reaching this point means hyper saw the response fully consumed; any other ending
        // (mid-stream error, cancellation) errors out or drops this future before this point,
        // so a partially-consumed connection is never pooled. (A stray 101 leaves the
        // connection upgraded; never reuse it.)
        if !sender.is_closed() && parts.status != http::StatusCode::SWITCHING_PROTOCOLS {
            inner.pool.borrow_mut().push(sender);
        }
        Ok(())
    }

    /// Opens a WebSocket, mirroring `kj::HttpClientImpl::openWebSocket()`. On a non-101
    /// response the outcome instead carries the regular response (like kj, where the caller
    /// receives a plain `WebSocketResponse` with a body).
    #[expect(
        clippy::expect_used,
        reason = "the generated base64 Sec-WebSocket-Key is always a valid HTTP HeaderValue"
    )]
    async fn open_websocket_impl<'a>(
        &'a self,
        url: &'a [u8],
        headers: HeadersRef<'a>,
    ) -> Result<Box<WsUpgradeOutcome>> {
        let inner = self.inner.clone();
        let uri = translate_url(url)?;

        // Everything except the connection-level and key/version headers passes through
        // verbatim — including Sec-WebSocket-Extensions and Sec-WebSocket-Protocol.
        let (mut header_map, mut header_case) = translate_upgrade_request_headers(headers)?;
        let key = ws::generate_websocket_key();
        // The computed handshake headers' canonical spellings (naive title-casing would write
        // "Sec-Websocket-*", but kj writes "Sec-WebSocket-*").
        header_case.append(
            http::header::SEC_WEBSOCKET_VERSION,
            bytes::Bytes::from_static(b"Sec-WebSocket-Version"),
        );
        header_case.append(
            http::header::SEC_WEBSOCKET_KEY,
            bytes::Bytes::from_static(b"Sec-WebSocket-Key"),
        );
        header_map.insert(
            http::header::CONNECTION,
            http::HeaderValue::from_static("Upgrade"),
        );
        header_map.insert(
            http::header::UPGRADE,
            http::HeaderValue::from_static("websocket"),
        );
        header_map.insert(
            http::header::SEC_WEBSOCKET_VERSION,
            http::HeaderValue::from_static("13"),
        );
        header_map.insert(
            http::header::SEC_WEBSOCKET_KEY,
            http::HeaderValue::from_str(&key).expect("base64 keys are always valid header values"),
        );

        // MANUAL compression model: parse the application's offer (if any) with kj's grammar so
        // the server's agreement can be validated; we never add offers of our own.
        let client_offer: Option<ws_ext::CompressionConfig> = headers
            .get(kj::http::HeaderId::SEC_WEBSOCKET_EXTENSIONS)
            .and_then(|value| std::str::from_utf8(value).ok())
            .and_then(|value| {
                ws_ext::find_valid_extension_offers(value)
                    .into_iter()
                    .next()
            });

        let mut request = http::Request::new(BridgeBody::Empty);
        *request.method_mut() = http::Method::GET;
        *request.uri_mut() = uri;
        *request.headers_mut() = header_map;

        let mut sender = checkout_connection(&inner).await?;
        let mut response = sender
            .send_request(request)
            .await
            .map_err(|e| kj_error_for_hyper("send WebSocket upgrade request", &e))?;

        let table = inner.table();

        if response.status() != http::StatusCode::SWITCHING_PROTOCOLS {
            // Not an upgrade: surface the regular response (kj does the same). The connection
            // is not returned to the pool (kj may reuse here; see hyper-http.h for the
            // documented divergence).
            let (parts, incoming) = response.into_parts();
            let expected = expected_body_size(&parts, &http::Method::GET, &incoming);
            return Ok(Box::new(WsUpgradeOutcome {
                status: parts.status.as_u16(),
                status_text: status_text_for(&parts),
                headers: translate_response_headers(table, &parts.headers)?,
                expected_body_size: expected,
                payload: WsOutcomePayload::Body(Some(Box::new(HyperRequestBody::new(incoming)))),
            }));
        }

        // --- Validate the 101 handshake and compression agreement, with kj's exact error texts.
        validate_upgrade_response(response.headers(), &key)?;
        let compression = parse_compression_agreement(response.headers(), client_offer.as_ref())?;
        let response_headers = response.headers();

        let kj_headers = translate_response_headers(table, response_headers)?;
        let status_text = response
            .extensions()
            .get::<hyper::ext::ReasonPhrase>()
            .and_then(|r| std::str::from_utf8(r.as_bytes()).ok())
            .map_or_else(
                || "Switching Protocols".to_owned(),
                std::borrow::ToOwned::to_owned,
            );

        // --- Perform the upgrade: hyper hands over the raw connection (with any bytes the
        // server already sent buffered inside `Upgraded`).
        let upgraded = hyper::upgrade::on(&mut response)
            .await
            .map_err(|e| kj_error_for_hyper("WebSocket upgrade", &e))?;
        drop(response); // The (empty) 101 body; the connection now belongs to the WebSocket.

        let session = WsSession::new(
            SharedIo::ready(upgraded),
            ws::Role::Client,
            compression,
            inner.jsgify_websocket_errors,
        );
        Ok(Box::new(WsUpgradeOutcome {
            status: 101,
            status_text,
            headers: kj_headers,
            expected_body_size: None,
            payload: WsOutcomePayload::WebSocket(Some(Box::new(session))),
        }))
    }

    /// Issues a CONNECT request, mirroring `kj::HttpClientImpl::connect()`: any 2xx response
    /// accepts the tunnel; anything else is a rejection carrying an error body.
    async fn connect_tunnel_impl<'a>(
        &'a self,
        host: &'a [u8],
        headers: HeadersRef<'a>,
    ) -> Result<Box<TunnelOutcome>> {
        let inner = self.inner.clone();

        let authority = std::str::from_utf8(host)
            .ok()
            .and_then(|h| h.parse::<http::uri::Authority>().ok())
            .ok_or_else(|| {
                KjError::new(
                    KjExceptionType::Failed,
                    format!(
                        "invalid CONNECT authority \"{}\"",
                        String::from_utf8_lossy(host)
                    ),
                )
            })?;
        let mut uri_parts = http::uri::Parts::default();
        uri_parts.authority = Some(authority);
        let uri = http::Uri::from_parts(uri_parts).map_err(|e| {
            KjError::new(
                KjExceptionType::Failed,
                format!("invalid CONNECT authority: {e}"),
            )
        })?;

        let (header_map, _framing, header_case) = translate_request_headers(headers)?;
        let mut request = http::Request::new(BridgeBody::Empty);
        *request.method_mut() = http::Method::CONNECT;
        *request.uri_mut() = uri;
        *request.headers_mut() = header_map;
        request.extensions_mut().insert(header_case);

        // CONNECT consumes the connection whether it succeeds (it becomes the tunnel) or fails
        // (kj marks the client closed); it is never returned to the pool.
        let mut sender = checkout_connection(&inner).await?;
        let mut response = sender
            .send_request(request)
            .await
            .map_err(|e| kj_error_for_hyper("send CONNECT request", &e))?;

        let table = inner.table();

        if !response.status().is_success() {
            // Rejected (kj: any status outside 2xx). The response body is the error body.
            let (parts, incoming) = response.into_parts();
            let expected = expected_body_size(&parts, &http::Method::CONNECT, &incoming);
            return Ok(Box::new(TunnelOutcome {
                status: parts.status.as_u16(),
                status_text: status_text_for(&parts),
                headers: translate_response_headers(table, &parts.headers)?,
                expected_body_size: expected,
                payload: TunnelOutcomePayload::Body(Some(Box::new(HyperRequestBody::new(
                    incoming,
                )))),
            }));
        }

        let kj_headers = translate_response_headers(table, response.headers())?;
        let status = response.status().as_u16();
        let status_text = response
            .extensions()
            .get::<hyper::ext::ReasonPhrase>()
            .and_then(|r| std::str::from_utf8(r.as_bytes()).ok())
            .map_or_else(
                || {
                    response
                        .status()
                        .canonical_reason()
                        .unwrap_or_default()
                        .to_owned()
                },
                std::borrow::ToOwned::to_owned,
            );

        let upgraded = hyper::upgrade::on(&mut response)
            .await
            .map_err(|e| kj_error_for_hyper("CONNECT tunnel", &e))?;
        drop(response);

        Ok(Box::new(TunnelOutcome {
            status,
            status_text,
            headers: kj_headers,
            expected_body_size: None,
            payload: TunnelOutcomePayload::Tunnel(Some(Box::new(HyperTunnel::new(
                SharedIo::ready(upgraded),
            )))),
        }))
    }

    /// Sends a request asking for an HTTP/1.1 protocol upgrade (`Connection: Upgrade` plus the
    /// caller's `Upgrade:` header, e.g. Docker's connection-hijacking `Upgrade: tcp`): a 101
    /// response yields the raw upgraded byte stream, anything else the regular response with
    /// its body (typically error details). `body`, when non-empty, is sent complete with an
    /// exact Content-Length. Like CONNECT, the connection is consumed either way (it becomes
    /// the upgraded stream, or is never pooled after a refusal mid-upgrade).
    async fn upgrade_request_impl<'a>(
        &'a self,
        method: Method,
        url: &'a [u8],
        headers: HeadersRef<'a>,
        body: &'a [u8],
    ) -> Result<Box<TunnelOutcome>> {
        let inner = self.inner.clone();

        let method = translate_method(method)?;
        let uri = translate_url(url)?;
        // The caller's headers pass through (including its `Upgrade:` token); the
        // connection-level headers are dropped by translation and re-added here as the upgrade
        // form. The body is complete, so framing is an exact Content-Length via the channel
        // body's size hint (any caller Content-Length was extracted by translation).
        let (mut header_map, _framing, header_case) = translate_request_headers(headers)?;
        header_map.insert(
            http::header::CONNECTION,
            http::HeaderValue::from_static("Upgrade"),
        );

        let request_body = if body.is_empty() {
            BridgeBody::Empty
        } else {
            let (tx, rx) = mpsc::channel(1);
            // Capacity 1 and this is the only send; the drop of `tx` ends the body after it.
            let _ = tx.try_send(Ok(Bytes::copy_from_slice(body)));
            BridgeBody::Channel {
                rx,
                length: Some(body.len() as u64),
            }
        };

        let mut request = http::Request::new(request_body);
        *request.method_mut() = method;
        *request.uri_mut() = uri;
        *request.headers_mut() = header_map;
        request.extensions_mut().insert(header_case);

        // Like CONNECT: the connection is consumed whether the upgrade succeeds (it becomes
        // the raw stream) or not (never pooled).
        let mut sender = checkout_connection(&inner).await?;
        let mut response = sender
            .send_request(request)
            .await
            .map_err(|e| kj_error_for_hyper("send HTTP upgrade request", &e))?;

        let table = inner.table();

        if response.status() != http::StatusCode::SWITCHING_PROTOCOLS {
            // Refused: the response is a regular one; its body carries the error details.
            // (Sizing only special-cases HEAD, and upgrade requests are never HEAD.)
            let (parts, incoming) = response.into_parts();
            let expected = expected_body_size(&parts, &http::Method::POST, &incoming);
            return Ok(Box::new(TunnelOutcome {
                status: parts.status.as_u16(),
                status_text: status_text_for(&parts),
                headers: translate_response_headers(table, &parts.headers)?,
                expected_body_size: expected,
                payload: TunnelOutcomePayload::Body(Some(Box::new(HyperRequestBody::new(
                    incoming,
                )))),
            }));
        }

        let kj_headers = translate_response_headers(table, response.headers())?;
        let status = response.status().as_u16();
        let status_text = response
            .extensions()
            .get::<hyper::ext::ReasonPhrase>()
            .and_then(|r| std::str::from_utf8(r.as_bytes()).ok())
            .map_or_else(
                || {
                    response
                        .status()
                        .canonical_reason()
                        .unwrap_or_default()
                        .to_owned()
                },
                std::borrow::ToOwned::to_owned,
            );

        let upgraded = hyper::upgrade::on(&mut response)
            .await
            .map_err(|e| kj_error_for_hyper("HTTP upgrade", &e))?;
        drop(response);

        Ok(Box::new(TunnelOutcome {
            status,
            status_text,
            headers: kj_headers,
            expected_body_size: None,
            payload: TunnelOutcomePayload::Tunnel(Some(Box::new(HyperTunnel::new(
                SharedIo::ready(upgraded),
            )))),
        }))
    }
}

fn handshake_error(message: String) -> KjError {
    // kj's default HttpClientErrorHandler::handleProtocolError() throws the description as a
    // FAILED exception; openWebSocket protocol errors funnel through it.
    KjError::new(KjExceptionType::Failed, message)
}

/// Validates a 101 response's Upgrade and Sec-WebSocket-Accept headers, with kj's exact error
/// texts (kj checks Upgrade first, and — mirroring kj verbatim — reuses the Upgrade-header
/// message when the Accept header is missing).
fn validate_upgrade_response(response_headers: &http::HeaderMap, key: &str) -> Result<()> {
    match response_headers.get(http::header::UPGRADE) {
        None => {
            return Err(handshake_error(
                "Server failed WebSocket handshake: missing Upgrade header.".to_owned(),
            ));
        }
        Some(value) if !value.as_bytes().eq_ignore_ascii_case(b"websocket") => {
            return Err(handshake_error(format!(
                "Server failed WebSocket handshake: incorrect Upgrade header: expected \
                 'websocket', got '{}'.",
                String::from_utf8_lossy(value.as_bytes())
            )));
        }
        Some(_) => {}
    }

    let expected_accept = ws::websocket_accept_key(key.as_bytes());
    match response_headers.get(http::header::SEC_WEBSOCKET_ACCEPT) {
        Some(value) if value.as_bytes() == expected_accept.as_bytes() => Ok(()),
        Some(value) => Err(handshake_error(format!(
            "Server failed WebSocket handshake: incorrect Sec-WebSocket-Accept header: \
             expected '{expected_accept}', got '{}'.",
            String::from_utf8_lossy(value.as_bytes())
        ))),
        None => Err(handshake_error(
            "Server failed WebSocket handshake: missing Upgrade header.".to_owned(),
        )),
    }
}

/// MANUAL-mode compression agreement (mirrors kj's tryParseExtensionAgreement path): the
/// server's Sec-WebSocket-Extensions response, if any, must be a valid agreement against the
/// application's offer.
fn parse_compression_agreement(
    response_headers: &http::HeaderMap,
    client_offer: Option<&ws_ext::CompressionConfig>,
) -> Result<Option<ws_ext::CompressionConfig>> {
    match response_headers.get(http::header::SEC_WEBSOCKET_EXTENSIONS) {
        None => Ok(None),
        Some(value) => {
            let value = std::str::from_utf8(value.as_bytes()).map_err(|_| {
                handshake_error(
                    "Server failed WebSocket handshake: the Sec-WebSocket-Extensions header in \
                     the Response included an invalid value."
                        .to_owned(),
                )
            })?;
            Ok(Some(
                ws_ext::try_parse_extension_agreement(client_offer, value)
                    .map_err(handshake_error)?,
            ))
        }
    }
}

// =======================================================================================
// Upgrade outcomes (consumed by the C++ HyperHttpService in hyper-http.h)

enum WsOutcomePayload {
    WebSocket(Option<Box<WsSession>>),
    Body(Option<Box<HyperRequestBody>>),
}

/// Result of `open_websocket()`: either an established WebSocket session or a regular HTTP
/// response (non-101), plus the translated response head shared by both cases.
pub struct WsUpgradeOutcome {
    status: u16,
    status_text: String,
    headers: Headers<'static>,
    expected_body_size: Option<u64>,
    payload: WsOutcomePayload,
}

impl WsUpgradeOutcome {
    #[must_use]
    pub fn status_code(&self) -> u32 {
        u32::from(self.status)
    }

    #[must_use]
    pub fn status_text(&self) -> &[u8] {
        self.status_text.as_bytes()
    }

    #[must_use]
    pub fn response_headers(&self) -> &kj::http::ffi::HttpHeaders {
        self.headers.as_ref().as_ffi()
    }

    #[must_use]
    pub fn is_websocket(&self) -> bool {
        matches!(&self.payload, WsOutcomePayload::WebSocket(_))
    }

    #[must_use]
    pub fn expected_body_size(&self) -> KjMaybe<u64> {
        self.expected_body_size.into()
    }

    pub fn take_websocket(&mut self) -> Result<Box<WsSession>> {
        match &mut self.payload {
            WsOutcomePayload::WebSocket(ws) => ws.take().ok_or_else(|| {
                KjError::new(
                    KjExceptionType::Failed,
                    "the WebSocket was already taken".to_owned(),
                )
            }),
            WsOutcomePayload::Body(_) => Err(KjError::new(
                KjExceptionType::Failed,
                "the server did not upgrade to a WebSocket".to_owned(),
            )),
        }
    }

    pub fn take_body(&mut self) -> Result<Box<HyperRequestBody>> {
        match &mut self.payload {
            WsOutcomePayload::Body(body) => body.take().ok_or_else(|| {
                KjError::new(
                    KjExceptionType::Failed,
                    "the response body was already taken".to_owned(),
                )
            }),
            WsOutcomePayload::WebSocket(_) => Err(KjError::new(
                KjExceptionType::Failed,
                "the server upgraded to a WebSocket; there is no body".to_owned(),
            )),
        }
    }
}

enum TunnelOutcomePayload {
    Tunnel(Option<Box<HyperTunnel>>),
    Body(Option<Box<HyperRequestBody>>),
}

/// Result of `connect_tunnel()`: an accepted tunnel or a rejection with an error body.
pub struct TunnelOutcome {
    status: u16,
    status_text: String,
    headers: Headers<'static>,
    expected_body_size: Option<u64>,
    payload: TunnelOutcomePayload,
}

impl TunnelOutcome {
    #[must_use]
    pub fn status_code(&self) -> u32 {
        u32::from(self.status)
    }

    #[must_use]
    pub fn status_text(&self) -> &[u8] {
        self.status_text.as_bytes()
    }

    #[must_use]
    pub fn response_headers(&self) -> &kj::http::ffi::HttpHeaders {
        self.headers.as_ref().as_ffi()
    }

    #[must_use]
    pub fn is_accepted(&self) -> bool {
        matches!(&self.payload, TunnelOutcomePayload::Tunnel(_))
    }

    #[must_use]
    pub fn expected_body_size(&self) -> KjMaybe<u64> {
        self.expected_body_size.into()
    }

    pub fn take_tunnel(&mut self) -> Result<Box<HyperTunnel>> {
        match &mut self.payload {
            TunnelOutcomePayload::Tunnel(tunnel) => tunnel.take().ok_or_else(|| {
                KjError::new(
                    KjExceptionType::Failed,
                    "the tunnel was already taken".to_owned(),
                )
            }),
            TunnelOutcomePayload::Body(_) => Err(KjError::new(
                KjExceptionType::Failed,
                "the CONNECT request was rejected; there is no tunnel".to_owned(),
            )),
        }
    }

    pub fn take_body(&mut self) -> Result<Box<HyperRequestBody>> {
        match &mut self.payload {
            TunnelOutcomePayload::Body(body) => body.take().ok_or_else(|| {
                KjError::new(
                    KjExceptionType::Failed,
                    "the rejection body was already taken".to_owned(),
                )
            }),
            TunnelOutcomePayload::Tunnel(_) => Err(KjError::new(
                KjExceptionType::Failed,
                "the CONNECT request was accepted; there is no rejection body".to_owned(),
            )),
        }
    }
}

// =======================================================================================
// The kj::HttpClient plain-request shape (consumed by the C++ HyperHttpClient in
// hyper-server-ffi.c++): kj::HttpClient::request() returns {body sink, response promise}
// immediately, so the request is split into a synchronous head-translation half
// (HyperClient::start_request) and this pending object, whose sink feeds the request-body
// channel and whose response() awaits the head.

/// The request-body writer returned as `kj::HttpClient::Request::body` (wrapped by
/// `RequestBodySinkStream` in hyper-server-ffi.c++). Writes feed the bounded request-body
/// channel that hyper's connection task drains (end-to-end upload backpressure); dropping the
/// writer ends the body — kj's drop-to-finish contract (the chunked terminator, or hyper's
/// too-short-body error for an unfinished Content-Length body, follow from the channel
/// closing).
pub struct RequestBodySink {
    kind: ClientSinkKind,
}

enum ClientSinkKind {
    /// GET/HEAD with an explicit empty body: kj's no-entity-body writer — writes fail.
    Null,
    /// `Content-Length: 0` on a body-bearing method: only empty writes are allowed.
    Exhausted,
    /// A streaming body; `remaining` enforces Content-Length when known.
    Channel {
        tx: mpsc::Sender<std::result::Result<Bytes, BodyError>>,
        remaining: Option<u64>,
    },
}

impl RequestBodySink {
    /// Corresponds to `kj::AsyncOutputStream::write()`. Error texts match kj's entity writers
    /// (see translate.rs's HyperResponseBodySink, the response-direction twin).
    pub async fn write(&mut self, buffer: &[u8]) -> Result<()> {
        match &mut self.kind {
            // Matches kj's HttpNullEntityWriter error text.
            ClientSinkKind::Null => Err(KjError::new(
                KjExceptionType::Failed,
                "HTTP message has no entity-body; can't write()".to_owned(),
            )),
            ClientSinkKind::Exhausted => {
                if buffer.is_empty() {
                    Ok(())
                } else {
                    // Matches kj's HttpFixedLengthEntityWriter KJ_REQUIRE text.
                    Err(KjError::new(
                        KjExceptionType::Failed,
                        "overwrote Content-Length".to_owned(),
                    ))
                }
            }
            ClientSinkKind::Channel { tx, remaining } => {
                if buffer.is_empty() {
                    return Ok(());
                }
                if let Some(remaining) = remaining {
                    let len = buffer.len() as u64;
                    if len > *remaining {
                        return Err(KjError::new(
                            KjExceptionType::Failed,
                            "overwrote Content-Length".to_owned(),
                        ));
                    }
                    *remaining -= len;
                }
                tx.send(Ok(Bytes::copy_from_slice(buffer)))
                    .await
                    .map_err(|_| {
                        KjError::new(
                            KjExceptionType::Disconnected,
                            "the HTTP connection was closed while sending the request body"
                                .to_owned(),
                        )
                    })
            }
        }
    }

    /// Corresponds to `kj::AsyncOutputStream::whenWriteDisconnected()`.
    pub async fn when_write_disconnected(&self) {
        match &self.kind {
            ClientSinkKind::Channel { tx, .. } => tx.closed().await,
            // kj's null entity writers never resolve this either.
            _ => std::future::pending().await,
        }
    }
}

/// One in-flight `kj::HttpClient::request()`: holds the translated request (with its body
/// channel) until [`response`](Self::response) sends it. Interior mutability because the
/// bridge exposes only shared references; the C++ wrapper is the sole owner.
pub struct PendingHttpRequest {
    inner: Rc<Inner>,
    /// The request method, for HEAD's expected-body-size special case.
    method: http::Method,
    request: RefCell<Option<http::Request<BridgeBody>>>,
    sink: RefCell<Option<RequestBodySink>>,
}

impl PendingHttpRequest {
    /// Take the request-body writer (kj::HttpClient::Request::body). May be taken once.
    pub fn take_body_sink(&self) -> Result<Box<RequestBodySink>> {
        self.sink.borrow_mut().take().map(Box::new).ok_or_else(|| {
            KjError::new(
                KjExceptionType::Failed,
                "the request body was already taken".to_owned(),
            )
        })
    }

    /// Send the request and await the response head (kj::HttpClient::Request::response). May be
    /// called once. The request body streams concurrently: hyper's connection task drains the
    /// sink's channel while this waits, and keeps draining while the caller reads the response
    /// body (HTTP/1.1 lets the server respond early).
    pub async fn response(&self) -> Result<Box<ClientResponseOutcome>> {
        let request = self.request.borrow_mut().take().ok_or_else(|| {
            KjError::new(
                KjExceptionType::Failed,
                "response() was already called".to_owned(),
            )
        })?;
        let inner = self.inner.clone();

        let mut sender = checkout_connection(&inner).await?;
        let head = sender
            .send_request(request)
            .await
            .map_err(|e| kj_error_for_hyper("send HTTP request", &e))?;

        let (parts, incoming) = head.into_parts();
        let expected = expected_body_size(&parts, &self.method, &incoming);
        let table = inner.table();
        let headers = translate_response_headers(table, &parts.headers)?;

        // Keep-alive: the connection goes back to the pool at head time; checkout only hands it
        // out again once hyper reports it ready — i.e. after the request body finished and this
        // response's body was fully consumed (kj's reuse rule). A body dropped early poisons
        // the connection, which checkout detects and discards; a stray 101 hands the connection
        // to the upgrade machinery and is never pooled.
        if !sender.is_closed() && parts.status != http::StatusCode::SWITCHING_PROTOCOLS {
            inner.pool.borrow_mut().push(sender);
        }

        // For HEAD the body is empty by definition but kj reports the advertised Content-Length
        // from tryGetLength() (kj's HeadResponseStream); override the empty body's own count.
        let mut body = HyperRequestBody::new(incoming);
        if self.method == http::Method::HEAD {
            body = body.with_remaining(expected);
        }

        Ok(Box::new(ClientResponseOutcome {
            status: parts.status.as_u16(),
            status_text: status_text_for(&parts),
            headers,
            expected_body_size: expected,
            body: Some(Box::new(body)),
        }))
    }
}

/// Result of [`PendingHttpRequest::response`]: the translated response head plus the body.
/// Mirrors [`WsUpgradeOutcome`]'s accessor shape; the C++ wrapper keeps this alive alongside
/// the body stream so the borrowed headers/status text satisfy kj::HttpClient::Response's
/// "valid until `body` is dropped" contract.
pub struct ClientResponseOutcome {
    status: u16,
    status_text: String,
    headers: Headers<'static>,
    expected_body_size: Option<u64>,
    body: Option<Box<HyperRequestBody>>,
}

impl ClientResponseOutcome {
    #[must_use]
    pub fn status_code(&self) -> u32 {
        u32::from(self.status)
    }

    #[must_use]
    pub fn status_text(&self) -> &[u8] {
        self.status_text.as_bytes()
    }

    #[must_use]
    pub fn response_headers(&self) -> &kj::http::ffi::HttpHeaders {
        self.headers.as_ref().as_ffi()
    }

    #[must_use]
    pub fn expected_body_size(&self) -> KjMaybe<u64> {
        self.expected_body_size.into()
    }

    pub fn take_body(&mut self) -> Result<Box<HyperRequestBody>> {
        self.body.take().ok_or_else(|| {
            KjError::new(
                KjExceptionType::Failed,
                "the response body was already taken".to_owned(),
            )
        })
    }
}

// =======================================================================================
// Connection management

/// Take a live pooled connection, or dial a new one if none are usable.
///
/// Client-shape requests ([`PendingHttpRequest::response`]) return their connection to the
/// pool at response-head time, so a pooled sender may still be busy finishing its previous
/// exchange. Only a sender that is ready *right now* (previous response fully consumed) is
/// reused; busy ones stay pooled for later. The exception is the single-connection Stream
/// upstream, where dialing anew is impossible — there the one connection is awaited, so
/// sequential requests on a `kj::newHttpClient(table, stream)` client queue like kj's.
async fn checkout_connection(inner: &Inner) -> Result<http1::SendRequest<BridgeBody>> {
    let single_connection = matches!(inner.upstream, Upstream::Stream(_));
    let mut busy: Vec<http1::SendRequest<BridgeBody>> = Vec::new();
    let mut found: Option<http1::SendRequest<BridgeBody>> = None;
    loop {
        let candidate = inner.pool.borrow_mut().pop();
        let Some(mut sender) = candidate else { break };
        if sender.is_closed() {
            continue;
        }
        // A single poll of ready() — no waiting. This also detects connections the server
        // closed while they sat idle in the pool.
        match sender.ready().now_or_never() {
            Some(Ok(())) => {
                found = Some(sender);
                break;
            }
            Some(Err(_)) => continue,  // Died while pooled; discard.
            None => busy.push(sender), // Mid-exchange; keep for later reuse.
        }
    }
    if single_connection && found.is_none() {
        // The one connection may still be finishing a previous exchange: wait for it rather
        // than failing (requests on a single-connection client serialize).
        if let Some(mut sender) = busy.pop() {
            inner.pool.borrow_mut().append(&mut busy);
            if sender.ready().await.is_ok() {
                return Ok(sender);
            }
            return new_connection(inner).await;
        }
    }
    inner.pool.borrow_mut().append(&mut busy);
    match found {
        Some(sender) => Ok(sender),
        None => new_connection(inner).await,
    }
}

enum DialError {
    Connect(std::io::Error),
    /// The resolved address(es) were refused by the restrictPeers filter (see `dial_filtered`).
    Blocked,
    /// TLS handshake failure (rustls errors arrive as `io::Error` from tokio-rustls).
    Tls(std::io::Error),
    Handshake(hyper::Error),
}

/// Resolve `host:port` and connect to the first resolved address the peer filter allows.
///
/// The check is atomic with the dial: DNS is resolved exactly once, each resolved `SocketAddr`
/// is checked against the same restrictPeers/CIDR rules kj enforces, and the connection goes to
/// *that* address — never re-resolved — mirroring kj's `connectImpl` and structurally closing
/// the DNS-rebinding gap (every `new_connection` runs this; there is no unfiltered dial).
/// Blocked candidates are skipped, allowed ones tried in order; if every candidate was blocked
/// the error is "`connect()` blocked by `restrictPeers()`" (kj's exact text and FAILED type).
async fn dial_filtered(
    host: &str,
    port: u16,
    filter: &SharedFilter,
) -> std::result::Result<TcpStream, DialError> {
    // One DNS resolution; the returned SocketAddrs are the only addresses considered below.
    // Resolve same-thread (no cross-thread waker): a tokio runtime task owns the blocking
    // getaddrinfo JoinHandle, so the blocking-pool completion wakes tokio's own scheduler
    // (unparking this loop) rather than the bridged future's kj waker; the result is forwarded on
    // the loop thread and this await resumes same-thread. Mirrors kj-rs-io's `resolve_host`.
    let host = host.to_owned();
    let (tx, rx) = tokio::sync::oneshot::channel::<std::io::Result<Vec<SocketAddr>>>();
    tokio::spawn(async move {
        let resolved = match tokio::task::spawn_blocking(move || {
            (host.as_str(), port)
                .to_socket_addrs()
                .map(std::iter::Iterator::collect::<Vec<SocketAddr>>)
        })
        .await
        {
            Ok(result) => result,
            Err(_) => Err(std::io::Error::other("getaddrinfo task failed")),
        };
        let _ = tx.send(resolved);
    });
    let addrs: Vec<SocketAddr> = rx
        .await
        .map_err(|_| DialError::Connect(std::io::Error::other("DNS resolver task dropped")))?
        .map_err(DialError::Connect)?;

    let mut last_err: Option<std::io::Error> = None;
    let mut saw_blocked = false;
    for addr in addrs {
        if !filter.should_allow(&addr) {
            // Refused by restrictPeers; skip and try the next candidate, like kj.
            saw_blocked = true;
            continue;
        }
        // Connect to the exact address just checked — no re-resolution.
        match TcpStream::connect(addr).await {
            Ok(stream) => return Ok(stream),
            Err(e) => last_err = Some(e),
        }
    }

    // No allowed candidate connected.
    if let Some(e) = last_err {
        return Err(DialError::Connect(e));
    }
    if saw_blocked {
        return Err(DialError::Blocked);
    }
    // No addresses resolved at all.
    Err(DialError::Connect(std::io::Error::new(
        std::io::ErrorKind::NotFound,
        "no addresses resolved",
    )))
}

/// Dial `host:port` (wrapping the connection in TLS when configured) and perform the HTTP/1.1
/// handshake, as a task on the KJ thread's loop runtime. The connection task (owning the
/// socket) stays a runtime task; only the `SendRequest` handle crosses back to the KJ-side
/// future. The dial is subject to the client's restrictPeers filter at the resolved IP (see
/// `dial_filtered`).
async fn new_connection(inner: &Inner) -> Result<http1::SendRequest<BridgeBody>> {
    let (host, port, filter, tls) = match &inner.upstream {
        Upstream::HostPort {
            host,
            port,
            filter,
            tls,
        } => (
            host.clone(),
            *port,
            filter.clone(),
            tls.as_ref()
                .map(|tls| (tls.connector.clone(), tls.server_name.clone())),
        ),
        Upstream::Stream(served) => {
            // Single-connection client: hand the pre-connected stream to hyper. No dial, no
            // peer filter (the creator connected the stream), no TLS (the stream carries
            // whatever the creator established). Consuming it twice means the one connection
            // died — fail like kj's client whose underlying stream disconnected.
            let served = served.borrow_mut().take().ok_or_else(|| {
                KjError::new(
                    KjExceptionType::Disconnected,
                    "the HTTP client's underlying connection was closed (single-connection \
                     client)"
                        .to_owned(),
                )
            })?;
            // The pump (for streams without a native socket) was split off at construction and
            // is driven by the C++ client wrapper as an owned kj promise; see
            // `Inner::stream_pump` / `drive_stream_pump`. `served.pump` is always None here.
            debug_assert!(served.pump.is_none());
            // The handshake runs as a loop-runtime task (like the dial below) so hyper can
            // spawn the connection driver from runtime context.
            let io = served.io;
            let join = kj_rs_tokio::spawn(async move {
                let _ = io.set_nodelay(true);
                finish_http1_handshake(io).await
            });
            return match join.await {
                Ok(Ok(sender)) => Ok(sender),
                Ok(Err(DialError::Handshake(e))) => {
                    Err(kj_error_for_hyper("HTTP/1.1 handshake", &e))
                }
                // finish_http1_handshake only produces Handshake errors.
                Ok(Err(DialError::Connect(e) | DialError::Tls(e))) => {
                    Err(kj_error_for_io("connect()", &e))
                }
                Ok(Err(DialError::Blocked)) => Err(KjError::new(
                    KjExceptionType::Failed,
                    "connect() blocked by restrictPeers()".to_owned(),
                )),
                Err(e) => Err(KjError::new(
                    KjExceptionType::Failed,
                    format!("hyper connection task failed: {e}"),
                )),
            };
        }
    };
    let join = kj_rs_tokio::spawn(async move {
        let stream = dial_filtered(&host, port, &filter).await?;
        // Match kj-http's latency-oriented TCP behavior for request/response traffic.
        let _ = stream.set_nodelay(true);
        match tls {
            None => finish_http1_handshake(stream).await,
            Some((connector, server_name)) => {
                let tls_stream = connector
                    .connect(server_name, stream)
                    .await
                    .map_err(DialError::Tls)?;
                finish_http1_handshake(tls_stream).await
            }
        }
    });
    match join.await {
        Ok(Ok(sender)) => Ok(sender),
        // Message shaped after KJ's syscall exceptions ("connect(): Connection refused").
        Ok(Err(DialError::Connect(e))) => Err(kj_error_for_io("connect()", &e)),
        // kj's exact restrictPeers rejection (kj_rs_io::async-io.c++ / kj async-io-unix.c++).
        Ok(Err(DialError::Blocked)) => Err(KjError::new(
            KjExceptionType::Failed,
            "connect() blocked by restrictPeers()".to_string(),
        )),
        Ok(Err(DialError::Tls(e))) => Err(crate::tls::kj_error_for_tls(&e)),
        Ok(Err(DialError::Handshake(e))) => Err(kj_error_for_hyper("HTTP/1.1 handshake", &e)),
        Err(e) => Err(KjError::new(
            KjExceptionType::Failed,
            format!("hyper connection task failed: {e}"),
        )),
    }
}

/// hyper HTTP/1.1 handshake over an established (possibly TLS-wrapped) byte stream; spawns the
/// connection driver task.
async fn finish_http1_handshake<S>(
    stream: S,
) -> std::result::Result<http1::SendRequest<BridgeBody>, DialError>
where
    S: tokio::io::AsyncRead + tokio::io::AsyncWrite + Send + Unpin + 'static,
{
    // I/O-stall watchdog (see the module docs and stall.rs): the transport records every I/O
    // poll outcome into the tracker, and the connection task below races hyper's connection
    // future against the watchdog.
    let stall_tracker = WriteStallTracker::new_client();
    let (sender, connection) = http1::Builder::new()
        // Emit header names in canonical Title-Case on the wire (Host, Content-Length, ...) rather
        // than the http crate's lowercase form. This mirrors kj::HttpClient, which serializes
        // header names in the case registered in the HttpHeaderTable, and is what Node's http
        // client sends -- some peers (and node:http tests) assert on the exact raw header case.
        .title_case_headers(true)
        // kj-http caps the whole message head at 128 KiB (MAX_BUFFER) with no per-header count
        // limit; hyper's default of 100 headers is far stricter and rejects responses kj-http
        // accepts (WPT fetch/api echoes 250+ headers). 4096 is well past anything a real peer
        // sends while keeping hyper's per-message parse scratch allocation modest.
        .max_headers(4096)
        .handshake::<_, BridgeBody>(TokioIo::new(StallIo::new(stream, stall_tracker.clone())))
        .await
        .map_err(DialError::Handshake)?;
    // Drive socket I/O and keep-alive for the connection's whole lifetime; `with_upgrades`
    // additionally performs the 101/CONNECT handoff. Errors surface through
    // `SendRequest`/`Incoming` on the request side, so the result is ignored here. If the
    // watchdog fires first the connection future is dropped, closing the socket; the pooled
    // `SendRequest` then reads as closed and is discarded. An upgraded connection is exempt:
    // `with_upgrades()` resolves at the handoff, ending this select and the watchdog with it.
    tokio::spawn(async move {
        tokio::select! {
            _ = connection.with_upgrades() => {}
            () = stall::client_stall_watchdog(stall_tracker) => {}
        }
    });
    Ok(sender)
}

// =======================================================================================
// Request body bridging (kj::AsyncInputStream -> hyper)

/// Error carried through the request-body channel into hyper when reading the KJ stream fails.
#[derive(Debug)]
pub struct BodyError(pub String);

impl std::fmt::Display for BodyError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for BodyError {}

/// The hyper request body: either known-empty, or fed chunk-by-chunk from the KJ request body
/// through a bounded channel (see module docs for the backpressure story).
pub enum BridgeBody {
    Empty,
    Channel {
        rx: mpsc::Receiver<std::result::Result<Bytes, BodyError>>,
        /// Exact body length if known; `None` means chunked transfer encoding.
        length: Option<u64>,
    },
}

impl http_body::Body for BridgeBody {
    type Data = Bytes;
    type Error = BodyError;

    fn poll_frame(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
    ) -> Poll<Option<std::result::Result<http_body::Frame<Bytes>, BodyError>>> {
        match self.get_mut() {
            Self::Empty => Poll::Ready(None),
            Self::Channel { rx, .. } => rx
                .poll_recv(cx)
                .map(|item| item.map(|result| result.map(http_body::Frame::data))),
        }
    }

    fn is_end_stream(&self) -> bool {
        matches!(self, Self::Empty)
    }

    fn size_hint(&self) -> http_body::SizeHint {
        match self {
            Self::Empty => http_body::SizeHint::with_exact(0),
            Self::Channel {
                length: Some(n), ..
            } => http_body::SizeHint::with_exact(*n),
            Self::Channel { length: None, .. } => http_body::SizeHint::default(),
        }
    }
}

/// Read the KJ request body to EOF, feeding chunks into hyper's body channel. Returns the KJ
/// exception if reading failed (after also propagating a matching error to hyper).
pub async fn pump_request_body(
    mut body: Pin<&mut AsyncInputStream>,
    tx: mpsc::Sender<std::result::Result<Bytes, BodyError>>,
) -> Option<KjError> {
    let mut buffer = vec![0u8; READ_CHUNK_SIZE];
    loop {
        match body.as_mut().try_read(&mut buffer, 1).await {
            // Fewer bytes than requested (i.e. zero) means EOF; dropping `tx` ends the body.
            Ok(0) => return None,
            Ok(n) => {
                if tx
                    .send(Ok(Bytes::copy_from_slice(&buffer[..n])))
                    .await
                    .is_err()
                {
                    // hyper dropped the body receiver (e.g. the response completed and the
                    // connection is winding down); nothing more to pump.
                    return None;
                }
            }
            Err(e) => {
                // Tell hyper the body failed so it aborts the request, and surface the original
                // KJ exception to the caller.
                let _ = tx
                    .send(Err(BodyError(format!(
                        "reading request body: {}",
                        e.description()
                    ))))
                    .await;
                return Some(e);
            }
        }
    }
}

// =======================================================================================
// Translation helpers

pub fn translate_method(method: Method) -> Result<http::Method> {
    // Spellings match kj-http's KJ_HTTP_FOR_EACH_METHOD list exactly.
    let name = match method {
        Method::GET => return Ok(http::Method::GET),
        Method::HEAD => return Ok(http::Method::HEAD),
        Method::POST => return Ok(http::Method::POST),
        Method::PUT => return Ok(http::Method::PUT),
        Method::DELETE => return Ok(http::Method::DELETE),
        Method::PATCH => return Ok(http::Method::PATCH),
        Method::OPTIONS => return Ok(http::Method::OPTIONS),
        Method::TRACE => return Ok(http::Method::TRACE),
        Method::PURGE => "PURGE",
        Method::COPY => "COPY",
        Method::LOCK => "LOCK",
        Method::MKCOL => "MKCOL",
        Method::MOVE => "MOVE",
        Method::PROPFIND => "PROPFIND",
        Method::PROPPATCH => "PROPPATCH",
        Method::SEARCH => "SEARCH",
        Method::UNLOCK => "UNLOCK",
        Method::ACL => "ACL",
        Method::REPORT => "REPORT",
        Method::MKACTIVITY => "MKACTIVITY",
        Method::CHECKOUT => "CHECKOUT",
        Method::MERGE => "MERGE",
        Method::MSEARCH => "MSEARCH",
        Method::NOTIFY => "NOTIFY",
        Method::SUBSCRIBE => "SUBSCRIBE",
        Method::UNSUBSCRIBE => "UNSUBSCRIBE",
        Method::QUERY => "QUERY",
        Method::BAN => "BAN",
        _ => {
            return Err(KjError::new(
                KjExceptionType::Failed,
                format!("unknown HTTP method: {method:?}"),
            ));
        }
    };
    http::Method::from_bytes(name.as_bytes()).map_err(|e| {
        KjError::new(
            KjExceptionType::Failed,
            format!("invalid HTTP method \"{name}\": {e}"),
        )
    })
}

/// Parse the request URL. Like kj-http's host-bound `HttpClient`, the URL is expected in
/// origin-form ("/path?query"); it is passed to hyper as given.
pub fn translate_url(url: &[u8]) -> Result<http::Uri> {
    let text = std::str::from_utf8(url)
        .map_err(|_| KjError::new(KjExceptionType::Failed, "invalid request URL".to_owned()))?;
    text.parse::<http::Uri>().map_err(|e| {
        KjError::new(
            KjExceptionType::Failed,
            format!("invalid request URL \"{text}\": {e}"),
        )
    })
}

fn translate_header_entry(
    entry: &kj::http::HeaderEntry,
) -> Result<(http::HeaderName, http::HeaderValue)> {
    let name = http::HeaderName::from_bytes(entry.name.as_bytes()).map_err(|e| {
        KjError::new(
            KjExceptionType::Failed,
            format!("invalid header name \"{}\": {e}", entry.name),
        )
    })?;
    let value = http::HeaderValue::from_bytes(&entry.value).map_err(|e| {
        KjError::new(
            KjExceptionType::Failed,
            format!("invalid value for header \"{}\": {e}", entry.name),
        )
    })?;
    Ok((name, value))
}

/// Translate `kj::HttpHeaders` into a hyper `HeaderMap`, preserving multi-value headers and
/// non-UTF-8 (obs-text) value bytes. Connection-level headers are removed because hyper manages
/// message framing itself; a caller-supplied Content-Length is returned separately as a body
/// length hint (matching kj-http, where expectedBodySize wins over the header).
pub fn translate_request_headers(
    headers: HeadersRef<'_>,
) -> Result<(http::HeaderMap, RequestFraming, hyper::ext::HeaderCaseMap)> {
    let mut map = http::HeaderMap::new();
    // Original header-name spellings for hyper's encoder (see translate.rs's
    // translate_response_headers): kj-http preserves the application's exact case.
    let mut case = hyper::ext::HeaderCaseMap::default();
    let mut framing = RequestFraming {
        content_length: None,
        has_transfer_encoding: false,
    };
    for entry in headers.entries() {
        if entry.name.eq_ignore_ascii_case("content-length") {
            framing.content_length = std::str::from_utf8(&entry.value)
                .ok()
                .and_then(|v| v.trim().parse().ok());
            continue;
        }
        if entry.name.eq_ignore_ascii_case("transfer-encoding") {
            // Stripped like kj (the client's framing decision replaces it), but remembered:
            // kj's client treats an explicit Transfer-Encoding on a GET/HEAD as the signal
            // that the caller really wants to send a body (the pass-through HACK in
            // HttpClientImpl::request()).
            framing.has_transfer_encoding = true;
            continue;
        }
        if entry.name.eq_ignore_ascii_case("connection")
            || entry.name.eq_ignore_ascii_case("keep-alive")
        {
            continue;
        }
        let (name, value) = translate_header_entry(&entry)?;
        case.append(name.clone(), Bytes::copy_from_slice(entry.name.as_bytes()));
        map.append(name, value);
    }
    Ok((map, framing, case))
}

/// The framing-relevant application headers `translate_request_headers` stripped: the parsed
/// Content-Length value, and whether a Transfer-Encoding header was present (see kj's
/// GET-with-body pass-through rule).
pub struct RequestFraming {
    pub content_length: Option<u64>,
    pub has_transfer_encoding: bool,
}

/// Header translation for WebSocket upgrade requests: like kj's `serializeRequest` with the
/// `WEBSOCKET_CONNECTION_HEADERS` overrides, the connection-level and handshake-owned headers are
/// dropped (the caller re-adds the computed ones); everything else — including
/// Sec-WebSocket-Extensions and Sec-WebSocket-Protocol — passes through verbatim.
fn translate_upgrade_request_headers(
    headers: HeadersRef<'_>,
) -> Result<(http::HeaderMap, hyper::ext::HeaderCaseMap)> {
    const DROPPED: &[&str] = &[
        "connection",
        "keep-alive",
        "te",
        "trailer",
        "upgrade",
        "content-length",
        "transfer-encoding",
        "sec-websocket-key",
        "sec-websocket-version",
        "sec-websocket-accept",
    ];
    let mut map = http::HeaderMap::new();
    let mut case = hyper::ext::HeaderCaseMap::default();
    for entry in headers.entries() {
        if DROPPED.iter().any(|d| entry.name.eq_ignore_ascii_case(d)) {
            continue;
        }
        let (name, value) = translate_header_entry(&entry)?;
        case.append(name.clone(), Bytes::copy_from_slice(entry.name.as_bytes()));
        map.append(name, value);
    }
    Ok((map, case))
}

/// Expected size of the response body, as reported to the C++ `Response::send()`. For HEAD the
/// body is empty by definition and the advertised size is Content-Length; otherwise hyper's body
/// size hint is exact iff the response has a Content-Length.
fn expected_body_size(
    parts: &http::response::Parts,
    method: &http::Method,
    incoming: &hyper::body::Incoming,
) -> Option<u64> {
    if *method == http::Method::HEAD {
        parts
            .headers
            .get(http::header::CONTENT_LENGTH)?
            .to_str()
            .ok()?
            .trim()
            .parse()
            .ok()
    } else {
        http_body::Body::size_hint(incoming).exact()
    }
}

/// Translate the response head to KJ and send it, obtaining the response body output stream.
fn send_response_head<'a>(
    response: ServiceResponse<'a>,
    table: &HeaderTable,
    parts: &http::response::Parts,
    expected: Option<u64>,
) -> Result<kj::io::AsyncOutputStream<'a>> {
    let headers = translate_response_headers(table, &parts.headers)?;
    response.send(
        u32::from(parts.status.as_u16()),
        &status_text_for(parts),
        &headers,
        expected,
    )
}

/// Translate a hyper response `HeaderMap` into `kj::HttpHeaders` allocated against `table`.
/// All headers are passed through verbatim (kj-http overrides connection-level headers during
/// serialization, so keeping them is safe and maximizes fidelity). Multi-value headers produce
/// one kj header entry per value, in received order.
pub fn translate_response_headers<'t>(
    table: &'t HeaderTable,
    map: &http::HeaderMap,
) -> Result<Headers<'t>> {
    let mut headers = Headers::new(table);
    for (name, value) in map {
        headers.add(name.as_str(), value.as_bytes())?;
    }
    Ok(headers)
}

/// Response status text. hyper preserves the wire reason phrase in an extension when it differs
/// from the canonical one, so this matches kj-http's behavior (which always preserves the wire
/// text) except for non-UTF-8 reason phrases, which fall back to the canonical reason.
fn status_text_for(parts: &http::response::Parts) -> String {
    if let Some(reason) = parts.extensions.get::<hyper::ext::ReasonPhrase>()
        && let Ok(text) = std::str::from_utf8(reason.as_bytes())
    {
        return text.to_owned();
    }
    parts
        .status
        .canonical_reason()
        .unwrap_or_default()
        .to_owned()
}

// =======================================================================================
// Error mapping

/// Map an I/O error to a `kj::Exception`, using DISCONNECTED for connection-level failures the
/// way KJ's syscall error translation does (e.g. ECONNREFUSED, ECONNRESET, EPIPE).
pub fn kj_error_for_io(context: &str, e: &std::io::Error) -> KjError {
    use std::io::ErrorKind;
    let exception_type = match e.kind() {
        ErrorKind::ConnectionRefused
        | ErrorKind::ConnectionReset
        | ErrorKind::ConnectionAborted
        | ErrorKind::BrokenPipe
        | ErrorKind::NotConnected
        | ErrorKind::UnexpectedEof => KjExceptionType::Disconnected,
        _ => KjExceptionType::Failed,
    };
    KjError::new(exception_type, format!("{context}: {e}"))
}
