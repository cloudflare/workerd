// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::future::Future;
use std::future::poll_fn;
use std::io::Read;
use std::io::Write;
use std::net::TcpStream;
use std::task::Poll;
use std::time::Duration;

use kj_rs_io::TokioAddress;
use tokio::io::AsyncReadExt;

use super::*;

/// A listening socket on a port of the system's choice, as the first loop thread binds one, and
/// its port. Needs the calling thread's loop.
fn bind() -> (ListenerSockets, u16) {
    let listener = TokioAddress::from_socket_addrs(vec!["127.0.0.1:0".parse().unwrap()])
        .listen()
        .unwrap();
    let port = listener.port().unwrap();
    (listener.into_sockets().map_err(Error::from).unwrap(), port)
}

/// A front with one socket of `protocol`, not serving yet: the front, its queue and the port.
async fn front(protocol: Protocol) -> (Front, Queue, u16) {
    front_with_limit(protocol, DEFAULT_QUEUE_LIMIT).await
}

/// [`front`], whose queue holds `limit` items.
async fn front_with_limit(protocol: Protocol, limit: usize) -> (Front, Queue, u16) {
    let (sockets, port) = bind();
    let queue = Queue::new(limit);
    let socket = FrontSocket {
        index: 3,
        sockets,
        protocol,
    };
    let front = Front::new(2, queue.clone(), vec![socket]).await.unwrap();
    (front, queue, port)
}

const HTTP: Protocol = Protocol::Http {
    tls: None,
    peer_cf_blob: true,
};

/// Waits until `condition` holds, for a while.
async fn eventually(what: &str, condition: impl Fn() -> bool) {
    for _ in 0..2000 {
        if condition() {
            return;
        }
        tokio::time::sleep(Duration::from_millis(5)).await;
    }
    panic!("{what} did not happen");
}

/// What a loop that is about to sleep takes: the next item, or `None` once the queue has
/// ended; panics if there is neither after a while.
async fn take(worker: &Worker) -> Option<Work> {
    let parking = worker.parking();
    for _ in 0..2000 {
        parking.before_park();
        let taken = poll_fn(|cx| Poll::Ready(worker.poll_take(cx))).await;
        parking.after_unpark();
        match taken {
            Poll::Ready(Some((work, _))) => return Some(work),
            Poll::Ready(None) => return None,
            Poll::Pending => tokio::time::sleep(Duration::from_millis(5)).await,
        }
    }
    panic!("the queue neither had work nor ended");
}

/// Whether `future` is still pending when polled now.
async fn is_pending(future: &mut (impl Future + Unpin)) -> bool {
    futures::poll!(future).is_pending()
}

fn run<F: Future<Output = ()> + 'static>(test: F) {
    let mut runtime = kj_rs_tokio::Runtime::new().unwrap();
    runtime.block_on(test).unwrap();
}

#[test]
fn nothing_is_accepted_before_the_front_serves() {
    run(async {
        let (front, queue, port) = front(Protocol::Tcp { tls: None }).await;
        let worker = queue.worker();
        // The system completes the connection; the front has not accepted it.
        let mut client = TcpStream::connect(("127.0.0.1", port)).unwrap();
        client.write_all(b"ping").unwrap();
        tokio::time::sleep(Duration::from_millis(50)).await;
        assert!(queue.is_empty());

        front.serve();
        let Some(Work::Tcp(mut work)) = take(&worker).await else {
            panic!("a TCP connection is queued as one");
        };
        assert_eq!(work.socket, 3);
        assert_eq!(work.peer, Some(client.local_addr().unwrap()));
        assert!(work.starttls.is_none());
        // The transport is the connection, usable on this thread though the front's runtime
        // has it registered.
        let mut ping = [0; 4];
        work.io.read_exact(&mut ping).await.unwrap();
        assert_eq!(&ping, b"ping");
    });
}

#[test]
fn a_drained_front_closes_its_queue_once_its_connections_have_ended() {
    run(async {
        let (mut front, queue, port) = front(HTTP).await;
        let worker = queue.worker();
        let mut watch = front.watch();
        front.serve();
        let mut client = TcpStream::connect(("127.0.0.1", port)).unwrap();
        client
            .write_all(b"GET / HTTP/1.1\r\nHost: example.com\r\n\r\n")
            .unwrap();
        eventually("the request is queued", || queue.len() == 1).await;

        // The front stops accepting, but the request it queued is still to be served.
        front.drain();
        let Some(Work::Http(work)) = take(&worker).await else {
            panic!("an HTTP request is queued as one");
        };
        assert_eq!(work.socket, 3);
        let blob = work.cf_blob.as_deref().unwrap();
        assert!(blob.contains(&client.local_addr().unwrap().to_string()));
        tokio::time::sleep(Duration::from_millis(50)).await;
        assert!(is_pending(&mut watch).await);
        assert!(TcpStream::connect(("127.0.0.1", port)).is_err());

        // The call ends without an answer, which closes the connection; it was the last.
        drop(work);
        let mut rest = Vec::new();
        client.read_to_end(&mut rest).unwrap();
        assert!(rest.is_empty());
        assert!(take(&worker).await.is_none());
        watch.await.unwrap();
    });
}

