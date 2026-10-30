// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

fn runtime() -> tokio::runtime::Runtime {
    tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .unwrap()
}

#[test]
fn host_of_address_strips_the_port_only() {
    assert_eq!(host_of_address("127.0.0.1:8080"), "127.0.0.1");
    assert_eq!(host_of_address("*:8080"), "*");
    assert_eq!(host_of_address("[::1]:8080"), "[::1]");
    assert_eq!(host_of_address("::1"), "::1");
    assert_eq!(host_of_address("localhost"), "localhost");
    assert_eq!(host_of_address("unix:/tmp/sock"), "unix:/tmp/sock");
}

#[test]
fn the_peer_cf_blob_names_the_client() {
    runtime().block_on(async {
        let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
        let client = tokio::net::TcpStream::connect(listener.local_addr().unwrap())
            .await
            .unwrap();
        let (stream, peer) = listener.accept().await.unwrap();
        let accepted = Accepted::Socket(Socket::Tcp(stream), Some(peer));
        let blob: serde_json::Value =
            serde_json::from_str(&peer_cf_blob(&accepted).unwrap()).unwrap();
        assert_eq!(blob["clientIp"], peer.to_string());
        drop(client);
    });
}

#[cfg(unix)]
#[test]
fn a_unix_peer_cf_blob_carries_credentials() {
    runtime().block_on(async {
        let (a, _b) = tokio::net::UnixStream::pair().unwrap();
        let uid = a.peer_cred().unwrap().uid();
        let accepted = Accepted::Socket(Socket::Unix(a), None);
        let blob: serde_json::Value =
            serde_json::from_str(&peer_cf_blob(&accepted).unwrap()).unwrap();
        assert_eq!(blob["clientUid"], u64::from(uid));
    });
}

#[test]
fn a_loopback_socket_listens_in_process_with_no_peer_identity() {
    let loopback = Loopback::default();
    loopback.enable();
    runtime().block_on(async {
        let listener = listen("loopback:svc", 80, &loopback).await.unwrap();
        assert_eq!(listener.port().unwrap(), 0);
        let _client = loopback.connect("svc").unwrap();
        let accepted = listener.accept().await.unwrap();
        assert!(matches!(accepted, Accepted::Loopback(_)));
        assert_eq!(peer_cf_blob(&accepted), None);
    });
}

/// Listens on a loopback queue for any address but those in `taken`, recording what it tried.
fn listen_unless_taken<'a>(
    loopback: &'a Loopback,
    taken: &'a [&'a str],
    tried: &'a std::cell::RefCell<Vec<String>>,
) -> impl AsyncFnMut(&str) -> Result<StreamListener> + 'a {
    async move |address: &str| {
        tried.borrow_mut().push(address.to_owned());
        if taken.contains(&address) {
            return Err(kj::failed!("bind(): Address already in use"));
        }
        loopback.listen(address).map(StreamListener::Loopback)
    }
}

#[test]
fn a_taken_port_moves_to_the_next_free_one() {
    let loopback = Loopback::default();
    loopback.enable();
    runtime().block_on(async {
        let tried = std::cell::RefCell::new(Vec::new());
        let taken = ["localhost:8787", "localhost:8788"];
        let listen = listen_unless_taken(&loopback, &taken, &tried);
        let (_, bound) = listen_on_free_port("localhost:8787", 100, listen)
            .await
            .unwrap();
        assert_eq!(bound, "localhost:8789");
        assert_eq!(
            *tried.borrow(),
            ["localhost:8787", "localhost:8788", "localhost:8789"]
        );

        // A free port is used as it is.
        let listen = listen_unless_taken(&loopback, &taken, &tried);
        let (_, bound) = listen_on_free_port("localhost:9000", 100, listen)
            .await
            .unwrap();
        assert_eq!(bound, "localhost:9000");
    });
}

#[test]
fn no_free_port_is_the_first_ports_error() {
    let loopback = Loopback::default();
    loopback.enable();
    runtime().block_on(async {
        let tried = std::cell::RefCell::new(Vec::new());
        let taken = ["localhost:8787", "localhost:8788", "localhost:8789"];
        let listen = listen_unless_taken(&loopback, &taken, &tried);
        let Err(error) = listen_on_free_port("localhost:8787", 3, listen).await else {
            panic!("listened on a taken port");
        };
        assert_eq!(tried.borrow().len(), 3);
        let description = error.description();
        assert!(
            description.contains("no free port in 8787-8789"),
            "{description}"
        );
        assert!(
            description.contains("Address already in use"),
            "{description}"
        );

        // One attempt: the port's own error.
        let listen = listen_unless_taken(&loopback, &taken, &tried);
        let Err(error) = listen_on_free_port("localhost:8787", 1, listen).await else {
            panic!("listened on a taken port");
        };
        assert!(!error.description().contains("no free port"));

        // The search stops at the last port there is.
        tried.borrow_mut().clear();
        let taken = ["localhost:65535"];
        let listen = listen_unless_taken(&loopback, &taken, &tried);
        assert!(
            listen_on_free_port("localhost:65535", 100, listen)
                .await
                .is_err()
        );
        assert_eq!(*tried.borrow(), ["localhost:65535"]);
    });
}
