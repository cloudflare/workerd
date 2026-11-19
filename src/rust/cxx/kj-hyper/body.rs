//! HTTP message bodies in both directions, and header conversion.
//!
//! Headers are copied at the crossing, and written as kj writes them. kj headers reach hyper
//! through `for_each_header` -> [`Head::append`], in the order `kj::HttpHeaders::forEach` yields:
//! the header table's first, in the table's order and spelling (see [`Head`]), then the rest as
//! added. The connection-level headers are the protocol's, not the application's, as under kj's
//! `connectionHeaders` ([`Head::claim`]), and [`Head`] always sets the framing
//! (`Content-Length` / `Transfer-Encoding: chunked`) and `Connection: close` (a drain, a
//! failure's answer, a refused CONNECT) itself, at kj's position, so hyper finds them and appends
//! none of its own. To get a header written ahead of the unknown ones with a fixed spelling, put
//! it in the `kj::HttpHeaderTable`.
//!
//! Where this differs from kj: a header's repeated values are written together
//! (`http::HeaderMap` groups them; kj writes each where it was added, a second `Set-Cookie` after
//! the table's headers), a GET or HEAD request whose body length is unknown is sent without a
//! body (hyper's rule) where kj would chunk it, a response to HEAD never says
//! `Transfer-Encoding`, and hyper closes a connection, saying so after the other headers, when
//! the request asked it to (`Connection: close`, HTTP/1.0 without keep-alive, which it also
//! answers as HTTP/1.0 and never chunked); kj kept those open. hyper's headers come back packed
//! ([`HeaderBlock`]) and C++ checks the block's bounds before building `kj::HttpHeaders`; a
//! server resolves each name's kj id once per connection ([`HeaderIds`]).

use std::cell::Cell;
use std::cell::RefCell;
use std::error::Error;
use std::fmt;
use std::future::Future;
use std::future::poll_fn;
use std::mem;
use std::pin::Pin;
use std::rc::Rc;
use std::sync::Arc;
use std::sync::Mutex;
use std::sync::MutexGuard;
use std::sync::PoisonError;
use std::task::Context;
use std::task::Poll;
use std::task::Waker;

use bytes::Buf;
use bytes::Bytes;
use cxx::KjError;
use cxx::KjExceptionType;
use http::header;
use http_body::Body;
use http_body::Frame;
use http_body::SizeHint;
use hyper::body::Incoming;
use hyper::ext::HeaderCaseMap;
use tokio::io::ReadBuf;

use crate::Result;
use crate::ffi;
use crate::ffi::Borrowing;
use crate::ffi::HttpHeaderTable;
use crate::ffi::HttpHeaders;
use crate::ffi::ReusedHeaders;
use crate::ffi::for_each_header;
use crate::ffi::headers_from_block;
use crate::ffi::new_header_ids;
use crate::scratch::Scratch;

struct BodyState {
    body: RefCell<Option<Incoming>>,
    buffered: RefCell<Bytes>,
}

/// An incoming body as a `kj::AsyncInputStream`. Each read owns a share of the body, which is
/// borrowed only inside a poll, so `tryGetLength()` may be called while a read waits.
pub struct RustBody(Rc<BodyState>);

impl RustBody {
    pub fn new(body: Incoming) -> Self {
        Self(Rc::new(BodyState {
            body: RefCell::new(Some(body)),
            buffered: RefCell::new(Bytes::new()),
        }))
    }

    /// `body` in place of the one this held, in the same state unless a read still holds that.
    pub fn refill(&mut self, body: Incoming) {
        match Rc::get_mut(&mut self.0) {
            Some(state) => {
                *state.body.get_mut() = Some(body);
                *state.buffered.get_mut() = Bytes::new();
            }
            None => *self = Self::new(body),
        }
    }

    /// Lets go of the body, unless a read still holds it: false then.
    pub fn clear(&mut self) -> bool {
        let Some(state) = Rc::get_mut(&mut self.0) else {
            return false;
        };
        *state.body.get_mut() = None;
        *state.buffered.get_mut() = Bytes::new();
        true
    }

