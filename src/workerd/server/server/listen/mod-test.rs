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
