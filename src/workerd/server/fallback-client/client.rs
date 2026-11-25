// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! [`Client`] and [`Worker`]: the caller's side of the fallback service client, and the thread
//! that talks to the service.

use std::convert::Infallible;
use std::future::poll_fn;
use std::pin::Pin;
use std::sync::mpsc as std_mpsc;
use std::task::Context;
use std::task::Poll;
use std::time::Duration;

use bytes::Bytes;
use cxx::KjExceptionType;
use hyper::body::Body;
use hyper::body::Frame;
use hyper::body::Incoming;
use hyper::body::SizeHint;
use hyper::client::conn::http1::SendRequest;
use hyper_util::rt::TokioIo;
use kj_rs_io::Socket;
use kj_rs_io::TokioAddress;
use tokio::io::AsyncRead;
use tokio::io::AsyncWrite;
use tokio::sync::mpsc;
use tokio::time::Instant;

use crate::protocol;
use crate::protocol::DecodeError;
use crate::protocol::HttpRequest;
use crate::protocol::Request;
use crate::protocol::Resolution;

/// The default port, as the C++ client passed it to `kj::Network::parseAddress()`.
const DEFAULT_PORT: u16 = 80;

/// How long a connection may sit idle before it is closed rather than reused: the default
/// `kj::HttpClientSettings::idleTimeout` of the C++ client's `kj::newHttpClient()`.
const IDLE_TIMEOUT: Duration = Duration::from_secs(5);

/// The caller's side of a fallback service client. Its [`Worker`] runs on a thread of its own.
pub struct Client {
    jobs: mpsc::UnboundedSender<Job>,
}

/// The fallback service client's thread: it connects to the service, and answers requests until
/// the [`Client`] is dropped.
///
/// It runs the KJ event loop on kj-rs-tokio (a `kj_rs_tokio::Runtime`), which is what
/// `kj::setupAsyncIo()` gives a C++ thread in the `workerd` binary, so that it reads and connects
/// to the address as KJ does (kj-rs-io's `TokioAddress`), and its errors have KJ's exception
/// types.
pub struct Worker {
    address: String,
    jobs: mpsc::UnboundedReceiver<Job>,
}

struct Job {
    request: Request,
    reply: std_mpsc::SyncSender<Resolution>,
}

/// A client of the fallback service at `address`, and the [`Worker`] that answers it.
///
/// `address` is in KJ's address grammar (`host[:port]`, `[ipv6][:port]`, `unix:path`, a service
/// name for the port), with port 80 by default. The worker must be run, on a thread with no KJ
/// event loop of its own, for the client to answer.
#[must_use]
pub fn new(address: String) -> (Client, Worker) {
    let (jobs, received) = mpsc::unbounded_channel();
    (
        Client { jobs },
        Worker {
            address,
            jobs: received,
        },
    )
}

impl Client {
    /// Asks the service for `request`, blocking until the worker answers.
    ///
    /// This blocks the calling thread outright, without yielding to any runtime it is on, as
    /// synchronous module resolution must. [`Resolution::ShutDown`] if the worker has exited.
    #[must_use]
    pub fn resolve(&self, request: Request) -> Resolution {
        let (reply, answer) = std_mpsc::sync_channel(1);
        if self.jobs.send(Job { request, reply }).is_err() {
            return Resolution::ShutDown;
        }
        // A worker that exits with this request queued resolves it to nothing.
        answer.recv().unwrap_or(Resolution::NotFound)
    }
}

impl Worker {
    /// Answers requests until the [`Client`] is dropped. If the KJ event loop cannot be set up or
    /// the address cannot be parsed or resolved, it logs why and returns at once, and requests
    /// resolve to [`Resolution::ShutDown`] from then on.
    pub fn run(self) {
        let Self { address, jobs } = self;
        let mut runtime = match kj_rs_tokio::Runtime::new() {
            Ok(runtime) => runtime,
            Err(e) => {
                tracing::error!(
                    exception = %e.description(),
                    "Fallback service thread exiting; module resolution disabled"
                );
                return;
            }
        };
        if let Err(e) = runtime.block_on(serve(address, jobs)) {
            tracing::error!(
                exception = %e.description(),
                "Fallback service thread exiting; module resolution disabled"
            );
        }
    }
}

async fn serve(address: String, mut jobs: mpsc::UnboundedReceiver<Job>) {
    let target = match TokioAddress::parse_str(&address, DEFAULT_PORT).await {
        Ok(target) => target,
        Err(e) => {
            tracing::error!(
                exception = %e,
                "Fallback service thread exiting; module resolution disabled"
            );
            return;
        }
    };
    let mut connection = None;
    while let Some(job) = jobs.recv().await {
        let resolution = resolve(&target, &mut connection, &job.request).await;
        let _ = job.reply.send(resolution);
    }
}