    /// kj's `tryRead`: reads until `min_bytes` or the body's end.
    pub fn read<'b>(
        &self,
        mut buf: ReadBuf<'b>,
        min_bytes: usize,
    ) -> impl Future<Output = Result<usize>> + use<'b> {
        let state = self.0.clone();
        let min_bytes = min_bytes.min(buf.capacity());
        poll_fn(move |cx| {
            let mut buffered = state.buffered.borrow_mut();
            let mut body = state.body.borrow_mut();
            loop {
                let n = buffered.len().min(buf.remaining());
                buf.put_slice(&buffered[..n]);
                buffered.advance(n);
                if buf.filled().len() >= min_bytes {
                    return Poll::Ready(Ok(buf.filled().len()));
                }
                let Some(incoming) = body.as_mut() else {
                    return Poll::Ready(Ok(buf.filled().len()));
                };
                match std::task::ready!(Pin::new(incoming).poll_frame(cx)) {
                    Some(Ok(frame)) => {
                        if let Ok(data) = frame.into_data() {
                            *buffered = data;
                        }
                    }
                    Some(Err(e)) => {
                        return Poll::Ready(Err(KjError::new(
                            KjExceptionType::Disconnected,
                            format!("HTTP body: {e}"),
                        )));
                    }
                    None => *body = None,
                }
            }
        })
    }

    #[must_use]
    pub fn length(&self) -> Option<u64> {
        let hint = self
            .0
            .body
            .borrow()
            .as_ref()
            .map_or_else(SizeHint::new, Body::size_hint);
        hint.exact()
            .map(|n| n + self.0.buffered.borrow().len() as u64)
    }
}

/// What a [`BodySink`] and the [`ChannelBody`] it feeds share: a channel with room for one
/// chunk, as a write waits for the chunk before it to be read. A mutex, not a cell: the client's
/// bodies must be `Send`, though each side is only ever used on one thread at a time.
#[derive(Default)]
pub struct Pipe(Mutex<PipeState>);

#[derive(Default)]
struct PipeState {
    /// Written and not yet read.
    chunk: Option<Bytes>,
    /// The sink is gone: the body ends once its chunk is read.
    ended: bool,
    /// The body fails instead of ending ([`BodyAbort`]).
    aborted: bool,
    /// The body is gone: nothing written reaches anyone.
    gone: bool,
    /// The body, waiting for a chunk or its end.
    reader: Option<Waker>,
    /// A write waiting for room.
    writer: Option<Waker>,
    /// Waiting for the body to go (`whenWriteDisconnected()`).
    disconnect: Vec<Waker>,
}

impl Pipe {
    fn ended(chunk: Option<Bytes>) -> Self {
        Self(Mutex::new(PipeState {
            chunk,
            ended: true,
            ..PipeState::default()
        }))
    }

    fn state(&self) -> MutexGuard<'_, PipeState> {
        // Nothing panics while holding the lock, so its state is never left half-changed.
        self.0.lock().unwrap_or_else(PoisonError::into_inner)
    }

    /// Puts `data` in the pipe once it has room.
    fn poll_put(&self, cx: &Context<'_>, data: &mut Option<Bytes>) -> Poll<Result<()>> {
        let mut state = self.state();
        if state.gone {
            return Poll::Ready(Err(peer_gone()));
        }
        if state.chunk.is_some() {
            register(&mut state.writer, cx);
            return Poll::Pending;
        }
        state.chunk = data.take();
        let reader = state.reader.take();
        drop(state);
        wake(reader);
        Poll::Ready(Ok(()))
    }
}

/// Keeps the waker of the task polling with `cx` in `slot`.
fn register(slot: &mut Option<Waker>, cx: &Context<'_>) {
    if !slot.as_ref().is_some_and(|w| w.will_wake(cx.waker())) {
        *slot = Some(cx.waker().clone());
    }
}