// A reset is noticed while nothing is read from the connection, which Windows has no way to do
// (`kj_rs_io::when_write_disconnected`).
#[cfg(unix)]
#[test]
fn the_request_of_a_client_that_went_away_leaves_the_queue() {
    run(async {
        let (front, queue, port) = front(HTTP).await;
        let _worker = queue.worker();
        front.serve();
        let mut client = TcpStream::connect(("127.0.0.1", port)).unwrap();
        client
            .write_all(b"GET / HTTP/1.1\r\nHost: example.com\r\n\r\n")
            .unwrap();
        eventually("the request is queued", || queue.len() == 1).await;
        // The client resets the connection, which a server notices without reading from it.
        socket2::SockRef::from(&client)
            .set_linger(Some(Duration::ZERO))
            .unwrap();
        drop(client);
        eventually("the request leaves the queue", || queue.is_empty()).await;
    });
}

#[cfg(unix)]
#[test]
fn a_client_that_went_away_drops_the_call_a_loop_took() {
    run(async {
        let (front, queue, port) = front(HTTP).await;
        let worker = queue.worker();
        front.serve();
        let mut client = TcpStream::connect(("127.0.0.1", port)).unwrap();
        client
            .write_all(b"GET / HTTP/1.1\r\nHost: example.com\r\n\r\n")
            .unwrap();
        let Some(Work::Http(mut work)) = take(&worker).await else {
            panic!("an HTTP request is queued as one");
        };
        assert!(is_pending(&mut std::pin::pin!(work.call.closed())).await);
        socket2::SockRef::from(&client)
            .set_linger(Some(Duration::ZERO))
            .unwrap();
        drop(client);
        // The loop that serves the request sees its call dropped.
        tokio::time::timeout(Duration::from_secs(30), work.call.closed())
            .await
            .unwrap();
    });
}

#[test]
fn stopping_the_front_drops_its_connections_and_their_calls() {
    run(async {
        let (front, queue, port) = front(HTTP).await;
        let worker = queue.worker();
        front.serve();
        let mut client = TcpStream::connect(("127.0.0.1", port)).unwrap();
        client
            .write_all(b"GET / HTTP/1.1\r\nHost: example.com\r\n\r\n")
            .unwrap();
        let Some(Work::Http(mut work)) = take(&worker).await else {
            panic!("an HTTP request is queued as one");
        };
        // Joins the front's threads: nothing of it runs afterwards.
        drop(front);
        tokio::time::timeout(Duration::from_secs(30), work.call.closed())
            .await
            .unwrap();
        // The request watches its connection for a hang-up, which keeps the connection open
        // until the loop lets go of the request, as it does once the call is dropped.
        drop(work);
        let mut rest = Vec::new();
        // The connection is gone; whether that reads as an end or a reset is the system's.
        let _ = client.read_to_end(&mut rest);
        assert!(rest.is_empty());
    });
}

/// A client that has sent one request for `path`.
fn request(port: u16, path: &str) -> TcpStream {
    let mut client = TcpStream::connect(("127.0.0.1", port)).unwrap();
    let request = format!("GET {path} HTTP/1.1\r\nHost: example.com\r\n\r\n");
    client.write_all(request.as_bytes()).unwrap();
    client
}

/// The path of the request `work` is.
fn path_of(work: Option<Work>) -> String {
    let Some(Work::Http(work)) = work else {
        panic!("an HTTP request is queued as one");
    };
    work.request.uri().path().to_owned()
}

#[test]
fn a_full_queue_holds_requests_and_connections_back() {
    run(async {
        let (front, queue, port) = front_with_limit(HTTP, 2).await;
        let worker = queue.worker();
        front.serve();
        // More clients than the queue holds, one after the other.
        let mut clients = Vec::new();
        for index in 0..6 {
            clients.push(request(port, &format!("/{index}")));
            if index < 2 {
                eventually("the request is queued", || queue.len() == index + 1).await;
            }
        }
        // No loop takes anything: the queue stays at its limit, whatever the clients sent.
        tokio::time::sleep(Duration::from_millis(200)).await;
        assert_eq!(queue.len(), 2);
        // Each item a loop takes makes room for one more, in the order the clients came, and
        // nobody was answered meanwhile.
        for index in 0..6 {
            assert_eq!(path_of(take(&worker).await), format!("/{index}"));
            let queued = (5 - index).min(2);
            eventually("the next request is queued", || queue.len() == queued).await;
            tokio::time::sleep(Duration::from_millis(20)).await;
            assert_eq!(queue.len(), queued);
        }
    });
}

