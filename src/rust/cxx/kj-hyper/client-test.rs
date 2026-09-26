use std::future::pending;
use std::io;
use std::sync::Mutex;

use tokio::io::AsyncReadExt;
use tokio::io::AsyncWriteExt;
use tokio::io::duplex;
use tokio::net::TcpListener;
use tokio::runtime::Builder;
use tokio::runtime::Runtime;

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
        let client = Client::new(&table, ClientSettings::default(), Peer::Origin, move || {
            let io = dialed.lock().unwrap().take();
            let io = io.map(|io| Dialed::with_hangup(io, Box::pin(pending())));
            async move { io.ok_or_else(|| io::Error::other("dialed twice")) }
        });
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
fn an_internet_client_sets_the_host_from_the_url() {
    let table = HeaderTable::builtin();
    let connect = |host: String, port| async move { connect_allowed(&host, port, |_| true).await };
    let client = Client::internet(&table, ClientSettings::default(), None, connect);
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
        let client = Client::internet(&table, ClientSettings::default(), None, connect);
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