fn wake(waker: Option<Waker>) {
    if let Some(waker) = waker {
        waker.wake();
    }
}

fn peer_gone() -> KjError {
    KjError::new(
        KjExceptionType::Disconnected,
        "HTTP body: the peer went away".to_owned(),
    )
}

/// A `kj::AsyncOutputStream` feeding an outgoing hyper body. Dropping it ends the body.
pub struct BodySink {
    /// `None` for a body that is discarded (a response to HEAD).
    pipe: Option<Arc<Pipe>>,
    /// Where a server's writes are copied to.
    scratch: Option<Rc<Scratch>>,
    /// Bytes still to be written, for a body of declared length. A `Cell`: kj writes through
    /// `&self`, as `whenWriteDisconnected()` may be awaited alongside a write.
    remaining: Cell<Option<u64>>,
}

impl BodySink {
    /// A sink whose writes are accepted and dropped, as kj's for a response to HEAD.
    #[must_use]
    pub fn discarding() -> Self {
        Self {
            pipe: None,
            scratch: None,
            remaining: Cell::new(None),
        }
    }

    fn copy(&self, data: &[u8]) -> Bytes {
        self.scratch.as_ref().map_or_else(
            || Bytes::copy_from_slice(data),
            |scratch| scratch.copy(data),
        )
    }

    /// A write beyond the declared length fails before any of it is queued, as kj's does.
    fn check_length(&self, len: usize) -> Result<Option<u64>> {
        match self.remaining.get() {
            Some(remaining) if len as u64 > remaining => Err(KjError::new(
                KjExceptionType::Failed,
                "overwrote Content-Length".to_owned(),
            )),
            remaining => Ok(remaining.map(|r| r - len as u64)),
        }
    }

    /// Queues `data` if the body has room for it, which a write finds nearly always: true once
    /// it is queued (or discarded), false if [`BodySink::write`] must wait for room instead.
    ///
    /// # Errors
    ///
    /// A write past the declared length, or to a body whose peer went away.
    pub fn try_write(&self, data: &[u8]) -> Result<bool> {
        let Some(pipe) = &self.pipe else {
            return Ok(true);
        };
        let remaining = self.check_length(data.len())?;
        if data.is_empty() {
            return Ok(true);
        }
        let mut state = pipe.state();
        if state.gone {
            return Err(peer_gone());
        }
        if state.chunk.is_some() {
            return Ok(false);
        }
        self.remaining.set(remaining);
        state.chunk = Some(self.copy(data));
        let reader = state.reader.take();
        drop(state);
        wake(reader);
        Ok(true)
    }

    /// Queues `data`; resolves once the body has room for it.
    pub fn write(&self, data: &[u8]) -> impl Future<Output = Result<()>> + use<> {
        let queued = match &self.pipe {
            None => Ok(None),
            Some(pipe) => self.check_length(data.len()).map(|remaining| {
                self.remaining.set(remaining);
                (!data.is_empty()).then(|| (Arc::clone(pipe), self.copy(data)))
            }),
        };
        async move {
            let Some((pipe, data)) = queued? else {
                return Ok(());
            };
            let mut data = Some(data);
            poll_fn(|cx| pipe.poll_put(cx, &mut data)).await
        }
    }

    pub fn when_write_disconnected(&self) -> impl Future<Output = ()> + use<> {
        let pipe = self.pipe.clone();
        poll_fn(move |cx| {
            let Some(pipe) = &pipe else {
                return Poll::Pending;
            };
            let mut state = pipe.state();
            if state.gone {
                return Poll::Ready(());
            }
            if !state.disconnect.iter().any(|w| w.will_wake(cx.waker())) {
                state.disconnect.push(cx.waker().clone());
            }
            Poll::Pending
        })
    }
}

impl Drop for BodySink {
    fn drop(&mut self) {
        if let Some(pipe) = &self.pipe {
            let mut state = pipe.state();
            state.ended = true;
            let reader = state.reader.take();
            drop(state);
            wake(reader);
        }
    }
}

