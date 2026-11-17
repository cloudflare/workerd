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

/// A bucket response: only the parts the bucket clients read.
pub struct HttpResponse {
    pub status: u16,
    pub etag: String,
    /// The `x-goog-generation` header: an object's version on Cloud Storage.
    pub generation: String,
    /// The `Date` header, the bucket's clock.
    pub date: String,
    /// The `Metadata-Flavor` header, which Google's metadata server sets to
    /// `Google`.
    pub metadata_flavor: String,
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
/// The most of a response body held in memory rather than written to a file.
pub const MAX_BODY: usize = 16 << 20;

/// Error text for a response body larger than [`MAX_BODY`].
pub const TOO_LARGE: &str = "response body too large";

/// Clones share the connection pool.
#[derive(Clone)]
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
        let generation = header("x-goog-generation");
        let metadata_flavor = header("metadata-flavor");
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
                // Records, listings, deployments and errors: an endpoint sending more is broken.
                if body.len() + chunk.len() > MAX_BODY {
                    return Err(TOO_LARGE.to_owned());
                }
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
            generation,
            date,
            metadata_flavor,
            body,
        })
    }
}

tokio::task_local! {
    /// What `prompt` set: the bound, and what it may send twice.
    static ATTEMPT: (Duration, &'static [Repeat]);
}

/// What a request sent twice may be, as `prompt` names it. Two copies of
/// one request may both take effect, in either order, or one may take
/// effect after the other answered: each kind but `Read` is safe only by
/// what the caller does with it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Repeat {
    /// A GET: a read or a listing, which changes nothing.
    Read,
    /// A DELETE, safe only where no key the calls delete is written again:
    /// a late copy then deletes nothing written since.
    Delete,
    /// A PUT with a precondition, of which at most one copy lands. The
    /// bucket client reports a refusal of one sent twice as of unknown
    /// outcome (`Attempt::sent_twice`, `Error::OutcomeUnknown`). Safe only
    /// where the caller's answer to that holds when the other copy lands
    /// later still, as a renewal's reread does: a read back, as
    /// `Bucket::put_if_settled` makes, may come before it lands.
    GuardedWrite,
}

/// Runs `calls` with each bucket request they make that is of a kind in
/// `repeat` sent once more when it has not answered within `attempt`, the
/// first copy kept in flight: a bucket's slow answers are single requests,
/// and the same request sent again answers in its usual time, but a slow
/// bucket answers every request late, the copy sent first soonest. The
/// caller's deadline bounds the whole. A pause between retries of a refused
/// request is not part of any request's wait.
///
/// The copy that loses is dropped, and its connection closes, never to be
/// reused: the client speaks only HTTP/1.1, and hyper closes a connection
/// whose request's caller is gone rather than return it to the pool. The
/// dropped copy may still take effect, before or after the other.
pub async fn prompt<F: Future>(
    attempt: Duration,
    repeat: &'static [Repeat],
    calls: F,
) -> F::Output {
    ATTEMPT.scope((attempt, repeat), calls).await
}

/// A request, as `attempt` judges it.
pub struct Call<'a> {
    pub method: &'a str,
    /// A write with a precondition.
    pub guarded: bool,
    pub body: &'a Payload,
    /// Where the answer's body goes instead of memory.
    pub to: Option<&'a Path>,
}

/// The largest body a request sent again may have: a larger one's own bound
/// grows with its size, which `prompt`'s does not.
const REPEATABLE_BODY: usize = 1 << 20;

impl Call<'_> {
    /// The kind of request this is if it could be sent twice: one with a
    /// small body in memory and its answer kept in memory, other than an
    /// unguarded write. Anything else goes once, whatever `prompt` names.
    pub fn repeat(&self) -> Option<Repeat> {
        let small = matches!(self.body, Payload::Bytes(bytes) if bytes.len() < REPEATABLE_BODY);
        if !small || self.to.is_some() {
            return None;
        }
        match self.method {
            "GET" => Some(Repeat::Read),
            "DELETE" => Some(Repeat::Delete),
            "PUT" if self.guarded => Some(Repeat::GuardedWrite),
            _ => None,
        }
    }
}

/// What `attempt` answered.
pub struct Attempt<T> {
    pub answer: T,
    /// Whether the request went twice. The copy `answer` is not of may have
    /// taken effect before it, or may yet, its own answer dropped: a guarded
    /// write refused may have been refused against that copy's write.
    pub sent_twice: bool,
}

/// `call`, sent with `send`, and once more if `prompt` bounds it, names its
/// kind, and it has not answered in time. The first answer that `settles`
/// is the call's. An answer that does not (an error, or a status the
/// caller would rather the other copy's answer replaced) waits up to one
/// more bound for the other copy's; when neither settles, the first answer
/// that is not an error is the call's. `what` names it in the log.
pub async fn attempt<T, E, F: Future<Output = Result<T, E>>>(
    call: &Call<'_>,
    what: impl FnOnce() -> String,
    settles: impl Fn(&T) -> bool,
    mut send: impl FnMut() -> F,
) -> Attempt<Result<T, E>> {
    let once = |answer| Attempt {
        answer,
        sent_twice: false,
    };
    let twice = |answer| Attempt {
        answer,
        sent_twice: true,
    };
    let scope = ATTEMPT.try_with(|scope| *scope).ok();
    let bound = scope.and_then(|(bound, repeat)| {
        call.repeat()
            .filter(|kind| repeat.contains(kind))
            .map(|_| bound)
    });
    let Some(bound) = bound else {
        return once(send().await);
    };
    let first = send();
    tokio::pin!(first);
    if let Ok(answer) = tokio::time::timeout(bound, &mut first).await {
        return once(answer);
    }
    tracing::warn!(
        "cluster: {} had no answer in {} ms; sending it again",
        what(),
        bound.as_millis()
    );
    let second = send();
    tokio::pin!(second);
    let (answer, other) = match futures::future::select(first, second).await {
        futures::future::Either::Left(answered) | futures::future::Either::Right(answered) => {
            answered
        }
    };
    if answer.as_ref().is_ok_and(&settles) {
        return twice(answer);
    }
    twice(match tokio::time::timeout(bound, other).await {
        Ok(later) if answer.is_err() || later.as_ref().is_ok_and(&settles) => later,
        _ => answer,
    })
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

#[cfg(test)]
#[path = "http-test.rs"]
mod tests;
