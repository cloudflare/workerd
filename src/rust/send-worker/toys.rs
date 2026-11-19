// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Two workers that are almost nothing, to measure the boundary with: neither runs JavaScript
//! or has an `IoContext`.

use bytes::Bytes;
use futures::future::BoxFuture;
use http::HeaderValue;
use http::header::CONTENT_TYPE;

use crate::Body;
use crate::Context;
use crate::Request;
use crate::Response;
use crate::Result;
use crate::Worker;

/// Answers every request `200 OK` with [`Hello::BODY`], without reading the request body or
/// using a channel: what is left is the cost of the boundary itself.
pub struct Hello;

impl Hello {
    pub const BODY: &str = "Hello, world!";
}

impl Worker for Hello {
    fn request(
        &self,
        _context: Context,
        _request: Request,
    ) -> BoxFuture<'static, Result<Response>> {
        Box::pin(async {
            let mut response = Response::new(Body::full(Bytes::from_static(Self::BODY.as_bytes())));
            response
                .headers_mut()
                .insert(CONTENT_TYPE, HeaderValue::from_static("text/plain"));
            Ok(response)
        })
    }
}

/// How a [`Fetch`] worker passes bodies on.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Relay {
    /// The body it was given is the body it sends: its chunks go from one KJ stream to the
    /// other on the loop thread and never reach the worker's threads.
    Direct,
    /// A task of the worker reads each chunk and sends it on, as a worker that looked at or
    /// changed the data would: every chunk crosses to the worker's threads and back.
    Copy,
}

/// Forwards every request as a subrequest on channel [`Fetch::CHANNEL`] and streams the
/// response back: `return fetch(request)`.
pub struct Fetch {
    relay: Relay,
}

impl Fetch {
    /// The subrequest channel requests are forwarded on.
    pub const CHANNEL: u32 = 0;

    #[must_use]
    pub const fn new(relay: Relay) -> Self {
        Self { relay }
    }
}

/// `body`, read and sent on chunk by chunk by a task of its own.
fn copy(mut body: Body) -> Body {
    let length = body.length();
    if length == Some(0) {
        return body;
    }
    let (mut sender, copied) = Body::channel(length);
    tokio::spawn(async move {
        while let Some(chunk) = body.chunk().await {
            match chunk {
                Ok(chunk) => {
                    if sender.send(chunk).await.is_err() {
                        return;
                    }
                }
                Err(error) => return sender.fail(error).await,
            }
        }
        sender.finish();
    });
    copied
}

impl Worker for Fetch {
    fn request(&self, context: Context, request: Request) -> BoxFuture<'static, Result<Response>> {
        let relay = self.relay;
        Box::pin(async move {
            let request = match relay {
                Relay::Direct => request,
                Relay::Copy => request.map(copy),
            };
            let response = context
                .channels()
                .subrequest(Self::CHANNEL, request)
                .await?;
            Ok(match relay {
                Relay::Direct => response,
                Relay::Copy => response.map(copy),
            })
        })
    }
}