/// Ends a [`ChannelBody`] with an error, so hyper drops the connection instead of framing what
/// was written as a complete message. Dropping it unused has no effect.
pub struct BodyAbort(Arc<Pipe>);

impl BodyAbort {
    /// The pipe, for a later body once nothing else holds it ([`Scratch::take_pipe`]).
    pub fn into_pipe(self) -> Arc<Pipe> {
        self.0
    }

    pub fn abort(self) {
        let mut state = self.0.state();
        state.aborted = true;
        let reader = state.reader.take();
        drop(state);
        wake(reader);
    }
}

/// The outgoing body a [`BodySink`] feeds.
pub struct ChannelBody {
    /// `None` for a body that was built empty.
    pipe: Option<Arc<Pipe>>,
    length: Option<u64>,
}

impl ChannelBody {
    #[must_use]
    pub const fn empty() -> Self {
        Self {
            pipe: None,
            length: Some(0),
        }
    }

    /// A body of exactly `data`.
    #[must_use]
    pub fn full(data: Bytes) -> Self {
        Self {
            length: Some(data.len() as u64),
            pipe: Some(Arc::new(Pipe::ended(Some(data)))),
        }
    }

    /// A body with the same framing as this one but no data in it. A request whose body put
    /// nothing on the wire is sent again with it.
    #[must_use]
    pub fn ended(&self) -> Self {
        Self {
            pipe: self.pipe.as_ref().map(|_| Arc::new(Pipe::ended(None))),
            length: self.length,
        }
    }
}

impl Drop for ChannelBody {
    fn drop(&mut self) {
        if let Some(pipe) = &self.pipe {
            let mut state = pipe.state();
            state.gone = true;
            state.chunk = None;
            let writer = state.writer.take();
            let disconnect = mem::take(&mut state.disconnect);
            drop(state);
            wake(writer);
            disconnect.into_iter().for_each(Waker::wake);
        }
    }
}

/// A body, the sink that feeds it, and the handle that fails it; `length` is the declared size,
/// if known.
#[must_use]
pub fn channel(length: Option<u64>) -> (BodySink, BodyAbort, ChannelBody) {
    channel_with(Arc::new(Pipe::default()), None, length)
}

/// [`channel`], for a served connection whose responses reuse `scratch`.
pub fn channel_in(
    scratch: &Rc<Scratch>,
    length: Option<u64>,
) -> (BodySink, BodyAbort, ChannelBody) {
    channel_with(scratch.take_pipe(), Some(Rc::clone(scratch)), length)
}

fn channel_with(
    pipe: Arc<Pipe>,
    scratch: Option<Rc<Scratch>>,
    length: Option<u64>,
) -> (BodySink, BodyAbort, ChannelBody) {
    (
        BodySink {
            pipe: Some(Arc::clone(&pipe)),
            scratch,
            remaining: Cell::new(length),
        },
        BodyAbort(Arc::clone(&pipe)),
        ChannelBody {
            pipe: Some(pipe),
            length,
        },
    )
}

/// The failure of an aborted body.
#[derive(Debug)]
pub struct Aborted;

impl fmt::Display for Aborted {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("the body was aborted")
    }
}

impl Error for Aborted {}

impl Body for ChannelBody {
    type Data = Bytes;
    type Error = Aborted;

    fn poll_frame(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
    ) -> Poll<Option<Result<Frame<Bytes>, Aborted>>> {
        let Some(pipe) = &self.pipe else {
            return Poll::Ready(None);
        };
        let mut state = pipe.state();
        if state.aborted {
            return Poll::Ready(Some(Err(Aborted)));
        }
        if let Some(chunk) = state.chunk.take() {
            let writer = state.writer.take();
            drop(state);
            wake(writer);
            return Poll::Ready(Some(Ok(Frame::data(chunk))));
        }
        if state.ended {
            return Poll::Ready(None);
        }
        register(&mut state.reader, cx);
        Poll::Pending
    }

