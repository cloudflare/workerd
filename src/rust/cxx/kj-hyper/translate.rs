//! http-crate ⇄ KJ translation for consumers that dispatch requests arriving as `http` types
//! into a C++ `kj::HttpService` and stream the response back out as an `http_body::Body` — the
//! hyper inbound server (server.rs) today; any transport delivering `http`-crate requests fits.
//!
//! The pieces here are transport-neutral:
//!
//! - [`translate_method`] / [`translate_response_headers`]: `http` ⇄ kj-http metadata mapping.
//! - [`HyperRequestBody`]: any `http_body::Body` as the Rust side of a `kj::AsyncInputStream`
//!   (wrapped by `HyperRequestBodyStream` in hyper-server-ffi.c++), with kj's remaining-length
//!   semantics. The body itself is polled by a pump task on the KJ thread's loop runtime
//!   (`kj_rs_tokio`, where the transport's connection task also lives) feeding a bounded frame
//!   channel, so KJ-side reads cost at most one channel wake per refill instead of one waker
//!   round-trip per HTTP frame.
//! - [`ServerBody`] + [`HyperResponseBodySink`]: the response-body channel pair — the C++
//!   service writes into the sink (a `kj::AsyncOutputStream`), the transport reads the
//!   [`ServerBody`] as an `http_body::Body` — with bounded-channel backpressure and an abort
//!   path so a failed response never looks like a clean end.

use std::cell::Cell;
use std::cell::RefCell;
use std::collections::VecDeque;
use std::pin::Pin;
use std::rc::Rc;
use std::task::Context;
use std::task::Poll;
use std::task::Waker;

use bytes::Bytes;
use cxx::KjError;
use cxx::KjExceptionType;
use http_body_util::BodyExt;
use hyper::body::Incoming;
use kj::Result;
use kj::http::Method;
use kj_rs::KjMaybe;
use tokio::sync::mpsc;
use tokio::sync::oneshot;

// =======================================================================================
// Response body bridging (KJ -> http_body)

/// Error carried into a response body to abort a transport whose response can no longer be
/// completed (service threw after the head was sent).
#[derive(Debug)]
pub struct BodyError(pub String);

impl std::fmt::Display for BodyError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for BodyError {}

/// How many writes the response-body channel may buffer ahead of the transport (the mirror
/// image of [`REQUEST_BODY_CHANNEL_FRAMES`]): a full channel costs the writer a waker round
/// trip plus a KJ event-loop turn per write. Depth 16 benchmarked at parity with kj-http on
/// the 1 MiB-upload-echo benchmark (depth 1 was ~6x slower); chunks are still handed to the
/// transport as soon as it can take them, so streaming latency (e.g. SSE) is unaffected.
///
/// On the hyper serve path (server.rs) this is also the depth of the same-task
/// [`ResponseBodyShared`] ring, so backpressure behaves identically to the old mpsc.
pub const RESPONSE_BODY_CHANNEL_CHUNKS: usize = 16;

/// Terminal state of a [`ResponseBodyShared`] streaming buffer.
enum BodyState {
    /// The producer may still write; the consumer waits (`Pending`) when the queue is empty.
    Open,
    /// The producer finished cleanly. The consumer drains the queue, then observes end-of-stream.
    /// Set only once the request completed successfully (mirrors the old `body_tx` clone being
    /// held until success so a clean chunked terminator can't race ahead of an abort).
    Eof,
    /// The producer aborted mid-body (the service threw after the head was sent, or was
    /// disconnected). The consumer surfaces this error *before* any queued data or clean EOF,
    /// mirroring the old abort oneshot being polled ahead of the data channel — the abort must
    /// win over a clean chunked termination.
    Aborted(BodyError),
    /// The consumer (hyper's transport) is gone: further writes fail with DISCONNECTED and
    /// `when_write_disconnected` resolves (mirrors the old `mpsc::Sender::closed()`).
    ConsumerGone,
}

