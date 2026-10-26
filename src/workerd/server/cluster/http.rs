// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The bucket's HTTP client: hyper's pooled HTTP/1.1 client over TCP, with
//! rustls and the platform's roots for `https` endpoints.

use std::future::Future;
use std::future::poll_fn;
use std::io;
use std::io::Read;
use std::io::Write;
use std::path::Path;
use std::path::PathBuf;
use std::pin::Pin;
use std::sync::Arc;
use std::task::Context;
use std::task::Poll;
use std::time::Duration;

use http::Uri;
use http_body::Body as _;
use http_body::Frame;
use hyper_util::client::legacy;
use hyper_util::client::legacy::connect::Connected;
use hyper_util::client::legacy::connect::Connection;
use hyper_util::rt::TokioExecutor;
use hyper_util::rt::TokioIo;
use rustls::ClientConfig;
use rustls::pki_types::ServerName;
use rustls_platform_verifier::ConfigVerifierExt;
use tokio::io::AsyncRead;
use tokio::io::AsyncWrite;
use tokio::io::ReadBuf;
use tokio::net::TcpStream;
use tokio_rustls::TlsConnector;
use tokio_rustls::client::TlsStream;

/// A bucket response: only the parts the S3 client reads.
pub struct HttpResponse {
    pub status: u16,
    pub etag: String,
    /// The `Date` header, the bucket's clock.
    pub date: String,
    /// Empty when a 2xx body went to a file.
    pub body: Vec<u8>,
}

/// What a request sends: bytes, or a file read in chunks on the blocking
/// pool.
#[derive(Clone)]
pub enum Payload {
    Bytes(bytes::Bytes),
    File(PathBuf),
}

/// The chunks a file is sent and received in.
const CHUNK: usize = 256 << 10;

pub struct Http {
    client: legacy::Client<Connector, Body>,
}

impl Http {
    pub fn new() -> Result<Self, rustls::Error> {
        let tls = Arc::new(ClientConfig::with_platform_verifier()?);
        let client = legacy::Client::builder(TokioExecutor::new()).build(Connector(tls));
        Ok(Self { client })
    }

    /// `headers` alternates names and values. A 2xx body goes to the file
    /// `to`, if any. `head` bounds the wait for the response's head, which
    /// includes sending the body; `idle`, each wait for the body's next
    /// chunk.
    pub async fn send(
        &self,
        method: String,
        url: String,
        headers: Vec<String>,
        payload: Payload,
        to: Option<&Path>,
        (head, idle): (Duration, Duration),
    ) -> Result<HttpResponse, String> {
        let mut request = http::Request::builder().method(method.as_str()).uri(url);
        for pair in headers.chunks(2) {
            request = request.header(&pair[0], &pair[1]);
        }
        let body = match payload {
            Payload::Bytes(bytes) => Body::Bytes(Some(bytes)),
            Payload::File(path) => Body::file(&path).map_err(|e| e.to_string())?,
        };
        let request = request.body(body).map_err(|e| e.to_string())?;
        let response = tokio::time::timeout(head, self.client.request(request))
            .await
            .map_err(|_| "timed out".to_owned())?
            .map_err(|e| e.to_string())?;
        let header = |name| {
            let value = response.headers().get(name);
            value.and_then(|v| v.to_str().ok()).unwrap_or("").to_owned()
        };
        let (status, etag, date) = (response.status().as_u16(), header("etag"), header("date"));
        let mut incoming = response.into_body();
        let to = to
            .filter(|_| (200..300).contains(&status))
            .map(Path::to_owned);
        let (tx, mut rx) = tokio::sync::mpsc::channel::<bytes::Bytes>(4);
        let writer = to.map(|path| {
            tokio::task::spawn_blocking(move || -> io::Result<()> {
                let mut file = io::BufWriter::new(std::fs::File::create(path)?);
                while let Some(chunk) = rx.blocking_recv() {
                    file.write_all(&chunk)?;
                }
                file.flush()
            })
        });
        let mut body = Vec::new();
        loop {
            let frame = poll_fn(|cx| Pin::new(&mut incoming).poll_frame(cx));
            let frame = tokio::time::timeout(idle, frame)
                .await
                .map_err(|_| "timed out reading the body".to_owned())?;
            let Some(frame) = frame else { break };
            let chunk = frame
                .map_err(|e| e.to_string())?
                .into_data()
                .unwrap_or_default();
            if writer.is_none() {
                body.extend(chunk);
            } else if tx.send(chunk).await.is_err() {
                break;
            }
        }
        drop(tx);
        if let Some(writer) = writer {
            writer
                .await
                .map_err(|e| e.to_string())?
                .map_err(|e| e.to_string())?;
        }
        Ok(HttpResponse {
            status,
            etag,
            date,
            body,
        })
    }
}