    // Only a body that was built empty: hyper drops the `Content-Length: 0` of a message whose
    // body says it has ended and, for a response, writes its own after the other headers.
    fn is_end_stream(&self) -> bool {
        self.pipe.is_none()
    }

    fn size_hint(&self) -> SizeHint {
        self.length.map_or_else(SizeHint::new, SizeHint::with_exact)
    }
}

// =======================================================================================
// Headers

/// hyper parses at most this many headers per message (its default is 100). kj bounds only the
/// header block's size (128 KiB), so the count limit is set well above what real messages carry.
pub const MAX_HEADERS: usize = 16 * 1024;

/// kj's builtin headers through `Host`, in the order kj writes them
/// (`KJ_HTTP_FOR_EACH_BUILTIN_HEADER`), ahead of every other header.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Builtin {
    Connection,
    KeepAlive,
    Te,
    Trailer,
    Upgrade,
    ContentLength,
    TransferEncoding,
    SecWebSocketKey,
    SecWebSocketVersion,
    SecWebSocketAccept,
    SecWebSocketExtensions,
    Host,
}

/// The [`Builtin`] headers as kj's table spells them, indexed by the enum.
const BUILTIN: [&str; 12] = [
    "Connection",
    "Keep-Alive",
    "TE",
    "Trailer",
    "Upgrade",
    "Content-Length",
    "Transfer-Encoding",
    "Sec-WebSocket-Key",
    "Sec-WebSocket-Version",
    "Sec-WebSocket-Accept",
    "Sec-WebSocket-Extensions",
    "Host",
];

/// The [`Builtin`] headers' names, indexed by the enum.
static BUILTIN_NAMES: [http::HeaderName; BUILTIN.len()] = [
    header::CONNECTION,
    http::HeaderName::from_static("keep-alive"),
    header::TE,
    header::TRAILER,
    header::UPGRADE,
    header::CONTENT_LENGTH,
    header::TRANSFER_ENCODING,
    header::SEC_WEBSOCKET_KEY,
    header::SEC_WEBSOCKET_VERSION,
    header::SEC_WEBSOCKET_ACCEPT,
    header::SEC_WEBSOCKET_EXTENSIONS,
    header::HOST,
];

/// The builtin headers a message's protocol sets itself (kj's `connectionHeaders`): the
/// application's values for them are not sent.
#[derive(Clone, Copy, Debug)]
pub enum Protocol {
    /// A response to HEAD whose application described the body: `Connection` through `Upgrade`.
    HeadResponse,
    /// A request or a response: `Connection` through `Transfer-Encoding`.
    Message,
    /// A WebSocket handshake: `Connection` through `Sec-WebSocket-Extensions`.
    WebSocket,
}

impl Protocol {
    /// How many of the builtin headers, from the first, are the protocol's.
    const fn count(self) -> usize {
        match self {
            Self::HeadResponse => Builtin::ContentLength as usize,
            Self::Message => Builtin::SecWebSocketKey as usize,
            Self::WebSocket => Builtin::Host as usize,
        }
    }
}

/// Outgoing headers, written as kj writes them.
///
/// kj writes the headers of its table first, in the table's order and spelling, then the rest in
/// the order they were added, which is the order `kj::HttpHeaders::forEach` yields. The table
/// starts with the [`Builtin`] headers, some of which the protocol sets in place of the
/// application ([`Protocol`]); a `Head` keeps those apart so that they go out first whoever set
/// them, and hyper, finding its framing and `Connection` headers already there, adds none at
/// the end.
///
/// `http::HeaderName` lowercases, so the spellings ride along in hyper's `HeaderCaseMap`
/// extension, which its encoder honors (made public by
/// patches/rust/crates/hyper/public-header-case-map.patch). hyper title-cases a name it has no
/// spelling for (kj-hyper sets `title_case_headers`), so a message whose names are all spelled as
/// hyper title-cases them, as most are, and whose values are not empty carries no extension.
/// hyper pairs a name's spellings with its values in order and title-cases the values past its
/// last spelling, so a name with a spelling for any of its values has one for each.
pub struct Head {
    builtin: [Option<http::HeaderValue>; BUILTIN.len()],
    /// Every other header, in kj's order.
    others: Vec<(http::HeaderName, http::HeaderValue)>,
    /// None until a name is spelled other than as hyper title-cases it.
    spellings: Option<HeaderCaseMap>,
    /// Where a served connection's values are copied to, and `others` is kept.
    scratch: Option<Rc<Scratch>>,
}

