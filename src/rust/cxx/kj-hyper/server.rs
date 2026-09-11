//! The hyper-backed inbound HTTP/1.1 server: each externally-accepted connection is served by
//! hyper, dispatching every request to a C++ `kj::HttpService`.
//!
//! # Runtime interplay
//!
//! Everything runs on one thread: the KJ event loop and the KJ thread's per-thread tokio
//! `current_thread` runtime (`kj_rs_tokio`), which the loop's `TokioEventPort` drives whenever
//! the KJ queue is empty. `serve()` (a Rust future kj-rs wraps into a `kj::Promise<void>`) is
//! the whole connection: it drives hyper's `serve_connection` future *inline on the KJ event
//! loop* (no per-connection tokio task) alongside a set of in-flight per-request service-call
//! futures. hyper's `service_fn` calls `kj::HttpService::request()`/`connect()` inline — the
//! request is the closure's argument and the response head is its return value, with no
//! request/head channel hop. Each poll of `serve()` enters this thread's tokio runtime context
//! (mirroring kj-rs-io's `with_runtime`) so hyper's timer and the socket's tokio reactor
//! registration still work while the KJ loop, not a `block_on`, is doing the polling. The
//! response *body* streams through a same-task one-slot(-ring) buffer (translate.rs
//! `ResponseBodyShared`) that the fixed-point driver hands between the service call and hyper
//! within one `serve()` poll; the request *body* still streams through a bounded channel
//! (translate.rs). Backpressure propagates end to end in both directions.
//!
//! `acceptWebSocket()` validates the handshake the way `kj::HttpServer` does (including
//! `MANUAL_COMPRESSION` extension matching), sends the 101, and returns a `WsSession` over the
//! hyper upgrade; CONNECT dispatches to `connect()` with a lazily-upgraded tunnel stream.
//!
//! # Cancellation
//!
//! The connection future holds a `watch` sender that drops when `serve_connection` finishes.
//! Every per-request KJ-side future selects the service call against that closure signal; when
//! the client goes away the service-call future is dropped, which synchronously cancels the
//! underlying C++ `kj::Promise` — preserving the cancellation contract workerd relies on.
//! Dropping `serve()` itself drops the connection future and all in-flight service calls
//! directly (no separate kill channel). Once a request upgrades, the watcher stops cancelling
//! the service call; disconnects then surface through the WebSocket/tunnel I/O itself, as in kj.
//!
//! # Drain
//!
//! `shutdown()` triggers hyper's `graceful_shutdown()`: an idle connection closes immediately,
//! an in-flight request runs to completion (`Connection: close`), then `serve()` resolves —
//! the per-connection half of `kj::HttpServer::drain()` + `listenHttpCleanDrain()`.
//! Divergences are documented on `newHyperHttpConnection()` in hyper-http.h.
//!
//! # I/O-stall watchdog
//!
//! Each connection tracks I/O progress via stall.rs (see its module docs for the wedge this
//! prevents): a write outstanding with zero bytes accepted for [`stall::WRITE_STALL_GRACE`]
//! (tightened to [`stall::WRITE_STALL_DRAIN_GRACE`] once draining, when zero-progress reads
//! count too) marks the connection dead, aborting it and cancelling its service call — exactly
//! what a kernel-reported disconnect would have done. Read-idleness outside of drain is never
//! a stall (keep-alive gaps, slow uploaders). Divergence from kj, which has no watchdog.

use std::cell::Cell;
use std::cell::RefCell;
use std::pin::Pin;
use std::pin::pin;
use std::rc::Rc;
use std::sync::Arc;
use std::sync::atomic;
use std::sync::atomic::AtomicBool;
use std::task::Context;
use std::task::Poll;
use std::task::Waker;

use bytes::Bytes;
use cxx::KjError;
use cxx::KjExceptionType;
use futures::FutureExt;
use futures::StreamExt;
use futures::future::LocalBoxFuture;
use futures::select_biased;
use futures::stream::FuturesUnordered;
use hyper::body::Incoming;
use hyper::server::conn::http1;
use hyper::service::service_fn;
use hyper_util::rt::TokioIo;
use hyper_util::rt::TokioTimer;
use kj::Result;
use kj::http::Headers;
use kj::http::HeadersRef;
use kj_rs::KjMaybe;
use tokio::sync::watch;

use crate::ffi;
use crate::ffi::HeaderTablePtr;
use crate::ffi::ServicePtr;
use crate::stall;
use crate::stall::StallIo;
use crate::stall::WriteStallTracker;
use crate::translate::BodyError;
use crate::translate::FramingHeaders;
use crate::translate::HyperResponseBodySink;
use crate::translate::ResponseBodyShared;
use crate::translate::ServeRequestBody;
use crate::translate::SinkKind;
use crate::translate::is_framing_header;
use crate::translate::translate_method;
use crate::translate::translate_response_headers;
use crate::upgraded_io::HyperTunnel;
use crate::upgraded_io::SharedIo;
use crate::ws;
use crate::ws::WsSession;
use crate::ws_ext;

/// The set of in-flight per-request service-call futures for one connection. hyper's
/// `service_fn` pushes a call here (via [`dispatch_inline`]) and `serve()`'s driver polls them
/// concurrently with the connection future — the same overlap the old cross-task dispatcher
/// gave, but with no channel between hyper and the KJ side.
type Inflight = Rc<RefCell<FuturesUnordered<LocalBoxFuture<'static, ()>>>>;

/// The rendezvous between a per-request service-call future (which produces the response head by
/// calling `kj::HttpService::Response::send()` / `acceptWebSocket()` / `connect()`'s accept, or
/// by generating a kj-style error) and hyper's `service_fn` closure (which must return that head
/// to hyper). It replaces the old `head_tx`/`head_rx` oneshot: the value is delivered through a
/// same-task `Rc<RefCell<_>>` cell polled directly under the KJ waker, never a tokio channel
/// whose waker would clone into a cross-thread promise fulfiller.
///
/// A head set during a call poll is observed within the same `serve()` poll (the driver's
/// fixed point); a head set OUTSIDE it — by the C++ service on its own KJ event while its
/// request() promise is still pending — is delivered by waking the stored `waker`.
#[derive(Default)]
struct HeadSlot {
    /// The response head, once produced. Taken by the `service_fn` closure when it resolves.
    head: Option<ResponseHead>,
    /// A head-producing decision (send/accept/reject/error, or an explicit abort) has been made.
    /// Mirrors the old `head_tx` being consumed: `!started` is exactly the old `head_tx.is_some()`
    /// ("the response has not started yet").
    started: bool,
    /// The `service_fn` closure may resolve now: either `head` is populated (return the response)
    /// or the call finished without producing one (abort the connection with no response).
    done: bool,
    /// Waker of the `service_fn` closure awaiting this head — the serve connection future's
    /// (same-thread KJ) waker. The head is often produced OUTSIDE `serve()`'s poll tree: the C++
    /// service runs on its own KJ events and may call send()/acceptWebSocket() while its
    /// request() promise is still pending (a WebSocket 101, a CONNECT accept, a streamed
    /// response head). Nothing else re-polls the connection future in that case — the service
    /// promise hasn't settled and there may be no socket activity — so delivery must wake it or
    /// the head never reaches the wire.
    waker: Option<Waker>,
}

impl HeadSlot {
    /// True while the response has not started (mirrors the old `head_tx.is_some()`).
    fn not_started(&self) -> bool {
        !self.started
    }

    /// Reserve the single response slot (mirrors `head_tx.take()`): errors if a response was
    /// already started. On success the caller must eventually [`deliver`](Self::deliver) or, on a
    /// mid-`send` error, leave the slot un-delivered (finalized as an abort when the call ends).
    fn begin(&mut self) -> Result<()> {
        if self.started {
            return Err(KjError::new(
                KjExceptionType::Failed,
                "already called send()".to_owned(),
            ));
        }
        self.started = true;
        Ok(())
    }

    /// Deliver the produced head to the closure (mirrors `head_tx.send(head)`). Also marks the
    /// slot started, so callers that deliver without a prior [`begin`](Self::begin) still block a
    /// second head. Wakes the awaiting closure (see `waker`).
    fn deliver(&mut self, head: ResponseHead) {
        self.started = true;
        self.head = Some(head);
        self.finish();
    }

    /// Suppress any response and abort the connection (mirrors dropping `head_tx` without
    /// sending). `head` stays `None`, so the closure resolves as a connection abort.
    fn suppress(&mut self) {
        self.started = true;
        self.finish();
    }

    /// Mark the slot resolvable and wake the awaiting `service_fn` closure.
    fn finish(&mut self) {
        self.done = true;
        if let Some(waker) = self.waker.take() {
            waker.wake();
        }
    }
}

// =======================================================================================
// Single-connection serving (workerd's rust-I/O-backend inbound path)

