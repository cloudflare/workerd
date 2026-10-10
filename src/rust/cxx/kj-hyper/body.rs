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
//! ([`HeaderBlock`]) and C++ checks the block's bounds before building `kj::HttpHeaders`.

use std::cell::Cell;
use std::cell::RefCell;
use std::error::Error;
use std::fmt;
use std::future::Future;
use std::future::pending;
use std::future::poll_fn;
use std::pin::Pin;
use std::rc::Rc;
use std::task::Context;
use std::task::Poll;

use bytes::Buf;
use bytes::Bytes;
use cxx::KjError;
use cxx::KjExceptionType;
use futures::FutureExt;
use http_body::Body;
use http_body::Frame;
use http_body::SizeHint;
use hyper::body::Incoming;
use hyper::ext::HeaderCaseMap;
use tokio::io::ReadBuf;
use tokio::sync::mpsc;
use tokio::sync::oneshot;

use crate::Result;
use crate::ffi::Borrowing;
use crate::ffi::HttpHeaderTable;
use crate::ffi::HttpHeaders;
use crate::ffi::for_each_header;
use crate::ffi::headers_from_block;

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

/// A `kj::AsyncOutputStream` feeding an outgoing hyper body. Dropping it ends the body.
pub struct BodySink {
    /// `None` for a body that is discarded (a response to HEAD).
    tx: Option<mpsc::Sender<Bytes>>,
    /// Bytes still to be written, for a body of declared length. A `Cell`: kj writes through
    /// `&self`, as `whenWriteDisconnected()` may be awaited alongside a write.
    remaining: Cell<Option<u64>>,
}

impl BodySink {
    /// A sink whose writes are accepted and dropped, as kj's for a response to HEAD.
    #[must_use]
    pub fn discarding() -> Self {
        Self {
            tx: None,
            remaining: Cell::new(None),
        }
    }

    /// Queues `data`; resolves once the body has room for more. A write beyond the declared
    /// length fails before any of it is queued, as kj's does.
    pub fn write(&self, data: &[u8]) -> impl Future<Output = Result<()>> + use<> {
        let queued = match (&self.tx, self.remaining.get()) {
            (None, _) => Ok(None),
            (Some(_), Some(remaining)) if data.len() as u64 > remaining => Err(KjError::new(
                KjExceptionType::Failed,
                "overwrote Content-Length".to_owned(),
            )),
            (Some(tx), remaining) => {
                self.remaining.set(remaining.map(|r| r - data.len() as u64));
                Ok(Some((tx.clone(), Bytes::copy_from_slice(data))))
            }
        };
        async move {
            let Some((tx, data)) = queued? else {
                return Ok(());
            };
            if data.is_empty() {
                return Ok(());
            }
            tx.send(data).await.map_err(|_| {
                KjError::new(
                    KjExceptionType::Disconnected,
                    "HTTP body: the peer went away".to_owned(),
                )
            })
        }
    }

    pub fn when_write_disconnected(&self) -> impl Future<Output = ()> + use<> {
        let tx = self.tx.clone();
        async move {
            match tx {
                Some(tx) => tx.closed().await,
                None => pending().await,
            }
        }
    }
}

/// Ends a [`ChannelBody`] with an error, so hyper drops the connection instead of framing what
/// was written as a complete message. Dropping it unused has no effect.
pub struct BodyAbort(oneshot::Sender<()>);

impl BodyAbort {
    pub fn abort(self) {
        let _ = self.0.send(());
    }
}

/// The outgoing body a [`BodySink`] feeds.
pub struct ChannelBody {
    rx: Option<mpsc::Receiver<Bytes>>,
    abort: Option<oneshot::Receiver<()>>,
    length: Option<u64>,
}

impl ChannelBody {
    #[must_use]
    pub fn empty() -> Self {
        Self {
            rx: None,
            abort: None,
            length: Some(0),
        }
    }

    /// A body of exactly `data`.
    #[must_use]
    pub fn full(data: Bytes) -> Self {
        let length = Some(data.len() as u64);
        let (tx, rx) = mpsc::channel(1);
        // A fresh channel has room for the one chunk.
        let _ = tx.try_send(data);
        Self {
            rx: Some(rx),
            abort: None,
            length,
        }
    }
}