impl Head {
    /// Everything the application set.
    pub fn new(headers: &HttpHeaders) -> Self {
        let mut head = Self::empty();
        for_each_header(headers, &mut head);
        head
    }

    /// [`Head::new`], for a served connection whose responses reuse `scratch`.
    pub fn new_in(scratch: &Rc<Scratch>, headers: &HttpHeaders) -> Self {
        let mut head = Self::empty_in(scratch);
        for_each_header(headers, &mut head);
        head
    }

    #[must_use]
    pub fn empty() -> Self {
        Self {
            builtin: Default::default(),
            others: Vec::new(),
            spellings: None,
            scratch: None,
        }
    }

    #[must_use]
    pub fn empty_in(scratch: &Rc<Scratch>) -> Self {
        Self {
            others: scratch.take_others(),
            scratch: Some(Rc::clone(scratch)),
            ..Self::empty()
        }
    }

    /// A header value of `bytes`, which are valid as one.
    fn value(&self, bytes: &[u8]) -> Option<http::HeaderValue> {
        match &self.scratch {
            Some(scratch) => http::HeaderValue::from_maybe_shared(scratch.copy(bytes)).ok(),
            None => http::HeaderValue::from_bytes(bytes).ok(),
        }
    }

    /// One header from kj, in order. kj validated both parts, so they are valid here too.
    pub fn append(&mut self, name: &[u8], value: &[u8]) {
        let Some(value) = self.value(value) else {
            return;
        };
        let builtin = BUILTIN
            .iter()
            .position(|spelling| spelling.as_bytes().eq_ignore_ascii_case(name));
        if let Some(builtin) = builtin {
            self.builtin[builtin] = Some(value);
        } else if let Ok(header) = http::HeaderName::from_bytes(name) {
            self.spell(&header, name);
            self.others.push((header, value));
        }
    }

    /// Records the spelling of the value of `name` about to be appended, if hyper would write it
    /// otherwise or the name's earlier values have spellings.
    fn spell(&mut self, name: &http::HeaderName, spelling: &[u8]) {
        let spelled = self
            .spellings
            .as_ref()
            .map_or(0, |spellings| spellings.get_all_internal(name).count());
        if spelled == 0 && is_title_case(spelling) {
            return;
        }
        let spellings = self.spellings.get_or_insert_with(HeaderCaseMap::default);
        // The name's earlier values went unspelled, as hyper would write them.
        let values = self
            .others
            .iter()
            .filter(|(other, _)| other == name)
            .count();
        for _ in spelled..values {
            spellings.append(name, title_case(name));
        }
        spellings.append(name, Bytes::copy_from_slice(spelling));
    }

    /// Drops the application's values of the headers that are `protocol`'s to set.
    pub fn claim(&mut self, protocol: Protocol) {
        self.builtin[..protocol.count()].fill(None);
    }

    pub fn set(&mut self, builtin: Builtin, value: http::HeaderValue) {
        self.builtin[builtin as usize] = Some(value);
    }

    #[must_use]
    pub const fn has(&self, builtin: Builtin) -> bool {
        self.builtin[builtin as usize].is_some()
    }