// A reset is noticed while nothing is read from the connection, which Windows has no way to do.
#[cfg(unix)]
#[test]
fn a_request_that_waits_for_room_leaves_with_its_client() {
    run(async {
        let (front, queue, port) = front_with_limit(HTTP, 1).await;
        let worker = queue.worker();
        front.serve();
        let _first = request(port, "/first");
        eventually("the request is queued", || queue.len() == 1).await;
        // The front accepted this connection before the queue was full, or accepts it once it
        // has room; either way its request is not queued, and the client gives up.
        let gone = request(port, "/gone");
        let _last = request(port, "/last");
        tokio::time::sleep(Duration::from_millis(100)).await;
        socket2::SockRef::from(&gone)
            .set_linger(Some(Duration::ZERO))
            .unwrap();
        drop(gone);
        tokio::time::sleep(Duration::from_millis(100)).await;
        assert_eq!(path_of(take(&worker).await), "/first");
        // A request of a client that is gone may be queued for the moment it takes the front
        // to notice, and leaves the queue then: the loop is not given it.
        eventually("the last request is queued", || queue.len() == 1).await;
        tokio::time::sleep(Duration::from_millis(100)).await;
        assert_eq!(path_of(take(&worker).await), "/last");
        assert!(queue.is_empty());
    });
}

const TCP: Protocol = Protocol::Tcp { tls: None };

/// A client of a TCP socket that has sent `ping`.
fn tcp_client(port: u16) -> TcpStream {
    let mut client = TcpStream::connect(("127.0.0.1", port)).unwrap();
    client.write_all(b"ping").unwrap();
    client
}

/// Reads the `ping` of a connection a loop took and answers `pong`, which the client gets:
/// the connection works in both directions.
async fn ping_pong(work: &mut TcpWork, client: &mut TcpStream) {
    use tokio::io::AsyncWriteExt;
    let mut ping = [0; 4];
    work.io.read_exact(&mut ping).await.unwrap();
    assert_eq!(&ping, b"ping");
    work.io.write_all(b"pong").await.unwrap();
    work.io.flush().await.unwrap();
    let mut pong = [0; 4];
    client.read_exact(&mut pong).unwrap();
    assert_eq!(&pong, b"pong");
}

#[test]
fn a_drained_front_waits_for_the_tcp_connections_the_loops_serve() {
    run(async {
        let (mut front, queue, port) = front(TCP).await;
        let worker = queue.worker();
        let mut watch = front.watch();
        front.serve();
        let mut taken_client = tcp_client(port);
        let Some(Work::Tcp(mut taken)) = take(&worker).await else {
            panic!("a TCP connection is queued as one");
        };
        let mut queued_client = tcp_client(port);
        eventually("the connection is queued", || queue.len() == 1).await;

        // The front stops accepting. The connection a loop serves and the one still queued
        // are its connections until the loops are done with them: the queue stays open, and
        // the front's runtime, which the streams are registered with, stays up.
        front.drain();
        tokio::time::sleep(Duration::from_millis(200)).await;
        assert!(is_pending(&mut watch).await);
        assert_eq!(queue.len(), 1);
        assert!(TcpStream::connect(("127.0.0.1", port)).is_err());
        ping_pong(&mut taken, &mut taken_client).await;
        let Some(Work::Tcp(mut queued)) = take(&worker).await else {
            panic!("a queued TCP connection is served after the drain");
        };
        ping_pong(&mut queued, &mut queued_client).await;

        // The loops finish them, one after the other; only then has the front drained.
        drop(taken);
        tokio::time::sleep(Duration::from_millis(100)).await;
        assert!(is_pending(&mut watch).await);
        drop(queued);
        assert!(take(&worker).await.is_none());
        watch.await.unwrap();
    });
}

// A reset is noticed while nothing is read from the connection, which Windows has no way to do.
#[cfg(unix)]
#[test]
fn a_tcp_connection_whose_client_went_away_leaves_the_queue() {
    run(async {
        let (front, queue, port) = front(TCP).await;
        let _worker = queue.worker();
        front.serve();
        let client = tcp_client(port);
        let _stays = tcp_client(port);
        eventually("the connections are queued", || queue.len() == 2).await;
        socket2::SockRef::from(&client)
            .set_linger(Some(Duration::ZERO))
            .unwrap();
        drop(client);
        eventually("the connection leaves the queue", || queue.len() == 1).await;
    });
}

