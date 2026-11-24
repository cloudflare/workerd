// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! An HTTP/1.1 server on a loopback port for tests, hyper's, that answers
//! each request as the test says and keeps what it received.
//!
//! The bucket's HTTP client (`crate::http`) reaches it over an in-memory
//! connection instead (`connect`), with nothing in the kernel between
//! client and server: what one end writes is ready for the other before the
//! writer's task yields. Anything else that connects, such as a probe of a
//! metadata server, connects over TCP.

use std::collections::BTreeMap;
use std::convert::Infallible;
use std::pin::Pin;
use std::sync::Arc;
use std::sync::Mutex;
use std::sync::atomic::AtomicUsize;
use std::sync::atomic::Ordering;
use std::task::Context;
use std::task::Poll;
use std::time::Duration;

use bytes::Bytes;
use http_body::Body as _;
use http_body::Frame;
use hyper_util::rt::TokioIo;
use tokio::io::AsyncRead;
use tokio::io::AsyncWrite;
use tokio::io::DuplexStream;
use tokio::net::TcpListener;
use tokio::sync::mpsc;

/// A request as the server received it.
#[derive(Clone, Debug)]
pub struct Request {
    pub method: String,
    pub path: String,
    /// As sent, without the `?`.
    pub query: String,
    pub headers: http::HeaderMap,
    pub body: Bytes,
}

impl Request {
    pub fn header(&self, name: &str) -> Option<&str> {
        self.headers.get(name).and_then(|v| v.to_str().ok())
    }
}