    /// Frames a body of `length` bytes, or a chunked one when the length is unknown.
    pub fn frame(&mut self, length: Option<u64>) {
        match length {
            Some(length) => {
                let mut digits = [0; 20];
                let len = format_decimal(length, &mut digits);
                let value = self
                    .value(&digits[digits.len() - len..])
                    .unwrap_or_else(|| http::HeaderValue::from(length));
                self.set(Builtin::ContentLength, value);
            }
            None => self.set(
                Builtin::TransferEncoding,
                http::HeaderValue::from_static("chunked"),
            ),
        }
    }

    /// Installs the headers and their spellings on a message's parts, in place of the headers
    /// there, reusing the map's room.
    pub fn apply(mut self, map: &mut http::HeaderMap, extensions: &mut http::Extensions) {
        let builtins = self.builtin.iter().flatten().count();
        map.clear();
        map.reserve(self.others.len() + builtins);
        for (index, value) in self.builtin.into_iter().enumerate() {
            let Some(value) = value else {
                continue;
            };
            let (spelling, name) = (BUILTIN[index], &BUILTIN_NAMES[index]);
            // A builtin header has one value, and no spelling yet.
            if !is_title_case(spelling.as_bytes()) {
                self.spellings
                    .get_or_insert_with(HeaderCaseMap::default)
                    .append(name, Bytes::from_static(spelling.as_bytes()));
            }
            map.append(name.clone(), value);
        }
        for (name, value) in self.others.drain(..) {
            map.append(name, value);
        }
        if let Some(scratch) = &self.scratch {
            scratch.keep_others(mem::take(&mut self.others));
        }
        // hyper's client writes an empty value as `Name:` only through the extension.
        let spellings = self.spellings.or_else(|| {
            map.values()
                .any(http::HeaderValue::is_empty)
                .then(HeaderCaseMap::default)
        });
        if let Some(spellings) = spellings {
            extensions.insert(spellings);
        }
    }
}

/// Writes `n` in decimal at the end of `digits`; returns how many digits it took.
fn format_decimal(mut n: u64, digits: &mut [u8; 20]) -> usize {
    let mut len = 0;
    loop {
        len += 1;
        digits[digits.len() - len] = b'0' + (n % 10) as u8;
        n /= 10;
        if n == 0 {
            return len;
        }
    }
}

/// Whether hyper's title-casing of the lowercased `spelling` gives `spelling` back: a letter at
/// the start or after a `-` is uppercase, and every other letter is lowercase.
fn is_title_case(spelling: &[u8]) -> bool {
    let mut after_dash = true;
    spelling.iter().all(|&c| {
        let cased = if after_dash {
            !c.is_ascii_lowercase()
        } else {
            !c.is_ascii_uppercase()
        };
        after_dash = c == b'-';
        cased
    })
}

/// `name` as hyper title-cases it.
fn title_case(name: &http::HeaderName) -> Bytes {
    let mut after_dash = true;
    let cased: Vec<u8> = name
        .as_str()
        .bytes()
        .map(|c| {
            let c = if after_dash {
                c.to_ascii_uppercase()
            } else {
                c
            };
            after_dash = c == b'-';
            c
        })
        .collect();
    cased.into()
}

/// A header in a [`HeaderBlock`] whose name has no slot in [`HeaderIds`]: C++ looks it up.
const UNINDEXED: u32 = u32::MAX;

/// How many names [`HeaderIds`] remembers, so a peer cannot grow it without end. Names past
/// that are looked up as kj looks them up.
const MAX_REMEMBERED_NAMES: usize = 128;

/// The kj ids of the header names one connection's requests carry, each looked up in the table
/// once instead of on every request (`kj::HttpHeaderTable::stringToId` hashes a name byte by
/// byte; hyper's `HeaderName` of a standard header hashes as one number).
pub struct HeaderIds<'t> {
    kj: Borrowing<'t, ffi::HeaderIds>,
    /// The slot in `kj` of each name looked up so far, or [`UNINDEXED`].
    slots: http::HeaderMap<u32>,
}

impl<'t> HeaderIds<'t> {
    pub fn new(table: &'t HttpHeaderTable) -> Self {
        Self {
            kj: new_header_ids(table),
            slots: http::HeaderMap::default(),
        }
    }