/// One already-accepted TCP connection served by hyper.
///
/// The accept side stays in C++ (`Server::HttpListener`), keeping kj's accept loop and
/// PeerIdentity/cf-blob handling intact; C++ hands the accepted `kj::AsyncIoStream` over and
/// its socket is taken natively (see `take_connection_socket` in ffi.rs). For https sockets a
/// rustls server config comes along and the TLS handshake runs here (tokio side). See
/// `newHyperHttpConnection()` in hyper-http.h.
pub struct HyperConnection {
    /// The `kj::HttpHeaderTable` request header objects are allocated against.
    ///
    /// Invariant: the creator guarantees the table outlives this connection. See
    /// [`HeaderTablePtr`].
    table: HeaderTablePtr,
    /// The C++ `kj::HttpService` every request is dispatched to.
    ///
    /// Invariant: the creator guarantees the service outlives this connection, and all calls
    /// happen on the KJ event-loop thread that owns the service.
    service: ServicePtr,
    /// The connection's transport, moved into the connection future by `serve()`: the native
    /// socket where one could be taken, else the consumer end of a duplex bridged by `pump`.
    stream: RefCell<Option<kj_rs_io::ServeIo>>,
    /// Present iff `stream` is the duplex tier: the pump that moves the bytes between the kj
    /// stream (which it owns) and the duplex. Driven by `serve()`'s fixed-point driver; the
    /// connection is only complete once it settles (the response tail has been flushed into
    /// the kj stream).
    pump: RefCell<Option<kj_rs_io::StreamPump>>,
    /// When present, the connection is TLS: the handshake runs before hyper takes over.
    tls: Option<tokio_rustls::TlsAcceptor>,
    /// Set to `true` by `shutdown()`; observed by the connection task (graceful shutdown).
    drain_tx: watch::Sender<bool>,
    /// Render WebSocket protocol errors the way workerd's `JsgifyWebSocketErrors` handler does
    /// (see `WsSession::jsgify_errors`).
    jsgify_websocket_errors: bool,
}

impl HyperConnection {
    /// Prepare to serve the connection's native socket `io`, performing a server-side TLS
    /// handshake first when `tls` is present. `table` and `service` carry the bridge caller's
    /// outlives/thread contract (discharged when they were wrapped in ffi.rs; see
    /// [`HeaderTablePtr`] and [`ServicePtr`]).
    pub(crate) fn new(
        table: HeaderTablePtr,
        service: ServicePtr,
        io: kj_rs_io::ServeIo,
        pump: Option<kj_rs_io::StreamPump>,
        jsgify_websocket_errors: bool,
        tls: Option<tokio_rustls::TlsAcceptor>,
    ) -> Self {
        // Match kj-http's latency-oriented TCP behavior for request/response traffic.
        let _ = io.set_nodelay(true);
        Self {
            table,
            service,
            stream: RefCell::new(Some(io)),
            pump: RefCell::new(pump),
            tls,
            drain_tx: watch::channel(false).0,
            jsgify_websocket_errors,
        }
    }

    /// Serve the connection until it closes (client disconnect, `shutdown()`, or upgrade
    /// handoff completion). The returned future *is* the connection: it drives hyper's
    /// `serve_connection` future inline on the KJ loop together with the in-flight per-request
    /// service calls. Dropping it aborts the connection and cancels every in-flight service call.
    ///
    /// A failed TLS handshake resolves `serve()` with the handshake error (which the caller
    /// logs, mirroring kj's "error accepting tls connection"), except peer-disconnect
    /// (DISCONNECTED) failures, which resolve cleanly like kj's TLS receiver dropping them.
    pub async fn serve(&self) -> Result<()> {
        let stream = self.stream.borrow_mut().take().ok_or_else(|| {
            KjError::new(
                KjExceptionType::Failed,
                "serve() was already called on this connection".to_owned(),
            )
        })?;

        // hyper's connection future must be polled inside a tokio runtime context so `ServeIo`'s
        // AsyncRead/Write can (re-)register with the reactor and hyper's `TokioTimer` can arm the
        // timer wheel — kj-rs-io's `with_runtime` does the same for its bridge futures. The KJ
        // loop, not a `block_on`, does the polling, so we enter the context per poll.
        let handle = kj_rs_tokio::current_handle().ok_or_else(|| {
            KjError::new(
                KjExceptionType::Failed,
                "no kj-rs-tokio runtime on this thread; the hyper server requires a \
                 TokioEventPort"
                    .to_owned(),
            )
        })?;

        let pump = self.pump.borrow_mut().take();
        let drain_rx = self.drain_tx.subscribe();
        let tls = self.tls.clone();

        // In-flight per-request service calls, fed inline by hyper's `service_fn` and drained by
        // the driver below. Shared with the connection future (which pushes onto it).
        let inflight: Inflight = Rc::new(RefCell::new(FuturesUnordered::new()));
        // Per-connection response-body progress flag (Stage 3): a streaming response body's
        // same-task buffer (translate.rs `ResponseBodyShared`) sets this whenever a chunk moves or
        // its terminal state changes, so the fixed-point driver below re-polls the opposite end
        // (writer↔hyper) within the same `serve()` poll instead of costing a fresh KJ loop turn.
        let body_activity: Rc<Cell<bool>> = Rc::new(Cell::new(false));
        // Set by the connection future if a TLS handshake fails; surfaced after the driver ends.
        let tls_error: Rc<Cell<Option<KjError>>> = Rc::new(Cell::new(None));

        // The socket is already registered with this thread's loop runtime (kj-rs-io created it
        // there, on both the unwrap and fd tiers). The connection future owns the connection's
        // `conn_alive` sender and drops it on exit, cancelling in-flight non-upgraded calls.
        let conn_future = connect_and_serve(
            stream,
            tls,
            self.table,
            self.service,
            self.jsgify_websocket_errors,
            inflight.clone(),
            drain_rx,
            tls_error.clone(),
            body_activity.clone(),
        );
        let mut conn_future = pin!(conn_future);
        let mut conn_done = false;

        // The duplex pump, when this is the pump tier: it IS the transport, so it is polled in
        // the same fixed point (its progress is what unblocks hyper's reads/writes) and gates
        // completion (the response tail must reach the kj stream). It ends when both directions
        // close — the connection future drops its duplex end on exit, which drains and ends the
        // pump. Pump failures surface to hyper as transport EOF/errors, so its own result is
        // not an error of serve() itself.
        let mut pump_future = pump.map(futures::FutureExt::fuse);
        let mut pump_done = pump_future.is_none();

        // The fused driver: poll the connection future and the in-flight service calls to a fixed
        // point each turn. Polling the connection may run `service_fn`, which pushes a new call
        // onto `inflight` and then awaits its head; re-polling `inflight` runs that call until it
        // produces the head, and re-polling the connection then observes it — all within one KJ
        // poll, no channel wake, no `block_on` handoff. `serve()` resolves once the connection
        // has finished *and* every in-flight call (e.g. a WebSocket/tunnel session that outlived
        // the connection future) has drained.
        std::future::poll_fn(|cx| {
            let _guard = handle.enter();
            loop {
                let mut progressed = false;

                if let Some(pump) = &mut pump_future {
                    if !pump_done && pump.poll_unpin(cx).is_ready() {
                        pump_done = true;
                        progressed = true;
                    }
                }

                // Drain any in-flight calls that are ready (a completed call is progress).
                while let Poll::Ready(Some(())) = inflight.borrow_mut().poll_next_unpin(cx) {
                    progressed = true;
                }

                if !conn_done {
                    // A poll of the connection may push new calls onto `inflight`; a growth in
                    // its length is progress that must be followed by another `inflight` poll.
                    let before = inflight.borrow().len();
                    if conn_future.as_mut().poll(cx).is_ready() {
                        conn_done = true;
                        progressed = true;
                    }
                    if inflight.borrow().len() != before {
                        progressed = true;
                    }
                }

                // A streaming response body chunk moved between a service call and hyper this
                // iteration (see translate.rs `ResponseBodyShared`): keep the fixed point going so
                // the opposite end is re-polled within this same `serve()` poll rather than
                // waiting for a fresh KJ loop turn. `write()` parks the producer's waker and
                // `poll_frame` wakes it, so the actual re-poll of the blocked side still happens;
                // this flag only stops the loop from breaking before the handoff completes.
                if body_activity.replace(false) {
                    progressed = true;
                }

                if !progressed {
                    break;
                }
            }

            if conn_done && pump_done && inflight.borrow().is_empty() {
                Poll::Ready(())
            } else {
                Poll::Pending
            }
        })
        .await;

        if let Some(error) = tls_error.take() {
            // Peer went away mid-handshake: not an error worth surfacing (kj's TLS receiver
            // silently drops DISCONNECTED handshake failures too).
            if error.exception_type() != KjExceptionType::Disconnected {
                return Err(error);
            }
        }
        Ok(())
    }

    /// Begin a graceful shutdown: an idle connection closes immediately, an in-flight request
    /// finishes (its response carries `Connection: close`), after which `serve()` resolves.
    pub fn shutdown(&self) {
        let _ = self.drain_tx.send(true);
    }
}

// =======================================================================================
// tokio side: connection task, hyper service

/// A parsed request handed from hyper's `service_fn` to the inline dispatcher.
enum IncomingCall {
    Request(IncomingRequest),
    Connect(IncomingConnect),
}

/// A regular (possibly upgradeable) HTTP request.
struct IncomingRequest {
    method: http::Method,
    uri: http::Uri,
    headers: http::HeaderMap,
    body: Incoming,
    /// hyper's upgrade handle for this request; resolves with the raw connection once a 101
    /// response head has been written. Used by `acceptWebSocket()`.
    on_upgrade: hyper::upgrade::OnUpgrade,
}

/// A CONNECT request, dispatched to the C++ service's `connect()`.
struct IncomingConnect {
    authority: String,
    headers: http::HeaderMap,
    /// Resolves with the raw connection once the 2xx response head has been written.
    on_upgrade: hyper::upgrade::OnUpgrade,
}