/// The same-task, single-connection response-body buffer that replaces the response-body `mpsc`
/// plus abort `oneshot` on the hyper serve path (Stage 3 of the kj↔tokio serve fusion).
/// Cross-task transports keep using [`ServerBody::Channel`].
///
/// Producer: the KJ-side [`HyperResponseBodySink::write`] (a `kj::AsyncOutputStream`). Consumer:
/// hyper's body serializer, via server.rs's `ServeBody::poll_frame` (which delegates to
/// [`Self::poll_next_chunk`]). Both ends live in the one `serve()` poll tree (server.rs), so a
/// chunk handed over is observed within the *same* `serve()` poll: enqueue/dequeue set the
/// per-connection `activity` flag and `serve()`'s fixed-point driver re-polls the opposite end
/// until quiescent. No waker is cloned into a `CrossThreadPromiseFulfiller` — the hop the old
/// cross-task mpsc forced (and the extra FFI poll per chunk it cost) is gone.
///
/// Backpressure is preserved exactly: the queue is bounded at [`RESPONSE_BODY_CHANNEL_CHUNKS`];
/// a `write()` that would overflow it suspends — parking the producer's waker, which is the
/// `FuturesUnordered` member waker for its service call — until the consumer frees a slot and
/// wakes it.
pub(crate) struct ResponseBodyShared {
    /// Chunks written but not yet handed to the transport (bounded ring; depth =
    /// [`RESPONSE_BODY_CHANNEL_CHUNKS`]).
    queue: VecDeque<Bytes>,
    state: BodyState,
    /// The producer's waker, parked while `write()` / `when_write_disconnected()` await a freed
    /// slot or consumer teardown. It is the producer service call's `FuturesUnordered` member
    /// waker, so waking it makes `serve()`'s driver re-poll that call. Same-thread; never a
    /// cross-thread fulfiller.
    producer_waker: Option<Waker>,
    /// The consumer's (hyper body poll's) waker, parked when `poll_next_chunk` returns Pending.
    /// The producer usually runs on its own KJ event OUTSIDE `serve()`'s poll tree (the C++
    /// service writing while its request() promise is still pending), so a queued chunk or a
    /// terminal transition must wake the connection future — nothing else re-polls it when the
    /// socket is quiet. Same-thread (the serve future's KJ waker).
    consumer_waker: Option<Waker>,
    /// Per-connection progress flag shared with `serve()`'s fixed-point driver (server.rs): set
    /// whenever a chunk moves or the terminal state changes, so the driver keeps its loop going
    /// and re-polls both ends within the same `serve()` poll (writer→consumer and
    /// consumer→writer).
    activity: Rc<Cell<bool>>,
}

impl ResponseBodyShared {
    /// Create a streaming response-body buffer plus the [`SinkKind`] producer end that feeds it.
    /// `remaining` enforces a known Content-Length on the producer side (`None` = chunked);
    /// `activity` is the connection's driver progress flag.
    pub(crate) fn new_pair(
        remaining: Option<u64>,
        activity: Rc<Cell<bool>>,
    ) -> (Rc<RefCell<Self>>, SinkKind) {
        let shared = Rc::new(RefCell::new(Self {
            queue: VecDeque::new(),
            state: BodyState::Open,
            producer_waker: None,
            consumer_waker: None,
            activity,
        }));
        let sink = SinkKind::Buffer {
            shared: shared.clone(),
            remaining,
        };
        (shared, sink)
    }

    /// Signal `serve()`'s driver that a chunk moved (or a terminal transition happened), so it
    /// keeps its fixed-point loop going and re-polls the opposite end within this same poll.
    fn note_activity(&self) {
        self.activity.set(true);
    }

    /// Wake the parked producer (a freed slot, or consumer teardown) so `FuturesUnordered`
    /// re-polls its service call.
    fn wake_producer(&mut self) {
        if let Some(waker) = self.producer_waker.take() {
            waker.wake();
        }
    }

    /// Wake the parked consumer (a queued chunk, or a terminal transition) so the connection
    /// future re-polls `poll_frame`.
    fn wake_consumer(&mut self) {
        if let Some(waker) = self.consumer_waker.take() {
            waker.wake();
        }
    }

    /// Mark a clean end-of-stream (producer finished successfully). No-op unless still `Open`, so
    /// it can neither override an abort nor resurrect a torn-down consumer (the abort-wins
    /// invariant, and the "clean EOF only after success" invariant the old held `body_tx` gave).
    pub(crate) fn finish(shared: &Rc<RefCell<Self>>) {
        let mut b = shared.borrow_mut();
        if matches!(b.state, BodyState::Open) {
            b.state = BodyState::Eof;
            b.note_activity();
            b.wake_consumer();
        }
    }

    /// Abort the in-flight body with `error` (the service threw / was disconnected after the head
    /// was sent). Overrides any queued data and any pending clean EOF; the consumer surfaces the
    /// error on its next `poll_frame` (abort-wins). No-op if the consumer is already gone.
    pub(crate) fn abort(shared: &Rc<RefCell<Self>>, error: BodyError) {
        let mut b = shared.borrow_mut();
        if !matches!(b.state, BodyState::ConsumerGone) {
            b.state = BodyState::Aborted(error);
            b.note_activity();
            b.wake_consumer();
            // The service call awaiting `when_write_disconnected` (if any) is done; leave the
            // producer waker in place — it will be dropped with the buffer.
        }
    }