/// What the server does with a request.
#[derive(Clone, Debug)]
pub enum Answer {
    /// Closes the connection without answering.
    Close,
    /// Never answers, and keeps the connection open until the client
    /// closes it (`Server::abandoned`).
    Stall,
    /// The answer, this long after the request arrived.
    After(Duration, Box<Self>),
    /// The answer, once the server has received another request after
    /// this one.
    WithNext(Box<Self>),
    /// The answer, once the server has answered the request after this
    /// one.
    AfterNextAnswered(Box<Self>),
    /// The head of a 200 answer with these headers, then a byte of its body
    /// every 20 ms.
    Dribble(Vec<(&'static str, String)>),
    /// As `Dribble`, but the body ends after its first byte, short of its
    /// length.
    Cut(Vec<(&'static str, String)>),
    Reply {
        status: u16,
        headers: Vec<(&'static str, String)>,
        body: String,
    },
}

/// `status` with `body` and no other headers.
pub fn reply(status: u16, body: &str) -> Answer {
    Answer::Reply {
        status,
        headers: Vec::new(),
        body: body.to_owned(),
    }
}

type Handler = dyn Fn(&Request, usize) -> Answer + Send + Sync;

/// How long a test waits for what the server should see promptly before it
/// fails: far longer than any stall of a busy machine.
pub const WATCHDOG: Duration = Duration::from_secs(10);

/// Where to send the server's end of a new in-memory connection to a
/// server, and how many more of them it hangs up on (`Server::hang_up`).
type Accept = (mpsc::UnboundedSender<DuplexStream>, Arc<AtomicUsize>);

/// Each server's host, with its `Accept`.
static IN_MEMORY: Mutex<BTreeMap<String, Accept>> = Mutex::new(BTreeMap::new());

/// How many bytes an in-memory connection holds in each direction before a
/// write waits for the other end to read.
const IN_MEMORY_BUFFER: usize = 64 << 10;

/// A new in-memory connection to the server at `host`, `ip:port`, if one
/// serves there.
pub fn connect(host: &str) -> Option<DuplexStream> {
    let (accept, hang_ups) = IN_MEMORY.lock().unwrap().get(host)?.clone();
    let (client, server) = tokio::io::duplex(IN_MEMORY_BUFFER);
    let hang_up = hang_ups.fetch_update(Ordering::SeqCst, Ordering::SeqCst, |n| n.checked_sub(1));
    if hang_up.is_ok() {
        // Closed before the client reads from it or writes to it.
        drop(server);
    } else {
        accept.send(server).ok()?;
    }
    Some(client)
}

pub struct Server {
    /// `127.0.0.1:<port>`.
    pub host: String,
    received: Arc<Mutex<Vec<Request>>>,
    connections: Arc<AtomicUsize>,
    hang_ups: Arc<AtomicUsize>,
    abandoned: Arc<Abandoned>,
    progress: Arc<Progress>,
}

/// A server's entry in `IN_MEMORY`, removed when the server stops accepting
/// connections: when its runtime ends, as its TCP listener closes, and not
/// when its `Server` is dropped.
struct Registered {
    host: String,
    accept: mpsc::UnboundedSender<DuplexStream>,
}

impl Drop for Registered {
    fn drop(&mut self) {
        let mut servers = IN_MEMORY.lock().unwrap();
        // A later server on the same port replaced this one's entry.
        if servers
            .get(&self.host)
            .is_some_and(|(s, _)| s.same_channel(&self.accept))
        {
            servers.remove(&self.host);
        }
    }
}

/// The requests a server has received and answered, numbered from 1 in the
/// order they arrived, and a signal for each change.
#[derive(Default)]
struct Progress {
    counts: Mutex<Counts>,
    changed: tokio::sync::Notify,
}

#[derive(Default)]
struct Counts {
    received: usize,
    answered: std::collections::BTreeSet<usize>,
}

impl Progress {
    fn update(&self, f: impl FnOnce(&mut Counts)) {
        f(&mut self.counts.lock().unwrap());
        self.changed.notify_waiters();
    }

    async fn until(&self, done: impl Fn(&Counts) -> bool) {
        loop {
            let changed = self.changed.notified();
            if done(&self.counts.lock().unwrap()) {
                return;
            }
            changed.await;
        }
    }
}

/// The numbers of the requests whose client stopped waiting for their
/// answer, and a signal for each one added.
#[derive(Default)]
struct Abandoned {
    numbers: Mutex<Vec<usize>>,
    added: tokio::sync::Notify,
}

/// Adds its request's number to `Abandoned` if dropped before its answer
/// went out: hyper drops an unanswered request's service once its client
/// closed the connection.
struct Unanswered {
    n: usize,
    abandoned: Arc<Abandoned>,
    answered: bool,
}

impl Drop for Unanswered {
    fn drop(&mut self) {
        if !self.answered {
            self.abandoned.numbers.lock().unwrap().push(self.n);
            self.abandoned.added.notify_waiters();
        }
    }
}

impl Server {
    /// Answers each request as `handler` says, given the request and its
    /// number, from 1.
    pub async fn serving(
        handler: impl Fn(&Request, usize) -> Answer + Send + Sync + 'static,
    ) -> Self {
        let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let host = listener.local_addr().unwrap().to_string();
        let (accept, mut accepted) = mpsc::unbounded_channel();
        let hang_ups = Arc::new(AtomicUsize::new(0));
        IN_MEMORY
            .lock()
            .unwrap()
            .insert(host.clone(), (accept.clone(), hang_ups.clone()));
        let registered = Registered {
            host: host.clone(),
            accept,
        };
        let received = Arc::new(Mutex::new(Vec::new()));
        let connections = Arc::new(AtomicUsize::new(0));
        let abandoned = Arc::new(Abandoned::default());
        let progress = Arc::new(Progress::default());
        let serve = Serve {
            handler: Arc::new(handler),
            log: received.clone(),
            connections: connections.clone(),
            abandoned: abandoned.clone(),
            progress: progress.clone(),
        };
        let tcp = serve.clone();
        tokio::spawn(async move {
            loop {
                let (stream, _) = listener.accept().await.unwrap();
                tcp.connection(stream);
            }
        });
        tokio::spawn(async move {
            let _registered = registered;
            while let Some(stream) = accepted.recv().await {
                serve.connection(stream);
            }
        });
        Self {
            host,
            received,
            connections,
            hang_ups,
            abandoned,
            progress,
        }
    }

    /// The next `n` in-memory connections to the server close as they open,
    /// before the client can send anything on them. They are not accepted,
    /// nor counted in `connections`.
    pub fn hang_up(&self, n: usize) {
        self.hang_ups.store(n, Ordering::SeqCst);
    }

    /// How many of the connections `hang_up` named are still to open.
    pub fn hang_ups(&self) -> usize {
        self.hang_ups.load(Ordering::SeqCst)
    }

    /// The connections accepted so far.
    pub fn connections(&self) -> usize {
        self.connections.load(Ordering::SeqCst)
    }

    /// Waits until the client has stopped waiting for the answer to request
    /// `n`, numbered as the handler's, and closed its connection.
    pub async fn abandoned(&self, n: usize) {
        loop {
            let added = self.abandoned.added.notified();
            if self.abandoned.numbers.lock().unwrap().contains(&n) {
                return;
            }
            added.await;
        }
    }

    /// The requests received so far.
    pub fn received(&self) -> Vec<Request> {
        self.received.lock().unwrap().clone()
    }

    /// The requests received since the last call.
    pub fn take(&self) -> Vec<Request> {
        std::mem::take(&mut self.received.lock().unwrap())
    }

    pub fn requests(&self) -> usize {
        self.received.lock().unwrap().len()
    }

    /// Runs `call` until the server has received `n` more requests than
    /// it had, and for `window` after: how many more it received by then,
    /// and what `call` returned if it ended. A test that a call sends no
    /// more than `n` requests starts its short window only once they have
    /// arrived, however busy the machine. Panics after `WATCHDOG`.
    pub async fn requests_during<F: Future>(
        &self,
        n: usize,
        window: Duration,
        call: F,
    ) -> (usize, Option<F::Output>) {
        let start = self.progress.counts.lock().unwrap().received;
        let received = || self.progress.counts.lock().unwrap().received - start;
        let mut call = std::pin::pin!(call);
        let arrived = std::pin::pin!(self.progress.until(|c| c.received >= start + n));
        let first = futures::future::select(call.as_mut(), arrived);
        let first = tokio::time::timeout(WATCHDOG, first).await;
        if let futures::future::Either::Left((output, _)) = first.expect("the requests came") {
            return (received(), Some(output));
        }
        let output = tokio::time::timeout(window, call).await.ok();
        (received(), output)
    }
}

/// What serves each connection a server accepts.
#[derive(Clone)]
struct Serve {
    handler: Arc<Handler>,
    log: Arc<Mutex<Vec<Request>>>,
    connections: Arc<AtomicUsize>,
    abandoned: Arc<Abandoned>,
    progress: Arc<Progress>,
}

impl Serve {
    fn connection(&self, stream: impl AsyncRead + AsyncWrite + Send + Unpin + 'static) {
        self.connections.fetch_add(1, Ordering::SeqCst);
        let serve = self.clone();
        let service = hyper::service::service_fn(move |request| answer(request, serve.clone()));
        let connection = hyper::server::conn::http1::Builder::new()
            .serve_connection(TokioIo::new(stream), service);
        tokio::spawn(connection);
    }
}

async fn answer(
    request: http::Request<hyper::body::Incoming>,
    serve: Serve,
) -> Result<http::Response<Body>, &'static str> {
    let Serve {
        handler,
        log,
        abandoned,
        progress,
        ..
    } = serve;
    let (head, mut incoming) = request.into_parts();
    let mut body = Vec::new();
    while let Some(frame) = std::future::poll_fn(|cx| Pin::new(&mut incoming).poll_frame(cx)).await
    {
        if let Ok(Ok(data)) = frame.map(Frame::into_data) {
            body.extend_from_slice(&data);
        }
    }
    let request = Request {
        method: head.method.to_string(),
        path: head.uri.path().to_owned(),
        query: head.uri.query().unwrap_or("").to_owned(),
        headers: head.headers,
        body: body.into(),
    };
    let n = {
        let mut log = log.lock().unwrap();
        log.push(request.clone());
        log.len()
    };
    let mut number = 0;
    progress.update(|c| {
        c.received += 1;
        number = c.received;
    });
    let mut unanswered = Unanswered {
        n,
        abandoned,
        answered: false,
    };
    let mut answer = handler(&request, n);
    let response = loop {
        break match answer {
            Answer::After(after, then) => {
                tokio::time::sleep(after).await;
                answer = *then;
                continue;
            }
            Answer::WithNext(then) => {
                progress.until(|c| c.received > number).await;
                answer = *then;
                continue;
            }
            Answer::AfterNextAnswered(then) => {
                progress.until(|c| c.answered.contains(&(number + 1))).await;
                answer = *then;
                continue;
            }
            // hyper closes the connection on a service's error.
            Answer::Close => Err("close"),
            Answer::Stall => std::future::pending().await,
            Answer::Dribble(ref headers) | Answer::Cut(ref headers) => {
                let mut response = http::Response::builder().header("content-length", "100000");
                for (name, value) in headers {
                    response = response.header(*name, value);
                }
                let interval = tokio::time::interval(Duration::from_millis(20));
                let left = if matches!(answer, Answer::Cut(_)) {
                    1
                } else {
                    usize::MAX
                };
                Ok(response.body(Body::Dribble(interval, left)).unwrap())
            }
            Answer::Reply {
                status,
                headers,
                body,
            } => {
                let mut response = http::Response::builder().status(status);
                for (name, value) in headers {
                    response = response.header(name, value);
                }
                let date = httpdate::fmt_http_date(std::time::SystemTime::now());
                let body = Body::Full(Some(Bytes::from(body)));
                Ok(response.header("date", date).body(body).unwrap())
            }
        };
    };
    unanswered.answered = true;
    progress.update(|c| {
        c.answered.insert(number);
    });
    response
}

enum Body {
    Full(Option<Bytes>),
    /// A byte a tick, this many more.
    Dribble(tokio::time::Interval, usize),
}

impl http_body::Body for Body {
    type Data = Bytes;
    type Error = Infallible;

    fn poll_frame(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
    ) -> Poll<Option<Result<Frame<Bytes>, Infallible>>> {
        match self.get_mut() {
            Self::Full(bytes) => Poll::Ready(bytes.take().map(|b| Ok(Frame::data(b)))),
            Self::Dribble(_, 0) => Poll::Ready(None),
            Self::Dribble(interval, left) => interval.poll_tick(cx).map(|_| {
                *left -= 1;
                Some(Ok(Frame::data(Bytes::from_static(b" "))))
            }),
        }
    }
}