/// The response body hyper serializes on the serve path (Stage 3 of the kj↔tokio serve fusion).
/// Unlike the cross-task [`crate::translate::ServerBody`] (which must be `Send`),
/// this is same-task and `!Send`: [`Self::Buffer`] holds an `Rc` into a [`ResponseBodyShared`]
/// one-slot(-ring) buffer shared with the KJ-side `HyperResponseBodySink`, handed over within one
/// `serve()` poll via the fixed-point driver (see the module docs and translate.rs). No
/// cross-task channel, no cloned cross-thread waker.
enum ServeBody {
    Empty,
    Full(Option<Bytes>),
    Buffer {
        shared: Rc<RefCell<ResponseBodyShared>>,
        length: Option<u64>,
    },
}

impl http_body::Body for ServeBody {
    type Data = Bytes;
    type Error = BodyError;

    fn poll_frame(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
    ) -> Poll<Option<std::result::Result<http_body::Frame<Bytes>, BodyError>>> {
        match self.get_mut() {
            Self::Empty => Poll::Ready(None),
            Self::Full(data) => Poll::Ready(data.take().map(|b| Ok(http_body::Frame::data(b)))),
            // On Pending the buffer parks `cx`'s waker (see translate.rs `consumer_waker`), so a
            // chunk written by the C++ service on its own KJ event — outside `serve()`'s poll
            // tree — wakes the connection future; a producer inside the poll tree is observed
            // the same poll via the connection `activity` flag.
            Self::Buffer { shared, .. } => ResponseBodyShared::poll_next_chunk(shared, cx)
                .map(|opt| opt.map(|res| res.map(http_body::Frame::data))),
        }
    }

    fn is_end_stream(&self) -> bool {
        match self {
            Self::Empty => true,
            Self::Full(data) => data.is_none(),
            Self::Buffer { .. } => false,
        }
    }

    fn size_hint(&self) -> http_body::SizeHint {
        match self {
            Self::Empty | Self::Full(None) => http_body::SizeHint::with_exact(0),
            Self::Full(Some(data)) => http_body::SizeHint::with_exact(data.len() as u64),
            Self::Buffer {
                length: Some(n), ..
            } => http_body::SizeHint::with_exact(*n),
            Self::Buffer { length: None, .. } => http_body::SizeHint::default(),
        }
    }
}

impl Drop for ServeBody {
    fn drop(&mut self) {
        // hyper dropped the response body (connection done/aborted): tell the KJ-side producer
        // the consumer is gone so blocked writes fail with DISCONNECTED and
        // `when_write_disconnected` resolves.
        if let Self::Buffer { shared, .. } = self {
            ResponseBodyShared::consumer_gone(shared);
        }
    }
}

/// The response head produced by the service call, handed back to hyper's `service_fn` closure
/// through the request's [`HeadSlot`].
struct ResponseHead {
    status: http::StatusCode,
    /// Non-canonical status text, if any.
    reason: Option<Vec<u8>>,
    headers: http::HeaderMap,
    /// Original header-name spellings for hyper's encoder (kj-http writes names exactly as the
    /// application spelled them; anything absent falls back to title-casing).
    case: hyper::ext::HeaderCaseMap,
    body: ServeBody,
}

/// What the per-connection loop should do next.
enum ConnStep {
    Done,
    /// Drain was signalled (`true` means graceful shutdown was actually requested).
    Drain(bool),
}

// =======================================================================================
// The connection future: hyper `serve_connection` driven inline on the KJ loop, plus the
// I/O-stall watchdog (see the module docs above and stall.rs, the implementation shared with the
// outbound client in client.rs).

/// Perform the server-side TLS handshake if `tls` is present, then serve the connection with
/// hyper. On a handshake failure the error is stashed in `tls_error` for `serve()` to surface.
#[expect(clippy::too_many_arguments)]
async fn connect_and_serve(
    stream: kj_rs_io::ServeIo,
    tls: Option<tokio_rustls::TlsAcceptor>,
    table: HeaderTablePtr,
    service: ServicePtr,
    jsgify_websocket_errors: bool,
    inflight: Inflight,
    drain_rx: watch::Receiver<bool>,
    tls_error: Rc<Cell<Option<KjError>>>,
    body_activity: Rc<Cell<bool>>,
) {
    match tls {
        None => {
            run_connection(
                stream,
                table,
                service,
                jsgify_websocket_errors,
                inflight,
                drain_rx,
                body_activity,
            )
            .await;
        }
        Some(acceptor) => match acceptor.accept(stream).await {
            Ok(tls_stream) => {
                run_connection(
                    tls_stream,
                    table,
                    service,
                    jsgify_websocket_errors,
                    inflight,
                    drain_rx,
                    body_activity,
                )
                .await;
            }
            Err(e) => {
                tls_error.set(Some(crate::tls::kj_error_for_tls(&e)));
            }
        },
    }
}

/// Transport wrapper implementing the read-activity half of kj's drain rule (see
/// [`run_connection`]): `dirty` is set whenever bytes arrive from the peer, and cleared by the
/// dispatcher when a request head is consumed and when a service call completes. At drain time
/// an idle connection with `dirty` set holds a partially-received request (kj's
/// `!isCleanDrain()`), which must be served — with `Connection: close` — rather than closed
/// away. Atomic only because hyper's upgrade machinery requires the transport to be `Send`; it
/// is only ever touched from the one KJ loop thread.
struct DrainMarkIo<S> {
    inner: S,
    dirty: Arc<AtomicBool>,
}

impl<S: tokio::io::AsyncRead + Unpin> tokio::io::AsyncRead for DrainMarkIo<S> {
    fn poll_read(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &mut tokio::io::ReadBuf<'_>,
    ) -> Poll<std::io::Result<()>> {
        let this = self.get_mut();
        let before = buf.filled().len();
        let poll = Pin::new(&mut this.inner).poll_read(cx, buf);
        if matches!(poll, Poll::Ready(Ok(()))) && buf.filled().len() > before {
            this.dirty.store(true, atomic::Ordering::Relaxed);
        }
        poll
    }
}

impl<S: tokio::io::AsyncWrite + Unpin> tokio::io::AsyncWrite for DrainMarkIo<S> {
    fn poll_write(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &[u8],
    ) -> Poll<std::io::Result<usize>> {
        Pin::new(&mut self.get_mut().inner).poll_write(cx, buf)
    }

