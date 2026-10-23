use std::io::Read;
use std::task::Context;
use std::task::Waker;

use static_assertions::assert_impl_all;

use super::*;

// Send + Sync like every kj-rs-io handle (lib.rs asserts this at compile time).
assert_impl_all!(TokioStream: Send, Sync);

/// A connected localhost TCP pair: the server end as a kj-rs-io stream registered with
/// `port`'s runtime, the client end as a std socket.
// Takes the port only to make the caller prove one exists (registration needs it).
fn connected_pair(_port: &kj_rs_tokio::TokioPort) -> (TokioStream, std::net::TcpStream) {
    let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
    let client = std::net::TcpStream::connect(listener.local_addr().unwrap()).unwrap();
    let (server, _) = listener.accept().unwrap();
    server.set_nonblocking(true).unwrap();
    let server = TcpStream::from_std(server).unwrap();
    (TokioStream::new(Socket::Tcp(server)).unwrap(), client)
}

fn poll_once<T>(fut: &mut std::pin::Pin<Box<impl Future<Output = T>>>) -> std::task::Poll<T> {
    let mut cx = Context::from_waker(Waker::noop());
    fut.as_mut().poll(&mut cx)
}

/// The ownership model of the module docs: a pending operation owns a share of the socket,
/// so dropping the handle (the C++ wrapper's Box) while the read is in flight neither
/// dangles nor closes the socket; the read is cancelled cleanly afterwards.
#[test]
fn dropping_the_handle_with_a_read_in_flight_is_safe() {
    let port = kj_rs_tokio::TokioPort::new();
    let (stream, client) = connected_pair(&port);

    let mut buf = [MaybeUninit::<u8>::uninit(); 8];
    let mut read = Box::pin(stream.try_read_min(&mut buf, 1));
    assert!(poll_once(&mut read).is_pending());
    assert_eq!(
        Arc::strong_count(&stream.inner),
        2,
        "the handle and the read own a share"
    );

    drop(stream);
    // The socket is still open: the peer does not see EOF (a non-blocking read blocks).
    client.set_nonblocking(true).unwrap();
    let mut probe = [0u8; 1];
    assert_eq!(
        (&client).read(&mut probe).map_err(|e| e.kind()).err(),
        Some(std::io::ErrorKind::WouldBlock),
        "peer must not observe a close while the read owns the socket"
    );
    assert!(poll_once(&mut read).is_pending());
    drop(read); // releases the last share: now the socket closes
    // The close reaches the peer asynchronously (loopback still hands the FIN over after
    // close(2) returns), so wait for it rather than sampling once: blocking with a timeout.
    client.set_nonblocking(false).unwrap();
    client
        .set_read_timeout(Some(std::time::Duration::from_secs(5)))
        .unwrap();
    let n = (&client).read(&mut probe);
    assert!(
        matches!(n, Ok(0))
            || matches!(&n, Err(e) if e.kind() == std::io::ErrorKind::ConnectionReset),
        "peer observes the close (EOF, or a reset if unread data was pending): {n:?}"
    );
}

#[test]
fn concurrent_read_and_write_both_in_flight_share_the_socket() {
    // A read and a write can be in flight at once (kj's one-read + one-write contract),
    // each owning a share; the socket outlives the handle until both are gone.
    let port = kj_rs_tokio::TokioPort::new();
    let (stream, _client) = connected_pair(&port);

    let mut buf = [MaybeUninit::<u8>::uninit(); 8];
    let mut read = Box::pin(stream.try_read_min(&mut buf, 1));
    // 1 MiB: larger than the socket buffer, so the write cannot drain in one go and the
    // future stays pending, holding its share.
    let payload = vec![0u8; 1024 * 1024];
    let mut write = Box::pin(stream.write_all(&payload));
    assert!(poll_once(&mut read).is_pending());
    assert!(poll_once(&mut write).is_pending());
    assert_eq!(Arc::strong_count(&stream.inner), 3);
    drop(read);
    assert_eq!(Arc::strong_count(&stream.inner), 2);
    drop(write);
    assert_eq!(Arc::strong_count(&stream.inner), 1);
}

