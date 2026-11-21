// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! HTTP/1.1 requests to the Docker Engine API, and to the egress sidecar's own API, with hyper.
//!
//! Every request goes out on a connection of its own, dialed from an address in kj-rs-io's
//! grammar (`unix:/var/run/docker.sock`, `127.0.0.1:2375`) or a `loopback:` name of the server
//! (`listen::loopback`), which the in-process test server gives Docker.
//!
//! `POST /exec/<id>/start` hijacks its connection: Docker answers `Upgrade: tcp` with
//! `101 UPGRADED` and the connection carries the process's I/O from then on ([`Api::upgrade`]).
//! kj-hyper's client upgrades only WebSockets and CONNECT, so this is plain hyper.

use std::convert::Infallible;
use std::future::poll_fn;
use std::pin::Pin;
use std::pin::pin;
use std::task::Context;
use std::task::Poll;

use bytes::Bytes;
use futures::future::Either;
use futures::future::select;
use http::HeaderValue;
use http::Method;
use http::header;
use hyper::body::Body as _;
use hyper::body::Frame;
use hyper::body::Incoming;
use hyper::body::SizeHint;
use hyper::upgrade::Upgraded;
use hyper_util::rt::TokioIo;
use kj_rs_io::Socket;
use kj_rs_io::TokioAddress;
use tokio::io::AsyncRead;
use tokio::io::AsyncWrite;

use crate::Error;
use crate::Result;
use crate::listen::loopback::Loopback;

/// The most a JSON answer may hold; Docker's are small.
pub const MAX_JSON_RESPONSE_SIZE: usize = 16 * 1024 * 1024;

/// A connection to the API.
pub trait Io: AsyncRead + AsyncWrite + Unpin + Send {}
impl<T: AsyncRead + AsyncWrite + Unpin + Send> Io for T {}

/// The connection of a hijacked exec, which carries the process's I/O.
pub type Hijacked = TokioIo<Upgraded>;

/// A request body sent whole, with its length known.
struct Full(Option<Bytes>);

impl hyper::body::Body for Full {
    type Data = Bytes;
    type Error = Infallible;

    fn poll_frame(
        mut self: Pin<&mut Self>,
        _: &mut Context<'_>,
    ) -> Poll<Option<std::result::Result<Frame<Bytes>, Infallible>>> {
        Poll::Ready(self.0.take().map(|data| Ok(Frame::data(data))))
    }

    fn is_end_stream(&self) -> bool {
        self.0.is_none()
    }

    fn size_hint(&self) -> SizeHint {
        SizeHint::with_exact(self.0.as_ref().map_or(0, |data| data.len() as u64))
    }
}

/// A request's body and its `Content-Type`.
pub struct Body {
    pub content_type: &'static str,
    pub data: Bytes,
}

impl Body {
    #[must_use]
    pub fn json(text: impl Into<String>) -> Self {
        Self {
            content_type: "application/json",
            data: Bytes::from(text.into()),
        }
    }

    #[must_use]
    pub fn tar(data: Vec<u8>) -> Self {
        Self {
            content_type: "application/x-tar",
            data: Bytes::from(data),
        }
    }
}

/// A response read whole.
#[derive(Debug)]
pub struct Response {
    pub status: u16,
    pub body: Vec<u8>,
}

impl Response {
    /// The body as text, with invalid UTF-8 replaced.
    #[must_use]
    pub fn text(&self) -> String {
        String::from_utf8_lossy(&self.body).into_owned()
    }

    /// The body decoded as JSON.
    ///
    /// # Errors
    ///
    /// A body that is not `T`.
    pub fn json<T: serde::de::DeserializeOwned>(&self) -> Result<T> {
        serde_json::from_slice(&self.body).map_err(|e| kj::failed!("invalid JSON from Docker: {e}"))
    }
}

/// The answer to a request to hijack the connection.
pub enum Upgrade {
    /// `101`: the connection, now the process's.
    Hijacked(Hijacked),
    /// Any other status, with its reason phrase and body.
    Refused {
        status: u16,
        status_text: String,
        body: String,
    },
}

fn http_error(e: impl std::fmt::Display) -> Error {
    kj::disconnected!("Docker API request failed: {e}")
}

/// The API at one address.
#[derive(Clone)]
pub struct Api {
    address: String,
    loopback: Loopback,
}

impl Api {
    #[must_use]
    pub fn new(address: impl Into<String>, loopback: &Loopback) -> Self {
        Self {
            address: address.into(),
            loopback: loopback.clone(),
        }
    }

    /// The same server's `loopback:` registry, at another address.
    #[must_use]
    pub fn at(&self, address: impl Into<String>) -> Self {
        Self::new(address, &self.loopback)
    }

    /// A new connection to the API.
    async fn dial(&self) -> Result<Box<dyn Io>> {
        if let Some(name) = self.address.strip_prefix("loopback:") {
            return Ok(Box::new(self.loopback.connect(name)?));
        }
        let address = TokioAddress::parse_str(&self.address, 0)
            .await
            .map_err(Error::from)?;
        Ok(match address.connect_first().await.map_err(Error::from)? {
            Socket::Tcp(stream) => Box::new(stream),
            #[cfg(unix)]
            Socket::Unix(stream) => Box::new(stream),
        })
    }

    fn request(method: Method, endpoint: &str, body: Option<Body>) -> Result<http::Request<Full>> {
        let mut builder = http::Request::builder()
            .method(method)
            .uri(endpoint)
            .header(header::HOST, "localhost");
        let mut data = None;
        if let Some(body) = body {
            builder = builder
                .header(header::CONTENT_TYPE, body.content_type)
                .header(header::CONTENT_LENGTH, body.data.len());
            data = Some(body.data);
        }
        builder.body(Full(data)).map_err(http_error)
    }