    /// Consumer side (hyper's body, via server.rs's `ServeBody::poll_frame`): take the next frame,
    /// or observe abort / clean EOF / pending. Mirrors the old [`ServerBody::Channel`] poll order
    /// exactly — an abort wins over any still-queued data and over a clean EOF. On Pending the
    /// consumer's waker is parked (see `consumer_waker`): a producer running inside `serve()`'s
    /// poll is observed the same poll via `activity`, and one running on its own KJ event wakes
    /// the connection future through the waker.
    pub(crate) fn poll_next_chunk(
        shared: &Rc<RefCell<Self>>,
        cx: &mut Context<'_>,
    ) -> Poll<Option<std::result::Result<Bytes, BodyError>>> {
        let mut b = shared.borrow_mut();
        if matches!(b.state, BodyState::Aborted(_))
            && let BodyState::Aborted(error) =
                std::mem::replace(&mut b.state, BodyState::ConsumerGone)
        {
            return Poll::Ready(Some(Err(error)));
        }
        if let Some(chunk) = b.queue.pop_front() {
            // A freed slot: let the (possibly backpressured) producer refill this poll.
            b.note_activity();
            b.wake_producer();
            return Poll::Ready(Some(Ok(chunk)));
        }
        match b.state {
            BodyState::Eof | BodyState::ConsumerGone => Poll::Ready(None),
            BodyState::Open | BodyState::Aborted(_) => {
                b.consumer_waker = Some(cx.waker().clone());
                Poll::Pending
            }
        }
    }

    /// Consumer side: the transport dropped the body. Fail any further/blocked writes with
    /// DISCONNECTED and resolve `when_write_disconnected` (mirrors the old `mpsc` rx drop).
    pub(crate) fn consumer_gone(shared: &Rc<RefCell<Self>>) {
        let mut b = shared.borrow_mut();
        b.state = BodyState::ConsumerGone;
        b.note_activity();
        b.wake_producer();
    }
}

/// The response body as an `http_body::Body` for cross-task transports (which need it to be
/// `Send`): known-empty, a complete buffer (error responses),
/// or fed chunk-by-chunk from the service's `kj::AsyncOutputStream` writes through a bounded
/// channel (backpressure: the writer suspends until the transport drains). `abort_rx` lets the
/// KJ side turn an in-progress streaming body into a transport error, so consumers observe an
/// aborted response rather than a clean end.
///
/// The hyper serve path (server.rs) uses its own same-task `ServeBody` fed by a
/// [`ResponseBodyShared`] buffer instead — see that type — because a same-task `Rc`-backed body
/// cannot be `Send`, which cross-task transports require.
pub enum ServerBody {
    Empty,
    Full(Option<Bytes>),
    Channel {
        rx: mpsc::Receiver<Bytes>,
        abort_rx: Option<oneshot::Receiver<BodyError>>,
        length: Option<u64>,
    },
}

impl http_body::Body for ServerBody {
    type Data = Bytes;
    type Error = BodyError;

    fn poll_frame(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
    ) -> Poll<Option<std::result::Result<http_body::Frame<Bytes>, BodyError>>> {
        match self.get_mut() {
            Self::Empty => Poll::Ready(None),
            Self::Full(data) => Poll::Ready(data.take().map(|b| Ok(http_body::Frame::data(b)))),
            Self::Channel { rx, abort_rx, .. } => {
                if let Some(abort) = abort_rx {
                    match Pin::new(abort).poll(cx) {
                        Poll::Ready(Ok(error)) => {
                            *abort_rx = None;
                            return Poll::Ready(Some(Err(error)));
                        }
                        // Abort sender dropped without firing: the request finished normally.
                        Poll::Ready(Err(_)) => *abort_rx = None,
                        Poll::Pending => {}
                    }
                }
                rx.poll_recv(cx)
                    .map(|item| item.map(|b| Ok(http_body::Frame::data(b))))
            }
        }
    }

    fn is_end_stream(&self) -> bool {
        match self {
            Self::Empty => true,
            Self::Full(data) => data.is_none(),
            Self::Channel { .. } => false,
        }
    }

    fn size_hint(&self) -> http_body::SizeHint {
        match self {
            Self::Empty | Self::Full(None) => http_body::SizeHint::with_exact(0),
            Self::Full(Some(data)) => http_body::SizeHint::with_exact(data.len() as u64),
            Self::Channel {
                length: Some(n), ..
            } => http_body::SizeHint::with_exact(*n),
            Self::Channel { length: None, .. } => http_body::SizeHint::default(),
        }
    }
}

pub(crate) fn is_framing_header(name: &str) -> bool {
    name.eq_ignore_ascii_case("content-length") || name.eq_ignore_ascii_case("transfer-encoding")
}

/// Whether application-supplied framing headers (Content-Length, Transfer-Encoding) are copied
/// into the response.
#[derive(PartialEq, Eq, Clone, Copy)]
pub enum FramingHeaders {
    /// Dropped; the framing computed by the transport wins (hyper's `send()` path).
    Drop,
    /// Kept verbatim (kj's HEAD passthrough rule; the http-over-capnp path, where the C++
    /// `HttpOverCapnpFactory` transports all headers verbatim and framing is carried by
    /// `bodySize`).
    Keep,
}

