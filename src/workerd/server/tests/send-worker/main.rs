// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! `workerd` with toy services, for the send worker's end-to-end test and benchmark.
//!
//! The binary is the `workerd` command line over a server that has five more services than its
//! config names (`workerd_server::native`), so that one process serves the same toy three ways
//! and a config can put JavaScript workers beside and in front of them:
//!
//! - `kj-toy`: the toy as a `worker::Interface` on the KJ loop, like `worker::ok::Worker`;
//! - `send-toy`: the toy as a `send_worker::Worker`, behind the adapter;
//! - `send-hello`: `send_worker::toys::Hello`, behind the adapter;
//! - `send-fetch` and `send-fetch-copy`: `send_worker::toys::Fetch` in front of `kj-toy`, handing
//!   bodies through or copying them in a task of the worker.
//!
//! The toy answers `Hello, world!`, except that `GET /bytes/<n>` answers `n` zero bytes in
//! writes of 64 KiB and a `POST` or `PUT` reads its body and answers the number of bytes read.
//!
//! `SEND_WORKER_THREADS` sizes the send worker runtime.

use std::pin::Pin;
use std::process::ExitCode;
use std::rc::Rc;
use std::sync::Arc;
use std::time::SystemTime;

use bytes::Bytes;
use futures::future::BoxFuture;
use kj::http::ConnectResponse;
use kj::http::ConnectSettings;
use kj::http::HeaderId;
use kj::http::Headers;
use kj::http::HeadersRef;
use kj::http::Method;
use kj::http::ServiceResponse;
use kj::io::AsyncInputStream;
use kj::io::AsyncIoStream;
use send_worker::Body;
use send_worker::Context;
use send_worker::Request;
use send_worker::Response;
use send_worker::Worker;
use send_worker::runtime;
use send_worker::toys::Fetch;
use send_worker::toys::Hello;
use send_worker::toys::Relay;
use worker::AlarmResult;
use worker::Interface;
use worker::KjOwn;
use worker::ScheduledResult;
use workerd_server::Result;
use workerd_server::channels::Channel;
use workerd_server::channels::PendingToken;
use workerd_server::channels::RequestMetadata;
use workerd_server::channels::TokenUsage;
use workerd_server::channels::WorkerInterface;
use workerd_server::config::Factory;
use workerd_server::native;
use workerd_server::services::header_table;
use workerd_server::services::not_transferable;
use workerd_server::services::send_worker::SendWorkerService;
use workerd_server::services::unsupported;

/// The size of the writes that make up a `/bytes/<n>` response.
const CHUNK: usize = 64 * 1024;

/// What a request asks the toy for.
enum Ask {
    Hello,
    /// `GET /bytes/<n>`.
    Bytes(u64),
    /// A `POST` or `PUT`: the count of the body's bytes.
    Count,
}

impl Ask {
    fn of(upload: bool, path: &str) -> Self {
        if upload {
            return Self::Count;
        }
        path.strip_prefix("/bytes/")
            .and_then(|count| count.parse().ok())
            .map_or(Self::Hello, Self::Bytes)
    }
}

// =======================================================================================
// The toy on the KJ loop

/// The toy as a service whose events stay on the KJ loop.
struct KjToy(Rc<Factory>);

impl Channel for KjToy {
    fn start_request(&self, _metadata: KjOwn<RequestMetadata>) -> Result<KjOwn<WorkerInterface>> {
        Ok(KjToyRequest(Rc::clone(&self.0)).into_kj())
    }

    fn token(&self, _usage: TokenUsage) -> Result<KjOwn<PendingToken>> {
        Err(not_transferable("The toy service"))
    }
}

struct KjToyRequest(Rc<Factory>);

