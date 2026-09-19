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
use http_body::Body;
use http_body::Frame;
use http_body::SizeHint;
use hyper::body::Incoming;
use tokio::io::ReadBuf;
use tokio::sync::mpsc;

use crate::ffi::HttpHeaders;

/// Set once an application drops a response body it has not read to the end.
pub type Abandoned = Rc<Cell<bool>>;

struct BodyState {
    body: RefCell<Option<Incoming>>,
    buffered: RefCell<Bytes>,
    abandoned: Option<Abandoned>,
}

impl Drop for BodyState {
    fn drop(&mut self) {
        let unfinished = self
            .body
            .get_mut()
            .as_ref()
            .is_some_and(|body| !body.is_end_stream());
        if unfinished && let Some(flag) = &self.abandoned {
            flag.set(true);
        }
    }
}

/// An incoming body as a `kj::AsyncInputStream`. Each read owns a share of the body, which is
/// borrowed only inside a poll, so `tryGetLength()` may be called while a read waits.
pub struct RustBody(Rc<BodyState>);

impl RustBody {
    /// `abandoned` is set if the body is dropped before its end.
    pub fn new(body: Incoming, abandoned: Option<Abandoned>) -> Self {
        Self(Rc::new(BodyState {
            body: RefCell::new(Some(body)),
            buffered: RefCell::new(Bytes::new()),
            abandoned,
        }))
    }

    pub fn empty() -> Self {
        Self(Rc::new(BodyState {
            body: RefCell::new(None),
            buffered: RefCell::new(Bytes::new()),
            abandoned: None,
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

/// The outgoing body a [`BodySink`] feeds.
pub struct ChannelBody {
    rx: Option<mpsc::Receiver<Bytes>>,
    length: Option<u64>,
}

impl ChannelBody {
    #[must_use]
    pub fn empty() -> Self {
        Self {
            rx: None,
            length: Some(0),
        }
    }
}

/// A body and the sink that feeds it; `length` is the declared size, if known.
#[must_use]
pub fn channel(length: Option<u64>) -> (BodySink, ChannelBody) {
    let (tx, rx) = mpsc::channel(1);
    (
        BodySink {
            tx: Some(tx),
            remaining: Cell::new(length),
        },
        ChannelBody {
            rx: Some(rx),
            length,
        },
    )
}

impl Body for ChannelBody {
    type Data = Bytes;
    type Error = std::convert::Infallible;

    fn poll_frame(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
    ) -> Poll<Option<Result<Frame<Bytes>, Self::Error>>> {
        let Some(rx) = &mut self.get_mut().rx else {
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

/// Outgoing headers, spelled as kj wrote them: `http::HeaderName` lowercases, so the original
/// spellings ride along in hyper's `HeaderCaseMap` extension, which its encoder honors (made
/// public by patches/rust/hyper-public-header-case-map.patch).
pub struct Head {
    map: http::HeaderMap,
    spellings: hyper::ext::HeaderCaseMap,
}

impl Head {
    pub fn new(headers: &HttpHeaders) -> Self {
        let mut head = Self {
            map: http::HeaderMap::new(),
            spellings: hyper::ext::HeaderCaseMap::default(),
        };
        crate::ffi::for_each_header(headers, &mut head);
        head
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

/// Incoming headers packed for C++ to build `kj::HttpHeaders` from in one allocation: each
/// header's name and value, each followed by a NUL, and their lengths. Names keep the peer's
/// spelling where hyper recorded it (`preserve_header_case`); hyper groups repeated names.
#[derive(Default)]
pub struct HeaderBlock {
    pub arena: Vec<u8>,
    pub lens: Vec<u32>,
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
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_write_beyond_the_declared_length_fails_before_queueing() {
        futures::executor::block_on(async {
            let (sink, mut body) = channel(Some(5));
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
}
