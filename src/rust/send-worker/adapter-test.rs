// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The adapter under real HTTP: kj-hyper serves a connection on the KJ loop of a
//! `kj_rs_tokio::Runtime`, each request goes through `Adapter::into_kj()` (so the adapter's
//! future is polled by KJ, as in the server), and the worker runs on a multi-thread runtime of
//! the test's own. The client is the other end of an in-memory pipe and speaks HTTP by hand, so
//! that the tests see framing and timing as they are on the wire.

use std::cell::Cell;
use std::future::Future;
use std::ops::Deref;
use std::ops::DerefMut;
use std::sync::Mutex;
use std::sync::atomic::AtomicUsize;
use std::sync::atomic::Ordering;
use std::time::Duration;
use std::time::Instant;

use bytes::Bytes;
use futures::future::BoxFuture;
use kj::KjExceptionType;
use kj::http::Headers;
use kj_hyper::server::Connect;
use kj_hyper::server::Handler;
use kj_hyper::server::ServerSettings;
use kj_hyper::server::Shutdown;
use kj_hyper::server::serve_connection;
use tokio::io::AsyncReadExt;
use tokio::io::AsyncWriteExt;
use tokio::io::DuplexStream;
use tokio::io::ReadHalf;
use tokio::io::WriteHalf;
use tokio::io::duplex;
use tokio::sync::Notify;
use tokio::sync::oneshot;
use tokio::time::sleep;
use tokio::time::timeout;
use worker::CxxWorkerInterface;
use worker::Interface;

use super::*;
use crate::BodySender;
use crate::body::CHANNEL_CHUNKS;
use crate::body::Closed;
use crate::runtime::Runtime;
use crate::toys::Fetch;
use crate::toys::Hello;
use crate::toys::Relay;

/// How long a test waits for something that must happen.
const SOON: Duration = Duration::from_secs(10);

/// How long a test waits before it concludes that something does not happen.
const SETTLE: Duration = Duration::from_millis(200);

/// The size of each direction of a connection's pipe, unless a test chooses another.
const PIPE: usize = 64 * 1024;

/// The pipe of the backpressure tests: smaller than any chunk, so that it holds next to nothing.
const SMALL_PIPE: usize = 4096;

/// The size of the client's writes and of the workers' chunks in the tests of large bodies.
const WRITE_SIZE: usize = 64 * 1024;

/// What a subrequest channel reaches: a fresh `WorkerInterface` for each request.
type Target = Box<dyn Fn() -> Result<KjOwn<WorkerInterface>>>;

struct TestHost {
    table: KjOwn<HeaderTable>,
    channels: Vec<Target>,
}

impl Host for TestHost {
    fn header_table(&self) -> &HeaderTable {
        &self.table
    }

    fn start_subrequest(&self, channel: u32) -> Result<KjOwn<WorkerInterface>> {
        let target = self
            .channels
            .get(channel as usize)
            .ok_or_else(|| kj::failed!("no subrequest channel {channel}"))?;
        target()
    }
}

/// The server's side of a connection: every request is one event on a new adapter, called
/// through the C++ `WorkerInterface` it becomes.
struct Front {
    worker: Arc<dyn Worker>,
    runtime: Handle,
    host: Rc<TestHost>,
}

#[async_trait::async_trait(?Send)]
impl Handler for Front {
    async fn request<'a>(
        &'a self,
        method: Method,
        url: &'a [u8],
        headers: HeadersRef<'a>,
        body: Pin<&'a mut AsyncInputStream>,
        response: ServiceResponse<'a>,
    ) -> Result<()> {
        let host: Rc<dyn Host> = self.host.clone();
        let adapter = Adapter::new(Arc::clone(&self.worker), self.runtime.clone(), host);
        let mut worker = CxxWorkerInterface::new(adapter.into_kj());
        kj::http::Service::request(&mut worker, method, url, headers, body, response).await
    }

    async fn connect<'a>(
        &'a self,
        _host: &'a [u8],
        _headers: HeadersRef<'a>,
        _connect: Connect,
    ) -> Result<()> {
        Err(kj::failed!("the tests make no CONNECT request"))
    }
}

/// The reading half of the client's end of a connection.
struct Reader {
    io: ReadHalf<DuplexStream>,
    read: Vec<u8>,
}

impl Reader {
    /// Reads more from the connection; false at its end.
    async fn fill(&mut self) -> bool {
        let mut buffer = vec![0; 64 * 1024];
        let count = timeout(SOON, self.io.read(&mut buffer))
            .await
            .expect("the server sent nothing")
            .unwrap_or(0);
        self.read.extend_from_slice(&buffer[..count]);
        count > 0
    }

    /// Reads through the next `delimiter` and returns what came before it.
    async fn until(&mut self, delimiter: &[u8]) -> String {
        loop {
            let found = self
                .read
                .windows(delimiter.len())
                .position(|window| window == delimiter);
            if let Some(at) = found {
                let rest = self.read.split_off(at + delimiter.len());
                let mut text = std::mem::replace(&mut self.read, rest);
                text.truncate(at);
                return String::from_utf8(text).unwrap();
            }
            assert!(
                self.fill().await,
                "the connection ended; read {:?}",
                String::from_utf8_lossy(&self.read)
            );
        }
    }

    /// A response head, without its final blank line.
    async fn head(&mut self) -> String {
        self.until(b"\r\n\r\n").await
    }

    async fn exactly(&mut self, count: usize) -> Vec<u8> {
        while self.read.len() < count {
            assert!(self.fill().await, "the connection ended early");
        }
        let rest = self.read.split_off(count);
        std::mem::replace(&mut self.read, rest)
    }

    /// One chunk of a chunked body; `None` for the last chunk.
    async fn chunk(&mut self) -> Option<Vec<u8>> {
        let size = usize::from_str_radix(&self.until(b"\r\n").await, 16).unwrap();
        let data = self.exactly(size).await;
        assert_eq!(self.exactly(2).await, b"\r\n");
        (size > 0).then_some(data)
    }