    fn poll_write_vectored(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        bufs: &[std::io::IoSlice<'_>],
    ) -> Poll<std::io::Result<usize>> {
        Pin::new(&mut self.get_mut().inner).poll_write_vectored(cx, bufs)
    }

    fn is_write_vectored(&self) -> bool {
        self.inner.is_write_vectored()
    }

    fn poll_flush(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<std::io::Result<()>> {
        Pin::new(&mut self.get_mut().inner).poll_flush(cx)
    }

    fn poll_shutdown(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<std::io::Result<()>> {
        Pin::new(&mut self.get_mut().inner).poll_shutdown(cx)
    }
}

/// Serve one connection with hyper (over plain TCP or an established TLS stream). Exits when the
/// connection closes (client disconnect, graceful drain, or upgrade handoff). hyper's
/// `service_fn` dispatches each request inline (see [`dispatch_inline`]). Dropping the
/// `conn_alive` sender on exit tells all in-flight service calls for this connection to cancel
/// (except upgraded ones, which own the socket from that point on).
async fn run_connection<S>(
    stream: S,
    table: HeaderTablePtr,
    service: ServicePtr,
    jsgify_websocket_errors: bool,
    inflight: Inflight,
    mut drain_rx: watch::Receiver<bool>,
    body_activity: Rc<Cell<bool>>,
) where
    S: tokio::io::AsyncRead + tokio::io::AsyncWrite + Send + Unpin + 'static,
{
    // Dropped when this future exits; in-flight non-upgraded calls observe it and cancel.
    let (conn_alive_tx, conn_alive_rx) = watch::channel(());

    // Write-stall watchdog: see the module docs and stall.rs.
    let stall_tracker = WriteStallTracker::new();
    let mut stall =
        pin!(stall::write_stall_watchdog(stall_tracker.clone(), drain_rx.clone()).fuse());
    let stream = StallIo::new(stream, stall_tracker);

    // kj's drain rule (HttpServer::Connection::loop): at drain, an idle connection with an empty
    // receive buffer closes immediately, but one holding a partially-received request serves that
    // request — its response carrying `Connection: close` — and closes after it. hyper's
    // graceful_shutdown treats buffered partial header bytes as idle and would close them away,
    // so the decision is made here from `dirty_read` (bytes received since the last consumed
    // request head / completed call; see DrainMarkIo).
    let dirty_read = Arc::new(AtomicBool::new(false));
    // When set, the next response head gains `Connection: close` (kj's draining closeAfterSend),
    // after which hyper closes the connection on its own.
    let close_next_response: Rc<Cell<bool>> = Rc::new(Cell::new(false));
    let stream = DrainMarkIo {
        inner: stream,
        dirty: dirty_read.clone(),
    };

    let svc = service_fn({
        let inflight = inflight.clone();
        let dirty_read = dirty_read.clone();
        let close_next_response = close_next_response.clone();
        move |req| {
            dispatch_inline(
                req,
                table,
                service,
                jsgify_websocket_errors,
                inflight.clone(),
                conn_alive_rx.clone(),
                body_activity.clone(),
                dirty_read.clone(),
                close_next_response.clone(),
            )
        }
    });
    let conn = http1::Builder::new()
        .timer(TokioTimer::new())
        // kj::HttpServer does not send a Date header; match it.
        .auto_date_header(false)
        // Emit header names in canonical Title-Case on the wire (Content-Type, ...) rather than
        // the http crate's lowercase form, mirroring kj::HttpServer's serialization (the same
        // setting the outbound client uses; some peers and tests assert the exact raw case).
        .title_case_headers(true)
        // kj-http caps the whole message head at 128 KiB (MAX_BUFFER) with no per-header count
        // limit; hyper's default of 100 headers is far stricter and rejects requests kj-http
        // accepts. 4096 is well past anything a real peer sends while keeping hyper's
        // per-message parse scratch allocation modest.
        .max_headers(4096)
        .serve_connection(TokioIo::new(stream), svc)
        // Enables 101/CONNECT handoff: hyper resolves the request's OnUpgrade after writing the
        // response head, and this connection future completes.
        .with_upgrades();
    let mut conn = pin!(conn);

    let mut draining = *drain_rx.borrow_and_update();
    if draining {
        conn.as_mut().graceful_shutdown();
    }
    loop {
        let step = if draining {
            select_biased! {
                () = stall.as_mut() => ConnStep::Done,  // write-dead peer: abort
                // Connection finished; result intentionally ignored (errors surface per-request).
                _ = conn.as_mut().fuse() => ConnStep::Done,
            }
        } else {
            select_biased! {
                changed = drain_rx.changed().fuse() => {
                    // On Err the server itself is gone; stop watching and abort.
                    if changed.is_err() {
                        ConnStep::Done
                    } else {
                        ConnStep::Drain(*drain_rx.borrow())
                    }
                }
                () = stall.as_mut() => ConnStep::Done,  // write-dead peer: abort
                _ = conn.as_mut().fuse() => ConnStep::Done,
            }
        };
        match step {
            ConnStep::Done => break,
            ConnStep::Drain(shutdown) => {
                draining = true;
                if shutdown {
                    if inflight.borrow().is_empty() && dirty_read.load(atomic::Ordering::Relaxed) {
                        // kj's dirty-buffer drain: a partially-received request must be served,
                        // its response carrying `Connection: close`, after which hyper closes
                        // the connection itself. graceful_shutdown here would treat the buffered
                        // partial head as idle and close it away.
                        close_next_response.set(true);
                    } else {
                        // In-flight request (hyper finishes it with Connection: close) or truly
                        // idle (closes immediately).
                        conn.as_mut().graceful_shutdown();
                    }
                }
            }
        }
    }
    drop(conn_alive_tx);
}

/// Error used to make hyper abort a connection without sending a response (mirroring how
/// `kj::HttpServer` closes the connection for DISCONNECTED-type application errors).
#[derive(Debug)]
struct ConnectionAborted(&'static str);
impl std::fmt::Display for ConnectionAborted {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(self.0)
    }
}

impl std::error::Error for ConnectionAborted {}

/// hyper's `service_fn` closure: dispatch the request to the C++ `kj::HttpService` *inline*.
/// The request is the argument and the response head the return value — no request/head channel
/// hop. The service call runs as its own future pushed onto `inflight` (so its body streaming
/// can outlive this closure's return, and so it is driven concurrently with the connection),
/// while this closure awaits the head the call produces through a same-task [`HeadSlot`]. hyper
/// drops this future if the connection dies before the head is produced.
async fn dispatch_inline(
    mut req: http::Request<Incoming>,
    table: HeaderTablePtr,
    service: ServicePtr,
    jsgify_websocket_errors: bool,
    inflight: Inflight,
    conn_alive: watch::Receiver<()>,
    body_activity: Rc<Cell<bool>>,
    dirty_read: Arc<AtomicBool>,
    close_next_response: Rc<Cell<bool>>,
) -> std::result::Result<http::Response<ServeBody>, ConnectionAborted> {
    // The request head was consumed off the wire: the receive buffer is "clean" again as far as
    // kj's drain rule is concerned (see DrainMarkIo / run_connection).
    dirty_read.store(false, atomic::Ordering::Relaxed);
    // The upgrade handle must be extracted before the request is torn apart; it is how the KJ
    // side gets at the raw connection after a 101 (WebSocket) or 2xx (CONNECT) response.
    let on_upgrade = hyper::upgrade::on(&mut req);

    let call = if req.method() == http::Method::CONNECT {
        // Mirror kj::HttpServer::onConnect: CONNECT requests must not carry a payload; a
        // Content-Length or Transfer-Encoding header is rejected up front with a 400.
        if req.headers().contains_key(http::header::CONTENT_LENGTH)
            || req.headers().contains_key(http::header::TRANSFER_ENCODING)
        {
            let mut response = simple_response(http::StatusCode::BAD_REQUEST, "ERROR: Bad Request");
            response.headers_mut().insert(
                http::header::CONNECTION,
                http::HeaderValue::from_static("close"),
            );
            return Ok(response);
        }
        let (parts, _body) = req.into_parts();
        IncomingCall::Connect(IncomingConnect {
            // The request-target of a CONNECT is the authority; hyper stores it in the URI.
            authority: parts
                .uri
                .authority()
                .map_or_else(|| parts.uri.to_string(), std::string::ToString::to_string),
            headers: parts.headers,
            on_upgrade,
        })
    } else {
        let (parts, body) = req.into_parts();
        IncomingCall::Request(IncomingRequest {
            method: parts.method,
            uri: parts.uri,
            headers: parts.headers,
            body,
            on_upgrade,
        })
    };

    // Drive the service call as its own future so its response-body streaming continues after we
    // return the head. It reports the head back through this slot.
    let head_slot: Rc<RefCell<HeadSlot>> = Rc::new(RefCell::new(HeadSlot::default()));
    inflight.borrow_mut().push(
        handle_call(
            table,
            service,
            jsgify_websocket_errors,
            call,
            head_slot.clone(),
            conn_alive,
            body_activity,
            dirty_read,
            close_next_response,
        )
        .boxed_local(),
    );

    // Await the head. The stored waker covers heads produced outside `serve()`'s poll tree (the
    // C++ service delivering on its own KJ event mid-request); a head produced during a call
    // poll is additionally observed on the next connection poll within the same `serve()` poll.
    std::future::poll_fn(|cx| {
        let mut slot = head_slot.borrow_mut();
        if slot.done {
            match slot.head.take() {
                Some(head) => Poll::Ready(Ok(build_response(head))),
                // The service produced no head (DISCONNECTED, or the client vanished): close the
                // connection without a response, as kj does.
                None => Poll::Ready(Err(ConnectionAborted("connection aborted by the service"))),
            }
        } else {
            slot.waker = Some(cx.waker().clone());
            Poll::Pending
        }
    })
    .await
}

fn build_response(head: ResponseHead) -> http::Response<ServeBody> {
    let mut response = http::Response::new(head.body);
    *response.status_mut() = head.status;
    *response.headers_mut() = head.headers;
    response.extensions_mut().insert(head.case);
    if let Some(reason) = head.reason
        && let Ok(reason) = hyper::ext::ReasonPhrase::try_from(reason)
    {
        response.extensions_mut().insert(reason);
    }
    response
}

/// A complete fixed-body response built on one side or the other (error responses).
fn simple_response(status: http::StatusCode, message: &str) -> http::Response<ServeBody> {
    let mut response = http::Response::new(ServeBody::Full(Some(Bytes::copy_from_slice(
        message.as_bytes(),
    ))));
    *response.status_mut() = status;
    response.headers_mut().insert(
        http::header::CONTENT_TYPE,
        http::HeaderValue::from_static("text/plain"),
    );
    response.headers_mut().insert(
        http::header::CONTENT_LENGTH,
        http::HeaderValue::from(message.len() as u64),
    );
    response
}

/// Everything `acceptWebSocket()` needs to know about the request, captured at dispatch time.
struct WebSocketRequestInfo {
    is_get: bool,
    /// The request had `Upgrade: websocket` (kj's `HttpHeaders::isWebSocket()`).
    is_websocket: bool,
    version_ok: bool,
    key: Option<String>,
    /// The request's raw `Sec-WebSocket-Extensions` value (first header instance), for
    /// MANUAL-mode matching against the application's response config.
    extensions: Option<String>,
}

impl WebSocketRequestInfo {
    fn from_request(method: &http::Method, headers: &http::HeaderMap) -> Self {
        Self {
            is_get: *method == http::Method::GET,
            is_websocket: headers
                .get(http::header::UPGRADE)
                .is_some_and(|v| v.as_bytes().eq_ignore_ascii_case(b"websocket")),
            version_ok: headers
                .get(http::header::SEC_WEBSOCKET_VERSION)
                .is_some_and(|v| v.as_bytes() == b"13"),
            key: headers
                .get(http::header::SEC_WEBSOCKET_KEY)
                .and_then(|v| std::str::from_utf8(v.as_bytes()).ok())
                .map(std::borrow::ToOwned::to_owned),
            extensions: headers
                .get(http::header::SEC_WEBSOCKET_EXTENSIONS)
                .and_then(|v| std::str::from_utf8(v.as_bytes()).ok())
                .map(std::borrow::ToOwned::to_owned),
        }
    }
}

/// State shared between the per-request service-call future and the `kj::HttpService::Response`
/// handed to the C++ service, so the call can generate kj-style error responses when the
/// service fails or forgets to respond.
struct ResponseShared {
    is_head: bool,
    /// Delivers the response head to hyper's `service_fn` closure (see [`HeadSlot`]).
    head_slot: Rc<RefCell<HeadSlot>>,
    /// Present while a streaming response body is in flight: the dispatcher's handle to the
    /// same-task [`ResponseBodyShared`] buffer (translate.rs), used to signal a clean EOF on
    /// success or an abort on failure. Replaces the old `body_tx`/`body_abort` mpsc+oneshot:
    /// there is no cross-task channel, and the "clean chunked terminator can only happen after a
    /// successful request" rule (the abort must win the race) is expressed by
    /// [`ResponseBodyShared::finish`]/[`ResponseBodyShared::abort`] rather than by holding a `tx`
    /// clone until success.
    body: Option<Rc<RefCell<crate::translate::ResponseBodyShared>>>,
    /// The connection's response-body progress flag, cloned into each streaming body buffer this
    /// request creates so `serve()`'s driver re-polls the writer/hyper handoff (see [`HeadSlot`]).
    body_activity: Rc<Cell<bool>>,
    /// WebSocket-upgrade inputs, present for non-CONNECT requests.
    ws_info: Option<WebSocketRequestInfo>,
    on_upgrade: Option<hyper::upgrade::OnUpgrade>,
    /// Set when the request successfully upgraded (101 sent / CONNECT accepted); the dispatcher
    /// then stops treating connection-task exit as a client disconnect.
    upgraded: Rc<Cell<bool>>,
    /// The 400 head was already sent for a bad WebSocket handshake; the connection should close.
    sent_websocket_error: bool,
    /// See `WsSession::jsgify_errors`.
    jsgify_websocket_errors: bool,
    /// kj's draining closeAfterSend (see run_connection's drain rule): when set, `send()` adds
    /// `Connection: close` to the response head, after which hyper closes the connection.
    close_next_response: Rc<Cell<bool>>,
}

/// The Rust side of the `kj::HttpService::Response` given to the C++ service (wrapped by
/// `HyperResponseImpl` in hyper-server-ffi.c++). `send()` applies `kj::HttpServer`'s framing
/// rules and hands the head to the connection task; `accept_websocket()` mirrors
/// `kj::HttpServer`'s `acceptWebSocket()`.
pub struct HyperResponseSender {
    shared: Rc<RefCell<ResponseShared>>,
}

impl HyperResponseSender {
    /// Corresponds to `kj::HttpService::Response::send()`. Applies kj-http's framing rules
    /// (Content-Length vs chunked, HEAD, 204/205/304) and returns the response body sink.
    pub fn send(
        &self,
        status_code: u32,
        status_text: &[u8],
        headers: &ffi::HttpHeaders,
        expected_body_size: KjMaybe<u64>,
    ) -> Result<Box<HyperResponseBodySink>> {
        let expected_body_size: Option<u64> = expected_body_size.into();
        let mut shared = self.shared.borrow_mut();
        // Matches kj::HttpServer's KJ_REQUIRE text for a second send(). Reserving the slot before
        // validating the status mirrors the old `head_tx.take()`: a later error leaves the slot
        // started-but-undelivered, which the dispatch closure finalizes as a connection abort.
        shared.head_slot.borrow_mut().begin()?;

        let status = u16::try_from(status_code)
            .ok()
            .and_then(|code| http::StatusCode::from_u16(code).ok())
            .ok_or_else(|| {
                KjError::new(
                    KjExceptionType::Failed,
                    format!("invalid HTTP status code: {status_code}"),
                )
            })?;

        let reason = status_text_for(status, status_text);
        let is_head = shared.is_head;

        // --- kj::HttpServer framing rules (see HttpServer::Connection::send in kj/compat/http.c++).
        let no_body_status =
            status == http::StatusCode::NO_CONTENT || status == http::StatusCode::NOT_MODIFIED;
        let reset_content = status == http::StatusCode::RESET_CONTENT;

        // For HEAD requests, an application-supplied Content-Length or Transfer-Encoding header
        // is passed through instead of the computed framing (kj's HEAD passthrough rule).
        let entries = HeadersRef::from(headers).entries();
        let app_has_framing = is_head && entries.iter().any(|e| is_framing_header(&e.name));
        let (mut map, case) = translate_response_headers(
            &entries,
            if app_has_framing {
                FramingHeaders::Keep
            } else {
                FramingHeaders::Drop
            },
        )?;

        if shared.close_next_response.get() {
            // kj's draining closeAfterSend: the connection-level slot precedes everything in
            // kj's serialization, so the header goes first. hyper closes the connection after
            // a response carrying Connection: close.
            let mut with_close = http::HeaderMap::with_capacity(map.len() + 1);
            with_close.append(
                http::header::CONNECTION,
                http::HeaderValue::from_static("close"),
            );
            for (n, v) in &map {
                with_close.append(n.clone(), v.clone());
            }
            map = with_close;
        }

        let mut content_length: Option<u64> = None;
        let mut streaming: Option<Option<u64>> = None; // Some(len) => body streams, len per framing
        let mut sink_kind = SinkKind::Discard;

        if no_body_status {
            // 204/304: no entity-body, no framing headers. Writing to the sink is an error.
            sink_kind = SinkKind::Null;
        } else if reset_content {
            // 205: no body, but it must be explicitly encoded as empty.
            content_length = Some(0);
            sink_kind = SinkKind::Null;
        } else if let Some(size) = expected_body_size {
            // HACK (from kj): a zero expectedBodySize on a HEAD response means "don't set a
            // Content-Length header at all".
            if (!is_head || size > 0) && !app_has_framing {
                content_length = Some(size);
            }
            if is_head {
                sink_kind = SinkKind::Discard;
            } else {
                streaming = Some(Some(size));
            }
        } else if is_head {
            // Unknown size on a HEAD response: no body. (kj advertises "Transfer-Encoding:
            // chunked" here; hyper never writes framing headers for HEAD — observable divergence.)
            sink_kind = SinkKind::Discard;
        } else {
            // Unknown size: hyper uses chunked transfer encoding automatically.
            streaming = Some(None);
        }

        // kj::HttpServer serializes headers in header-table id order, with the computed framing
        // header (Content-Length / Transfer-Encoding) occupying its builtin slot: AFTER the five
        // lower-id builtins (Connection, Keep-Alive, TE, Trailer, Upgrade) and BEFORE everything
        // else. The translated map preserves kj's iteration order (indexed by id, then
        // unindexed), so those five can only appear as a leading run — insert the framing header
        // right after that run so responses match kj byte for byte. Pre-inserting
        // Transfer-Encoding also just pins its position: hyper sees it and chunk-encodes as it
        // would have anyway.
        let framing: Option<(http::header::HeaderName, http::HeaderValue)> =
            if let Some(length) = content_length {
                Some((
                    http::header::CONTENT_LENGTH,
                    http::HeaderValue::from(length),
                ))
            } else if streaming == Some(None) && !is_head {
                Some((
                    http::header::TRANSFER_ENCODING,
                    http::HeaderValue::from_static("chunked"),
                ))
            } else {
                None
            };
        if let Some((name, value)) = framing {
            fn precedes_framing(name: &http::header::HeaderName) -> bool {
                matches!(
                    name.as_str(),
                    "connection" | "keep-alive" | "te" | "trailer" | "upgrade"
                )
            }
            let mut ordered = http::HeaderMap::with_capacity(map.len() + 1);
            let mut inserted = false;
            for (n, v) in &map {
                if !inserted && !precedes_framing(n) {
                    ordered.append(name.clone(), value.clone());
                    inserted = true;
                }
                ordered.append(n.clone(), v.clone());
            }
            if !inserted {
                ordered.append(name, value);
            }
            map = ordered;
        }

        let body = match streaming {
            Some(length) if length != Some(0) => {
                // Same-task streaming buffer (Stage 3): no cross-task mpsc, no abort oneshot.
                let (buf, sink) =
                    ResponseBodyShared::new_pair(length, shared.body_activity.clone());
                shared.body = Some(buf.clone());
                sink_kind = sink;
                ServeBody::Buffer {
                    shared: buf,
                    length,
                }
            }
            Some(_zero) => {
                // Content-Length: 0. The sink still enforces kj's "overwrote Content-Length".
                sink_kind = SinkKind::Exhausted;
                ServeBody::Empty
            }
            None => ServeBody::Empty,
        };

        // If the connection is already gone the head cannot be delivered; kj's send() would
        // also succeed here (it only buffers) with the failure surfacing on write, which is
        // exactly what the returned sink will do.
        shared.head_slot.borrow_mut().deliver(ResponseHead {
            status,
            reason,
            headers: map,
            case,
            body,
        });

        Ok(Box::new(HyperResponseBodySink { kind: sink_kind }))
    }

    /// Corresponds to `kj::HttpService::Response::acceptWebSocket()`. Mirrors `kj::HttpServer`'s
    /// implementation: handshake validation (with kj's exact error texts and its
    /// 400-then-throw-DISCONNECTED behavior for client protocol errors), `MANUAL_COMPRESSION`
    /// extension matching between the request's offers and the application-supplied response
    /// config, and the computed 101 response head. Application headers pass through verbatim
    /// except the connection-level and handshake-owned ones (kj's
    /// `WEBSOCKET_CONNECTION_HEADERS` override).
    #[expect(
        clippy::expect_used,
        reason = "documented handshake-state invariants: after the `head_slot.started` early-return above, `ws_info` is Some (set when the request was received); and base64 accept keys / generated permessage-deflate extension strings are always valid HTTP HeaderValues"
    )]
    pub fn accept_websocket(&self, headers: &ffi::HttpHeaders) -> Result<Box<WsSession>> {
        let mut shared = self.shared.borrow_mut();

        {
            let info = shared.ws_info.as_ref().ok_or_else(|| {
                KjError::new(
                    KjExceptionType::Failed,
                    "can't call acceptWebSocket() if the request headers didn't have Upgrade: \
                     WebSocket"
                        .to_owned(),
                )
            })?;
            if !info.is_websocket {
                return Err(KjError::new(
                    KjExceptionType::Failed,
                    "can't call acceptWebSocket() if the request headers didn't have Upgrade: \
                     WebSocket"
                        .to_owned(),
                ));
            }
        }
        if shared.head_slot.borrow().started {
            return Err(KjError::new(
                KjExceptionType::Failed,
                "already called send()".to_owned(),
            ));
        }
        let info = shared.ws_info.as_ref().expect("checked above");
        if !info.is_get {
            return Err(KjError::new(
                KjExceptionType::Failed,
                "WebSocket must be initiated with a GET request.".to_owned(),
            ));
        }

        if !info.version_ok {
            return Self::send_websocket_error(
                &mut shared,
                "The requested WebSocket version is not supported.",
            );
        }
        let Some(key) = info.key.clone() else {
            return Self::send_websocket_error(&mut shared, "Missing Sec-WebSocket-Key");
        };

        // --- MANUAL_COMPRESSION: use the application-provided response headers as the manual
        // config and find a configuration compatible with the client's request offers.
        let app_headers = HeadersRef::from(headers);
        let accepted_parameters = app_headers
            .get(kj::http::HeaderId::SEC_WEBSOCKET_EXTENSIONS)
            .and_then(|value| std::str::from_utf8(value).ok())
            .and_then(ws_ext::try_parse_extension_offers)
            .and_then(|manual_config| {
                let request_offers = shared.ws_info.as_ref()?.extensions.as_deref()?;
                ws_ext::try_parse_all_extension_offers(request_offers, manual_config)
            });

        // --- Build the 101 head: application headers verbatim minus the handshake-owned ones.
        let (mut map, mut case) = translate_websocket_response_headers(&app_headers.entries())?;
        // The computed handshake headers' canonical spellings (naive title-casing would write
        // "Sec-Websocket-*", but kj writes "Sec-WebSocket-*").
        case.append(
            http::header::SEC_WEBSOCKET_ACCEPT,
            bytes::Bytes::from_static(b"Sec-WebSocket-Accept"),
        );
        case.append(
            http::header::SEC_WEBSOCKET_EXTENSIONS,
            bytes::Bytes::from_static(b"Sec-WebSocket-Extensions"),
        );
        // Handshake headers precede the application headers, in kj::HttpServer's exact order
        // (its serializeResponse connectionHeaders): Connection, Upgrade, Sec-WebSocket-Accept,
        // then the negotiated extensions.
        let mut ordered = http::HeaderMap::with_capacity(map.len() + 4);
        ordered.insert(
            http::header::CONNECTION,
            http::HeaderValue::from_static("Upgrade"),
        );
        ordered.insert(
            http::header::UPGRADE,
            http::HeaderValue::from_static("websocket"),
        );
        ordered.insert(
            http::header::SEC_WEBSOCKET_ACCEPT,
            http::HeaderValue::from_str(&ws::websocket_accept_key(key.as_bytes()))
                .expect("base64 accept keys are always valid header values"),
        );
        if let Some(parameters) = &accepted_parameters {
            ordered.insert(
                http::header::SEC_WEBSOCKET_EXTENSIONS,
                http::HeaderValue::from_str(&ws_ext::generate_extension_response(*parameters))
                    .expect("generated extension strings are always valid header values"),
            );
        }
        for (name, value) in &map {
            ordered.append(name.clone(), value.clone());
        }
        let map = ordered;

        // Reserve the slot (checked `!started` above, so this cannot fail) before taking the
        // upgrade handle: if that take fails we abort with no head, as the old take-then-drop did.
        shared.head_slot.borrow_mut().begin()?;
        let on_upgrade = shared.on_upgrade.take().ok_or_else(|| {
            KjError::new(
                KjExceptionType::Failed,
                "already called acceptWebSocket()".to_owned(),
            )
        })?;
        shared.upgraded.set(true);

        shared.head_slot.borrow_mut().deliver(ResponseHead {
            status: http::StatusCode::SWITCHING_PROTOCOLS,
            reason: None,
            headers: map,
            case,
            body: ServeBody::Empty,
        });

        Ok(Box::new(WsSession::new(
            SharedIo::pending(on_upgrade),
            ws::Role::Server,
            accepted_parameters,
            shared.jsgify_websocket_errors,
        )))
    }

    /// Mirrors `kj::HttpServer`'s `sendWebSocketError()`: send a 400 with the error message (via
    /// the default error handler's "ERROR: <description>" rendering), then throw DISCONNECTED
    /// "received bad WebSocket handshake" to the application; the dispatcher recognizes the
    /// already-sent response and lets the connection close.
    fn send_websocket_error<T>(shared: &mut ResponseShared, message: &str) -> Result<T> {
        let mut slot = shared.head_slot.borrow_mut();
        if slot.not_started() {
            let mut headers = http::HeaderMap::new();
            let body_text = format!("ERROR: {message}");
            headers.insert(
                http::header::CONTENT_TYPE,
                http::HeaderValue::from_static("text/plain"),
            );
            headers.insert(
                http::header::CONTENT_LENGTH,
                http::HeaderValue::from(body_text.len() as u64),
            );
            // kj sets closeAfterSend for client protocol errors.
            headers.insert(
                http::header::CONNECTION,
                http::HeaderValue::from_static("close"),
            );
            slot.deliver(ResponseHead {
                status: http::StatusCode::BAD_REQUEST,
                reason: None,
                headers,
                case: hyper::ext::HeaderCaseMap::default(),
                body: ServeBody::Full(Some(Bytes::from(body_text.into_bytes()))),
            });
        }
        drop(slot);
        shared.sent_websocket_error = true;
        Err(KjError::new(
            KjExceptionType::Disconnected,
            format!("received bad WebSocket handshake; {message}"),
        ))
    }
}

/// Application response headers for a 101: everything passes through verbatim except the
/// headers kj's `WEBSOCKET_CONNECTION_HEADERS` override replaces (connection-level headers, the
/// handshake headers, and Sec-WebSocket-Extensions, which is replaced by the computed
/// agreement).
fn translate_websocket_response_headers(
    entries: &[kj::http::HeaderEntry],
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
        "sec-websocket-extensions",
    ];
    let mut map = http::HeaderMap::new();
    let mut case = hyper::ext::HeaderCaseMap::default();
    for entry in entries {
        if DROPPED.iter().any(|d| entry.name.eq_ignore_ascii_case(d)) {
            continue;
        }
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
        case.append(name.clone(), Bytes::copy_from_slice(entry.name.as_bytes()));
        map.append(name, value);
    }
    Ok((map, case))
}

