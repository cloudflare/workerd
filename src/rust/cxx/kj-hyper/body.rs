//! HTTP message bodies in both directions, and header conversion.

use std::pin::Pin;
use std::task::Context;
use std::task::Poll;

use bytes::Buf;
use bytes::Bytes;
use http_body::Frame;
use http_body::SizeHint;
use http_body_util::BodyExt;
use hyper::body::Incoming;
use kj::http::HeaderTable;
use kj::http::Headers;
use kj::http::HeadersRef;
use kj::http::ffi::HttpHeaders;
use tokio::io::ReadBuf;
use tokio::sync::mpsc;

/// An incoming body as a `kj::AsyncInputStream`.
pub struct RustBody {
    body: Option<Incoming>,
    buffered: Bytes,
}

impl RustBody {
    pub fn new(body: Incoming) -> Self {
        Self {
            body: Some(body),
            buffered: Bytes::new(),
        }
    }

    pub fn empty() -> Self {
        Self {
            body: None,
            buffered: Bytes::new(),
        }
    }

    pub async fn read(&mut self, buf: &mut ReadBuf<'_>, min_bytes: usize) -> kj::Result<usize> {
        let min_bytes = min_bytes.min(buf.capacity());
        loop {
            let n = self.buffered.len().min(buf.remaining());
            buf.put_slice(&self.buffered[..n]);
            self.buffered.advance(n);
            if buf.filled().len() >= min_bytes {
                return Ok(buf.filled().len());
            }
            let Some(body) = &mut self.body else {
                return Ok(buf.filled().len());
            };
            match body.frame().await {
                Some(Ok(frame)) => {
                    if let Ok(data) = frame.into_data() {
                        self.buffered = data;
                    }
                }
                Some(Err(e)) => {
                    return Err(kj::KjError::new(
                        kj::KjExceptionType::Disconnected,
                        format!("HTTP body: {e}"),
                    ));
                }
                None => self.body = None,
            }
        }
    }

    #[must_use]
    pub fn length(&self) -> Option<u64> {
        let hint = self
            .body
            .as_ref()
            .map_or_else(SizeHint::new, http_body::Body::size_hint);
        hint.exact().map(|n| n + self.buffered.len() as u64)
    }
}

/// A `kj::AsyncOutputStream` feeding an outgoing hyper body. Dropping it ends the body.
pub struct BodySink(mpsc::Sender<Bytes>);

impl BodySink {
    pub async fn write(&self, data: &[u8]) -> kj::Result<()> {
        self.0
            .send(Bytes::copy_from_slice(data))
            .await
            .map_err(|_| {
                kj::KjError::new(
                    kj::KjExceptionType::Disconnected,
                    "HTTP body: the peer went away".to_owned(),
                )
            })
    }

    pub async fn when_write_disconnected(&self) {
        self.0.closed().await;
    }

    /// Queues one chunk without waiting (a fresh sink has room for it).
    pub fn try_send(&self, data: &[u8]) -> bool {
        self.0.try_send(Bytes::copy_from_slice(data)).is_ok()
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
        BodySink(tx),
        ChannelBody {
            rx: Some(rx),
            length,
        },
    )
}

impl http_body::Body for ChannelBody {
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

/// Outgoing headers, spelled as kj wrote them: `http::HeaderName` lowercases, so the original
/// spellings ride along in hyper's `HeaderCaseMap` extension, which its encoder honors (made
/// public by patches/rust/hyper-public-header-case-map.patch).
/// hyper parses at most this many headers per message (its default is 100). kj bounds only the
/// header block's size (128 KiB), so the count limit is set well above what real messages carry.
pub const MAX_HEADERS: usize = 16 * 1024;

pub struct Head {
    pub map: http::HeaderMap,
    spellings: hyper::ext::HeaderCaseMap,
}

impl Head {
    pub fn new(headers: &HttpHeaders) -> Self {
        let mut head = Self {
            map: http::HeaderMap::new(),
            spellings: hyper::ext::HeaderCaseMap::default(),
        };
        for entry in HeadersRef::from(headers).entries() {
            if let (Ok(name), Ok(value)) = (
                http::HeaderName::from_bytes(entry.name.as_bytes()),
                http::HeaderValue::from_bytes(&entry.value),
            ) {
                head.spellings
                    .append(&name, Bytes::copy_from_slice(entry.name.as_bytes()));
                head.map.append(name, value);
            }
        }
        head
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

pub fn from_header_map<'t>(table: &'t HeaderTable, map: &http::HeaderMap) -> Headers<'t> {
    let mut headers = Headers::new(table);
    // Values hyper accepted can only fail kj's validation on bytes kj rejects; drop those.
    let _ = headers.add_all(
        map.iter()
            .map(|(name, value)| (name.as_str(), value.as_bytes())),
    );
    headers
}