/// Translate the service's `kj::HttpHeaders` entries into an `http::HeaderMap`, preserving
/// multi-value order and non-UTF-8 value bytes — plus the original header-name spellings for
/// hyper's encoder ([`hyper::ext::HeaderCaseMap`], constructible via workerd's hyper patch):
/// kj-http writes names exactly as the application spelled them, and workerd's tests assert
/// the raw bytes. Names absent from the map fall back to hyper's title-casing (correct for the
/// framing/connection headers inserted with their canonical spellings).
pub fn translate_response_headers(
    entries: &[kj::http::HeaderEntry],
    framing: FramingHeaders,
) -> Result<(http::HeaderMap, hyper::ext::HeaderCaseMap)> {
    let mut map = http::HeaderMap::new();
    let mut case = hyper::ext::HeaderCaseMap::default();
    for entry in entries {
        if framing == FramingHeaders::Drop && is_framing_header(&entry.name) {
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

/// Translate an `http` method to `kj::HttpMethod`. Spellings match kj-http's
/// `KJ_HTTP_FOR_EACH_METHOD` list exactly; anything else is unrecognized (kj rejects it with a
/// 501 protocol error at parse time).
#[must_use]
pub fn translate_method(method: &http::Method) -> Option<Method> {
    Some(match method.as_str() {
        "GET" => Method::GET,
        "HEAD" => Method::HEAD,
        "POST" => Method::POST,
        "PUT" => Method::PUT,
        "DELETE" => Method::DELETE,
        "PATCH" => Method::PATCH,
        "PURGE" => Method::PURGE,
        "OPTIONS" => Method::OPTIONS,
        "TRACE" => Method::TRACE,
        "COPY" => Method::COPY,
        "LOCK" => Method::LOCK,
        "MKCOL" => Method::MKCOL,
        "MOVE" => Method::MOVE,
        "PROPFIND" => Method::PROPFIND,
        "PROPPATCH" => Method::PROPPATCH,
        "SEARCH" => Method::SEARCH,
        "UNLOCK" => Method::UNLOCK,
        "ACL" => Method::ACL,
        "REPORT" => Method::REPORT,
        "MKACTIVITY" => Method::MKACTIVITY,
        "CHECKOUT" => Method::CHECKOUT,
        "MERGE" => Method::MERGE,
        "MSEARCH" => Method::MSEARCH,
        "NOTIFY" => Method::NOTIFY,
        "SUBSCRIBE" => Method::SUBSCRIBE,
        "UNSUBSCRIBE" => Method::UNSUBSCRIBE,
        "QUERY" => Method::QUERY,
        "BAN" => Method::BAN,
        _ => return None,
    })
}

/// Map a hyper error to a `kj::Exception`, using DISCONNECTED for connection-level failures the
/// way KJ's error translation does.
pub(crate) fn kj_error_for_hyper(context: &str, e: &hyper::Error) -> KjError {
    let exception_type = if hyper_error_is_disconnect(e) {
        KjExceptionType::Disconnected
    } else {
        KjExceptionType::Failed
    };
    KjError::new(exception_type, format!("{context}: {e}"))
}

/// Whether a hyper error is a connection-level failure (KJ's DISCONNECTED). Besides hyper's own
/// flags, body errors ("error reading a body from connection") carry their connection-level
/// cause in the source chain — e.g. the `io::ErrorKind::UnexpectedEof` hyper wraps around a
/// body truncated before its Content-Length, which kj-http reports as DISCONNECTED
/// ("premature EOF") — so walk the chain for disconnect-shaped causes.
fn hyper_error_is_disconnect(e: &hyper::Error) -> bool {
    if e.is_incomplete_message() || e.is_canceled() || e.is_closed() {
        return true;
    }
    let mut source = std::error::Error::source(e);
    while let Some(cause) = source {
        if let Some(io) = cause.downcast_ref::<std::io::Error>() {
            return matches!(
                io.kind(),
                std::io::ErrorKind::UnexpectedEof
                    | std::io::ErrorKind::ConnectionReset
                    | std::io::ErrorKind::ConnectionAborted
                    | std::io::ErrorKind::BrokenPipe
                    | std::io::ErrorKind::NotConnected
            );
        }
        if let Some(inner) = cause.downcast_ref::<hyper::Error>()
            && (inner.is_incomplete_message() || inner.is_canceled() || inner.is_closed())
        {
            return true;
        }
        source = cause.source();
    }
    false
}

// =======================================================================================
// Request body bridging (http_body -> KJ)

/// How many frames the request-body pump may run ahead of the KJ-side reader. A depth of 4
/// (~32–64 KiB at hyper's typical frame sizes) hides the waker-round-trip-plus-loop-turn cost
/// of an empty channel without meaningfully weakening backpressure; benchmarked
/// (1 MiB-upload-echo) at parity with kj-http together with `RESPONSE_BODY_CHANNEL_CHUNKS`.
const REQUEST_BODY_CHANNEL_FRAMES: usize = 4;

/// The tokio-side pump: poll the body where its transport lives and push each data frame into
/// the bounded channel. Ends at EOF (dropping `tx` closes the channel cleanly), on a body
/// error (delivered as the final item), or when the KJ-side reader is dropped (`tx.closed()`),
/// which drops the body so the transport observes the abandonment.
async fn pump_body_frames<B>(body: B, tx: mpsc::Sender<std::result::Result<Bytes, KjError>>)
where
    B: http_body::Body<Data = Bytes, Error = KjError> + Send + 'static,
{
    let mut body = std::pin::pin!(body);
    loop {
        let frame = tokio::select! {
            biased;
            // Reader dropped (canceled request / body no longer wanted): stop pumping.
            () = tx.closed() => return,
            frame = std::future::poll_fn(|cx| http_body::Body::poll_frame(body.as_mut(), cx)) => {
                frame
            }
        };
        match frame {
            None => return,
            Some(Ok(frame)) => {
                if let Ok(data) = frame.into_data()
                    && !data.is_empty()
                    && tx.send(Ok(data)).await.is_err()
                {
                    // Reader dropped mid-send; nothing more to pump.
                    return;
                }
                // Trailers are not supported by kj-http; drop them.
            }
            Some(Err(e)) => {
                let _ = tx.send(Err(e)).await;
                return;
            }
        }
    }
}

/// The Rust side of the `kj::AsyncInputStream` handed to the C++ service as a request body —
/// and, on the client side, the response-body pump (client.rs drains it into the caller's
/// output stream) as well as non-101/rejection bodies. Wrapped by `HyperRequestBodyStream`
/// in hyper-server-ffi.c++. Any `http_body::Body` with `Bytes` data works as the source
/// ([`from_body`](Self::from_body)); hyper's `Incoming` is the original use.
///
/// The body is consumed by [`pump_body_frames`] as a task on the KJ thread's loop runtime;
/// this object only reads the resulting frame channel (see [`REQUEST_BODY_CHANNEL_FRAMES`] for
/// the latency/backpressure rationale). Dropping it closes the channel, which aborts the pump
/// and drops the body.
pub struct HyperRequestBody {
    /// Frames from the pump; closed after EOF or an error (the error arrives as the last item).
    rx: mpsc::Receiver<std::result::Result<Bytes, KjError>>,
    /// Bytes received from the pump but not yet handed to the reader.
    buffered: Bytes,
    /// A body error popped while opportunistically coalescing past `min_bytes`; the bytes read
    /// so far are delivered first and the error surfaces on the next read.
    pending_error: Option<KjError>,
    done: bool,
    /// Bytes not yet handed to the reader, when the total is known (Content-Length bodies).
    /// Mirrors kj-http, whose entity-body streams report the *remaining* length.
    remaining: Option<u64>,
}

impl HyperRequestBody {
    pub(crate) fn new(body: Incoming) -> Self {
        Self::from_body(body.map_err(|e| kj_error_for_hyper("read HTTP request body", &e)))
    }

    /// Wrap any `http_body::Body` whose errors are already `KjError`s, for out-of-crate
    /// transports handing request bodies to a C++ `kj::HttpService`.
    pub fn from_body<B>(body: B) -> Self
    where
        B: http_body::Body<Data = Bytes, Error = KjError> + Send + 'static,
    {
        let remaining = body.size_hint().exact();
        let (tx, rx) = mpsc::channel(REQUEST_BODY_CHANNEL_FRAMES);
        // Known-empty bodies (GET/HEAD, Content-Length: 0) skip the pump task entirely; `tx`
        // drops here, so the channel is born closed.
        let done = body.is_end_stream();
        if !done {
            kj_rs_tokio::spawn(pump_body_frames(body, tx));
        }
        Self {
            rx,
            buffered: Bytes::new(),
            pending_error: None,
            done,
            remaining,
        }
    }

    /// Override the reported remaining length (kj's HEAD-response rule: the body is empty by
    /// definition but `tryGetLength()` reports the advertised Content-Length, like kj's
    /// HeadResponseStream).
    #[must_use]
    pub(crate) fn with_remaining(mut self, remaining: Option<u64>) -> Self {
        self.remaining = remaining;
        self
    }

    /// Corresponds to `kj::AsyncInputStream::tryRead(buffer, minBytes, buffer.len())`: reads
    /// until at least `min_bytes` are available or EOF; returns the number of bytes read.
    /// Once `min_bytes` is satisfied, frames the pump already delivered are coalesced toward
    /// `buffer.len()` without waiting, so large reads cross the FFI boundary once per channel
    /// drain rather than once per HTTP frame.
    pub async fn read(&mut self, buffer: &mut [u8], min_bytes: usize) -> Result<usize> {
        use bytes::Buf;
        if self.buffered.is_empty()
            && let Some(e) = self.pending_error.take()
        {
            return Err(e);
        }
        let mut filled = 0;
        loop {
            if !self.buffered.is_empty() && filled < buffer.len() {
                let n = std::cmp::min(buffer.len() - filled, self.buffered.len());
                buffer[filled..filled + n].copy_from_slice(&self.buffered[..n]);
                self.buffered.advance(n);
                filled += n;
            }
            if self.done || filled == buffer.len() {
                break;
            }
            if filled >= min_bytes {
                // minBytes is satisfied: only take frames that are already in the channel.
                match self.rx.try_recv() {
                    Ok(Ok(data)) => self.buffered = data,
                    Ok(Err(e)) => {
                        // Deliver the bytes read so far; the error surfaces on the next read.
                        self.pending_error = Some(e);
                        self.done = true;
                    }
                    Err(mpsc::error::TryRecvError::Empty) => break,
                    Err(mpsc::error::TryRecvError::Disconnected) => self.done = true,
                }
            } else {
                match self.rx.recv().await {
                    None => self.done = true,
                    Some(Ok(data)) => {
                        debug_assert!(self.buffered.is_empty());
                        self.buffered = data;
                    }
                    Some(Err(e)) => return Err(e),
                }
            }
        }
        if let Some(remaining) = &mut self.remaining {
            *remaining = remaining.saturating_sub(filled as u64);
        }
        Ok(filled)
    }

    /// Corresponds to `kj::AsyncInputStream::tryGetLength()`.
    #[must_use]
    pub fn try_get_length(&self) -> KjMaybe<u64> {
        self.remaining.into()
    }
}

// =======================================================================================
// Serve-path request body (hyper `Incoming` -> KJ, same-task, no pump)

/// One non-blocking poll of the request body while coalescing past `min_bytes`.
enum TryFrame {
    Data(Bytes),
    /// No frame ready without awaiting — kj's read returns what it already has (mirrors the old
    /// `mpsc::try_recv` returning `Empty`).
    Empty,
    /// Clean end-of-stream.
    Done,
    Err(KjError),
}

/// The Rust side of the `kj::AsyncInputStream` handed to the C++ service as a request body on the
/// hyper **serve** path (server.rs) — Stage 4 of the kj↔tokio serve fusion. Unlike
/// [`HyperRequestBody`] (which pumps its body on a spawned task feeding a cross-task channel, for
/// genuinely cross-task transports and the client path), this reads hyper's `Incoming`
/// *inline* within `serve()`'s single poll tree: [`Self::read`] polls the `Incoming` directly, so
/// a `Pending` body read yields to the fixed-point driver, which re-polls the connection future —
/// the producer that reads request frames off the socket — and hyper wakes this read when the
/// frame lands. Mutual progress, no spawned pump, no `block_on`, and no cross-thread waker; this
/// is exactly the original body-pump deadlock site, safe now only because the connection future is
/// a KJ node (Stage 1). hyper's own `Incoming` buffer supplies the bounded read-ahead and
/// end-to-end upload backpressure the pump's channel used to (polling `Incoming` is what makes
/// hyper read request bytes off the socket, so a slow reader slows the socket read — not a busy
/// spin, not unbounded).
///
/// This type is `!Send` (same-task only), like server.rs's `ServeBody`; the C++ side reads it on
/// the one KJ event-loop thread that owns the connection. Dropping it (teardown / client abort)
/// abandons the body, which hyper surfaces to the peer exactly as the old pump dropping it did.
pub struct ServeRequestBody {
    /// hyper's request body; `None` when the body was known-empty up front (GET/HEAD,
    /// `Content-Length: 0`), so it is never polled.
    body: Option<Incoming>,
    /// Bytes read from `body` but not yet handed to the reader.
    buffered: Bytes,
    /// A body error popped while opportunistically coalescing past `min_bytes`; the bytes read so
    /// far are delivered first and the error surfaces on the next read (mirrors [`HyperRequestBody`]).
    pending_error: Option<KjError>,
    done: bool,
    /// Bytes not yet handed to the reader, when the total is known (Content-Length bodies).
    /// Mirrors kj-http, whose entity-body streams report the *remaining* length.
    remaining: Option<u64>,
}

impl ServeRequestBody {
    #[must_use]
    pub(crate) fn new(body: Incoming) -> Self {
        let remaining = http_body::Body::size_hint(&body).exact();
        // Known-empty bodies (GET/HEAD, Content-Length: 0) are never polled.
        let done = http_body::Body::is_end_stream(&body);
        Self {
            body: if done { None } else { Some(body) },
            buffered: Bytes::new(),
            pending_error: None,
            done,
            remaining,
        }
    }

    /// Await the next request-body data frame (skipping empty frames and trailers, which kj-http
    /// has no representation for), a clean EOF (`None`), or an error. `Pending` from hyper parks
    /// the reader's waker; `serve()`'s driver re-polls the connection future, which reads the
    /// socket and wakes it — mutual progress, no spawned pump.
    async fn recv_frame(&mut self) -> Option<std::result::Result<Bytes, KjError>> {
        let body = self.body.as_mut()?;
        loop {
            let frame =
                std::future::poll_fn(|cx| http_body::Body::poll_frame(Pin::new(&mut *body), cx))
                    .await;
            match frame {
                None => return None,
                Some(Ok(frame)) => match frame.into_data() {
                    Ok(data) if !data.is_empty() => return Some(Ok(data)),
                    // Empty data frame or trailers: keep reading.
                    _ => {}
                },
                Some(Err(e)) => {
                    return Some(Err(kj_error_for_hyper("read HTTP request body", &e)));
                }
            }
        }
    }

    /// A single non-blocking poll of the request body, used to coalesce frames hyper has already
    /// buffered once `min_bytes` is satisfied (no await, no real waker registered — matching the
    /// old `mpsc::try_recv`): a not-yet-ready frame is reported as [`TryFrame::Empty`] and the read
    /// returns what it has, so a large read still crosses the FFI boundary once per hyper read
    /// rather than once per HTTP frame.
    fn try_frame(&mut self) -> TryFrame {
        let Some(body) = self.body.as_mut() else {
            return TryFrame::Done;
        };
        let mut cx = Context::from_waker(Waker::noop());
        loop {
            match http_body::Body::poll_frame(Pin::new(&mut *body), &mut cx) {
                Poll::Pending => return TryFrame::Empty,
                Poll::Ready(None) => return TryFrame::Done,
                Poll::Ready(Some(Ok(frame))) => match frame.into_data() {
                    Ok(data) if !data.is_empty() => return TryFrame::Data(data),
                    // Empty data frame or trailers: keep reading.
                    _ => {}
                },
                Poll::Ready(Some(Err(e))) => {
                    return TryFrame::Err(kj_error_for_hyper("read HTTP request body", &e));
                }
            }
        }
    }

    /// Corresponds to `kj::AsyncInputStream::tryRead(buffer, minBytes, buffer.len())`. Identical
    /// coalescing / `remaining` / `pending_error` semantics to [`HyperRequestBody::read`], reading
    /// hyper's `Incoming` inline instead of a pump channel.
    pub async fn read(&mut self, buffer: &mut [u8], min_bytes: usize) -> Result<usize> {
        use bytes::Buf;
        if self.buffered.is_empty()
            && let Some(e) = self.pending_error.take()
        {
            return Err(e);
        }
        let mut filled = 0;
        loop {
            if !self.buffered.is_empty() && filled < buffer.len() {
                let n = std::cmp::min(buffer.len() - filled, self.buffered.len());
                buffer[filled..filled + n].copy_from_slice(&self.buffered[..n]);
                self.buffered.advance(n);
                filled += n;
            }
            if self.done || filled == buffer.len() {
                break;
            }
            if filled >= min_bytes {
                // minBytes is satisfied: only take frames hyper has already buffered.
                match self.try_frame() {
                    TryFrame::Data(data) => self.buffered = data,
                    TryFrame::Err(e) => {
                        // Deliver the bytes read so far; the error surfaces on the next read.
                        self.pending_error = Some(e);
                        self.done = true;
                    }
                    TryFrame::Empty => break,
                    TryFrame::Done => self.done = true,
                }
            } else {
                match self.recv_frame().await {
                    None => self.done = true,
                    Some(Ok(data)) => {
                        debug_assert!(self.buffered.is_empty());
                        self.buffered = data;
                    }
                    Some(Err(e)) => return Err(e),
                }
            }
        }
        if let Some(remaining) = &mut self.remaining {
            *remaining = remaining.saturating_sub(filled as u64);
        }
        Ok(filled)
    }

    /// Corresponds to `kj::AsyncInputStream::tryGetLength()`.
    #[must_use]
    pub fn try_get_length(&self) -> KjMaybe<u64> {
        self.remaining.into()
    }
}

// =======================================================================================
// Response body sink (the KJ-side kj::AsyncOutputStream feeding a ServerBody)

pub(crate) enum SinkKind {
    /// HEAD response: writes are accepted and discarded (kj's `HttpDiscardingEntityWriter`;
    /// also the http-over-capnp no-body path, where C++ hands the service a `kj::NullStream`).
    Discard,
    /// 204/205/304: writes fail (kj's `HttpNullEntityWriter`).
    Null,
    /// Content-Length: 0 on a non-HEAD response: only empty writes are allowed.
    Exhausted,
    /// Streaming body over a cross-task channel; `remaining`
    /// enforces Content-Length when known.
    Channel {
        tx: mpsc::Sender<Bytes>,
        remaining: Option<u64>,
    },
    /// Streaming body over the same-task [`ResponseBodyShared`] buffer (the hyper serve path —
    /// Stage 3); `remaining` enforces Content-Length when known.
    Buffer {
        shared: Rc<RefCell<ResponseBodyShared>>,
        remaining: Option<u64>,
    },
}

/// The Rust side of the `kj::AsyncOutputStream` returned from `Response::send()` (wrapped by
/// `HyperResponseBodyStream` in hyper-server-ffi.c++).
pub struct HyperResponseBodySink {
    pub(crate) kind: SinkKind,
}

impl HyperResponseBodySink {
    /// A sink that accepts and discards every write (kj `NullStream` semantics), for
    /// responses that carry no body.
    #[must_use]
    pub fn discarding() -> Self {
        Self {
            kind: SinkKind::Discard,
        }
    }

    /// A sink feeding `tx` (bounded channel: transport backpressure); `remaining` enforces a
    /// fixed body size with kj's "overwrote Content-Length" error on excess.
    #[must_use]
    pub fn channel(tx: mpsc::Sender<Bytes>, remaining: Option<u64>) -> Self {
        Self {
            kind: SinkKind::Channel { tx, remaining },
        }
    }

    /// Corresponds to `kj::AsyncOutputStream::write()`.
    pub async fn write(&mut self, buffer: &[u8]) -> Result<()> {
        match &mut self.kind {
            SinkKind::Discard => Ok(()),
            // Matches kj's HttpNullEntityWriter error text.
            SinkKind::Null => Err(KjError::new(
                KjExceptionType::Failed,
                "HTTP message has no entity-body; can't write()".to_owned(),
            )),
            SinkKind::Exhausted => {
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
            SinkKind::Channel { tx, remaining } => {
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
                tx.send(Bytes::copy_from_slice(buffer)).await.map_err(|_| {
                    KjError::new(
                        KjExceptionType::Disconnected,
                        "the HTTP connection was closed while sending the response body".to_owned(),
                    )
                })
            }
            SinkKind::Buffer { shared, remaining } => {
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
                let shared = shared.clone();
                let mut chunk = Some(Bytes::copy_from_slice(buffer));
                // Backpressure: park until the same-task ring has a free slot (or the consumer is
                // gone). `serve()`'s driver re-polls this service call when `poll_frame` frees a
                // slot and wakes `producer_waker`; no cross-task channel, no cloned fulfiller.
                std::future::poll_fn(move |cx| {
                    let mut b = shared.borrow_mut();
                    if matches!(b.state, BodyState::ConsumerGone) {
                        return Poll::Ready(Err(KjError::new(
                            KjExceptionType::Disconnected,
                            "the HTTP connection was closed while sending the response body"
                                .to_owned(),
                        )));
                    }
                    if b.queue.len() < RESPONSE_BODY_CHANNEL_CHUNKS {
                        if let Some(chunk) = chunk.take() {
                            b.queue.push_back(chunk);
                            b.note_activity();
                            b.wake_consumer();
                        }
                        Poll::Ready(Ok(()))
                    } else {
                        b.producer_waker = Some(cx.waker().clone());
                        Poll::Pending
                    }
                })
                .await
            }
        }
    }

    /// Corresponds to `kj::AsyncOutputStream::whenWriteDisconnected()`.
    pub async fn when_write_disconnected(&self) {
        match &self.kind {
            SinkKind::Channel { tx, .. } => tx.closed().await,
            SinkKind::Buffer { shared, .. } => {
                let shared = shared.clone();
                std::future::poll_fn(move |cx| {
                    let mut b = shared.borrow_mut();
                    if matches!(b.state, BodyState::ConsumerGone) {
                        Poll::Ready(())
                    } else {
                        b.producer_waker = Some(cx.waker().clone());
                        Poll::Pending
                    }
                })
                .await;
            }
            // kj's null/discarding entity writers never resolve this either.
            _ => std::future::pending().await,
        }
    }
}

// =======================================================================================
// WebSocket handshake helpers (RFC 6455 §4). Only the *handshake* lives on the Rust side;
// framing is kj's own WebSocketImpl (`kj::newWebSocket()` over the upgraded tunnel stream, see
// hyper-http.h / hyper-server-ffi.c++).

/// Computes the `Sec-WebSocket-Accept` value for a `Sec-WebSocket-Key` (RFC 6455 §4.2.2).
#[must_use]
pub fn websocket_accept_key(key: &[u8]) -> String {
    use base64::Engine;
    use sha1::Digest;
    const WEBSOCKET_GUID: &[u8] = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    let mut hasher = sha1::Sha1::new();
    hasher.update(key);
    hasher.update(WEBSOCKET_GUID);
    base64::engine::general_purpose::STANDARD.encode(hasher.finalize())
}

/// Generates a random `Sec-WebSocket-Key` (16 random bytes, base64).
#[must_use]
pub fn generate_websocket_key() -> String {
    use base64::Engine;
    let bytes: [u8; 16] = rand::random();
    base64::engine::general_purpose::STANDARD.encode(bytes)
}