/// A connection to the service, kept for the next request.
struct Connection {
    sender: SendRequest<Payload>,
    idle_since: Instant,
}

/// What a request has learned so far. Kept across the retry, as the C++ client kept it: a
/// redirect read before the body failed still counts if the retry fails too.
#[derive(Default)]
struct Outcome {
    payload: Option<Vec<u8>>,
    redirect: bool,
}

async fn resolve(
    target: &TokioAddress,
    connection: &mut Option<Connection>,
    request: &Request,
) -> Resolution {
    let http_request = protocol::encode(request);
    let mut outcome = Outcome::default();
    // Every failure is retried once. A disconnect on the first attempt (a pooled connection the
    // service closed, or a service that is restarting) is retried without logging.
    for attempt in 0..2 {
        match fetch(target, connection, &http_request, &mut outcome).await {
            Ok(()) => break,
            Err(e) if attempt == 0 && e.is_disconnected() => {}
            Err(e) => log_fetch_failure(&http_request, &e),
        }
    }
    handle_return_payload(outcome, &http_request.expected_name)
}

fn log_fetch_failure(request: &HttpRequest, e: &FetchError) {
    if request.method == http::Method::GET {
        tracing::error!(
            exception = %e,
            spec = %request.target,
            "Fallback service failed to fetch module"
        );
    } else {
        tracing::error!(exception = %e, "Fallback service failed to fetch module");
    }
}

/// The resolution an outcome means: the C++ client's `handleReturnPayload()`.
fn handle_return_payload(outcome: Outcome, expected_name: &[u8]) -> Resolution {
    let Some(payload) = outcome.payload else {
        return Resolution::NotFound;
    };
    // An empty payload means the service failed to fetch the module.
    if payload.is_empty() {
        return Resolution::NotFound;
    }
    if outcome.redirect {
        return Resolution::Redirect(payload);
    }
    let specifier = String::from_utf8_lossy(expected_name);
    match protocol::decode_module(&payload, expected_name) {
        Ok(module) => Resolution::Module(module),
        Err(DecodeError::NameMismatch { name }) => {
            tracing::error!(
                name = %String::from_utf8_lossy(&name),
                specifier = %specifier,
                "Fallback service failed to fetch module: returned module name does not match \
                 specifier"
            );
            Resolution::NotFound
        }
        Err(e) => {
            tracing::error!(
                exception = %e,
                specifier = %specifier,
                "Fallback service failed to fetch module"
            );
            Resolution::NotFound
        }
    }
}

/// Why an attempt failed.
#[derive(Debug)]
pub enum FetchError {
    Io(std::io::Error),
    Hyper(hyper::Error),
    Http(http::Error),
}

impl FetchError {
    /// Whether KJ would have thrown a `DISCONNECTED` exception, as the C++ client's retry tests.
    pub fn is_disconnected(&self) -> bool {
        match self {
            Self::Io(e) => kj_rs_io::exception_type(e) == KjExceptionType::Disconnected,
            Self::Hyper(e) => hyper_is_disconnected(e),
            Self::Http(_) => false,
        }
    }
}

/// Whether a hyper error is a connection that ended: the peer closed it, or it failed with an
/// I/O error KJ types as `DISCONNECTED`. A malformed response is not.
fn hyper_is_disconnected(e: &hyper::Error) -> bool {
    if e.is_incomplete_message() || e.is_closed() || e.is_canceled() {
        return true;
    }
    let mut source = std::error::Error::source(e);
    while let Some(cause) = source {
        if let Some(io) = cause.downcast_ref::<std::io::Error>() {
            return kj_rs_io::exception_type(io) == KjExceptionType::Disconnected;
        }
        source = cause.source();
    }
    false
}

impl std::fmt::Display for FetchError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Io(e) => write!(f, "{e}"),
            Self::Hyper(e) => write!(f, "{e}"),
            Self::Http(e) => write!(f, "{e}"),
        }
    }
}