    /// `method endpoint`, the response read whole: at most `max_body` bytes of it.
    ///
    /// # Errors
    ///
    /// A connection that fails, or a body over `max_body`.
    pub async fn send(
        &self,
        method: Method,
        endpoint: &str,
        body: Option<Body>,
        max_body: usize,
    ) -> Result<Response> {
        let request = Self::request(method, endpoint, body)?;
        let io = self.dial().await?;
        let (mut sender, connection) = hyper::client::conn::http1::handshake(TokioIo::new(io))
            .await
            .map_err(http_error)?;
        let exchange = pin!(async move {
            let response = sender.send_request(request).await.map_err(http_error)?;
            let status = response.status().as_u16();
            let body = read_body(response.into_body(), max_body).await?;
            Ok(Response { status, body })
        });
        match select(exchange, pin!(connection)).await {
            Either::Left((result, _)) => result,
            // The connection is done; what it delivered is still to be read.
            Either::Right((Ok(()), exchange)) => exchange.await,
            Either::Right((Err(e), _)) => Err(http_error(e)),
        }
    }

    /// A request with an optional JSON body whose answer is read as JSON is.
    ///
    /// # Errors
    ///
    /// As [`Api::send`].
    pub async fn json(
        &self,
        method: Method,
        endpoint: &str,
        body: Option<String>,
    ) -> Result<Response> {
        self.send(
            method,
            endpoint,
            body.map(Body::json),
            MAX_JSON_RESPONSE_SIZE,
        )
        .await
    }

    /// `GET endpoint`.
    ///
    /// # Errors
    ///
    /// As [`Api::send`].
    pub async fn get(&self, endpoint: &str) -> Result<Response> {
        self.json(Method::GET, endpoint, None).await
    }

    /// `POST endpoint` with `body`, which hijacks the connection.
    ///
    /// # Errors
    ///
    /// A connection that fails.
    pub async fn upgrade(&self, endpoint: &str, body: String) -> Result<Upgrade> {
        let mut request = Self::request(Method::POST, endpoint, Some(Body::json(body)))?;
        // Docker's CLI asks for the upgrade explicitly: a plain POST can answer 200 and then not
        // behave like the raw stream.
        let headers = request.headers_mut();
        headers.insert(header::CONNECTION, HeaderValue::from_static("Upgrade"));
        headers.insert(header::UPGRADE, HeaderValue::from_static("tcp"));
        self.hijack(request, |status| status == 101).await
    }

    /// `CONNECT authority` with `headers`, which a 2xx answer turns into a tunnel.
    ///
    /// # Errors
    ///
    /// A connection that fails.
    pub async fn connect(&self, authority: &str, headers: &[(&str, &str)]) -> Result<Upgrade> {
        let mut builder = http::Request::builder()
            .method(Method::CONNECT)
            .uri(authority)
            .header(header::HOST, authority);
        for (name, value) in headers {
            builder = builder.header(*name, *value);
        }
        let request = builder.body(Full(None)).map_err(http_error)?;
        self.hijack(request, |status| (200..300).contains(&status))
            .await
    }

    /// Sends `request`, whose answer hands over the connection when `hijacked` says so of its
    /// status.
    async fn hijack(
        &self,
        request: http::Request<Full>,
        hijacked: impl Fn(u16) -> bool,
    ) -> Result<Upgrade> {
        let io = self.dial().await?;
        let (mut sender, connection) = hyper::client::conn::http1::handshake(TokioIo::new(io))
            .await
            .map_err(http_error)?;
        let exchange = pin!(async move {
            let response = sender.send_request(request).await.map_err(http_error)?;
            let status = response.status().as_u16();
            if hijacked(status) {
                let upgraded = hyper::upgrade::on(response).await.map_err(http_error)?;
                return Ok(Upgrade::Hijacked(TokioIo::new(upgraded)));
            }
            let status_text = reason(&response);
            let body = read_body(response.into_body(), MAX_JSON_RESPONSE_SIZE).await?;
            Ok(Upgrade::Refused {
                status,
                status_text,
                body: String::from_utf8_lossy(&body).into_owned(),
            })
        });
        match select(exchange, pin!(connection.with_upgrades())).await {
            Either::Left((result, _)) => result,
            // The connection has handed itself to the upgrade, or ended; the exchange finishes
            // without it.
            Either::Right((Ok(()), exchange)) => exchange.await,
            Either::Right((Err(e), _)) => Err(http_error(e)),
        }
    }
}

/// The status line's reason phrase as the server sent it, else the canonical one.
pub fn reason<B>(response: &http::Response<B>) -> String {
    response
        .extensions()
        .get::<hyper::ext::ReasonPhrase>()
        .map_or_else(
            || {
                response
                    .status()
                    .canonical_reason()
                    .unwrap_or_default()
                    .to_owned()
            },
            |reason| String::from_utf8_lossy(reason.as_bytes()).into_owned(),
        )
}

/// Reads `body` whole, failing past `max` bytes.
///
/// # Errors
///
/// A connection that fails, or a body over `max`.
pub async fn read_body(mut body: Incoming, max: usize) -> Result<Vec<u8>> {
    let mut bytes = Vec::new();
    while let Some(frame) = poll_fn(|cx| Pin::new(&mut body).poll_frame(cx)).await {
        let frame = frame.map_err(http_error)?;
        if let Ok(data) = frame.into_data() {
            if bytes.len() + data.len() > max {
                kj::fail_require!("Docker API response body exceeded {max} bytes");
            }
            bytes.extend_from_slice(&data);
        }
    }
    Ok(bytes)
}

#[cfg(test)]
#[path = "docker-test.rs"]
mod tests;
