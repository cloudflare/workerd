// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The reverse direction: a worker's [`IoChannels`] calls, performed on the loop thread.
//!
//! A worker's [`IoChannels`] is a [`LoopChannels`], which sends each call as a [`Call`] to the
//! adapter of the request it belongs to. [`serve`], a part of that adapter's request future,
//! performs the calls through the [`Host`]: the host starts a request on the channel and hands
//! back a `WorkerInterface`, on which the call is made as any KJ caller would make it.
//!
//! Every call in flight is a future [`serve`] owns, so cancelling the adapter's request cancels
//! them, as dropping an `IoContext` cancels the subrequests of a JavaScript worker's request.
//!
//! The queue of calls not yet started holds [`CALL_QUEUE`] calls, and at most [`MAX_IN_FLIGHT`]
//! are performed at once; a worker that makes more waits.

use std::future::poll_fn;
use std::pin::pin;
use std::task::Poll;

use futures::FutureExt;
use futures::StreamExt;
use futures::future::BoxFuture;
use futures::stream::FuturesUnordered;
use kj::http::HeadersRef;
use tokio::sync::mpsc;
use tokio::sync::oneshot;

use crate::Body;
use crate::Host;
use crate::IoChannels;
use crate::Request;
use crate::Response;
use crate::Result;
use crate::ffi::Subrequest;
use crate::head;
use crate::pump;

/// How many calls wait for the loop thread before a caller waits.
pub const CALL_QUEUE: usize = 16;

/// How many calls of one request the loop thread performs at once.
pub const MAX_IN_FLIGHT: usize = 64;

/// One [`IoChannels`] call on its way to the loop thread.
pub enum Call {
    Subrequest {
        channel: u32,
        request: Request,
        reply: oneshot::Sender<Result<Response>>,
    },
}

/// The [`IoChannels`] of one request: a sender of [`Call`]s to the request's adapter.
pub struct LoopChannels(mpsc::Sender<Call>);

impl LoopChannels {
    pub const fn new(calls: mpsc::Sender<Call>) -> Self {
        Self(calls)
    }
}

impl IoChannels for LoopChannels {
    fn subrequest(&self, channel: u32, request: Request) -> BoxFuture<'static, Result<Response>> {
        let calls = self.0.clone();
        Box::pin(async move {
            let gone = || kj::disconnected!("the request this subrequest belongs to is over");
            let (reply, response) = oneshot::channel();
            let call = Call::Subrequest {
                channel,
                request,
                reply,
            };
            calls.send(call).await.map_err(|_| gone())?;
            response.await.map_err(|_| gone())?
        })
    }
}

/// Performs the calls arriving on `calls` until every sender is gone and every call has
/// finished. Dropping the future cancels the calls in flight.
pub async fn serve(mut calls: mpsc::Receiver<Call>, host: &dyn Host) {
    let mut in_flight = FuturesUnordered::new();
    let mut open = true;
    poll_fn(|cx| {
        loop {
            while in_flight.poll_next_unpin(cx) == Poll::Ready(Some(())) {}
            if !open || in_flight.len() >= MAX_IN_FLIGHT {
                break;
            }
            match calls.poll_recv(cx) {
                // The new call gets its first poll on the way around.
                Poll::Ready(Some(call)) => in_flight.push(perform(host, call)),
                Poll::Ready(None) => open = false,
                Poll::Pending => break,
            }
        }
        if !open && in_flight.is_empty() {
            Poll::Ready(())
        } else {
            Poll::Pending
        }
    })
    .await;
}

async fn perform(host: &dyn Host, call: Call) {
    match call {
        Call::Subrequest {
            channel,
            request,
            reply,
        } => subrequest(host, channel, request, reply).await,
    }
}