    fn slot(&mut self, name: &http::HeaderName) -> u32 {
        if let Some(slot) = self.slots.get(name) {
            return *slot;
        }
        let slot = u32::try_from(self.kj.as_mut().resolve(name.as_str())).unwrap_or(UNINDEXED);
        if self.slots.len() < MAX_REMEMBERED_NAMES {
            self.slots.insert(name.clone(), slot);
        }
        slot
    }
}

/// Incoming headers packed for C++ to build `kj::HttpHeaders` from.
///
/// Each header's name and value, each followed by a NUL, and per header its name's slot in
/// [`HeaderIds`] (or [`UNINDEXED`]) and the two lengths. Names keep the peer's spelling where
/// hyper recorded it (`preserve_header_case`); hyper groups repeated names. A server reuses its
/// blocks, so filling one allocates only when its headers outgrow it.
#[derive(Default)]
pub struct HeaderBlock {
    arena: Vec<u8>,
    fields: Vec<u32>,
}

impl HeaderBlock {
    /// A block of `map`'s headers, every name to be looked up by C++.
    pub fn new(map: &http::HeaderMap, extensions: &http::Extensions) -> Self {
        let mut block = Self::default();
        block.fill(map, extensions, None);
        block
    }

    /// Packs `map`'s headers in place of the block's, with the slots `ids` gives their names.
    pub fn fill(
        &mut self,
        map: &http::HeaderMap,
        extensions: &http::Extensions,
        mut ids: Option<&mut HeaderIds<'_>>,
    ) {
        let spellings = extensions.get::<HeaderCaseMap>();
        // A spelling is as long as its name.
        let size = map
            .iter()
            .map(|(name, value)| name.as_str().len() + value.len() + 2)
            .sum();
        self.arena.clear();
        self.arena.reserve(size);
        self.fields.clear();
        self.fields.reserve(3 * map.len());
        for name in map.keys() {
            let slot = ids.as_mut().map_or(UNINDEXED, |ids| ids.slot(name));
            let mut spelled = spellings.map(|s| s.get_all_internal(name));
            for value in map.get_all(name) {
                let spelling = spelled.as_mut().and_then(Iterator::next);
                let name = spelling.map_or(name.as_str().as_bytes(), |s| s.as_ref());
                // hyper bounds a header block far below 4 GiB.
                let (Ok(name_len), Ok(value_len)) =
                    (u32::try_from(name.len()), u32::try_from(value.len()))
                else {
                    continue;
                };
                self.arena.extend_from_slice(name);
                self.arena.push(0);
                self.arena.extend_from_slice(value.as_bytes());
                self.arena.push(0);
                self.fields.extend([slot, name_len, value_len]);
            }
        }
    }

    /// The headers as `kj::HttpHeaders` over `table`, which they borrow, owning a copy of the
    /// block.
    ///
    /// # Errors
    ///
    /// A block whose lengths do not describe its bytes.
    pub fn to_kj<'t>(&self, table: &'t HttpHeaderTable) -> Result<Borrowing<'t, HttpHeaders>> {
        headers_from_block(table, &self.arena, &self.fields)
    }

    /// The headers as `kj::HttpHeaders` over the table behind `ids`, which filled the block,
    /// pointing into the block, which they borrow.
    ///
    /// # Errors
    ///
    /// A block whose lengths do not describe its bytes, or with a slot `ids` lacks.
    pub fn as_kj<'a, 't>(
        &'a self,
        ids: &HeaderIds<'t>,
        headers: &'a mut ReusedHeaders<'t>,
    ) -> Result<&'a HttpHeaders> {
        headers.over(&ids.kj, &self.arena, &self.fields)
    }

    /// Whether the block is small enough to keep for another request.
    pub fn reusable(&self) -> bool {
        self.arena.capacity() <= 8192
    }
}

#[cfg(test)]
#[path = "body-test.rs"]
mod tests;