/// Non-canonical status text to put on the wire, if any.
fn status_text_for(status: http::StatusCode, status_text: &[u8]) -> Option<Vec<u8>> {
    match status.canonical_reason() {
        Some(canonical) if canonical.as_bytes() == status_text => None,
        _ if status_text.is_empty() => None,
        _ => Some(status_text.to_vec()),
    }
}

// =======================================================================================
// KJ side: per-request dispatch into the C++ kj::HttpService

#[expect(clippy::too_many_arguments)]
async fn handle_call(
    table: HeaderTablePtr,
    service: ServicePtr,
    jsgify_websocket_errors: bool,
    msg: IncomingCall,
    head_slot: Rc<RefCell<HeadSlot>>,
    conn_alive: watch::Receiver<()>,
    body_activity: Rc<Cell<bool>>,
    dirty_read: Arc<AtomicBool>,
    close_next_response: Rc<Cell<bool>>,
) {
    match msg {
        IncomingCall::Request(request) => {
            handle_request(
                table,
                service,
                jsgify_websocket_errors,
                request,
                head_slot,
                conn_alive,
                body_activity,
                close_next_response,
            )
            .await;
        }
        IncomingCall::Connect(connect) => {
            handle_connect(
                table,
                service,
                connect,
                head_slot,
                conn_alive,
                body_activity,
            )
            .await;
        }
    }
    // The call is over and its message fully consumed: the receive buffer is clean again for
    // kj's drain rule (any body bytes read during the request marked it dirty).
    dirty_read.store(false, atomic::Ordering::Relaxed);
}

