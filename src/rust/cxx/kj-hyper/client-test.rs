use std::future::pending;
use std::io;
use std::sync::Mutex;

use tokio::io::AsyncReadExt;
use tokio::io::AsyncWriteExt;
use tokio::io::duplex;
use tokio::net::TcpListener;
use tokio::runtime::Builder;
use tokio::runtime::Runtime;
use tokio::time::Instant;
use tokio::time::sleep;
use tokio::time::timeout;

use super::*;

fn runtime() -> Runtime {
    Builder::new_current_thread().enable_all().build().unwrap()
}

/// One HTTP/1.1 exchange over a raw stream, answered with `response`; returns the request.
async fn peer(mut io: impl AsyncRead + AsyncWrite + Unpin, response: &str) -> String {
    let mut request = Vec::new();
    while !request.ends_with(b"\r\n\r\n") {
        let mut byte = [0];
        io.read_exact(&mut byte).await.unwrap();
        request.push(byte[0]);
    }
    io.write_all(response.as_bytes()).await.unwrap();
    String::from_utf8(request).unwrap()
}

#[test]
fn a_fixed_client_dials_through_its_dialer_and_sends_the_url_as_given() {
    let table = HeaderTable::builtin();
    runtime().block_on(async {
        let (ours, theirs) = duplex(1 << 16);
        let dialed = Mutex::new(Some(ours));
        let pool = ClientPool::new(ClientSettings::default(), Peer::Origin, move || {
            let io = dialed.lock().unwrap().take();
            let io = io.map(|io| Dialed::with_hangup(io, Box::pin(pending())));
            async move { io.ok_or_else(|| io::Error::other("dialed twice")) }
        });
        let client = pool.client(&table);
        let mut head = Head::empty();
        head.set(Builtin::Host, http::HeaderValue::from_static("example.com"));
        let (uri, host) = client.target(b"/path?q=1").unwrap();
        assert!(host.is_none());
        let request = Client::request_of(http::Method::GET, uri, head, ChannelBody::empty());
        let server = tokio::spawn(peer(
            theirs,
            "HTTP/1.1 204 No Content\r\nX-Reply: yes\r\n\r\n",
        ));
        let response = client.send(request).await.unwrap();
        let request = server.await.unwrap();
        assert!(
            request.starts_with("GET /path?q=1 HTTP/1.1\r\n"),
            "{request}"
        );
        assert!(request.contains("\r\nHost: example.com\r\n"), "{request}");
        assert_eq!(response.status(), http::StatusCode::NO_CONTENT);
        let headers = HeaderBlock::new(response.headers(), response.extensions())
            .to_kj(&table)
            .unwrap();
        assert_eq!(
            HeadersRef::from(&*headers).get_by_name("x-reply"),
            Some(&b"yes"[..])
        );
    });
}

#[test]
fn an_idle_connection_is_closed_after_the_idle_timeout() {
    let table = HeaderTable::builtin();
    runtime().block_on(async {
        let (ours, mut theirs) = duplex(1 << 16);
        let dialed = Mutex::new(Some(ours));
        let idle_timeout = Duration::from_millis(100);
        let settings = ClientSettings {
            idle_timeout,
            ..ClientSettings::default()
        };
        let pool = ClientPool::new(settings, Peer::Origin, move || {
            let io = dialed.lock().unwrap().take();
            let io = io.map(|io| Dialed::with_hangup(io, Box::pin(pending())));
            async move { io.ok_or_else(|| io::Error::other("dialed twice")) }
        });
        let client = pool.client(&table);
        let (uri, _) = client.target(b"/").unwrap();
        let request =
            Client::request_of(http::Method::GET, uri, Head::empty(), ChannelBody::empty());
        let (response, _) = futures::join!(
            client.send(request),
            peer(&mut theirs, "HTTP/1.1 204 No Content\r\n\r\n"),
        );
        assert_eq!(response.unwrap().status(), http::StatusCode::NO_CONTENT);
        let idle_since = Instant::now();
        let mut rest = Vec::new();
        timeout(idle_timeout * 10, theirs.read_to_end(&mut rest))
            .await
            .expect("the idle connection was never closed")
            .unwrap();
        assert!(rest.is_empty());
        assert!(idle_since.elapsed() >= idle_timeout);
    });
}

#[test]
fn an_internet_client_sets_the_host_from_the_url() {
    let table = HeaderTable::builtin();
    let connect = |host: String, port| async move { connect_allowed(&host, port, |_| true).await };
    let client = ClientPool::internet(ClientSettings::default(), None, connect).client(&table);
    let (uri, host) = client.target(b"https://example.com:8443/p").unwrap();
    assert_eq!(uri.scheme_str(), Some("https"));
    assert_eq!(host.unwrap(), "example.com:8443");
    assert!(client.target(b"/relative").is_err());
}

