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
    let (sockets, port) = bind();
    let queue = Queue::new();
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