    /// A response whose body has a `Content-Length`: its head and its body.
    async fn response(&mut self) -> (String, String) {
        let head = self.head().await;
        let body = self.exactly(content_length(&head)).await;
        (head, String::from_utf8(body).unwrap())
    }

    /// Everything up to the end of the connection.
    async fn rest(&mut self) -> Vec<u8> {
        while self.fill().await {}
        std::mem::take(&mut self.read)
    }
}

/// The client's end of a connection. Dropping it hangs up.
struct Client {
    writer: WriteHalf<DuplexStream>,
    reader: Reader,
    _hangup: oneshot::Sender<()>,
}

impl Client {
    async fn send(&mut self, data: impl AsRef<[u8]>) {
        self.writer.write_all(data.as_ref()).await.unwrap();
    }

    /// The two directions, to write and read at once.
    fn halves(&mut self) -> (&mut WriteHalf<DuplexStream>, &mut Reader) {
        (&mut self.writer, &mut self.reader)
    }
}

impl Deref for Client {
    type Target = Reader;

    fn deref(&self) -> &Reader {
        &self.reader
    }
}

impl DerefMut for Client {
    fn deref_mut(&mut self) -> &mut Reader {
        &mut self.reader
    }
}

/// The value of the `Content-Length` header of `head`.
fn content_length(head: &str) -> usize {
    head.split("\r\n")
        .find_map(|line| line.strip_prefix("Content-Length: "))
        .unwrap_or_else(|| panic!("no Content-Length: {head}"))
        .parse()
        .unwrap()
}

/// A server of connections to one worker, on the KJ loop, with the worker on its runtime.
struct Server {
    front: Rc<Front>,
    host: Rc<TestHost>,
    buffer: usize,
}

impl Server {
    /// Serves one connection and runs `client` against it. Completes once the client future
    /// has, which hangs up unless the server closed first, and the server is done with the
    /// connection.
    async fn connection<T, Fut>(&self, client: impl FnOnce(Client) -> Fut) -> T
    where
        Fut: Future<Output = T>,
    {
        let (ours, theirs) = duplex(self.buffer);
        let (hangup, hung_up) = oneshot::channel::<()>();
        let shutdown = Shutdown::new();
        let front: Rc<dyn Handler> = self.front.clone();
        let served = serve_connection(
            ours,
            Box::pin(async move {
                let _ = hung_up.await;
                Ok(())
            }),
            &self.host.table,
            Rc::new(ServerSettings::default()),
            front,
            &shutdown,
        );
        let (reader, writer) = tokio::io::split(theirs);
        let client = client(Client {
            writer,
            reader: Reader {
                io: reader,
                read: Vec::new(),
            },
            _hangup: hangup,
        });
        let (served, output) = futures::join!(served, client);
        served.unwrap();
        output
    }
}

/// Runs `test` against a server of `worker`, whose subrequest channels reach `channels`.
/// `buffer` is the size of each direction of a connection's pipe. Every connection is served
/// on the same KJ loop, and the worker on the same runtime.
fn run<T, Fut>(
    worker: impl Worker,
    channels: impl FnOnce() -> Vec<Target> + 'static,
    buffer: usize,
    test: impl FnOnce(Rc<Server>) -> Fut + 'static,
) -> T
where
    T: 'static,
    Fut: Future<Output = T>,
{
    let mut kj = kj_rs_tokio::Runtime::new().unwrap();
    let workers = Runtime::new(2).unwrap();
    let runtime = workers.handle();
    let worker: Arc<dyn Worker> = Arc::new(worker);
    let output = kj
        .block_on(async move {
            let host = Rc::new(TestHost {
                table: HeaderTable::builtin(),
                channels: channels(),
            });
            let front = Rc::new(Front {
                worker,
                runtime,
                host: Rc::clone(&host),
            });
            test(Rc::new(Server {
                front,
                host,
                buffer,
            }))
            .await
        })
        .unwrap();
    workers.shutdown(SOON);
    output
}

/// Serves one connection to `worker`, whose subrequest channels reach `channels`, and runs
/// `client` against it. `buffer` is the size of each direction of the connection's pipe.
fn serve_with<T, Fut>(
    worker: impl Worker,
    channels: impl FnOnce() -> Vec<Target> + 'static,
    buffer: usize,
    client: impl FnOnce(Client) -> Fut + 'static,
) -> T
where
    T: 'static,
    Fut: Future<Output = T>,
{
    run(worker, channels, buffer, |server| async move {
        server.connection(client).await
    })
}

fn serve<T, Fut>(worker: impl Worker, client: impl FnOnce(Client) -> Fut + 'static) -> T
where
    T: 'static,
    Fut: Future<Output = T>,
{
    serve_with(worker, Vec::new, PIPE, client)
}

/// A worker made of a closure.
struct FnWorker<F>(F);

impl<F, Fut> Worker for FnWorker<F>
where
    F: Fn(Context, Request) -> Fut + Send + Sync + 'static,
    Fut: Future<Output = Result<Response>> + Send + 'static,
{
    fn request(&self, context: Context, request: Request) -> BoxFuture<'static, Result<Response>> {
        Box::pin((self.0)(context, request))
    }
}

/// A value a worker takes once, such as the sending end of the channel it reports through.
struct Once<T>(Mutex<Option<T>>);

impl<T> Once<T> {
    fn new(value: T) -> Arc<Self> {
        Arc::new(Self(Mutex::new(Some(value))))
    }

    fn take(&self) -> T {
        self.0.lock().unwrap().take().expect("taken twice")
    }
}

/// Sends on its channel when dropped.
struct SendOnDrop(Option<oneshot::Sender<()>>);

impl Drop for SendOnDrop {
    fn drop(&mut self) {
        if let Some(sender) = self.0.take() {
            let _ = sender.send(());
        }
    }
}

