use std::future::pending;
use std::future::ready;
use std::str::from_utf8;

use futures::future::Either;
use futures::future::select;
use kj::http::HeaderId;
use kj::http::Headers;
use tokio::io::AsyncReadExt;
use tokio::io::AsyncWriteExt;
use tokio::io::duplex;
use tokio::runtime::Builder;
use tokio::runtime::Runtime;
use tokio::sync::mpsc::UnboundedReceiver;
use tokio::sync::mpsc::unbounded_channel;

use super::*;
use crate::Result;
use crate::io::io_kj_error;

fn runtime() -> Runtime {
    Builder::new_current_thread().enable_all().build().unwrap()
}

fn table() -> KjOwn<HeaderTable> {
    HeaderTable::builtin()
}

/// Answers requests with an empty 200 naming the request, echoes on CONNECT tunnels, and
/// fails `/fail`. Everything it does on the kj side is synchronous, so no kj event loop is
/// needed.
struct Echo<'t>(&'t HeaderTable);

#[async_trait::async_trait(?Send)]
impl Handler for Echo<'_> {
    async fn request<'a>(
        &'a self,
        method: Method,
        url: &'a [u8],
        headers: HeadersRef<'a>,
        _body: Pin<&'a mut AsyncInputStream>,
        response: ServiceResponse<'a>,
    ) -> Result<()> {
        if url == b"/fail" {
            return Err(failed("the handler failed"));
        }
        if url == b"/hang" {
            // The head of a body of unknown length, and then nothing.
            let _body = response.send(200, "OK", &Headers::new(self.0), None)?;
            return pending().await;
        }
        assert_eq!(method, Method::GET);
        assert_eq!(headers.get(HeaderId::HOST), Some(&b"example.com"[..]));
        let mut sent = Headers::new(self.0);
        sent.set(HeaderId::CONTENT_TYPE, "text/plain");
        sent.set(HeaderId::LOCATION, from_utf8(url).unwrap());
        if headers.get(HeaderId::UPGRADE).is_some() {
            // The handshake answer: the WebSocket itself would need a kj event loop.
            drop(response.accept_websocket(&sent)?);
            return Ok(());
        }
        response.send(200, "OK", &sent, Some(0))?;
        Ok(())
    }

    async fn connect<'a>(
        &'a self,
        host: &'a [u8],
        _headers: HeadersRef<'a>,
        connect: Connect,
    ) -> Result<()> {
        assert_eq!(host, b"example.com:443");
        let mut tunnel = connect.accept(200, "OK", (&Headers::new(self.0)).into())?;
        let mut buf = [0; 64];
        loop {
            let n = tunnel.read(&mut buf).await.map_err(|e| io_kj_error(&e))?;
            if n == 0 {
                return Ok(());
            }
            tunnel
                .write_all(&buf[..n])
                .await
                .map_err(|e| io_kj_error(&e))?;
        }
    }
}

/// Serves `request` and returns what the peer read until the server closed the connection.
fn exchange(request: &[u8]) -> String {
    let table = table();
    runtime().block_on(async {
        let (ours, mut peer) = duplex(1 << 16);
        let shutdown = Shutdown::new();
        let served = serve_connection(
            ours,
            Box::pin(pending()),
            &table,
            Rc::new(ServerSettings::default()),
            Rc::new(Echo(&table)),
            &shutdown,
        );
        let client = async {
            peer.write_all(request).await.unwrap();
            peer.shutdown().await.unwrap();
            let mut response = Vec::new();
            peer.read_to_end(&mut response).await.unwrap();
            String::from_utf8(response).unwrap()
        };
        let (served, response) = futures::join!(served, client);
        served.unwrap();
        response
    })
}

#[test]
fn a_request_reaches_the_handler_with_kj_types_and_its_response_is_written() {
    let response = exchange(b"GET /hello?x=1 HTTP/1.1\r\nHost: example.com\r\n\r\n");
    assert!(response.starts_with("HTTP/1.1 200 OK\r\n"), "{response}");
    // Header spellings are the application's, and Content-Length is kj's.
    assert!(
        response.contains("\r\nContent-Type: text/plain\r\n"),
        "{response}"
    );
    assert!(
        response.contains("\r\nLocation: /hello?x=1\r\n"),
        "{response}"
    );
    assert!(response.contains("\r\nContent-Length: 0\r\n"), "{response}");
}

#[test]
fn a_failed_call_is_answered_with_a_bare_500_and_closes_the_connection() {
    let response = exchange(b"GET /fail HTTP/1.1\r\nHost: example.com\r\n\r\n");
    assert_eq!(
        response,
        "HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n\
             Content-Length: 21\r\n\r\nInternal Server Error"
    );
}