#[test]
fn an_internet_client_connects_to_the_urls_host_through_its_connect() {
    let table = HeaderTable::builtin();
    runtime().block_on(async {
        let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let address = listener.local_addr().unwrap();
        let asked = Arc::new(Mutex::new(Vec::new()));
        let connect = {
            let asked = Arc::clone(&asked);
            move |host, port| {
                asked.lock().unwrap().push((host, port));
                TcpStream::connect(address)
            }
        };
        let client = ClientPool::internet(ClientSettings::default(), None, connect).client(&table);
        let (uri, _) = client.target(b"http://example.com:8080/p").unwrap();
        let request =
            Client::request_of(http::Method::GET, uri, Head::empty(), ChannelBody::empty());
        let server = tokio::spawn(async move {
            let (io, _) = listener.accept().await.unwrap();
            peer(io, "HTTP/1.1 204 No Content\r\n\r\n").await
        });
        let response = client.send(request).await.unwrap();
        assert!(server.await.unwrap().starts_with("GET /p HTTP/1.1\r\n"));
        assert_eq!(response.status(), http::StatusCode::NO_CONTENT);
        assert_eq!(*asked.lock().unwrap(), [("example.com".to_owned(), 8080)]);
    });
}

#[test]
fn a_denied_peer_is_not_connected() {
    runtime().block_on(async {
        let Err(error) = connect_allowed("127.0.0.1", 9, |_| false).await else {
            panic!("a denied peer was connected");
        };
        assert_eq!(error.kind(), io::ErrorKind::PermissionDenied);
    });
}

fn addr(ip: &str, port: u16) -> SocketAddr {
    SocketAddr::new(ip.parse().unwrap(), port)
}

#[test]
fn ipv4_addresses_are_tried_first_and_the_last_failure_is_returned() {
    runtime().block_on(async {
        let addrs = vec![
            addr("::1", 1),
            addr("127.0.0.1", 2),
            addr("::1", 3),
            addr("127.0.0.1", 4),
        ];
        let tried = Mutex::new(Vec::new());
        let connect = |a: SocketAddr| {
            tried.lock().unwrap().push(a.port());
            async move { Err::<(), _>(io::Error::other(a.port().to_string())) }
        };
        let last = race(addrs, Duration::ZERO, connect)
            .await
            .unwrap_err()
            .unwrap();
        assert_eq!(tried.into_inner().unwrap(), [2, 4, 1, 3]);
        assert_eq!(last.to_string(), "3");
    });
}

#[test]
fn no_addresses_fail_without_an_error() {
    runtime().block_on(async {
        let connect = |a: SocketAddr| async move { Ok(a.port()) };
        let raced = race(Vec::new(), Duration::ZERO, connect).await;
        assert!(raced.unwrap_err().is_none());
    });
}

#[test]
fn a_hanging_attempt_does_not_block_the_next() {
    runtime().block_on(async {
        let connect = |a: SocketAddr| async move {
            if a.port() == 0 {
                pending::<()>().await;
            }
            Ok(a.port())
        };
        let addrs = vec![addr("127.0.0.1", 0), addr("127.0.0.1", 1)];
        assert_eq!(race(addrs, Duration::ZERO, connect).await.unwrap(), 1);
    });
}

#[test]
fn a_failed_attempt_starts_the_next_without_the_delay() {
    runtime().block_on(async {
        let connect = |a: SocketAddr| async move {
            if a.port() == 0 {
                return Err(io::Error::other("refused"));
            }
            Ok(a.port())
        };
        let addrs = vec![addr("127.0.0.1", 0), addr("127.0.0.1", 1)];
        let raced = timeout(
            Duration::from_secs(10),
            race(addrs, Duration::from_hours(1), connect),
        );
        assert_eq!(raced.await.expect("waited for the delay").unwrap(), 1);
    });
}

#[test]
fn an_attempt_keeps_running_after_the_next_starts() {
    runtime().block_on(async {
        let connect = |a: SocketAddr| async move {
            if a.port() == 0 {
                sleep(Duration::from_millis(10)).await;
            } else {
                pending::<()>().await;
            }
            Ok(a.port())
        };
        let addrs = vec![addr("127.0.0.1", 0), addr("127.0.0.1", 1)];
        let raced = timeout(
            Duration::from_secs(10),
            race(addrs, Duration::ZERO, connect),
        );
        assert_eq!(
            raced.await.expect("the first attempt was dropped").unwrap(),
            0
        );
    });
}