/// Waits until the connection dies, unless (or until) the request upgraded — after an upgrade,
/// connection exit is the normal handoff, not a client disconnect, and the service call must
/// keep running (it owns the socket through the WebSocket/tunnel).
async fn wait_disconnect(mut conn_alive: watch::Receiver<()>, upgraded: Rc<Cell<bool>>) {
    while conn_alive.changed().await.is_ok() {}
    if upgraded.get() {
        std::future::pending::<()>().await;
    }
}

/// Translate the request, call the C++ `kj::HttpService`, and generate kj-style error responses
/// as needed. Runs on the KJ event loop; cancels the service call if the connection dies. The
/// produced head (or an abort) is reported through `head_slot`.
#[expect(clippy::too_many_arguments)]
async fn handle_request(
    table: HeaderTablePtr,
    service: ServicePtr,
    jsgify_websocket_errors: bool,
    msg: IncomingRequest,
    head_slot: Rc<RefCell<HeadSlot>>,
    conn_alive: watch::Receiver<()>,
    body_activity: Rc<Cell<bool>>,
    close_next_response: Rc<Cell<bool>>,
) {
    let IncomingRequest {
        method,
        uri,
        headers,
        body,
        on_upgrade,
    } = msg;

    let is_head = method == http::Method::HEAD;
    let upgraded = Rc::new(Cell::new(false));
    let ws_info = WebSocketRequestInfo::from_request(&method, &headers);
    let shared = Rc::new(RefCell::new(ResponseShared {
        is_head,
        head_slot: head_slot.clone(),
        body: None,
        body_activity,
        ws_info: Some(ws_info),
        on_upgrade: Some(on_upgrade),
        upgraded: upgraded.clone(),
        sent_websocket_error: false,
        jsgify_websocket_errors,
        close_next_response,
    }));

    // kj-http rejects unrecognized methods while parsing, with a 501 protocol error.
    let Some(kj_method) = translate_method(&method) else {
        send_error_response(
            &mut shared.borrow_mut(),
            http::StatusCode::NOT_IMPLEMENTED,
            "ERROR: Unrecognized request method.",
        );
        return;
    };

    // The URL as kj-http passes it to services: the request-target as received (origin-form
    // "/path?query" for direct requests, absolute-form for proxy-style requests).
    let url = uri.to_string();

    // The borrowed header table; sound by the bridge caller's outlives contract, discharged
    // when the pointer was wrapped in a `HeaderTablePtr` (see ffi.rs).
    let table = table.get();
    let mut kj_headers = Headers::new(table);
    // Borrow every request header's name/value into one kj-owned arena buffer (a single
    // allocation for the whole block) rather than allocating two kj::Strings per header. Same
    // order/indexing/validation as the per-header add(); see Headers::add_all / add_headers_arena.
    if kj_headers
        .add_all(
            headers
                .iter()
                .map(|(name, value)| (name.as_str(), value.as_bytes())),
        )
        .is_err()
    {
        // Unreachable in practice: hyper already validated the wire bytes. kj would reject
        // such a request with a 400 protocol error.
        send_error_response(
            &mut shared.borrow_mut(),
            http::StatusCode::BAD_REQUEST,
            "ERROR: The headers sent by your client are not valid.",
        );
        return;
    }

    // Stage 4: the request body is read inline within `serve()`'s poll tree (no spawned pump) —
    // see `ServeRequestBody`. Dropping the service call (below) drops this reader and abandons the
    // body, exactly as the old pump task's drop did.
    let mut body_own = ffi::new_serve_request_body_stream(Box::new(ServeRequestBody::new(body)));
    let mut resp_own = ffi::new_hyper_response(Box::new(HyperResponseSender {
        shared: shared.clone(),
    }));

    let service_call = async move {
        // Shared (`&HttpService`) receiver — kj services are shared-reentrant, so this future may
        // run concurrently with other multiplexed inbound requests on the same service without
        // forming an aliasing `&mut` (the F1b fix; see `ServicePtr`). The (non-const) kj `request()`
        // call happens on the C++ side inside `hyper_service_request`. Sound by the bridge caller's
        // outlives/thread contract, discharged when the pointer was wrapped in a `ServicePtr` (see
        // ffi.rs). Concurrent inbound requests share the one kj::HttpService exactly as
        // kj::HttpServer does.
        ffi::hyper_service_request(
            service.get(),
            kj_method,
            url.as_bytes(),
            kj_headers.as_ref().as_ffi(),
            body_own.as_mut(),
            resp_own.as_mut(),
        )
        .await
        .map_err(KjError::from)
    };
    let mut service_call = pin!(service_call.fuse());

    let mut disconnected = pin!(wait_disconnect(conn_alive, upgraded).fuse());

    let outcome = select_biased! {
        result = service_call => Some(result),
        () = disconnected => None,
    };

    match outcome {
        // The client disconnected mid-request: `service_call` is dropped (at the end of this
        // function), which synchronously cancels the C++ service's kj::Promise.
        None => {}
        Some(Ok(())) => {
            let mut shared = shared.borrow_mut();
            if shared.head_slot.borrow().not_started() {
                // kj::HttpServerErrorHandler::handleNoResponse().
                send_error_response(
                    &mut shared,
                    http::StatusCode::INTERNAL_SERVER_ERROR,
                    "ERROR: The HttpService did not generate a response.",
                );
            }
        }
        Some(Err(error)) => handle_application_error(&shared, &error),
    }

    // Finalize: whatever happened, the service call is over. A streaming response body still
    // Open completed cleanly, so mark it EOF now (mirrors the old held `body_tx` clone dropping
    // at end-of-request); `finish` is a no-op if it was already aborted or the consumer is gone,
    // preserving the abort-wins / clean-EOF-only-after-success invariants.
    if let Some(body) = shared.borrow().body.as_ref() {
        ResponseBodyShared::finish(body);
    }

    // If no head was ever produced (DISCONNECTED, a mid-`send` error, or the client vanished)
    // this makes the `service_fn` closure return a connection abort; otherwise the head was
    // already delivered and `finish` re-waking is harmless.
    head_slot.borrow_mut().finish();
}