/// One attempt: sends the request and reads the response into `outcome`, at the points where the
/// C++ client set its payload, so that a failure part way leaves what was read before it.
async fn fetch(
    target: &TokioAddress,
    connection: &mut Option<Connection>,
    request: &HttpRequest,
    outcome: &mut Outcome,
) -> Result<(), FetchError> {
    if connection
        .as_ref()
        .is_some_and(|c| c.sender.is_closed() || c.idle_since.elapsed() >= IDLE_TIMEOUT)
    {
        *connection = None;
    }
    let current = match connection {
        Some(current) => current,
        None => connection.insert(Connection {
            sender: connect(target).await?,
            idle_since: Instant::now(),
        }),
    };
    let result = exchange(&mut current.sender, request, outcome).await;
    match result {
        Ok(()) => current.idle_since = Instant::now(),
        // The connection is in an unknown state; the next attempt makes a new one.
        Err(_) => *connection = None,
    }
    result
}

async fn exchange(
    sender: &mut SendRequest<Payload>,
    request: &HttpRequest,
    outcome: &mut Outcome,
) -> Result<(), FetchError> {
    sender.ready().await.map_err(FetchError::Hyper)?;
    let response = sender
        .send_request(request.to_http().map_err(FetchError::Http)?)
        .await
        .map_err(FetchError::Hyper)?;
    let (head, body) = response.into_parts();
    match head.status.as_u16() {
        301 => {
            if let Some(location) = head.headers.get(http::header::LOCATION) {
                outcome.redirect = true;
                outcome.payload = Some(location.as_bytes().to_vec());
            } else {
                log_no_location(request);
            }
            // Read to the end, so that the connection can be reused.
            read_all(body).await.map_err(FetchError::Hyper)?;
        }
        200 => outcome.payload = Some(read_all(body).await.map_err(FetchError::Hyper)?),
        _ => {
            let body = read_all(body).await.map_err(FetchError::Hyper)?;
            log_failed_status(request, &body);
        }
    }
    Ok(())
}

fn log_no_location(request: &HttpRequest) {
    if request.method == http::Method::GET {
        tracing::error!(
            spec = %request.target,
            "Fallback service returned a redirect with no location"
        );
    } else {
        tracing::error!(
            specifier = %request.describe(),
            "Fallback service returned a redirect with no location"
        );
    }
}

fn log_failed_status(request: &HttpRequest, body: &[u8]) {
    let body = String::from_utf8_lossy(body);
    if request.method == http::Method::GET {
        tracing::error!(
            payload = %body,
            spec = %request.target,
            "Fallback service failed to fetch module"
        );
    } else {
        tracing::error!(
            body = %body,
            specifier = %request.describe(),
            "Fallback service failed to fetch module"
        );
    }
}

async fn connect(target: &TokioAddress) -> Result<SendRequest<Payload>, FetchError> {
    match target
        .connect_first()
        .await
        .map_err(|e| FetchError::Io(e.into()))?
    {
        Socket::Tcp(stream) => handshake(stream).await,
        #[cfg(unix)]
        Socket::Unix(stream) => handshake(stream).await,
    }
}

async fn handshake<S>(stream: S) -> Result<SendRequest<Payload>, FetchError>
where
    S: AsyncRead + AsyncWrite + Unpin + 'static,
{
    let (sender, connection) = hyper::client::conn::http1::handshake(TokioIo::new(stream))
        .await
        .map_err(FetchError::Hyper)?;
    // The connection ends, and this task with it, when `sender` is dropped or the service closes
    // the connection.
    kj_rs_tokio::spawn(connection);
    Ok(sender)
}

async fn read_all(mut body: Incoming) -> Result<Vec<u8>, hyper::Error> {
    let mut out = Vec::new();
    while let Some(frame) = poll_fn(|cx| Pin::new(&mut body).poll_frame(cx)).await {
        if let Ok(data) = frame?.into_data() {
            out.extend_from_slice(&data);
        }
    }
    Ok(out)
}

/// A request body: all of it at once, or none.
pub struct Payload(Option<Bytes>);

impl Payload {
    #[must_use]
    pub fn new(body: Option<Vec<u8>>) -> Self {
        Self(body.map(Bytes::from))
    }
}

impl Body for Payload {
    type Data = Bytes;
    type Error = Infallible;

    fn poll_frame(
        mut self: Pin<&mut Self>,
        _: &mut Context<'_>,
    ) -> Poll<Option<Result<Frame<Bytes>, Infallible>>> {
        Poll::Ready(self.0.take().map(|data| Ok(Frame::data(data))))
    }

    fn is_end_stream(&self) -> bool {
        self.0.is_none()
    }

    fn size_hint(&self) -> SizeHint {
        SizeHint::with_exact(self.0.as_ref().map_or(0, |data| data.len() as u64))
    }
}

#[cfg(test)]
#[path = "client-test.rs"]
mod tests;