#[async_trait::async_trait(?Send)]
impl kj::http::Service for KjToyRequest {
    async fn request<'a>(
        &'a mut self,
        method: Method,
        url: &'a [u8],
        _headers: HeadersRef<'a>,
        mut request_body: Pin<&'a mut AsyncInputStream>,
        response: ServiceResponse<'a>,
    ) -> Result<()> {
        let mut headers = Headers::new(header_table(&self.0));
        headers.set(HeaderId::CONTENT_TYPE, "text/plain");
        let url = http::Uri::try_from(url).map_err(|e| kj::failed!("request URL: {e}"))?;
        let upload = matches!(method, Method::POST | Method::PUT);
        match Ask::of(upload, url.path()) {
            Ask::Hello => {
                let text = Hello::BODY.as_bytes();
                let mut body = response.send(200, "OK", &headers, Some(text.len() as u64))?;
                body.write(text).await
            }
            Ask::Bytes(mut count) => {
                let chunk = vec![0; CHUNK];
                let mut body = response.send(200, "OK", &headers, Some(count))?;
                while count > 0 {
                    let size = usize::try_from(count).map_or(CHUNK, |count| count.min(CHUNK));
                    body.write(&chunk[..size]).await?;
                    count -= size as u64;
                }
                Ok(())
            }
            Ask::Count => {
                let mut buffer = vec![0; CHUNK];
                let mut count = 0;
                loop {
                    let read = request_body.as_mut().try_read(&mut buffer, 1).await?;
                    if read == 0 {
                        break;
                    }
                    count += read;
                }
                let text = count.to_string();
                let mut body = response.send(200, "OK", &headers, Some(text.len() as u64))?;
                body.write(text.as_bytes()).await
            }
        }
    }

    async fn connect<'a>(
        &'a mut self,
        _host: &'a [u8],
        _headers: HeadersRef<'a>,
        _connection: Pin<&'a mut AsyncIoStream>,
        _response: ConnectResponse<'a>,
        _settings: ConnectSettings<'a>,
    ) -> Result<()> {
        Err(unsupported("The toy service"))
    }
}

#[async_trait::async_trait(?Send)]
impl Interface for KjToyRequest {
    async fn run_scheduled(
        &mut self,
        _scheduled_time: &SystemTime,
        _cron: &str,
    ) -> Result<ScheduledResult> {
        Err(unsupported("The toy service"))
    }

    async fn run_alarm(
        &mut self,
        _scheduled_time: &SystemTime,
        _retry_count: u32,
    ) -> Result<AlarmResult> {
        Err(unsupported("The toy service"))
    }
}

// =======================================================================================
// The toy as a send worker

struct SendToy;

fn text(body: impl Into<Bytes>) -> Response {
    let mut response = Response::new(Body::full(body));
    response.headers_mut().insert(
        http::header::CONTENT_TYPE,
        http::HeaderValue::from_static("text/plain"),
    );
    response
}

impl Worker for SendToy {
    fn request(&self, _context: Context, request: Request) -> BoxFuture<'static, Result<Response>> {
        Box::pin(async move {
            let upload = matches!(*request.method(), http::Method::POST | http::Method::PUT);
            match Ask::of(upload, request.uri().path()) {
                Ask::Hello => Ok(text(Hello::BODY)),
                Ask::Bytes(mut count) => {
                    let (mut sender, body) = Body::channel(Some(count));
                    tokio::spawn(async move {
                        let chunk = Bytes::from(vec![0; CHUNK]);
                        while count > 0 {
                            let size =
                                usize::try_from(count).map_or(CHUNK, |count| count.min(CHUNK));
                            if sender.send(chunk.slice(..size)).await.is_err() {
                                return;
                            }
                            count -= size as u64;
                        }
                        sender.finish();
                    });
                    let mut response = text(Bytes::new());
                    *response.body_mut() = body;
                    Ok(response)
                }
                Ask::Count => {
                    let mut body = request.into_body();
                    let mut count = 0;
                    while let Some(chunk) = body.chunk().await {
                        count += chunk?.len();
                    }
                    Ok(text(count.to_string()))
                }
            }
        })
    }
}

// =======================================================================================

/// Registers `worker` as the service `name`, its subrequest channel 0 reaching `kj-toy`.
fn register(name: &'static str, worker: impl Worker, threads: usize) {
    native::register(name, move |factory, lookup| {
        let toy = lookup("kj-toy").ok_or_else(|| kj::failed!("the toy service is missing"))?;
        let runtime = runtime::shared(threads)?;
        let service = SendWorkerService::new(factory, Arc::new(worker), runtime, vec![toy]);
        Ok(Rc::new(service))
    });
}

fn main() -> ExitCode {
    let threads = std::env::var("SEND_WORKER_THREADS")
        .ok()
        .and_then(|threads| threads.parse().ok())
        .unwrap_or(runtime::DEFAULT_THREADS);
    native::register("kj-toy", |factory, _| {
        Ok(Rc::new(KjToy(Rc::clone(factory))))
    });
    register("send-toy", SendToy, threads);
    register("send-hello", Hello, threads);
    register("send-fetch", Fetch::new(Relay::Direct), threads);
    register("send-fetch-copy", Fetch::new(Relay::Copy), threads);
    workerd_cli::main()
}