fn start(
    host: &dyn Host,
    channel: u32,
    parts: &http::request::Parts,
    length: Option<u64>,
) -> Result<Subrequest> {
    let method = head::method_to_kj(&parts.method)?;
    let headers = head::headers_to_kj(host.header_table(), &parts.headers, &parts.extensions)?;
    let worker = host.start_subrequest(channel)?;
    Subrequest::start(
        worker,
        method,
        &parts.uri.to_string(),
        HeadersRef::from(&*headers),
        length,
    )
}

/// Writes the request body, then ends it. A target that stops reading is not a failure: its
/// response says what became of the request. The only error is the body's own.
async fn upload(mut body: Body, mut output: kj::io::AsyncOutputStream<'static>) -> Result<()> {
    while let Some(chunk) = body.chunk().await {
        if output.write(&chunk?).await.is_err() {
            break;
        }
    }
    Ok(())
}

enum Head {
    Arrived(Result<()>),
    /// The request body failed.
    Failed(crate::Error),
    /// The worker no longer waits for the response.
    Abandoned,
}

/// One subrequest, from the call to the end of its response body.
///
/// The worker gets the target's failure as the call's error before the response head, and as
/// the response body's error after it. A request body that fails cancels the subrequest: the
/// call fails with the body's error, or, once the head was delivered, the response body ends
/// with `Disconnected`.
async fn subrequest(
    host: &dyn Host,
    channel: u32,
    request: Request,
    mut reply: oneshot::Sender<Result<Response>>,
) {
    let (parts, body) = request.into_parts();
    let head_request = parts.method == http::Method::HEAD;
    let started = start(host, channel, &parts, body.length())
        .and_then(|mut call| Ok((call.take_request_body()?, call)));
    let (output, mut call) = match started {
        Ok(started) => started,
        Err(error) => {
            let _ = reply.send(Err(error));
            return;
        }
    };
    let mut upload = pin!(upload(body, output).fuse());

    let head = {
        let mut response = pin!(call.response());
        let mut abandoned = pin!(reply.closed());
        poll_fn(|cx| {
            if let Poll::Ready(Err(error)) = upload.as_mut().poll(cx) {
                return Poll::Ready(Head::Failed(error));
            }
            if abandoned.as_mut().poll(cx).is_ready() {
                return Poll::Ready(Head::Abandoned);
            }
            response.as_mut().poll(cx).map(Head::Arrived)
        })
        .await
    };
    let parts = match head {
        Head::Abandoned => return,
        Head::Failed(error) | Head::Arrived(Err(error)) => {
            let _ = reply.send(Err(error));
            return;
        }
        Head::Arrived(Ok(())) => response_parts(&call),
    };
    let mut parts = match parts {
        Ok(parts) => parts,
        Err(error) => {
            let _ = reply.send(Err(error));
            return;
        }
    };
    let length = call.body_length();
    let (sender, body) = if head_request {
        // KJ reports the length the body would have had; the head says it.
        if let Some(length) = length {
            parts
                .headers
                .entry(http::header::CONTENT_LENGTH)
                .or_insert_with(|| length.into());
        }
        (None, Body::empty())
    } else if length == Some(0) {
        (None, Body::empty())
    } else {
        let (sender, body) = Body::channel(length);
        (Some(sender), body)
    };
    if reply.send(Ok(Response::from_parts(parts, body))).is_err() {
        return;
    }
    let mut download = pin!(async {
        match sender {
            Some(sender) => pump::kj_to_send(&mut call, sender).await,
            // The end of even an empty body is the end of the target's call: KJ delivers it
            // once the target's `request()` has completed, and dropping the stream before
            // that would cancel the target.
            None => drop(pump::Source::read(&mut call, &mut [0; 1]).await),
        }
    });
    poll_fn(|cx| {
        if let Poll::Ready(Err(_)) = upload.as_mut().poll(cx) {
            // Dropping the pump drops the body's sender unfinished, which is the error.
            return Poll::Ready(());
        }
        download.as_mut().poll(cx)
    })
    .await;
}

fn response_parts(call: &Subrequest) -> Result<http::response::Parts> {
    head::response_parts(call.status()?, &call.status_text()?, call.headers()?)
}