/// Dispatch a CONNECT to the C++ service's `connect()`, mirroring `kj::HttpServer::onConnect`.
async fn handle_connect(
    table: HeaderTablePtr,
    service: ServicePtr,
    msg: IncomingConnect,
    head_slot: Rc<RefCell<HeadSlot>>,
    conn_alive: watch::Receiver<()>,
    body_activity: Rc<Cell<bool>>,
) {
    let IncomingConnect {
        authority,
        headers,
        on_upgrade,
    } = msg;

    let upgraded = Rc::new(Cell::new(false));
    let io = SharedIo::pending(on_upgrade);
    let shared = Rc::new(RefCell::new(ConnectShared {
        head_slot: head_slot.clone(),
        io: io.clone(),
        upgraded: upgraded.clone(),
        body: None,
        body_activity,
    }));

    // See handle_request: sound by the bridge caller's outlives contract.
    let table = table.get();
    let mut kj_headers = Headers::new(table);
    if kj_headers
        .add_all(
            headers
                .iter()
                .map(|(name, value)| (name.as_str(), value.as_bytes())),
        )
        .is_err()
    {
        send_connect_error_response(
            &mut shared.borrow_mut(),
            http::StatusCode::BAD_REQUEST,
            "ERROR: The headers sent by your client are not valid.",
        );
        return;
    }

    let mut tunnel_own = ffi::new_tunnel_stream(Box::new(HyperTunnel::new(io)));
    let mut resp_own = ffi::new_hyper_connect_response(Box::new(HyperConnectResponder {
        shared: shared.clone(),
    }));

    let service_call = async move {
        // Shared (`&HttpService`) receiver — see handle_request (the F1b fix; the inbound server
        // multiplexes many requests onto the one service). The (non-const) kj `connect()` call
        // happens on the C++ side inside `hyper_service_connect`, which passes kj::HttpServer's
        // default `HttpConnectSettings` (no TLS, no starter). Sound by the bridge caller's
        // outlives/thread contract (see ffi.rs).
        ffi::hyper_service_connect(
            service.get(),
            authority.as_bytes(),
            kj_headers.as_ref().as_ffi(),
            tunnel_own.as_mut(),
            resp_own.as_mut(),
        )
        .await
        .map_err(KjError::from)
    };
    let mut service_call = pin!(service_call.fuse());

    let mut disconnected = pin!(wait_disconnect(conn_alive, upgraded).fuse());

    let outcome = select_biased! {
        result = service_call => Some(result),
        () = disconnected => None,
    };

    match outcome {
        // None: client disconnected (service call canceled). Some(Ok(())): the service
        // returned; if it never called accept()/reject(), kj sends nothing and drops the
        // connection — leaving the head slot undelivered aborts it, which the finalize below does.
        None | Some(Ok(())) => {}
        Some(Err(error)) => handle_connect_application_error(&shared, &error),
    }

    // Finalize (see handle_request): a still-Open rejection body completed cleanly.
    if let Some(body) = shared.borrow().body.as_ref() {
        ResponseBodyShared::finish(body);
    }
    // Resolve the `service_fn` closure now that the call is over.
    head_slot.borrow_mut().finish();
}

/// State shared between the CONNECT dispatcher and the `ConnectResponse` handed to the service.
struct ConnectShared {
    /// Delivers the response head to hyper's `service_fn` closure (see [`HeadSlot`]).
    head_slot: Rc<RefCell<HeadSlot>>,
    io: Rc<SharedIo>,
    upgraded: Rc<Cell<bool>>,
    /// The rejection body's same-task buffer handle, if `reject()` returned a streaming sink (see
    /// [`ResponseShared::body`]).
    body: Option<Rc<RefCell<crate::translate::ResponseBodyShared>>>,
    /// See [`ResponseShared::body_activity`].
    body_activity: Rc<Cell<bool>>,
}