/// Waits until `condition` holds, which it must do within [`SOON`].
async fn eventually(what: &str, condition: impl Fn() -> bool) {
    let deadline = Instant::now() + SOON;
    while !condition() {
        assert!(Instant::now() < deadline, "{what} did not happen");
        sleep(Duration::from_millis(1)).await;
    }
}

/// Awaits a report of the worker, which must come within [`SOON`].
async fn report<T>(receiver: oneshot::Receiver<T>) -> T {
    timeout(SOON, receiver)
        .await
        .expect("no report within SOON")
        .expect("the reporter went away")
}

/// Answers with the request body, chunk for chunk, starting before the request body has ended.
fn echo() -> impl Worker {
    FnWorker(|_context, request: Request| async move {
        let mut body = request.into_body();
        let (mut sender, echoed) = Body::channel(None);
        tokio::spawn(async move {
            while let Some(chunk) = body.chunk().await {
                match chunk {
                    Ok(chunk) => {
                        if sender.send(chunk).await.is_err() {
                            return;
                        }
                    }
                    Err(error) => return sender.fail(error).await,
                }
            }
            sender.finish();
        });
        Ok(Response::new(echoed))
    })
}

/// A response with a channel body, fed by `feed` on a task of its own.
fn streamed<Fut>(length: Option<u64>, feed: impl FnOnce(BodySender) -> Fut) -> Response
where
    Fut: Future<Output = ()> + Send + 'static,
{
    let (sender, body) = Body::channel(length);
    tokio::spawn(feed(sender));
    Response::new(body)
}

/// The data the tests of large bodies send: the byte at offset `i` is the byte at `i % PERIOD`
/// of this, which holds two periods so that any `PERIOD` bytes from an offset are one slice.
/// The period is prime, so the chunks of a body start at ever different points of it.
const PERIOD: usize = 65_537;

fn pattern() -> Vec<u8> {
    (0..2 * PERIOD)
        .map(|i| u8::try_from(i % PERIOD % 251).unwrap())
        .collect()
}

/// Checks that `data` is the pattern from `offset` on.
fn check_pattern(pattern: &[u8], offset: usize, data: &[u8]) {
    let mut done = 0;
    while done < data.len() {
        let at = (offset + done) % PERIOD;
        let size = (data.len() - done).min(PERIOD);
        assert!(
            data[done..done + size] == pattern[at..at + size],
            "the bytes at offset {} differ",
            offset + done
        );
        done += size;
    }
}

/// Writes `total` bytes of the pattern in writes of [`WRITE_SIZE`], counting what the pipe
/// accepted in `written` as it goes.
async fn upload(writer: &mut WriteHalf<DuplexStream>, total: usize, written: &Cell<usize>) {
    let pattern = pattern();
    while written.get() < total {
        let at = written.get() % PERIOD;
        let size = WRITE_SIZE.min(total - written.get());
        let count = writer.write(&pattern[at..at + size]).await.unwrap();
        assert!(count > 0);
        written.set(written.get() + count);
    }
}

/// POSTs `total` bytes of the pattern and checks, while still writing, that the chunked response
/// echoes them.
async fn echo_large(mut client: Client, total: usize) {
    client
        .send(format!(
            "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: {total}\r\n\r\n"
        ))
        .await;
    let written = Cell::new(0);
    let (writer, reader) = client.halves();
    let reading = async {
        let head = reader.head().await;
        assert!(head.starts_with("HTTP/1.1 200 OK\r\n"), "{head}");
        assert!(head.contains("\r\nTransfer-Encoding: chunked"), "{head}");
        let pattern = pattern();
        let mut received = 0;
        while let Some(chunk) = reader.chunk().await {
            check_pattern(&pattern, received, &chunk);
            received += chunk.len();
            assert!(received <= total, "more came back than was sent");
        }
        received
    };
    let ((), received) = futures::join!(upload(writer, total, &written), reading);
    assert_eq!(written.get(), total);
    assert_eq!(received, total);
}

/// What the `?Send` subrequest targets of the tests do.
#[derive(Clone)]
enum Behavior {
    /// Answers `200 OK` at once and writes each read of the request body to the response.
    Echo,
    /// Answers `201 Made It` with an `X-MiXeD-Case` header and, as the body, the request's
    /// method, URL and `X-Custom` header.
    Describe,
    /// Sets `started`, then waits forever; `dropped` is set when the call is dropped.
    Hang(Rc<HangFlags>),
    /// Fails without responding.
    FailBeforeHead,
    /// Responds with a chunked body, writes `first`, waits to be notified and fails.
    FailAfterHead(Rc<Notify>),
}

#[derive(Default)]
struct HangFlags {
    started: Cell<bool>,
    dropped: Cell<bool>,
}

/// Sets its flag when dropped.
struct SetOnDrop(Rc<HangFlags>);

impl Drop for SetOnDrop {
    fn drop(&mut self) {
        self.0.dropped.set(true);
    }
}

/// A subrequest target on the loop thread.
struct TestTarget {
    behavior: Behavior,
    table: KjOwn<HeaderTable>,
}

impl TestTarget {
    fn channel(behavior: Behavior) -> Target {
        Box::new(move || {
            Ok(Self {
                behavior: behavior.clone(),
                table: HeaderTable::builtin(),
            }
            .into_kj())
        })
    }
}

