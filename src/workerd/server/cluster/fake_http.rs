// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! An HTTP/1.1 server on a loopback port for tests, hyper's, that answers
//! each request as the test says and keeps what it received.

use std::convert::Infallible;
use std::pin::Pin;
use std::sync::Arc;
use std::sync::Mutex;
use std::task::Context;
use std::task::Poll;

use bytes::Bytes;
use http_body::Frame;
use hyper_util::rt::TokioIo;
use tokio::net::TcpListener;

/// A request as the server received it.
#[derive(Clone, Debug)]
pub struct Request {
    pub path: String,
    /// As sent, without the `?`.
    pub query: String,
    pub headers: http::HeaderMap,
}

impl Request {
    pub fn header(&self, name: &str) -> Option<&str> {
        self.headers.get(name).and_then(|v| v.to_str().ok())
    }
}

/// What the server does with a request.
#[derive(Clone, Debug)]
pub enum Answer {
    Reply {
        status: u16,
        headers: Vec<(&'static str, String)>,
        body: String,
    },
}

type Handler = dyn Fn(&Request, usize) -> Answer + Send + Sync;

pub struct Server {
    /// `127.0.0.1:<port>`.
    pub host: String,
    received: Arc<Mutex<Vec<Request>>>,
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
        let log = received.clone();
        tokio::spawn(async move {
            loop {
                let (stream, _) = listener.accept().await.unwrap();
                let (handler, log) = (handler.clone(), log.clone());
                let service = hyper::service::service_fn(move |request| {
                    std::future::ready(Ok::<_, Infallible>(answer(request, &*handler, &log)))
                });
                let connection = hyper::server::conn::http1::Builder::new()
                    .serve_connection(TokioIo::new(stream), service);
                tokio::spawn(connection);
            }
        });
        Self { host, received }
    }

    /// The requests received since the last call.
    pub fn take(&self) -> Vec<Request> {
        std::mem::take(&mut self.received.lock().unwrap())
    }
}

fn answer(
    request: http::Request<hyper::body::Incoming>,
    handler: &Handler,
    log: &Mutex<Vec<Request>>,
) -> http::Response<Body> {
    // The clients' requests here have no body.
    let (head, _) = request.into_parts();
    let request = Request {
        path: head.uri.path().to_owned(),
        query: head.uri.query().unwrap_or("").to_owned(),
        headers: head.headers,
    };
    let n = {
        let mut log = log.lock().unwrap();
        log.push(request.clone());
        log.len()
    };
    match handler(&request, n) {
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
            response.header("date", date).body(body).unwrap()
        }
    }
}

enum Body {
    Full(Option<Bytes>),
}

impl http_body::Body for Body {
    type Data = Bytes;
    type Error = Infallible;

    fn poll_frame(
        self: Pin<&mut Self>,
        _: &mut Context<'_>,
    ) -> Poll<Option<Result<Frame<Bytes>, Infallible>>> {
        match self.get_mut() {
            Self::Full(bytes) => Poll::Ready(bytes.take().map(|b| Ok(Frame::data(b)))),
        }
    }
}