#[cfg(unix)]
#[test]
fn a_tcp_connection_a_loop_took_is_the_loops_to_end_when_its_client_goes_away() {
    run(async {
        let (mut front, queue, port) = front(TCP).await;
        let worker = queue.worker();
        let mut watch = front.watch();
        front.serve();
        let client = tcp_client(port);
        let Some(Work::Tcp(mut work)) = take(&worker).await else {
            panic!("a TCP connection is queued as one");
        };
        socket2::SockRef::from(&client)
            .set_linger(Some(Duration::ZERO))
            .unwrap();
        drop(client);
        // The loop learns of it by the connection's own signal, and its `connect()` event is
        // not dropped for it: the front waits.
        (&mut work.hangup).await.unwrap();
        front.drain();
        tokio::time::sleep(Duration::from_millis(100)).await;
        assert!(is_pending(&mut std::pin::pin!(work.call.closed())).await);
        assert!(is_pending(&mut watch).await);
        drop(work);
        watch.await.unwrap();
    });
}

#[test]
fn a_stopped_front_keeps_its_runtime_for_the_streams_the_loops_hold() {
    run(async {
        let (front, queue, port) = front(TCP).await;
        let worker = queue.worker();
        front.serve();
        let mut client = tcp_client(port);
        let Some(Work::Tcp(mut work)) = take(&worker).await else {
            panic!("a TCP connection is queued as one");
        };
        ping_pong(&mut work, &mut client).await;
        // Stopping drops the front's connections, which the loop that serves one sees.
        front.stop();
        tokio::time::timeout(Duration::from_secs(30), work.call.closed())
            .await
            .unwrap();
        tokio::time::sleep(Duration::from_millis(200)).await;
        // The stream is registered with the front's runtime, which is there for as long as
        // the front is: a loop that has not stopped yet is still woken for what arrives.
        let mut late = [0; 4];
        let mut read = std::pin::pin!(work.io.read_exact(&mut late));
        assert!(is_pending(&mut read).await);
        client.write_all(b"late").unwrap();
        tokio::time::timeout(Duration::from_secs(30), read)
            .await
            .unwrap()
            .unwrap();
        assert_eq!(&late, b"late");
        drop(work);
        drop(front);
    });
}

/// A connection of a TCP socket as the front queues it, and the front's end of its call.
fn tcp_work() -> (Work, oneshot::Receiver<()>) {
    let (call, finished) = oneshot::channel();
    let work = TcpWork {
        socket: 0,
        io: Box::pin(tokio::io::duplex(1).0),
        hangup: std::future::pending().boxed(),
        peer: None,
        starttls: None,
        call,
    };
    (Work::Tcp(work), finished)
}

#[test]
fn a_panic_in_a_connections_task_ends_that_connection_and_is_logged() {
    run(async {
        let queue = Queue::new(2);
        let worker = queue.worker();
        let (log, mut lines) = mpsc::channel(4);
        let (_phase, phase) = watch::channel(Phase::Serving);
        let shared = Shared {
            queue: queue.clone(),
            phase,
            log,
            shutdown: Shutdown::new(),
        };
        // A connection with one call that a loop took and one still queued, whose task then
        // panics.
        let (first, first_finished) = tcp_work();
        let (second, second_finished) = tcp_work();
        let taken = std::cell::RefCell::new(None);
        let connection = async {
            let _first = shared.queue.push(first).await;
            *taken.borrow_mut() = take(&worker).await;
            let _second = shared.queue.push(second).await;
            assert_eq!(queue.len(), 1);
            let _calls = (first_finished, second_finished);
            panic!("the connection's task went wrong");
        };
        guarded(&shared, "TCP connection failed", connection).await;

        // The panic took nothing else down, and what the task held went with it: the queued
        // call left the queue, which has its room back, and the loop is told to drop the other.
        assert!(queue.is_empty());
        let (third, _third_finished) = tcp_work();
        let (fourth, _fourth_finished) = tcp_work();
        let _tickets = (
            queue
                .push(third)
                .now_or_never()
                .expect("the queue has room"),
            queue
                .push(fourth)
                .now_or_never()
                .expect("the queue has room"),
        );
        let Some(Work::Tcp(mut taken)) = taken.into_inner() else {
            panic!("the loop took the first call");
        };
        tokio::time::timeout(Duration::from_secs(30), taken.call.closed())
            .await
            .unwrap();
        // It is logged as the connection's failure.
        let (what, error) = lines.recv().await.unwrap();
        assert_eq!(what, "TCP connection failed");
        let description = error.description();
        assert!(description.contains("panicked"), "{description}");
        assert!(
            description.contains("the connection's task went wrong"),
            "{description}"
        );
    });
}