/// A body, the sink that feeds it, and the handle that fails it; `length` is the declared size,
/// if known.
#[must_use]
pub fn channel(length: Option<u64>) -> (BodySink, BodyAbort, ChannelBody) {
    let (tx, rx) = mpsc::channel(1);
    let (abort_tx, abort_rx) = oneshot::channel();
    (
        BodySink {
            tx: Some(tx),
            remaining: Cell::new(length),
        },
        BodyAbort(abort_tx),
        ChannelBody {
            rx: Some(rx),
            abort: Some(abort_rx),
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
        let this = self.get_mut();
        if let Some(abort) = this.abort.as_mut() {
            match abort.poll_unpin(cx) {
                Poll::Ready(Ok(())) => return Poll::Ready(Some(Err(Aborted))),
                // The abort handle was dropped unused: nothing more to watch.
                Poll::Ready(Err(_)) => this.abort = None,
                Poll::Pending => {}
            }
        }
        let Some(rx) = &mut this.rx else {
            return Poll::Ready(None);
        };
        rx.poll_recv(cx)
            .map(|chunk| chunk.map(|data| Ok(Frame::data(data))))
    }

    // Only a body that was built empty: hyper drops the `Content-Length: 0` of a message whose
    // body says it has ended and, for a response, writes its own after the other headers.
    fn is_end_stream(&self) -> bool {
        self.rx.is_none()
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
/// patches/rust/crates/hyper/public-header-case-map.patch).
pub struct Head {
    builtin: [Option<http::HeaderValue>; BUILTIN.len()],
    /// Every other header, in kj's order.
    map: http::HeaderMap,
    spellings: HeaderCaseMap,
}

impl Head {
    /// Everything the application set.
    pub fn new(headers: &HttpHeaders) -> Self {
        let mut head = Self::empty();
        for_each_header(headers, &mut head);
        head
    }

    #[must_use]
    pub fn empty() -> Self {
        Self {
            builtin: Default::default(),
            map: http::HeaderMap::new(),
            spellings: HeaderCaseMap::default(),
        }
    }

    /// One header from kj, in order. kj validated both parts, so they are valid here too.
    pub fn append(&mut self, name: &[u8], value: &[u8]) {
        let Ok(value) = http::HeaderValue::from_bytes(value) else {
            return;
        };
        let builtin = BUILTIN
            .iter()
            .position(|spelling| spelling.as_bytes().eq_ignore_ascii_case(name));
        if let Some(builtin) = builtin {
            self.builtin[builtin] = Some(value);
        } else if let Ok(header) = http::HeaderName::from_bytes(name) {
            self.spellings.append(&header, Bytes::copy_from_slice(name));
            self.map.append(header, value);
        }
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
            Some(length) => self.set(Builtin::ContentLength, http::HeaderValue::from(length)),
            None => self.set(
                Builtin::TransferEncoding,
                http::HeaderValue::from_static("chunked"),
            ),
        }
    }

    /// Installs the headers and their spellings on a message's parts.
    pub fn apply(mut self, headers: &mut http::HeaderMap, extensions: &mut http::Extensions) {
        let mut map = http::HeaderMap::with_capacity(self.map.len() + BUILTIN.len());
        for (spelling, value) in BUILTIN.into_iter().zip(self.builtin) {
            // `from_bytes` lowercases; the spellings are valid header names.
            if let (Some(value), Ok(name)) =
                (value, http::HeaderName::from_bytes(spelling.as_bytes()))
            {
                self.spellings
                    .append(&name, Bytes::from_static(spelling.as_bytes()));
                map.append(name, value);
            }
        }
        map.extend(self.map);
        *headers = map;
        extensions.insert(self.spellings);
    }
}

/// Incoming headers packed for C++ to build `kj::HttpHeaders` from in one allocation.
///
/// Each header's name and value, each followed by a NUL, and their lengths. Names keep the
/// peer's spelling where hyper recorded it (`preserve_header_case`); hyper groups repeated names.
#[derive(Default)]
pub struct HeaderBlock {
    arena: Vec<u8>,
    lens: Vec<u32>,
}

impl HeaderBlock {
    pub fn new(map: &http::HeaderMap, extensions: &http::Extensions) -> Self {
        let spellings = extensions.get::<HeaderCaseMap>();
        let mut block = Self::default();
        for name in map.keys() {
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
                block.arena.extend_from_slice(name);
                block.arena.push(0);
                block.arena.extend_from_slice(value.as_bytes());
                block.arena.push(0);
                block.lens.extend([name_len, value_len]);
            }
        }
        block
    }

    /// The headers as `kj::HttpHeaders` over `table`, which they borrow.
    ///
    /// # Errors
    ///
    /// A block whose lengths do not describe its bytes.
    pub fn to_kj<'t>(&self, table: &'t HttpHeaderTable) -> Result<Borrowing<'t, HttpHeaders>> {
        headers_from_block(table, &self.arena, &self.lens)
    }
}

#[cfg(test)]
#[path = "body-test.rs"]
mod tests;