/// The hangup watcher's `dup(2)` is per stream, not per call: the first
/// `whenWriteDisconnected` creates it, and every later wait -- concurrent or not -- reuses
/// the same descriptor and registration.
/// `abortRead()` ends a read parked on readiness and makes later reads EOF, without relying on
/// the poller reporting the local shutdown (it does not on Windows).
#[test]
fn abort_read_ends_a_parked_read_and_later_reads_with_eof() {
    let port = kj_rs_tokio::TokioPort::new();
    let (stream, _client) = connected_pair(&port);
    let mut buf = [MaybeUninit::<u8>::uninit(); 8];
    let mut read = Box::pin(stream.try_read_min(&mut buf, 1));
    assert!(
        poll_once(&mut read).is_pending(),
        "nothing to read yet: parked"
    );
    stream.abort_read().unwrap();
    assert!(
        matches!(poll_once(&mut read), std::task::Poll::Ready(Ok(0))),
        "the parked read observes EOF"
    );
    drop(read);
    let mut later = Box::pin(stream.try_read_min(&mut buf, 1));
    assert!(matches!(
        poll_once(&mut later),
        std::task::Poll::Ready(Ok(0))
    ));
}

/// A stream carried to another loop thread is memory-safe, but its socket is registered with
/// its creator's driver: the first wait there fails instead of parking forever.
#[test]
fn a_read_parked_on_a_different_port_is_refused() {
    let port = kj_rs_tokio::TokioPort::new();
    let (stream, _client) = connected_pair(&port);
    let err = std::thread::spawn(move || {
        let _other_port = kj_rs_tokio::TokioPort::new();
        let mut buf = [MaybeUninit::<u8>::uninit(); 8];
        let mut read = Box::pin(stream.try_read_min(&mut buf, 1));
        match poll_once(&mut read) {
            std::task::Poll::Ready(Err(e)) => cxx::KjError::from(e),
            _ => panic!("a read on a foreign port must fail at its first wait"),
        }
    })
    .join()
    .unwrap();
    assert!(err.description().contains("different TokioEventPort"));
}

#[cfg(unix)]
#[test]
fn write_disconnected_dups_the_socket_once_per_stream() {
    use std::os::fd::AsRawFd;
    let port = kj_rs_tokio::TokioPort::new();
    let (stream, _client) = connected_pair(&port);
    let watch_fd = || {
        stream
            .inner
            .hangup_watch
            .get()
            .map(|afd| afd.get_ref().as_raw_fd())
    };
    assert_eq!(watch_fd(), None, "no dup before the first wait");
    let mut first = Box::pin(stream.when_write_disconnected());
    assert!(poll_once(&mut first).is_pending());
    let dup = watch_fd().expect("the first wait created the dup");
    assert_ne!(
        i64::from(dup),
        stream.raw_handle(),
        "it is a dup, not the socket"
    );
    let mut second = Box::pin(stream.when_write_disconnected());
    assert!(poll_once(&mut second).is_pending());
    assert_eq!(watch_fd(), Some(dup), "a concurrent wait reuses it");
    drop(first);
    drop(second);
    let mut third = Box::pin(stream.when_write_disconnected());
    assert!(poll_once(&mut third).is_pending());
    assert_eq!(watch_fd(), Some(dup), "a later wait reuses it too");
}

/// The lazy hangup-watch registration is the one registration that happens after
/// construction; it goes through the same loop-thread check as the constructors, so a
/// foreign runtime entered over the loop thread is an error, not a registration with a
/// driver that never turns.
#[cfg(unix)]
#[test]
fn write_disconnected_refuses_to_register_under_a_foreign_runtime() {
    let port = kj_rs_tokio::TokioPort::new();
    let (stream, _client) = connected_pair(&port);
    let auxiliary = tokio::runtime::Builder::new_current_thread()
        .build()
        .unwrap();
    let _guard = auxiliary.enter();
    let mut wait = Box::pin(stream.when_write_disconnected());
    let std::task::Poll::Ready(Err(e)) = poll_once(&mut wait) else {
        panic!("must fail under a foreign runtime");
    };
    assert!(
        cxx::KjError::from(e)
            .description()
            .contains("other than this thread's TokioEventPort runtime")
    );
}
