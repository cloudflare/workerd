// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! [`Body`]: a message body that crosses threads.
//!
//! A body is nothing, one `Bytes`, or the receiving end of a bounded channel of `Bytes` whose
//! sending end is a [`BodySender`]. It holds no code of whoever produces it, so either side of
//! the boundary can poll a body the other side made without running the other side's code.
//!
//! # Backpressure
//!
//! A channel holds at most [`CHANNEL_CHUNKS`] chunks: [`BodySender::send`] waits while it is
//! full. Nothing else is queued, so a slow reader stops the writer after that many chunks.
//!
//! # How a body ends
//!
//! A body ends cleanly only when its sender calls [`BodySender::finish`]. A sender that is
//! dropped instead, because its task panicked or was cancelled, ends the body with a
//! `Disconnected` error, so a truncated body is never mistaken for a complete one. A sender
//! learns that its reader is gone from [`BodySender::send`] failing or [`BodySender::closed`]
//! resolving.
//!
//! A body of declared length that delivers more or fewer bytes fails the same way a KJ stream
//! of declared length does.

use std::pin::Pin;
use std::sync::Arc;
use std::sync::atomic::AtomicBool;
use std::sync::atomic::Ordering;
use std::task::Context;
use std::task::Poll;
use std::task::ready;

use bytes::Bytes;
use http_body::Frame;
use http_body::SizeHint;
use tokio::sync::mpsc;

use crate::Error;
use crate::Result;

/// How many chunks a channel body holds before its sender waits.
pub const CHANNEL_CHUNKS: usize = 4;

/// A message body. See the module docs.
#[derive(Debug, Default)]
pub struct Body(Kind);

#[derive(Debug, Default)]
enum Kind {
    #[default]
    Empty,
    Full(Bytes),
    Channel(Channel),
}

#[derive(Debug)]
struct Channel {
    chunks: mpsc::Receiver<Result<Bytes>>,
    finished: Arc<AtomicBool>,
    /// Bytes still to come, for a body of declared length.
    remaining: Option<u64>,
}

impl Body {
    /// A body of no bytes.
    #[must_use]
    pub const fn empty() -> Self {
        Self(Kind::Empty)
    }

    /// A body of exactly `data`. It crosses the boundary with its head, in one step.
    #[must_use]
    pub fn full(data: impl Into<Bytes>) -> Self {
        let data = data.into();
        if data.is_empty() {
            Self::empty()
        } else {
            Self(Kind::Full(data))
        }
    }

    /// A body fed through the returned sender. `length` declares the body's size, which makes
    /// it a `Content-Length` message; without it the body is chunked.
    #[must_use]
    pub fn channel(length: Option<u64>) -> (BodySender, Self) {
        let (tx, rx) = mpsc::channel(CHANNEL_CHUNKS);
        let finished = Arc::new(AtomicBool::new(false));
        let sender = BodySender {
            chunks: tx,
            finished: Arc::clone(&finished),
        };
        let channel = Channel {
            chunks: rx,
            finished,
            remaining: length,
        };
        (sender, Self(Kind::Channel(channel)))
    }

    /// The bytes still to come, when known.
    #[must_use]
    pub fn length(&self) -> Option<u64> {
        match &self.0 {
            Kind::Empty => Some(0),
            Kind::Full(data) => Some(data.len() as u64),
            Kind::Channel(channel) => channel.remaining,
        }
    }

    /// The next chunk, `None` at the body's end. An error ends the body.
    pub async fn chunk(&mut self) -> Option<Result<Bytes>> {
        std::future::poll_fn(|cx| self.poll_chunk(cx)).await
    }

    /// As [`Body::chunk`].
    pub fn poll_chunk(&mut self, cx: &mut Context<'_>) -> Poll<Option<Result<Bytes>>> {
        let result = match &mut self.0 {
            Kind::Empty => return Poll::Ready(None),
            Kind::Full(data) => Ok(std::mem::take(data)),
            Kind::Channel(channel) => {
                let Some(result) = ready!(channel.poll(cx)) else {
                    self.0 = Kind::Empty;
                    return Poll::Ready(None);
                };
                result
            }
        };
        if matches!(self.0, Kind::Full(_)) || result.is_err() {
            self.0 = Kind::Empty;
        }
        Poll::Ready(Some(result))
    }
}

impl Channel {
    fn poll(&mut self, cx: &mut Context<'_>) -> Poll<Option<Result<Bytes>>> {
        match ready!(self.chunks.poll_recv(cx)) {
            Some(Ok(data)) => {
                if let Some(remaining) = &mut self.remaining {
                    let Some(rest) = remaining.checked_sub(data.len() as u64) else {
                        return Poll::Ready(Some(Err(kj::failed!(
                            "the body is longer than its declared length"
                        ))));
                    };
                    *remaining = rest;
                }
                Poll::Ready(Some(Ok(data)))
            }
            Some(Err(error)) => Poll::Ready(Some(Err(error))),
            // `finish()` stores the flag before it drops the sender, which is what closes the
            // channel, so a clean end is always seen as one.
            None if !self.finished.load(Ordering::Acquire) => Poll::Ready(Some(Err(
                kj::disconnected!("the body's sender went away before the body was complete"),
            ))),
            None if self.remaining.is_some_and(|remaining| remaining > 0) => Poll::Ready(Some(
                Err(kj::failed!("the body is shorter than its declared length")),
            )),
            None => Poll::Ready(None),
        }
    }
}

impl http_body::Body for Body {
    type Data = Bytes;
    type Error = Error;

    fn poll_frame(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
    ) -> Poll<Option<Result<Frame<Bytes>>>> {
        self.get_mut()
            .poll_chunk(cx)
            .map(|chunk| chunk.map(|data| data.map(Frame::data)))
    }

    fn is_end_stream(&self) -> bool {
        matches!(self.0, Kind::Empty)
    }

    fn size_hint(&self) -> SizeHint {
        self.length()
            .map_or_else(SizeHint::new, SizeHint::with_exact)
    }
}

/// The reader of a [`Body`] went away.
#[derive(Debug, PartialEq, Eq)]
pub struct Closed;

/// The sending end of a channel [`Body`]. See the module docs for how a body ends.
#[derive(Debug)]
pub struct BodySender {
    chunks: mpsc::Sender<Result<Bytes>>,
    finished: Arc<AtomicBool>,
}

impl BodySender {
    /// Queues `data`, waiting while the channel is full.
    ///
    /// # Errors
    ///
    /// The body's reader is gone.
    pub async fn send(&mut self, data: Bytes) -> Result<(), Closed> {
        if data.is_empty() {
            return Ok(());
        }
        self.chunks.send(Ok(data)).await.map_err(|_| Closed)
    }

    /// Ends the body cleanly.
    pub fn finish(self) {
        self.finished.store(true, Ordering::Release);
    }

    /// Ends the body with `error`, which its reader gets after the chunks already queued.
    pub async fn fail(self, error: Error) {
        // A reader that is gone needs no error.
        let _ = self.chunks.send(Err(error)).await;
    }

    /// Resolves once the body's reader is gone.
    pub async fn closed(&self) {
        self.chunks.closed().await;
    }
}

#[cfg(test)]
#[path = "body-test.rs"]
mod tests;