#[test]
fn an_unknown_method_is_refused() {
    let response = exchange(b"BREW /pot HTTP/1.1\r\nHost: example.com\r\n\r\n");
    assert!(
        response.starts_with("HTTP/1.1 501 Not Implemented\r\n"),
        "{response}"
    );
}

#[test]
fn a_request_that_does_not_parse_is_answered_with_hypers_description() {
    assert_eq!(
        exchange(b"GARBAGE\r\n\r\n"),
        "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\nConnection: close\r\n\
             Content-Length: 26\r\n\r\ninvalid HTTP method parsed"
    );
    // hyper takes a request target of at most 65534 bytes.
    let mut request = b"GET /".to_vec();
    request.extend(b"a".repeat(usize::from(u16::MAX)));
    request.extend(b" HTTP/1.1\r\nHost: example.com\r\n\r\n");
    assert_eq!(
        exchange(&request),
        "HTTP/1.1 414 URI Too Long\r\nContent-Type: text/plain\r\nConnection: close\r\n\
             Content-Length: 12\r\n\r\nURI too long"
    );
}

#[test]
fn a_head_request_that_does_not_parse_is_answered_without_a_body() {
    assert_eq!(
        exchange(b"HEAD / HTTP/1.1\r\nHost: example.com\r\nX-A : 1\r\n\r\n"),
        "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\nConnection: close\r\n\
             Content-Length: 26\r\n\r\n"
    );
}

#[test]
fn a_websocket_handshake_is_answered_through_kj() {
    let response = exchange(
        b"GET /ws HTTP/1.1\r\nHost: example.com\r\nUpgrade: websocket\r\n\
              Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\n\
              Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n",
    );
    assert!(
        response.starts_with("HTTP/1.1 101 Switching Protocols\r\n"),
        "{response}"
    );
    assert!(
        response.contains("\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"),
        "{response}"
    );
}

#[test]
fn a_bad_websocket_handshake_is_refused_as_kj_refuses_it() {
    let response = exchange(
        b"GET /ws HTTP/1.1\r\nHost: example.com\r\nUpgrade: websocket\r\n\
              Connection: Upgrade\r\nSec-WebSocket-Version: 12\r\n\r\n",
    );
    assert!(
        response.starts_with("HTTP/1.1 426 Upgrade Required\r\n"),
        "{response}"
    );
    assert!(
        response.contains("\r\nSec-WebSocket-Version: 13\r\n"),
        "{response}"
    );
    assert!(
        response.ends_with("ERROR: The requested WebSocket version is not supported."),
        "{response}"
    );
}

#[test]
fn a_connect_is_answered_in_rust_and_the_tunnel_taken_over() {
    let table = table();
    runtime().block_on(async {
        let (ours, mut peer) = duplex(1 << 16);
        let shutdown = Shutdown::new();
        let served = serve_connection(
            ours,
            Box::pin(pending()),
            &table,
            Rc::new(ServerSettings::default()),
            Rc::new(Echo(&table)),
            &shutdown,
        );
        let client = async {
            peer.write_all(b"CONNECT example.com:443 HTTP/1.1\r\nHost: example.com:443\r\n\r\n")
                .await
                .unwrap();
            let mut head = Vec::new();
            while !head.ends_with(b"\r\n\r\n") {
                let mut byte = [0];
                peer.read_exact(&mut byte).await.unwrap();
                head.push(byte[0]);
            }
            assert!(head.starts_with(b"HTTP/1.1 200 OK\r\n"));
            peer.write_all(b"ping").await.unwrap();
            let mut echoed = [0; 4];
            peer.read_exact(&mut echoed).await.unwrap();
            assert_eq!(&echoed, b"ping");
            peer.shutdown().await.unwrap();
            let mut rest = Vec::new();
            peer.read_to_end(&mut rest).await.unwrap();
            assert!(rest.is_empty());
        };
        let (served, ()) = futures::join!(served, client);
        served.unwrap();
    });
}

#[test]
fn shutdown_closes_an_idle_connection() {
    let table = table();
    runtime().block_on(async {
        let (ours, mut peer) = duplex(1 << 16);
        let shutdown = Shutdown::new();
        let served = serve_connection(
            ours,
            Box::pin(pending()),
            &table,
            Rc::new(ServerSettings::default()),
            Rc::new(Echo(&table)),
            &shutdown,
        );
        let client = async {
            shutdown.shutdown();
            let mut rest = Vec::new();
            peer.read_to_end(&mut rest).await.unwrap();
            assert!(rest.is_empty());
        };
        let (served, ()) = futures::join!(served, client);
        served.unwrap();
    });
}

#[test]
fn a_failure_to_watch_for_a_hang_up_fails_the_serve() {
    let table = table();
    runtime().block_on(async {
        let (ours, _peer) = duplex(1 << 16);
        let shutdown = Shutdown::new();
        let served = serve_connection(
            ours,
            Box::pin(ready(Err(failed("no watch")))),
            &table,
            Rc::new(ServerSettings::default()),
            Rc::new(Echo(&table)),
            &shutdown,
        )
        .await;
        assert_eq!(served.unwrap_err().description(), "no watch");
    });
}

