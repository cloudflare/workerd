// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! [`Adapter`]: a `worker::Interface` over a [`Worker`].
//!
//! The adapter lives on the loop thread and handles one event, as every `WorkerInterface` does.
//! Its `request()` spawns the worker's future on the worker runtime and is itself one future
//! made of three parts, all polled on the loop thread:
//!
//! - the upload reads the caller's request body and feeds the worker's [`Body`];
//! - the response part waits for the worker's response head, calls `Response::send`, and writes
//!   the worker's response body to the stream that returns;
//! - the calls part performs the worker's [`IoChannels`](crate::IoChannels) calls
//!   ([`crate::subrequest`]).
//!
//! `request()` completes when the response part does. The worker never sees a KJ object: what
//! it holds are channel ends.
//!
//! # Cancellation
//!
//! A KJ caller cancels a request by dropping its promise, which drops the adapter's future
//! before the drop returns. The three parts are that future's own state, so from that moment
//! nothing the caller lent is touched again (the header and URL slices, the request body
//! stream, the `Response`), and the subrequests in flight are cancelled the same way.
//!
//! The worker cannot be stopped synchronously: it runs on another thread. It is told, and
//! holds nothing of the caller's meanwhile:
//!
//! - a worker future that has not produced its response head is aborted, and is dropped at its
//!   next yield point;
//! - the request body ends with a `Disconnected` error;
//! - sending to the response body fails, and [`BodySender::closed`](crate::BodySender::closed)
//!   resolves;
//! - a pending [`IoChannels`](crate::IoChannels) call fails with `Disconnected`, and
//!   [`Context::closed`] resolves.
//!
//! A request that completes ends the same way for whatever the worker still runs. There is no
//! `waitUntil`: work a worker continues after its response body ended reaches nobody.
//!
//! # Backpressure
//!
//! Each body is a channel of [`CHANNEL_CHUNKS`](crate::body::CHANNEL_CHUNKS) chunks. The upload
//! reads the caller's stream only when the channel has room, in chunks of at most 64 KiB; the
//! response part takes a chunk from the worker only after KJ accepted the write of the previous
//! one. Calls queue up to [`CALL_QUEUE`](crate::subrequest::CALL_QUEUE). Nothing is unbounded.
//!
//! # Streaming and framing
//!
//! The three parts run concurrently, so a response head can go out before the request body has
//! ended, both bodies can flow at once, and a worker that never reads the request body does
//! not stall its own response. A response body of known length ([`Body::full`], or a channel
//! with a declared length) is sent with that length; any other is chunked. One chunk of the
//! worker is one write to KJ.
//!
//! A [`Body::full`] crosses with its head: the response is then one wake of the loop thread
//! and one KJ write.
//!
//! # Errors and panics
//!
//! - The worker fails, or panics, before it returns a head: `request()` fails without having
//!   called `send()`, with the worker's error or with a `Failed` exception that says a send
//!   worker panicked. A KJ caller sees what it sees when any `WorkerInterface` throws; the HTTP
//!   server answers 500.
//! - The response body fails after the head, or the task feeding it panics or is dropped:
//!   `request()` fails after `send()`. The HTTP server cannot take the head back, so it drops
//!   the connection, and the client sees a truncated response, never a complete one.
//! - The request body fails (the client went away mid-upload): the worker's request body ends
//!   with that error, and the request goes on until the worker answers or the caller cancels.
//!
//! A panic unwinds the worker's task only. The runtime's threads and the process go on.
//!
//! # What the adapter does not do
//!
//! - WebSockets: a worker has no way to accept an upgrade, so an upgrade request gets an
//!   ordinary response.
//! - `connect()`, scheduled events, alarms and the `test` event fail as unimplemented; custom
//!   events (JS RPC, queues, tail and trace events, hibernatable WebSocket events) are answered
//!   with the event's own `notSupported()`.
//! - Tail workers, tracing, limits and metrics: a request has no tracer, `LimitEnforcer` or
//!   `RequestObserver`.
//! - `waitUntil` and "deferred proxying": nothing of a request outlives its `request()` call.
//! - KJ's `tryPumpFrom`: a body that only passes through a worker is still read and written
//!   chunk by chunk on the loop thread, where two KJ streams could have been joined directly.

use std::future::poll_fn;
use std::pin::Pin;
use std::pin::pin;
use std::rc::Rc;
use std::sync::Arc;
use std::time::SystemTime;

use futures::FutureExt;
use kj::http::ConnectResponse;
use kj::http::ConnectSettings;
use kj::http::HeaderTable;
use kj::http::HeadersRef;
use kj::http::Method;
use kj::http::ServiceResponse;
use kj::io::AsyncInputStream;
use kj::io::AsyncIoStream;
use kj_rs::KjOwn;
use tokio::runtime::Handle;
use tokio::sync::mpsc;
use tokio::task::JoinHandle;
use worker::AlarmResult;
use worker::CustomEvent;
use worker::CustomEventResult;
use worker::ScheduledResult;
use worker::WorkerInterface;

use crate::Body;
use crate::Context;
use crate::Request;
use crate::Response;
use crate::Result;
use crate::Worker;
use crate::head;
use crate::pump;
use crate::subrequest;
use crate::subrequest::LoopChannels;

/// What an [`Adapter`] needs of the server it runs in, on the loop thread.
pub trait Host {
    /// The table every `kj::HttpHeaders` the adapter builds is built against. It is the
    /// server's one table, because callers look headers up by that table's ids.
    fn header_table(&self) -> &HeaderTable;

