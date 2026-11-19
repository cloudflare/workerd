// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! A `Send` worker behind `workerd::WorkerInterface`. Experimental.
//!
//! `worker::Interface` is typed in KJ: KJ promises, `kj::AsyncInputStream`,
//! `kj::HttpService::Response`. Everything behind it is therefore bound to the thread of the KJ
//! event loop. This crate is the same boundary with nothing of KJ in it:
//!
//! - [`Worker`] is what a worker implements: it takes a request as owned data and a `Send`
//!   body, and returns a `Send + 'static` future of the response.
//! - [`IoChannels`] is what a worker reaches the rest of the server through, the `Send`
//!   counterpart of `IoChannelFactory`. A worker gets one with each request, in its
//!   [`Context`].
//! - [`Adapter`] is a `worker::Interface` over a [`Worker`]. It lives on the loop thread, runs
//!   the worker's futures on another runtime ([`runtime`]) and translates in both directions, so
//!   that `Adapter::into_kj()` is an ordinary C++ `WorkerInterface` whose callers see nothing of
//!   this.
//!
//! The types are the ecosystem's: `http::Request` and `http::Response` for the heads,
//! `bytes::Bytes` for the data, and a [`Body`] that is an `http_body::Body`.
//!
//! # What crosses the boundary, and what does not
//!
//! Only HTTP requests do: [`Worker::request`] inward and [`IoChannels::subrequest`] outward.
//! The adapter answers every other `WorkerInterface` method itself, and several things a
//! JavaScript worker's request has are absent (see [`adapter`]'s module docs for the list and
//! for the cancellation, backpressure and error rules).
//!
//! # Threads
//!
//! A worker's futures run on [`runtime`]'s threads and never on the loop thread. No KJ object
//! and no `Rc` crosses: a [`Body`] is either data or the receiving end of a bounded channel, and
//! an [`IoChannels`] call is a message to the request's adapter. The crate needs no
//! `unsafe impl Send` or `Sync`; the compiler checks the boundary.

pub mod adapter;
pub mod body;
mod ffi;
mod head;
mod pump;
pub mod runtime;
mod subrequest;
pub mod toys;

use std::sync::Arc;

pub use adapter::Adapter;
pub use adapter::Host;
pub use body::Body;
pub use body::BodySender;
use futures::future::BoxFuture;
use tokio::sync::mpsc;

/// A failure on either side of the boundary.
///
/// It is plain data, so it crosses threads, and it keeps KJ's exception type: a `Disconnected`
/// raised by the loop thread's I/O is still `Disconnected` when the worker sees it.
pub type Error = cxx::KjError;
pub type Result<T, E = Error> = std::result::Result<T, E>;

/// A request head and its body.
///
/// The URL is the one the `WorkerInterface` caller passed, parsed as an `http::Uri`. Header
/// names are lowercase, as `http::HeaderMap` keeps them; the spelling a peer used rides along in
/// the extensions (hyper's `HeaderCaseMap`) and is restored when the head goes back to KJ.
pub type Request = http::Request<Body>;

/// A response head and its body.
///
/// A reason phrase other than the status's canonical one is the `hyper::ext::ReasonPhrase`
/// extension.
pub type Response = http::Response<Body>;

/// A worker on the far side of the boundary.
///
/// `request` is called on a thread of the worker runtime, concurrently for concurrent requests.
/// The future it returns is spawned there. When the caller of the `WorkerInterface` cancels the
/// request, the future is dropped at its next yield point, and the bodies and the [`Context`] it
/// was given report the cancellation ([`Body`], [`Context::closed`]).
pub trait Worker: Send + Sync + 'static {
    /// Handles one request. The response head may be returned before the request body has been
    /// read, and both bodies may flow at once.
    fn request(&self, context: Context, request: Request) -> BoxFuture<'static, Result<Response>>;
}

/// What a worker reaches the rest of the server through: the `Send` counterpart of
/// `IoChannelFactory`.
///
/// Channels are numbers, as a worker's bindings are; what is behind a number is the host's
/// business ([`Host::start_subrequest`]). An implementation carries the call to the loop
/// thread, where the adapter of the request the context belongs to performs it, so a call
/// lives no longer than that request: when the request ends or is cancelled, a pending call
/// fails with `Disconnected` and a response body in flight ends with that error.
///
/// Subrequests are the only kind of channel. Timers, the cache and actor channels would be
/// further methods, each carried the same way.
pub trait IoChannels: Send + Sync + 'static {
    /// Starts an HTTP request on subrequest channel `channel` and resolves to the response head
    /// once the target sent it. The request body is read as the target asks for it, and the
    /// response body arrives as the worker reads it; both are bounded ([`body`]).
    fn subrequest(&self, channel: u32, request: Request) -> BoxFuture<'static, Result<Response>>;
}

/// What a request carries besides its head and body.
#[derive(Clone)]
pub struct Context {
    channels: Arc<dyn IoChannels>,
    /// Closed when the adapter's request ends; nothing is ever sent on it.
    request: mpsc::Sender<()>,
}

// What crosses the boundary is `Send`, each way: what the adapter gives a worker, and what a
// worker gives back or sends the loop thread.
const _: () = {
    const fn crosses<T: Send + 'static>() {}
    crosses::<Request>();
    crosses::<Response>();
    crosses::<Context>();
    crosses::<BodySender>();
    crosses::<Error>();
    crosses::<subrequest::Call>();
};

impl Context {
    pub(crate) fn new(channels: Arc<dyn IoChannels>, request: mpsc::Sender<()>) -> Self {
        Self { channels, request }
    }

    /// The request's I/O channels.
    #[must_use]
    pub fn channels(&self) -> &dyn IoChannels {
        &*self.channels
    }

    /// Resolves once the request is over on the caller's side: its response was delivered, it
    /// failed, or the caller cancelled it. Work a worker keeps doing after that reaches nobody.
    pub async fn closed(&self) {
        self.request.closed().await;
    }
}
