//! HTTP message bodies in both directions, and header conversion.

use std::cell::Cell;
use std::cell::RefCell;
use std::future::Future;
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
use tokio::io::ReadBuf;
use tokio::sync::mpsc;
use tokio::sync::oneshot;

use crate::ffi::Borrowing;
use crate::ffi::HttpHeaders;

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
    ) -> impl Future<Output = crate::Result<usize>> + use<'b> {
        let state = self.0.clone();
        let min_bytes = min_bytes.min(buf.capacity());
        std::future::poll_fn(move |cx| {
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
    /// Bytes still to be written, for a body of declared length.
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
    pub fn write(&self, data: &[u8]) -> impl Future<Output = crate::Result<()>> + use<> {
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
                None => std::future::pending().await,
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

impl std::fmt::Display for Aborted {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("the body was aborted")
    }
}

impl std::error::Error for Aborted {}

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

    // hyper frames a body it knows is empty without a `Content-Length: 0`.
    fn is_end_stream(&self) -> bool {
        self.rx.is_none() || self.length == Some(0)
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

/// Outgoing headers, spelled as kj wrote them.
///
/// `http::HeaderName` lowercases, so the original spellings ride along in hyper's
/// `HeaderCaseMap` extension, which its encoder honors (made public by
/// patches/rust/hyper-public-header-case-map.patch).
pub struct Head {
    map: http::HeaderMap,
    spellings: hyper::ext::HeaderCaseMap,
}

impl Head {
    pub fn new(headers: &HttpHeaders) -> Self {
        let mut head = Self::empty();
        crate::ffi::for_each_header(headers, &mut head);
        head
    }

    #[must_use]
    pub fn empty() -> Self {
        Self {
            map: http::HeaderMap::new(),
            spellings: hyper::ext::HeaderCaseMap::default(),
        }
    }

    /// One header from kj, in order. kj validated both parts, so they are valid here too.
    pub fn append(&mut self, name: &[u8], value: &[u8]) {
        if let (Ok(header), Ok(value)) = (
            http::HeaderName::from_bytes(name),
            http::HeaderValue::from_bytes(value),
        ) {
            self.spellings.append(&header, Bytes::copy_from_slice(name));
            self.map.append(header, value);
        }
    }

    /// Sets a header the protocol requires, spelled `spelling`.
    pub fn set(
        &mut self,
        name: http::HeaderName,
        spelling: &'static str,
        value: http::HeaderValue,
    ) {
        self.spellings
            .append(&name, Bytes::from_static(spelling.as_bytes()));
        self.map.insert(name, value);
    }

    pub fn remove(&mut self, name: &http::HeaderName) {
        self.map.remove(name);
    }

    #[must_use]
    pub fn contains(&self, name: &http::HeaderName) -> bool {
        self.map.contains_key(name)
    }

    /// Puts `Content-Length`, when the size is known, after `Connection`, as kj serializes it.
    #[must_use]
    pub fn with_length(mut self, length: Option<u64>) -> Self {
        let Some(length) = length else { return self };
        let mut map = http::HeaderMap::with_capacity(self.map.len() + 1);
        for value in self.map.get_all(http::header::CONNECTION) {
            map.append(http::header::CONNECTION, value.clone());
        }
        map.insert(
            http::header::CONTENT_LENGTH,
            http::HeaderValue::from(length),
        );
        for (name, value) in &self.map {
            if name != http::header::CONTENT_LENGTH && name != http::header::CONNECTION {
                map.append(name.clone(), value.clone());
            }
        }
        self.map = map;
        self
    }

    /// Installs the headers and their spellings on a message's parts.
    pub fn apply(self, headers: &mut http::HeaderMap, extensions: &mut http::Extensions) {
        *headers = self.map;
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
        let spellings = extensions.get::<hyper::ext::HeaderCaseMap>();
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
    pub fn to_kj<'t>(
        &self,
        table: &'t crate::ffi::HttpHeaderTable,
    ) -> crate::Result<Borrowing<'t, HttpHeaders>> {
        crate::ffi::headers_from_block(table, &self.arena, &self.lens)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_write_beyond_the_declared_length_fails_before_queueing() {
        futures::executor::block_on(async {
            let (sink, _abort, mut body) = channel(Some(5));
            assert!(sink.write(b"0123456789").await.is_err());
            sink.write(b"01234").await.unwrap();
            drop(sink);
            let frame = std::future::poll_fn(|cx| Pin::new(&mut body).poll_frame(cx))
                .await
                .unwrap()
                .unwrap();
            assert_eq!(frame.into_data().unwrap(), &b"01234"[..]);
        });
    }

    #[test]
    fn an_aborted_body_fails_rather_than_ending() {
        futures::executor::block_on(async {
            let (sink, abort, mut body) = channel(None);
            drop(sink);
            abort.abort();
            let frame = std::future::poll_fn(|cx| Pin::new(&mut body).poll_frame(cx)).await;
            assert!(matches!(frame, Some(Err(Aborted))));
        });
    }

    #[test]
    fn a_dropped_abort_handle_lets_the_body_end() {
        futures::executor::block_on(async {
            let (sink, abort, mut body) = channel(None);
            drop(abort);
            drop(sink);
            let frame = std::future::poll_fn(|cx| Pin::new(&mut body).poll_frame(cx)).await;
            assert!(frame.is_none());
        });
    }
}