// =======================================================================================
// A connection served on one runtime, its requests handled on another thread

/// A request on its way from a connection to the test's handler thread.
struct Sent {
    request: RemoteRequest,
    /// Fails once the connection dropped the call.
    cancelled: oneshot::Receiver<()>,
    /// Dropped once the call has finished.
    done: oneshot::Sender<()>,
}

/// A `dispatch` for [`serve_connection_remote`] that sends each request to the returned queue.
fn dispatcher() -> (
    impl Fn(RemoteRequest) -> BoxFuture<'static, ()> + Send + 'static,
    UnboundedReceiver<Sent>,
) {
    let (queue, requests) = unbounded_channel();
    let dispatch = move |request| {
        let (cancel, cancelled) = oneshot::channel();
        let (done, finished) = oneshot::channel::<()>();
        let _ = queue.send(Sent {
            request,
            cancelled,
            done,
        });
        async move {
            let _cancel = cancel;
            let _ = finished.await;
        }
        .boxed()
    };
    (dispatch, requests)
}

/// A runtime of several threads, as a server's front is.
fn front() -> Runtime {
    Builder::new_multi_thread()
        .worker_threads(2)
        .enable_all()
        .build()
        .unwrap()
}

/// Handles the requests of `requests` on this thread, one at a time, until the queue closes.
/// Returns how many calls ran to their end and how many were cancelled.
fn handle_requests(mut requests: UnboundedReceiver<Sent>) -> (u32, u32) {
    let table = table();
    let settings = ServerSettings::default();
    runtime().block_on(async {
        let (mut finished, mut cancelled) = (0, 0);
        while let Some(sent) = requests.recv().await {
            let echo = Echo(&table);
            let call = pin!(sent.request.handle(&table, &settings, &echo));
            match select(call, sent.cancelled).await {
                Either::Left(_) => finished += 1,
                Either::Right(_) => cancelled += 1,
            }
            drop(sent.done);
        }
        (finished, cancelled)
    })
}

/// [`exchange`], with the connection served on another runtime than the handler's thread.
fn exchange_remote(request: &'static [u8]) -> String {
    let front = front();
    let (dispatch, requests) = dispatcher();
    let (ours, mut peer) = duplex(1 << 16);
    let served = front.spawn(async move {
        let shutdown = Shutdown::new();
        let timeout = ServerSettings::default().header_timeout;
        serve_connection_remote(ours, Box::pin(pending()), timeout, &shutdown, dispatch).await
    });
    let client = front.spawn(async move {
        peer.write_all(request).await.unwrap();
        peer.shutdown().await.unwrap();
        let mut response = Vec::new();
        peer.read_to_end(&mut response).await.unwrap();
        String::from_utf8(response).unwrap()
    });
    handle_requests(requests);
    front.block_on(served).unwrap().unwrap();
    front.block_on(client).unwrap()
}

#[test]
fn a_request_handled_on_another_thread_is_answered_as_a_local_one() {
    for request in [
        &b"GET /hello?x=1 HTTP/1.1\r\nHost: example.com\r\n\r\n"[..],
        b"GET /fail HTTP/1.1\r\nHost: example.com\r\n\r\n",
        b"BREW /pot HTTP/1.1\r\nHost: example.com\r\n\r\n",
        b"GET /ws HTTP/1.1\r\nHost: example.com\r\nUpgrade: websocket\r\n\
          Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\n\
          Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n",
        b"GET /ws HTTP/1.1\r\nHost: example.com\r\nUpgrade: websocket\r\n\
          Connection: Upgrade\r\nSec-WebSocket-Version: 12\r\n\r\n",
        b"GET /a HTTP/1.1\r\nHost: example.com\r\n\r\n\
          GET /b HTTP/1.1\r\nHost: example.com\r\n\r\n",
    ] {
        assert_eq!(exchange_remote(request), exchange(request));
    }
}

#[test]
fn a_connect_handled_on_another_thread_tunnels_the_connection() {
    let front = front();
    let (dispatch, requests) = dispatcher();
    let (ours, mut peer) = duplex(1 << 16);
    let served = front.spawn(async move {
        let shutdown = Shutdown::new();
        let timeout = ServerSettings::default().header_timeout;
        serve_connection_remote(ours, Box::pin(pending()), timeout, &shutdown, dispatch).await
    });
    let client = front.spawn(async move {
        peer.write_all(b"CONNECT example.com:443 HTTP/1.1\r\nHost: example.com:443\r\n\r\n")
            .await
            .unwrap();
        let mut head = Vec::new();
        while !head.ends_with(b"\r\n\r\n") {
            let mut byte = [0];
            peer.read_exact(&mut byte).await.unwrap();
            head.push(byte[0]);
        }
        assert!(head.starts_with(b"HTTP/1.1 200 OK\r\n"));
        peer.write_all(b"ping").await.unwrap();
        let mut echoed = [0; 4];
        peer.read_exact(&mut echoed).await.unwrap();
        assert_eq!(&echoed, b"ping");
        peer.shutdown().await.unwrap();
        let mut rest = Vec::new();
        peer.read_to_end(&mut rest).await.unwrap();
        assert!(rest.is_empty());
    });
    assert_eq!(handle_requests(requests), (1, 0));
    front.block_on(served).unwrap().unwrap();
    front.block_on(client).unwrap();
}