/// A request body: bytes in one piece, or a file's chunks as a blocking
/// task reads them. Either way its exact size is known, which S3 needs.
enum Body {
    Bytes(Option<bytes::Bytes>),
    File {
        chunks: tokio::sync::mpsc::Receiver<io::Result<bytes::Bytes>>,
        size: u64,
    },
}

impl Body {
    fn file(path: &Path) -> io::Result<Self> {
        let mut file = std::fs::File::open(path)?;
        let size = file.metadata()?.len();
        let (tx, chunks) = tokio::sync::mpsc::channel(4);
        tokio::task::spawn_blocking(move || {
            loop {
                let mut chunk = vec![0; CHUNK];
                match file.read(&mut chunk) {
                    Ok(0) => return,
                    Ok(n) => chunk.truncate(n),
                    Err(e) => return drop(tx.blocking_send(Err(e))),
                }
                if tx.blocking_send(Ok(chunk.into())).is_err() {
                    return;
                }
            }
        });
        Ok(Self::File { chunks, size })
    }
}

impl http_body::Body for Body {
    type Data = bytes::Bytes;
    type Error = io::Error;

    fn poll_frame(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
    ) -> Poll<Option<Result<Frame<Self::Data>, Self::Error>>> {
        match self.get_mut() {
            Self::Bytes(bytes) => Poll::Ready(bytes.take().map(|data| Ok(Frame::data(data)))),
            Self::File { chunks, .. } => {
                chunks.poll_recv(cx).map(|c| c.map(|c| c.map(Frame::data)))
            }
        }
    }

    fn size_hint(&self) -> http_body::SizeHint {
        match self {
            Self::Bytes(bytes) => {
                http_body::SizeHint::with_exact(bytes.as_ref().map_or(0, |b| b.len() as u64))
            }
            Self::File { size, .. } => http_body::SizeHint::with_exact(*size),
        }
    }
}

#[derive(Clone)]
struct Connector(Arc<ClientConfig>);

impl tower_service::Service<Uri> for Connector {
    type Response = TokioIo<Stream>;
    type Error = io::Error;
    type Future = Pin<Box<dyn Future<Output = io::Result<Self::Response>> + Send>>;

    fn poll_ready(&mut self, _: &mut Context<'_>) -> Poll<io::Result<()>> {
        Poll::Ready(Ok(()))
    }

    fn call(&mut self, uri: Uri) -> Self::Future {
        let tls = self.0.clone();
        Box::pin(async move {
            let invalid = || io::Error::new(io::ErrorKind::InvalidInput, uri.to_string());
            let host = uri.host().ok_or_else(invalid)?.to_owned();
            let https = uri.scheme_str() == Some("https");
            let port = uri.port_u16().unwrap_or(if https { 443 } else { 80 });
            let tcp = TcpStream::connect((host.as_str(), port)).await?;
            tcp.set_nodelay(true)?;
            if !https {
                return Ok(TokioIo::new(Stream::Tcp(tcp)));
            }
            let name = ServerName::try_from(host).map_err(|_| invalid())?;
            let tls = TlsConnector::from(tls).connect(name, tcp).await?;
            Ok(TokioIo::new(Stream::Tls(Box::new(tls))))
        })
    }
}

enum Stream {
    Tcp(TcpStream),
    Tls(Box<TlsStream<TcpStream>>),
}

impl Connection for Stream {
    fn connected(&self) -> Connected {
        Connected::new()
    }
}

impl AsyncRead for Stream {
    fn poll_read(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &mut ReadBuf<'_>,
    ) -> Poll<io::Result<()>> {
        match self.get_mut() {
            Self::Tcp(s) => Pin::new(s).poll_read(cx, buf),
            Self::Tls(s) => Pin::new(s).poll_read(cx, buf),
        }
    }
}

impl AsyncWrite for Stream {
    fn poll_write(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &[u8],
    ) -> Poll<io::Result<usize>> {
        match self.get_mut() {
            Self::Tcp(s) => Pin::new(s).poll_write(cx, buf),
            Self::Tls(s) => Pin::new(s).poll_write(cx, buf),
        }
    }

    fn poll_flush(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<io::Result<()>> {
        match self.get_mut() {
            Self::Tcp(s) => Pin::new(s).poll_flush(cx),
            Self::Tls(s) => Pin::new(s).poll_flush(cx),
        }
    }

    fn poll_shutdown(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<io::Result<()>> {
        match self.get_mut() {
            Self::Tcp(s) => Pin::new(s).poll_shutdown(cx),
            Self::Tls(s) => Pin::new(s).poll_shutdown(cx),
        }
    }
}
