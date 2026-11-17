// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! An HTTP/1.1 server on a loopback port for tests, hyper's, that answers
//! each request as the test says and keeps what it received.

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
use tokio::net::TcpListener;

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
    /// The head of a 200 answer, then a byte of its body every 20 ms.
    Dribble,
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

pub struct Server {
    /// `127.0.0.1:<port>`.
    pub host: String,
    received: Arc<Mutex<Vec<Request>>>,
    connections: Arc<AtomicUsize>,
    abandoned: Arc<Abandoned>,
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
        let handler: Arc<Handler> = Arc::new(handler);
        let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let host = listener.local_addr().unwrap().to_string();
        let received = Arc::new(Mutex::new(Vec::new()));
        let connections = Arc::new(AtomicUsize::new(0));
        let abandoned = Arc::new(Abandoned::default());
        let (log, accepted, dropped) = (received.clone(), connections.clone(), abandoned.clone());
        tokio::spawn(async move {
            loop {
                let (stream, _) = listener.accept().await.unwrap();
                accepted.fetch_add(1, Ordering::SeqCst);
                let (handler, log, dropped) = (handler.clone(), log.clone(), dropped.clone());
                let service = hyper::service::service_fn(move |request| {
                    answer(request, handler.clone(), log.clone(), dropped.clone())
                });
                let connection = hyper::server::conn::http1::Builder::new()
                    .serve_connection(TokioIo::new(stream), service);
                tokio::spawn(connection);
            }
        });
        Self {
            host,
            received,
            connections,
            abandoned,
        }
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
}

async fn answer(
    request: http::Request<hyper::body::Incoming>,
    handler: Arc<Handler>,
    log: Arc<Mutex<Vec<Request>>>,
    abandoned: Arc<Abandoned>,
) -> Result<http::Response<Body>, &'static str> {
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
            // hyper closes the connection on a service's error.
            Answer::Close => Err("close"),
            Answer::Stall => std::future::pending().await,
            Answer::Dribble => Ok(http::Response::builder()
                .header("content-length", "100000")
                .body(Body::Dribble(tokio::time::interval(Duration::from_millis(
                    20,
                ))))
                .unwrap()),
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
    response
}

enum Body {
    Full(Option<Bytes>),
    Dribble(tokio::time::Interval),
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
            Self::Dribble(interval) => interval
                .poll_tick(cx)
                .map(|_| Some(Ok(Frame::data(Bytes::from_static(b" "))))),
        }
    }
}