#[test]
fn a_hang_up_cancels_the_call_on_the_other_thread() {
    let front = front();
    let (dispatch, requests) = dispatcher();
    let (ours, mut peer) = duplex(1 << 16);
    let (peer_leaves, peer_left) = oneshot::channel::<()>();
    let served = front.spawn(async move {
        let shutdown = Shutdown::new();
        let timeout = ServerSettings::default().header_timeout;
        let gone = async move {
            let _ = peer_left.await;
            Ok(())
        };
        serve_connection_remote(ours, gone.boxed(), timeout, &shutdown, dispatch).await
    });
    let client = front.spawn(async move {
        peer.write_all(b"GET /hang HTTP/1.1\r\nHost: example.com\r\n\r\n")
            .await
            .unwrap();
        // Once the head arrived the call is running; then the peer goes away.
        let mut head = Vec::new();
        while !head.ends_with(b"\r\n\r\n") {
            let mut byte = [0];
            peer.read_exact(&mut byte).await.unwrap();
            head.push(byte[0]);
        }
        peer_leaves.send(()).unwrap();
        peer
    });
    assert_eq!(handle_requests(requests), (0, 1));
    front.block_on(served).unwrap().unwrap();
    drop(front.block_on(client).unwrap());
}

#[test]
fn a_call_dropped_after_its_head_cuts_the_response_short() {
    let front = front();
    let (dispatch, mut requests) = dispatcher();
    let (ours, mut peer) = duplex(1 << 16);
    let served = front.spawn(async move {
        let shutdown = Shutdown::new();
        let timeout = ServerSettings::default().header_timeout;
        serve_connection_remote(ours, Box::pin(pending()), timeout, &shutdown, dispatch).await
    });
    let (got_head, head_arrived) = oneshot::channel::<()>();
    let client = front.spawn(async move {
        peer.write_all(b"GET /hang HTTP/1.1\r\nHost: example.com\r\n\r\n")
            .await
            .unwrap();
        let mut response = Vec::new();
        while !response.ends_with(b"\r\n\r\n") {
            let mut byte = [0];
            peer.read_exact(&mut byte).await.unwrap();
            response.push(byte[0]);
        }
        got_head.send(()).unwrap();
        // The connection is dropped; whether that reads as an end or as an error is the pipe's.
        let _ = peer.read_to_end(&mut response).await;
        String::from_utf8(response).unwrap()
    });
    let table = table();
    let settings = ServerSettings::default();
    runtime().block_on(async {
        let sent = requests.recv().await.unwrap();
        let echo = Echo(&table);
        {
            let mut call = pin!(sent.request.handle(&table, &settings, &echo));
            // The handler sends its head and then hangs.
            assert!(futures::poll!(call.as_mut()).is_pending());
            head_arrived.await.unwrap();
        }
        drop(sent.done);
    });
    front.block_on(served).unwrap().unwrap();
    let response = front.block_on(client).unwrap();
    assert!(response.starts_with("HTTP/1.1 200 OK\r\n"), "{response}");
    // A chunked body that ended would say so.
    assert!(response.ends_with("\r\n\r\n"), "{response}");
    assert!(!response.contains("\r\n0\r\n"), "{response}");
}

#[test]
fn a_request_dropped_unhandled_closes_the_connection_unanswered() {
    let front = front();
    let (dispatch, mut requests) = dispatcher();
    let (ours, mut peer) = duplex(1 << 16);
    let served = front.spawn(async move {
        let shutdown = Shutdown::new();
        let timeout = ServerSettings::default().header_timeout;
        serve_connection_remote(ours, Box::pin(pending()), timeout, &shutdown, dispatch).await
    });
    let client = front.spawn(async move {
        peer.write_all(b"GET /hello HTTP/1.1\r\nHost: example.com\r\n\r\n")
            .await
            .unwrap();
        let mut response = Vec::new();
        let _ = peer.read_to_end(&mut response).await;
        response
    });
    drop(requests.blocking_recv().unwrap());
    front.block_on(served).unwrap().unwrap();
    assert!(front.block_on(client).unwrap().is_empty());
}