#[async_trait::async_trait(?Send)]
impl kj::http::Service for TestTarget {
    async fn request<'a>(
        &'a mut self,
        method: Method,
        url: &'a [u8],
        headers: HeadersRef<'a>,
        mut request_body: Pin<&'a mut AsyncInputStream>,
        response: ServiceResponse<'a>,
    ) -> Result<()> {
        match &self.behavior {
            Behavior::Echo => {
                let headers = Headers::new(&self.table);
                let mut output = response.send(200, "OK", &headers, None)?;
                let mut buffer = vec![0; 64 * 1024];
                loop {
                    let count = request_body.as_mut().try_read(&mut buffer, 1).await?;
                    if count == 0 {
                        return Ok(());
                    }
                    output.write(&buffer[..count]).await?;
                }
            }
            Behavior::Describe => {
                let custom = headers.get_by_name("x-custom").unwrap_or_default();
                let body = format!(
                    "{method:?} {} {}",
                    String::from_utf8_lossy(url),
                    String::from_utf8_lossy(custom)
                );
                let mut head = kj_hyper::Head::empty();
                head.append(b"X-MiXeD-Case", b"yes");
                let mut map = http::HeaderMap::new();
                let mut extensions = http::Extensions::new();
                head.apply(&mut map, &mut extensions);
                let headers = head::headers_to_kj(&self.table, &map, &extensions)?;
                let mut output = response.send(
                    201,
                    "Made It",
                    HeadersRef::from(&*headers),
                    Some(body.len() as u64),
                )?;
                output.write(body.as_bytes()).await
            }
            Behavior::Hang(flags) => {
                flags.started.set(true);
                let _dropped = SetOnDrop(Rc::clone(flags));
                std::future::pending().await
            }
            Behavior::FailBeforeHead => Err(kj::failed!("the target failed")),
            Behavior::FailAfterHead(go) => {
                let headers = Headers::new(&self.table);
                let mut output = response.send(200, "OK", &headers, None)?;
                output.write(b"first").await?;
                go.notified().await;
                Err(kj::failed!("the target failed after its head"))
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
        Err(kj::failed!("the tests make no CONNECT request"))
    }
}

#[async_trait::async_trait(?Send)]
impl Interface for TestTarget {
    async fn run_scheduled(
        &mut self,
        _scheduled_time: &SystemTime,
        _cron: &str,
    ) -> Result<ScheduledResult> {
        Err(kj::failed!("the tests run no scheduled event"))
    }

    async fn run_alarm(
        &mut self,
        _scheduled_time: &SystemTime,
        _retry_count: u32,
    ) -> Result<AlarmResult> {
        Err(kj::failed!("the tests run no alarm"))
    }
}

fn ok_target() -> Vec<Target> {
    vec![Box::new(|| Ok(worker::ok::Worker.into_kj()))]
}

const RELAYS: [Relay; 2] = [Relay::Direct, Relay::Copy];

// --- Framing and streaming

#[test]
fn hello_is_answered_with_a_content_length() {
    let response = serve(Hello, |mut client| async move {
        client
            .send("GET /a?b=c HTTP/1.1\r\nHost: example.com\r\n\r\n")
            .await;
        let head = client.head().await;
        let body = client.exactly(Hello::BODY.len()).await;
        (head, body)
    });
    let (head, body) = response;
    assert!(head.starts_with("HTTP/1.1 200 OK\r\n"), "{head}");
    assert!(head.contains("\r\nContent-Length: 13"), "{head}");
    assert!(head.contains("\r\nContent-Type: text/plain"), "{head}");
    assert!(!head.contains("Transfer-Encoding"), "{head}");
    assert_eq!(body, Hello::BODY.as_bytes());
}

#[test]
fn the_worker_runs_off_the_loop_thread() {
    let loop_thread = std::thread::current().id();
    let ran_on = Arc::new(Mutex::new(None));
    let worker = FnWorker({
        let ran_on = Arc::clone(&ran_on);
        move |_context, _request| {
            // Not in the future: the call itself.
            *ran_on.lock().unwrap() = Some(std::thread::current().id());
            async { Ok(Response::new(Body::empty())) }
        }
    });
    serve(worker, |mut client| async move {
        client.send("GET / HTTP/1.1\r\nHost: x\r\n\r\n").await;
        client.head().await;
    });
    let ran_on = ran_on.lock().unwrap().unwrap();
    assert_ne!(ran_on, loop_thread);
}

#[test]
fn a_channel_body_of_declared_length_has_a_content_length_and_any_other_is_chunked_chunk_for_chunk()
{
    let worker = FnWorker(|_context, request: Request| async move {
        let length = (request.uri().path() == "/declared").then_some(5);
        Ok(streamed(length, |mut sender| async move {
            sender.send(Bytes::from("ab")).await.unwrap();
            sender.send(Bytes::from("cde")).await.unwrap();
            sender.finish();
        }))
    });
    serve(worker, |mut client| async move {
        client
            .send("GET /declared HTTP/1.1\r\nHost: x\r\n\r\n")
            .await;
        let head = client.head().await;
        assert!(head.starts_with("HTTP/1.1 200 OK\r\n"), "{head}");
        assert!(head.contains("\r\nContent-Length: 5"), "{head}");
        assert!(!head.contains("Transfer-Encoding"), "{head}");
        assert_eq!(client.exactly(5).await, b"abcde");

        client
            .send("GET /chunked HTTP/1.1\r\nHost: x\r\n\r\n")
            .await;
        let head = client.head().await;
        assert!(head.contains("\r\nTransfer-Encoding: chunked"), "{head}");
        assert!(!head.contains("Content-Length"), "{head}");
        // Each chunk the worker sent is one chunk on the wire, however the two were timed.
        assert_eq!(client.chunk().await.unwrap(), b"ab");
        assert_eq!(client.chunk().await.unwrap(), b"cde");
        assert_eq!(client.chunk().await, None);
    });
}

#[test]
fn the_request_reaches_the_worker_as_plain_data() {
    let worker = FnWorker(|_context, request: Request| async move {
        let (parts, mut body) = request.into_parts();
        let length = body.length();
        let mut read = 0;
        while let Some(chunk) = body.chunk().await {
            read += chunk?.len();
        }
        let custom = parts
            .headers
            .get("x-custom")
            .map(|value| value.to_str().unwrap().to_owned());
        Ok(Response::new(Body::full(format!(
            "{} {} {custom:?} {length:?} {read}",
            parts.method, parts.uri
        ))))
    });
    serve(worker, |mut client| async move {
        client
            .send(
                "POST /a?b=c HTTP/1.1\r\nHost: x\r\nX-Custom: hello\r\nContent-Length: 5\r\n\r\n\
                 12345",
            )
            .await;
        let (_, body) = client.response().await;
        assert_eq!(body, r#"POST /a?b=c Some("hello") Some(5) 5"#);

        client
            .send(
                "POST /chunked HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n\
                 3\r\nabc\r\n0\r\n\r\n",
            )
            .await;
        let (_, body) = client.response().await;
        assert_eq!(body, "POST /chunked None None 3");

        client.send("GET /get HTTP/1.1\r\nHost: x\r\n\r\n").await;
        let (_, body) = client.response().await;
        assert_eq!(body, "GET /get None Some(0) 0");
    });
}

#[test]
fn a_response_head_precedes_the_end_of_the_request_body_and_both_bodies_flow_at_once() {
    serve(echo(), |mut client| async move {
        client
            .send("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n")
            .await;
        // Nothing of the body was sent, and the head is here.
        let head = client.head().await;
        assert!(head.starts_with("HTTP/1.1 200 OK\r\n"), "{head}");
        assert!(head.contains("\r\nTransfer-Encoding: chunked"), "{head}");
        for round in 0..5 {
            let data = format!("round {round}");
            client.send(format!("{:x}\r\n{data}\r\n", data.len())).await;
            assert_eq!(client.chunk().await.unwrap(), data.as_bytes());
        }
        client.send("0\r\n\r\n").await;
        assert_eq!(client.chunk().await, None);
    });
}

#[test]
fn the_chunks_of_a_response_body_arrive_in_order() {
    const CHUNKS: usize = 2000;
    let worker = FnWorker(|_context, _request| async {
        Ok(streamed(None, |mut sender| async move {
            for i in 0..CHUNKS {
                sender.send(Bytes::from(format!("{i},"))).await.unwrap();
            }
            sender.finish();
        }))
    });
    serve(worker, |mut client| async move {
        client.send("GET / HTTP/1.1\r\nHost: x\r\n\r\n").await;
        client.head().await;
        for i in 0..CHUNKS {
            assert_eq!(client.chunk().await.unwrap(), format!("{i},").as_bytes());
        }
        assert_eq!(client.chunk().await, None);
    });
}

#[test]
fn a_large_body_is_echoed_while_it_is_still_being_sent() {
    serve(echo(), |client| echo_large(client, 256 << 20));
}

// --- Backpressure

#[test]
fn a_client_that_does_not_read_stops_the_worker_after_a_few_chunks() {
    const CHUNKS: usize = 1000;
    let sent = Arc::new(AtomicUsize::new(0));
    let worker = FnWorker({
        let sent = Arc::clone(&sent);
        move |_context, _request| {
            let sent = Arc::clone(&sent);
            async move {
                Ok(streamed(None, |mut sender| async move {
                    let chunk = Bytes::from(vec![b'x'; WRITE_SIZE]);
                    for _ in 0..CHUNKS {
                        if sender.send(chunk.clone()).await.is_err() {
                            return;
                        }
                        sent.fetch_add(1, Ordering::SeqCst);
                    }
                    sender.finish();
                }))
            }
        }
    });
    serve_with(worker, Vec::new, SMALL_PIPE, move |mut client| async move {
        client.send("GET / HTTP/1.1\r\nHost: x\r\n\r\n").await;
        client.head().await;
        // The body's channel alone takes this many, whatever the client does.
        eventually("the first sends", || {
            sent.load(Ordering::SeqCst) >= CHANNEL_CHUNKS
        })
        .await;
        // Beyond the channel's CHANNEL_CHUNKS, a chunk is held by the adapter's write, one by
        // kj-hyper's body, at most 16 buffers or about 400 KiB (7 chunks) by hyper's write queue,
        // and less than one by the pipe and the client's one read: at most about 15 in all.
        // However long the client stalls, the count stays below that.
        for _ in 0..2 {
            sleep(SETTLE).await;
            let count = sent.load(Ordering::SeqCst);
            assert!(
                count < 40,
                "{count} chunks were sent to a client that reads nothing"
            );
        }

        let mut received = 0;
        while let Some(chunk) = client.chunk().await {
            received += chunk.len();
        }
        assert_eq!(received, CHUNKS * WRITE_SIZE);
        assert_eq!(sent.load(Ordering::SeqCst), CHUNKS);
    });
}

#[test]
fn a_worker_that_does_not_read_stops_the_upload_after_a_few_chunks() {
    const TOTAL: usize = 64 << 20;
    let (go_tx, go_rx) = oneshot::channel::<()>();
    let go = Once::new(go_rx);
    let worker = FnWorker(move |_context, request: Request| {
        let go = go.take();
        async move {
            go.await.unwrap();
            let mut body = request.into_body();
            let mut read = 0;
            while let Some(chunk) = body.chunk().await {
                read += chunk?.len();
            }
            Ok(Response::new(Body::full(read.to_string())))
        }
    });
    serve_with(worker, Vec::new, SMALL_PIPE, move |mut client| async move {
        client
            .send(format!(
                "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: {TOTAL}\r\n\r\n"
            ))
            .await;
        let written = Cell::new(0);
        let (writer, reader) = client.halves();
        let reading = async {
            eventually("the first write", || written.get() > 0).await;
            // What the client can write before the worker reads: the pipe (4 KiB), hyper's read
            // buffer (at most about 400 KiB), the frame kj-hyper's body stream holds (as much),
            // the upload's 64 KiB read and its channel of CHANNEL_CHUNKS chunks of 64 KiB plus
            // the one it is sending: at most about 1.3 MiB, however long the worker waits.
            for _ in 0..2 {
                sleep(SETTLE).await;
                let count = written.get();
                assert!(
                    count < 8 << 20,
                    "{count} bytes were written to a worker that reads nothing"
                );
            }
            go_tx.send(()).unwrap();
            reader.response().await
        };
        let ((), (head, body)) = futures::join!(upload(writer, TOTAL, &written), reading);
        assert!(head.starts_with("HTTP/1.1 200 OK\r\n"), "{head}");
        assert_eq!(body, TOTAL.to_string());
    });
}

// --- Deadlock freedom

#[test]
fn a_worker_that_never_reads_the_request_body_still_answers() {
    serve(Hello, |mut client| async move {
        client
            .send(format!(
                "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: {}\r\n\r\n",
                64 << 20
            ))
            .await;
        client.send(vec![b'x'; WRITE_SIZE]).await;
        let head = client.head().await;
        assert!(head.starts_with("HTTP/1.1 200 OK\r\n"), "{head}");
        assert_eq!(
            client.exactly(Hello::BODY.len()).await,
            Hello::BODY.as_bytes()
        );
    });
}

#[test]
fn a_worker_that_reads_the_whole_request_body_before_it_answers_gets_it_all() {
    const TOTAL: usize = 8 << 20;
    let worker = FnWorker(|_context, request: Request| async move {
        let mut body = request.into_body();
        let mut read = 0;
        while let Some(chunk) = body.chunk().await {
            read += chunk?.len();
        }
        Ok(Response::new(Body::full(read.to_string())))
    });
    serve(worker, |mut client| async move {
        client
            .send(format!(
                "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: {TOTAL}\r\n\r\n"
            ))
            .await;
        let written = Cell::new(0);
        upload(&mut client.writer, TOTAL, &written).await;
        let (head, body) = client.response().await;
        assert!(head.starts_with("HTTP/1.1 200 OK\r\n"), "{head}");
        assert_eq!(body, TOTAL.to_string());
    });
}

// --- Cancellation

#[test]
fn a_hang_up_mid_request_body_ends_the_body_with_disconnected_and_closes_the_context() {
    let (read_tx, read_rx) = oneshot::channel();
    let (report_tx, report_rx) = oneshot::channel();
    let reporters = Once::new((read_tx, report_tx));
    let worker = FnWorker(move |context: Context, request: Request| {
        let (read_tx, report_tx) = reporters.take();
        tokio::spawn(async move {
            let mut body = request.into_body();
            let mut read = 0;
            while read < 10 {
                read += body.chunk().await.unwrap().unwrap().len();
            }
            read_tx.send(read).unwrap();
            let next = body
                .chunk()
                .await
                .map(|chunk| chunk.map(|data| data.len()).map_err(|e| e.exception_type()));
            let closed = timeout(SOON, context.closed()).await.is_ok();
            report_tx.send((next, closed)).unwrap();
        });
        std::future::pending()
    });
    serve(worker, |mut client| async move {
        client
            .send("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1000000\r\n\r\n0123456789")
            .await;
        assert_eq!(report(read_rx).await, 10);
        drop(client);
        let (next, closed) = report(report_rx).await;
        assert_eq!(next, Some(Err(KjExceptionType::Disconnected)));
        assert!(closed, "Context::closed() did not resolve");
    });
}

#[test]
fn a_hang_up_mid_response_body_fails_the_senders_send() {
    let (report_tx, report_rx) = oneshot::channel();
    let reporter = Once::new(report_tx);
    let worker = FnWorker(move |_context, _request| {
        let report_tx = reporter.take();
        async move {
            Ok(streamed(None, |mut sender| async move {
                let failed = loop {
                    if let Err(closed) = sender.send(Bytes::from("tick")).await {
                        break closed;
                    }
                    sleep(Duration::from_millis(5)).await;
                };
                report_tx.send(failed).unwrap();
            }))
        }
    });
    serve(worker, |mut client| async move {
        client.send("GET / HTTP/1.1\r\nHost: x\r\n\r\n").await;
        client.head().await;
        assert_eq!(client.chunk().await.unwrap(), b"tick");
        drop(client);
        assert_eq!(report(report_rx).await, Closed);
    });
}

#[test]
fn a_hang_up_before_the_worker_answered_drops_the_workers_future() {
    let (started_tx, started_rx) = oneshot::channel();
    let (dropped_tx, dropped_rx) = oneshot::channel();
    let signals = Once::new((started_tx, dropped_tx));
    let worker = FnWorker(move |_context, _request| {
        let (started_tx, dropped_tx) = signals.take();
        async move {
            let _dropped = SendOnDrop(Some(dropped_tx));
            started_tx.send(()).unwrap();
            std::future::pending().await
        }
    });
    serve(worker, |mut client| async move {
        client.send("GET / HTTP/1.1\r\nHost: x\r\n\r\n").await;
        report(started_rx).await;
        drop(client);
        report(dropped_rx).await;
    });
}

#[test]
fn a_hang_up_cancels_a_subrequest_in_flight() {
    // Both the target and the client run on the loop thread.
    let flags = Rc::new(HangFlags::default());
    let behavior = Behavior::Hang(Rc::clone(&flags));
    serve_with(
        Fetch::new(Relay::Direct),
        move || vec![TestTarget::channel(behavior)],
        PIPE,
        move |mut client| async move {
            client.send("GET / HTTP/1.1\r\nHost: x\r\n\r\n").await;
            eventually("the subrequest", || flags.started.get()).await;
            assert!(!flags.dropped.get());
            drop(client);
            eventually("the subrequest's cancellation", || flags.dropped.get()).await;
        },
    );
}

// --- Errors and panics

#[test]
fn a_worker_that_fails_before_a_head_gets_a_500_that_does_not_say_why() {
    let worker = FnWorker(|_context, _request| async {
        Err(kj::failed!("a secret the client must not see"))
    });
    serve(worker, |mut client| async move {
        client.send("GET / HTTP/1.1\r\nHost: x\r\n\r\n").await;
        let head = client.head().await;
        assert!(
            head.starts_with("HTTP/1.1 500 Internal Server Error\r\n"),
            "{head}"
        );
        assert!(head.contains("\r\nConnection: close"), "{head}");
        let rest = String::from_utf8(client.rest().await).unwrap();
        assert!(
            !head.contains("secret") && !rest.contains("secret"),
            "{rest}"
        );
    });
}

/// Answers by path, each body ending once the client has read its first chunk and notified
/// `go`: `/fail` and `/fail-declared` fail, `/drop` drops its sender unfinished, `/panic` panics
/// in the task that feeds it; `/ok` is answered in full.
fn truncating(go: Arc<Notify>) -> impl Worker {
    FnWorker(move |_context, request: Request| {
        let go = Arc::clone(&go);
        async move {
            let path = request.uri().path().to_owned();
            if path == "/ok" {
                return Ok(Response::new(Body::full("ok")));
            }
            let length = (path == "/fail-declared").then_some(10);
            Ok(streamed(length, |mut sender| async move {
                sender.send(Bytes::from("first")).await.unwrap();
                go.notified().await;
                match path.as_str() {
                    "/fail" | "/fail-declared" => sender.fail(kj::failed!("boom")).await,
                    "/drop" => drop(sender),
                    "/panic" => panic!("the task feeding a response body panicked"),
                    _ => unreachable!(),
                }
            }))
        }
    })
}

/// Reads the head of a chunked response and its first chunk, lets the body end and checks that
/// the connection ends without the body's last chunk.
async fn truncated_chunked(mut client: Client, path: &str, go: &Notify) {
    client
        .send(format!("GET {path} HTTP/1.1\r\nHost: x\r\n\r\n"))
        .await;
    let head = client.head().await;
    assert!(head.starts_with("HTTP/1.1 200 OK\r\n"), "{head}");
    assert!(head.contains("\r\nTransfer-Encoding: chunked"), "{head}");
    assert_eq!(client.chunk().await.unwrap(), b"first");
    go.notify_one();
    let rest = client.rest().await;
    assert!(
        !rest.windows(5).any(|window| window == b"0\r\n\r\n"),
        "{path}: a truncated body ended as a complete one: {:?}",
        String::from_utf8_lossy(&rest)
    );
}

#[test]
fn a_response_body_that_fails_after_the_head_is_truncated() {
    let go = Arc::new(Notify::new());
    run(
        truncating(Arc::clone(&go)),
        Vec::new,
        PIPE,
        |server| async move {
            server
                .connection(|client| truncated_chunked(client, "/fail", &go))
                .await;
            server
                .connection(|mut client| async move {
                    client
                        .send("GET /fail-declared HTTP/1.1\r\nHost: x\r\n\r\n")
                        .await;
                    let head = client.head().await;
                    assert!(head.contains("\r\nContent-Length: 10"), "{head}");
                    assert_eq!(client.exactly(5).await, b"first");
                    go.notify_one();
                    let rest = client.rest().await;
                    assert!(rest.len() < 5, "{rest:?}");
                })
                .await;
        },
    );
}

#[test]
fn a_worker_that_panics_before_a_head_gets_a_500_and_the_server_goes_on() {
    let worker = FnWorker(|_context, request: Request| async move {
        assert_ne!(request.uri().path(), "/panic", "the worker panicked");
        Ok(Response::new(Body::full("fine")))
    });
    run(worker, Vec::new, PIPE, |server| async move {
        server
            .connection(|mut client| async move {
                client.send("GET /panic HTTP/1.1\r\nHost: x\r\n\r\n").await;
                let head = client.head().await;
                assert!(
                    head.starts_with("HTTP/1.1 500 Internal Server Error\r\n"),
                    "{head}"
                );
            })
            .await;
        server
            .connection(|mut client| async move {
                client.send("GET / HTTP/1.1\r\nHost: x\r\n\r\n").await;
                let (head, body) = client.response().await;
                assert!(head.starts_with("HTTP/1.1 200 OK\r\n"), "{head}");
                assert_eq!(body, "fine");
            })
            .await;
    });
}

#[test]
fn a_panic_in_the_task_feeding_a_response_body_truncates_it_and_the_server_goes_on() {
    let go = Arc::new(Notify::new());
    run(
        truncating(Arc::clone(&go)),
        Vec::new,
        PIPE,
        |server| async move {
            server
                .connection(|client| truncated_chunked(client, "/panic", &go))
                .await;
            server
                .connection(|mut client| async move {
                    client.send("GET /ok HTTP/1.1\r\nHost: x\r\n\r\n").await;
                    let (head, body) = client.response().await;
                    assert!(head.starts_with("HTTP/1.1 200 OK\r\n"), "{head}");
                    assert_eq!(body, "ok");
                })
                .await;
        },
    );
}

#[test]
fn a_response_body_whose_sender_is_dropped_unfinished_is_truncated() {
    let go = Arc::new(Notify::new());
    serve(truncating(Arc::clone(&go)), |client| async move {
        truncated_chunked(client, "/drop", &go).await;
    });
}

#[test]
fn a_response_body_longer_than_its_declared_length_ends_the_connection() {
    let worker = FnWorker(|_context, _request| async {
        Ok(streamed(Some(3), |mut sender| async move {
            let _ = sender.send(Bytes::from("abcd")).await;
            sender.finish();
        }))
    });
    serve(worker, |mut client| async move {
        // Two requests at once: a second response would mean the first was framed as complete.
        client
            .send("GET /1 HTTP/1.1\r\nHost: x\r\n\r\nGET /2 HTTP/1.1\r\nHost: x\r\n\r\n")
            .await;
        let rest = String::from_utf8(client.rest().await).unwrap();
        assert!(rest.matches("HTTP/1.1 ").count() <= 1, "{rest}");
        if let Some((head, body)) = rest.split_once("\r\n\r\n") {
            assert!(head.starts_with("HTTP/1.1 200 OK\r\n"), "{rest}");
            assert!(body.len() < 3, "{rest}");
        }
    });
}

// --- Fetch

#[test]
fn fetch_forwards_to_its_channel_and_streams_the_response_back() {
    for relay in RELAYS {
        let (head, body) = serve_with(
            Fetch::new(relay),
            ok_target,
            PIPE,
            |mut client| async move {
                client.send("GET / HTTP/1.1\r\nHost: x\r\n\r\n").await;
                (client.head().await, client.exactly(2).await)
            },
        );
        assert!(head.starts_with("HTTP/1.1 200 OK\r\n"), "{relay:?}: {head}");
        assert!(head.contains("\r\nContent-Length: 2"), "{relay:?}: {head}");
        assert_eq!(body, b"OK", "{relay:?}");
    }
}

#[test]
fn fetch_streams_the_request_body_to_the_target_and_its_response_body_back() {
    for relay in RELAYS {
        serve_with(
            Fetch::new(relay),
            || vec![TestTarget::channel(Behavior::Echo)],
            PIPE,
            move |mut client| async move {
                client
                    .send("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n")
                    .await;
                let head = client.head().await;
                assert!(head.starts_with("HTTP/1.1 200 OK\r\n"), "{relay:?}: {head}");
                assert!(
                    head.contains("\r\nTransfer-Encoding: chunked"),
                    "{relay:?}: {head}"
                );
                for round in 0..5 {
                    let data = format!("round {round}");
                    client.send(format!("{:x}\r\n{data}\r\n", data.len())).await;
                    assert_eq!(client.chunk().await.unwrap(), data.as_bytes(), "{relay:?}");
                }
                client.send("0\r\n\r\n").await;
                assert_eq!(client.chunk().await, None, "{relay:?}");
            },
        );
    }
}

#[test]
fn fetch_echoes_a_large_body() {
    for relay in RELAYS {
        serve_with(
            Fetch::new(relay),
            || vec![TestTarget::channel(Behavior::Echo)],
            PIPE,
            |client| echo_large(client, 32 << 20),
        );
    }
}

#[test]
fn fetch_carries_the_heads_unchanged_both_ways() {
    for relay in RELAYS {
        serve_with(
            Fetch::new(relay),
            || vec![TestTarget::channel(Behavior::Describe)],
            PIPE,
            move |mut client| async move {
                client
                    .send("GET /a?b=c HTTP/1.1\r\nHost: x\r\nX-Custom: hello\r\n\r\n")
                    .await;
                let (head, body) = client.response().await;
                assert!(
                    head.starts_with("HTTP/1.1 201 Made It\r\n"),
                    "{relay:?}: {head}"
                );
                assert!(head.contains("\r\nX-MiXeD-Case: yes"), "{relay:?}: {head}");
                assert_eq!(body, "GET /a?b=c hello", "{relay:?}");
            },
        );
    }
}

#[test]
fn a_target_that_fails_before_responding_gets_a_500() {
    for relay in RELAYS {
        let head = serve_with(
            Fetch::new(relay),
            || vec![TestTarget::channel(Behavior::FailBeforeHead)],
            PIPE,
            |mut client| async move {
                client.send("GET / HTTP/1.1\r\nHost: x\r\n\r\n").await;
                client.head().await
            },
        );
        assert!(
            head.starts_with("HTTP/1.1 500 Internal Server Error\r\n"),
            "{relay:?}: {head}"
        );
    }
}

#[test]
fn a_target_that_fails_after_its_head_truncates_the_response() {
    for relay in RELAYS {
        let go = Rc::new(Notify::new());
        let behavior = Behavior::FailAfterHead(Rc::clone(&go));
        serve_with(
            Fetch::new(relay),
            move || vec![TestTarget::channel(behavior)],
            PIPE,
            move |client| async move { truncated_chunked(client, "/", &go).await },
        );
    }
}

#[test]
fn a_subrequest_on_a_channel_that_does_not_exist_gets_a_500() {
    let head = serve(Fetch::new(Relay::Direct), |mut client| async move {
        client.send("GET / HTTP/1.1\r\nHost: x\r\n\r\n").await;
        client.head().await
    });
    assert!(
        head.starts_with("HTTP/1.1 500 Internal Server Error\r\n"),
        "{head}"
    );
}

#[test]
fn a_head_request_through_fetch_has_no_body_and_keeps_the_connection() {
    for relay in RELAYS {
        serve_with(
            Fetch::new(relay),
            ok_target,
            PIPE,
            move |mut client| async move {
                client.send("HEAD / HTTP/1.1\r\nHost: x\r\n\r\n").await;
                let head = client.head().await;
                assert!(head.starts_with("HTTP/1.1 200 OK\r\n"), "{relay:?}: {head}");
                assert!(head.contains("\r\nContent-Length: 2"), "{relay:?}: {head}");
                // Had the HEAD response a body, it would precede this response's head.
                client.send("GET / HTTP/1.1\r\nHost: x\r\n\r\n").await;
                let (head, body) = client.response().await;
                assert!(head.starts_with("HTTP/1.1 200 OK\r\n"), "{relay:?}: {head}");
                assert_eq!(body, "OK", "{relay:?}");
            },
        );
    }
}

// --- Other events

#[test]
fn the_adapter_prewarms_and_refuses_the_other_events() {
    let mut kj = kj_rs_tokio::Runtime::new().unwrap();
    let workers = Runtime::new(1).unwrap();
    let runtime = workers.handle();
    kj.block_on(async move {
        let host: Rc<dyn Host> = Rc::new(TestHost {
            table: HeaderTable::builtin(),
            channels: Vec::new(),
        });
        let mut adapter = Adapter::new(Arc::new(Hello), runtime, host);
        let now = SystemTime::now();
        adapter.prewarm("/").await.unwrap();
        let unimplemented = KjExceptionType::Unimplemented;
        let error = adapter.run_scheduled(&now, "* * * * *").await.unwrap_err();
        assert_eq!(error.exception_type(), unimplemented, "{error:?}");
        let error = adapter.run_alarm(&now, 0).await.unwrap_err();
        assert_eq!(error.exception_type(), unimplemented, "{error:?}");
        let error = adapter.test().await.unwrap_err();
        assert_eq!(error.exception_type(), unimplemented, "{error:?}");
        assert_eq!(adapter.abandon_alarm(&now).await.unwrap(), None);
    })
    .unwrap();
    workers.shutdown(SOON);
}