/// The Rust side of the `kj::HttpService::ConnectResponse` given to the C++ service (wrapped by
/// `HyperConnectResponseImpl` in hyper-server-ffi.c++).
pub struct HyperConnectResponder {
    shared: Rc<RefCell<ConnectShared>>,
}

impl HyperConnectResponder {
    /// Corresponds to `ConnectResponse::accept()`: sends the 2xx head; hyper then hands the raw
    /// connection to the tunnel stream.
    pub fn accept(
        &self,
        status_code: u32,
        status_text: &[u8],
        headers: &ffi::HttpHeaders,
    ) -> Result<()> {
        let shared = self.shared.borrow_mut();
        shared.head_slot.borrow_mut().begin()?;
        let status = u16::try_from(status_code)
            .ok()
            .and_then(|code| http::StatusCode::from_u16(code).ok())
            .filter(http::StatusCode::is_success)
            .ok_or_else(|| {
                KjError::new(
                    KjExceptionType::Failed,
                    "the statusCode must be 2xx for accept".to_owned(),
                )
            })?;
        // kj passes the application headers through verbatim; framing headers are dropped so
        // hyper treats this as the (bodiless) CONNECT success it is.
        let entries = HeadersRef::from(headers).entries();
        let (map, case) = translate_response_headers(&entries, FramingHeaders::Drop)?;
        shared.upgraded.set(true);
        shared.head_slot.borrow_mut().deliver(ResponseHead {
            status,
            reason: status_text_for(status, status_text),
            headers: map,
            case,
            body: ServeBody::Empty,
        });
        Ok(())
    }

    /// Corresponds to `ConnectResponse::reject()`: fails the tunnel, sends the error head, and
    /// returns the sink for the rejection body. The connection closes afterwards
    /// (kj sets closeAfterSend).
    pub fn reject(
        &self,
        status_code: u32,
        status_text: &[u8],
        headers: &ffi::HttpHeaders,
        expected_body_size: KjMaybe<u64>,
    ) -> Result<Box<HyperResponseBodySink>> {
        let expected_body_size: Option<u64> = expected_body_size.into();
        let mut shared = self.shared.borrow_mut();
        shared.head_slot.borrow_mut().begin()?;
        let status = u16::try_from(status_code)
            .ok()
            .and_then(|code| http::StatusCode::from_u16(code).ok())
            .filter(|s| !s.is_success())
            .ok_or_else(|| {
                KjError::new(
                    KjExceptionType::Failed,
                    "the statusCode must not be 2xx for reject.".to_owned(),
                )
            })?;

        // Reads and writes on the tunnel stream now fail, like kj's rejected write guard.
        shared.io.fail(
            KjExceptionType::Disconnected,
            "the tunnel request was rejected",
        );

        let entries = HeadersRef::from(headers).entries();
        let (mut map, case) = translate_response_headers(&entries, FramingHeaders::Drop)?;
        map.insert(
            http::header::CONNECTION,
            http::HeaderValue::from_static("close"),
        );

        let (body, sink_kind) = match expected_body_size {
            Some(0) => (ServeBody::Empty, SinkKind::Exhausted),
            length => {
                if let Some(n) = length {
                    map.insert(http::header::CONTENT_LENGTH, http::HeaderValue::from(n));
                }
                // Same-task streaming buffer (Stage 3); see HyperResponseSender::send.
                let (buf, sink) =
                    ResponseBodyShared::new_pair(length, shared.body_activity.clone());
                shared.body = Some(buf.clone());
                (
                    ServeBody::Buffer {
                        shared: buf,
                        length,
                    },
                    sink,
                )
            }
        };

        shared.head_slot.borrow_mut().deliver(ResponseHead {
            status,
            reason: status_text_for(status, status_text),
            headers: map,
            case,
            body,
        });
        Ok(Box::new(HyperResponseBodySink { kind: sink_kind }))
    }
}

/// Mirrors `kj::HttpServerErrorHandler::handleApplicationError()`: DISCONNECTED closes the
/// connection without a response; OVERLOADED/UNIMPLEMENTED/other map to 503/501/500 when the
/// response has not started; a response already streaming is aborted.
fn handle_application_error(shared: &Rc<RefCell<ResponseShared>>, error: &KjError) {
    let mut shared = shared.borrow_mut();

    if error.exception_type() == KjExceptionType::Disconnected {
        // Send no response; just close the connection (suppressing the head aborts the request).
        // If the response was already streaming, abort it. (For a bad WebSocket handshake the 400
        // was already sent and this exception is expected; kj likewise ignores it.)
        shared.head_slot.borrow_mut().suppress();
        if let Some(body) = shared.body.as_ref() {
            ResponseBodyShared::abort(
                body,
                BodyError("the HttpService was disconnected".to_owned()),
            );
        }
        return;
    }

    if shared.head_slot.borrow().not_started() {
        let (status, intro) = match error.exception_type() {
            KjExceptionType::Overloaded => (
                http::StatusCode::SERVICE_UNAVAILABLE,
                "ERROR: The server is temporarily unable to handle your request. Details:\n\n",
            ),
            KjExceptionType::Unimplemented => (
                http::StatusCode::NOT_IMPLEMENTED,
                "ERROR: The server does not implement this operation. Details:\n\n",
            ),
            _ => (
                http::StatusCode::INTERNAL_SERVER_ERROR,
                "ERROR: The server threw an exception. Details:\n\n",
            ),
        };
        // Divergence from kj: the details are the exception description only, not kj's full
        // "file:line: type: description" rendering.
        send_error_response(
            &mut shared,
            status,
            &format!("{intro}{}", error.description()),
        );
    } else if let Some(body) = shared.body.as_ref() {
        // Too late to change the response; break the streaming body so the client observes an
        // aborted response instead of a clean end (kj drops the connection here too).
        ResponseBodyShared::abort(
            body,
            BodyError(format!(
                "the HttpService threw an exception after starting the response: {}",
                error.description()
            )),
        );
    }
}

/// The CONNECT flavor of `handle_application_error` (kj funnels both through the same error
/// handler; the states differ slightly because `accept()` has no body).
fn handle_connect_application_error(shared: &Rc<RefCell<ConnectShared>>, error: &KjError) {
    let mut shared = shared.borrow_mut();

    if error.exception_type() == KjExceptionType::Disconnected {
        shared.head_slot.borrow_mut().suppress();
        if let Some(body) = shared.body.as_ref() {
            ResponseBodyShared::abort(
                body,
                BodyError("the HttpService was disconnected".to_owned()),
            );
        }
        return;
    }

    if shared.head_slot.borrow().not_started() {
        let (status, intro) = match error.exception_type() {
            KjExceptionType::Overloaded => (
                http::StatusCode::SERVICE_UNAVAILABLE,
                "ERROR: The server is temporarily unable to handle your request. Details:\n\n",
            ),
            KjExceptionType::Unimplemented => (
                http::StatusCode::NOT_IMPLEMENTED,
                "ERROR: The server does not implement this operation. Details:\n\n",
            ),
            _ => (
                http::StatusCode::INTERNAL_SERVER_ERROR,
                "ERROR: The server threw an exception. Details:\n\n",
            ),
        };
        send_connect_error_response(
            &mut shared,
            status,
            &format!("{intro}{}", error.description()),
        );
    } else if let Some(body) = shared.body.as_ref() {
        ResponseBodyShared::abort(
            body,
            BodyError(format!(
                "the HttpService threw an exception after starting the response: {}",
                error.description()
            )),
        );
    }
}

/// Send a kj-style plain-text error response, if the response has not started yet.
fn send_error_response(shared: &mut ResponseShared, status: http::StatusCode, message: &str) {
    let mut slot = shared.head_slot.borrow_mut();
    if !slot.not_started() {
        return;
    }
    let mut headers = http::HeaderMap::new();
    headers.insert(
        http::header::CONTENT_TYPE,
        http::HeaderValue::from_static("text/plain"),
    );
    headers.insert(
        http::header::CONTENT_LENGTH,
        http::HeaderValue::from(message.len() as u64),
    );
    let body = if shared.is_head {
        ServeBody::Empty
    } else {
        ServeBody::Full(Some(Bytes::copy_from_slice(message.as_bytes())))
    };
    slot.deliver(ResponseHead {
        status,
        reason: None,
        headers,
        case: hyper::ext::HeaderCaseMap::default(),
        body,
    });
}

fn send_connect_error_response(
    shared: &mut ConnectShared,
    status: http::StatusCode,
    message: &str,
) {
    let mut slot = shared.head_slot.borrow_mut();
    if !slot.not_started() {
        return;
    }
    shared.io.fail(
        KjExceptionType::Disconnected,
        "the tunnel request was rejected",
    );
    let mut headers = http::HeaderMap::new();
    headers.insert(
        http::header::CONTENT_TYPE,
        http::HeaderValue::from_static("text/plain"),
    );
    headers.insert(
        http::header::CONTENT_LENGTH,
        http::HeaderValue::from(message.len() as u64),
    );
    headers.insert(
        http::header::CONNECTION,
        http::HeaderValue::from_static("close"),
    );
    slot.deliver(ResponseHead {
        status,
        reason: None,
        headers,
        case: hyper::ext::HeaderCaseMap::default(),
        body: ServeBody::Full(Some(Bytes::copy_from_slice(message.as_bytes()))),
    });
}