    /// Starts a request on subrequest channel `channel` of the worker, for one
    /// [`IoChannels::subrequest`](crate::IoChannels::subrequest) call.
    ///
    /// # Errors
    ///
    /// The worker has no such channel, or the channel refuses the request.
    fn start_subrequest(&self, channel: u32) -> Result<KjOwn<WorkerInterface>>;
}

/// A `worker::Interface` over a [`Worker`]. See the module docs.
pub struct Adapter {
    worker: Arc<dyn Worker>,
    runtime: Handle,
    host: Rc<dyn Host>,
}

impl Adapter {
    /// An adapter for one event of `worker`, whose futures run on `runtime`.
    #[must_use]
    pub fn new(worker: Arc<dyn Worker>, runtime: Handle, host: Rc<dyn Host>) -> Self {
        Self {
            worker,
            runtime,
            host,
        }
    }
}

/// The worker's future on its runtime. Dropping it aborts the future if it still runs.
struct Task(JoinHandle<Result<Response>>);

impl Task {
    async fn join(&mut self) -> Result<Response> {
        match (&mut self.0).await {
            Ok(response) => response,
            Err(error) if error.is_panic() => Err(kj::failed!("a send worker panicked")),
            Err(_) => Err(kj::disconnected!("the send worker runtime shut down")),
        }
    }
}

impl Drop for Task {
    fn drop(&mut self) {
        self.0.abort();
    }
}

fn not_implemented(what: &str) -> crate::Error {
    kj::not_implemented!("a send worker does not handle {what}")
}

#[async_trait::async_trait(?Send)]
impl kj::http::Service for Adapter {
    async fn request<'a>(
        &'a mut self,
        method: Method,
        url: &'a [u8],
        headers: HeadersRef<'a>,
        mut request_body: Pin<&'a mut AsyncInputStream>,
        response: ServiceResponse<'a>,
    ) -> Result<()> {
        let parts = head::request_parts(method, url, headers)?;
        let length = request_body.as_mut().try_get_length();
        let (upload, body) = if length == Some(0) {
            (None, Body::empty())
        } else {
            let (sender, body) = Body::channel(length);
            (Some(sender), body)
        };
        let (calls, pending_calls) = mpsc::channel(subrequest::CALL_QUEUE);
        // Dropped with this future, which is what `Context::closed` waits for.
        let (open, _open) = mpsc::channel(1);
        let context = Context::new(Arc::new(LoopChannels::new(calls)), open);
        let request = Request::from_parts(parts, body);
        let worker = Arc::clone(&self.worker);
        // The worker's `request()` itself runs on the worker runtime, not here.
        let mut task = Task(
            self.runtime
                .spawn(async move { worker.request(context, request).await }),
        );
        let host = &*self.host;

        let upload = async {
            if let Some(sender) = upload {
                pump::kj_to_send(request_body, sender).await;
            }
        };
        let calls = subrequest::serve(pending_calls, host);
        let respond = async {
            let (parts, body) = task.join().await?.into_parts();
            let headers =
                head::headers_to_kj(host.header_table(), &parts.headers, &parts.extensions)?;
            let mut output = response.send(
                u32::from(parts.status.as_u16()),
                head::reason(&parts),
                HeadersRef::from(&*headers),
                body.length(),
            )?;
            pump::send_to_kj(body, &mut output).await
        };

        let mut upload = pin!(upload.fuse());
        let mut calls = pin!(calls.fuse());
        let mut respond = pin!(respond);
        poll_fn(|cx| {
            // Every part is polled on every wake, so none waits on another: the response can
            // complete while the upload is stalled on a worker that does not read, and the
            // upload proceeds while the response waits for the worker.
            let _ = upload.as_mut().poll(cx);
            let _ = calls.as_mut().poll(cx);
            respond.as_mut().poll(cx)
        })
        .await
    }

    /// Not handled: a send worker has no `connect` handler.
    async fn connect<'a>(
        &'a mut self,
        _host: &'a [u8],
        _headers: HeadersRef<'a>,
        _connection: Pin<&'a mut AsyncIoStream>,
        _response: ConnectResponse<'a>,
        _settings: ConnectSettings<'a>,
    ) -> Result<()> {
        Err(not_implemented("connect()"))
    }
}

#[async_trait::async_trait(?Send)]
impl worker::Interface for Adapter {
    /// Nothing to warm up: a send worker has no isolate to start.
    async fn prewarm(&mut self, _url: &str) -> Result<()> {
        Ok(())
    }

    /// Not handled: a send worker has no scheduled handler.
    async fn run_scheduled(
        &mut self,
        _scheduled_time: &SystemTime,
        _cron: &str,
    ) -> Result<ScheduledResult> {
        Err(not_implemented("scheduled events"))
    }

    /// Not handled: a send worker is not an actor.
    async fn run_alarm(
        &mut self,
        _scheduled_time: &SystemTime,
        _retry_count: u32,
    ) -> Result<AlarmResult> {
        Err(not_implemented("alarms"))
    }

    /// No alarm is stored, so there is none to abandon.
    async fn abandon_alarm(&mut self, _scheduled_time: &SystemTime) -> Result<Option<SystemTime>> {
        Ok(None)
    }

    /// Not handled: a send worker has no test handler.
    async fn test(&mut self) -> Result<bool> {
        Err(not_implemented("the test event"))
    }

    /// Not handled: JS RPC, queue, tail, trace and hibernatable WebSocket events each get their
    /// own `notSupported()` answer.
    async fn custom_event(&mut self, event: KjOwn<CustomEvent>) -> Result<CustomEventResult> {
        worker::not_supported(event).await
    }
}

#[cfg(test)]
#[path = "adapter-test.rs"]
mod tests;
