// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use tokio::io::AsyncReadExt;
use tokio::io::AsyncWriteExt;

use super::*;

fn runtime() -> tokio::runtime::Runtime {
    tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .unwrap()
}

async fn exchange(mut a: LoopbackStream, mut b: LoopbackStream) {
    a.write_all(b"ping").await.unwrap();
    let mut buf = [0; 4];
    b.read_exact(&mut buf).await.unwrap();
    assert_eq!(&buf, b"ping");
    b.write_all(b"pong").await.unwrap();
    a.read_exact(&mut buf).await.unwrap();
    assert_eq!(&buf, b"pong");
}

#[test]
fn loopback_is_off_until_enabled() {
    let loopback = Loopback::default();
    assert!(!loopback.is_enabled());
    let err = loopback.listen("svc").unwrap_err();
    assert!(err.description().contains("loopback:svc"));
    assert!(loopback.connect("svc").is_err());
    loopback.enable();
    assert!(loopback.is_enabled());
    assert!(loopback.listen("svc").is_ok());
}

#[test]
fn a_name_is_listened_on_once() {
    let loopback = Loopback::default();
    loopback.enable();
    let _listener = loopback.listen("svc").unwrap();
    assert!(
        loopback
            .listen("svc")
            .unwrap_err()
            .description()
            .contains("already listened on")
    );
    assert!(loopback.listen("other").is_ok());
}

#[test]
fn a_connection_made_before_listening_waits_in_the_queue() {
    let loopback = Loopback::default();
    loopback.enable();
    runtime().block_on(async {
        let client = loopback.connect("svc").unwrap();
        let listener = loopback.clone().listen("svc").unwrap();
        let server = listener.accept().await.unwrap();
        exchange(client, server).await;
    });
}

#[test]
fn a_listener_receives_later_connections() {
    let loopback = Loopback::default();
    loopback.enable();
    runtime().block_on(async {
        let listener = loopback.listen("svc").unwrap();
        let accepting = listener.accept();
        let client = loopback.connect("svc").unwrap();
        let server = accepting.await.unwrap();
        exchange(server, client).await;
    });
}

#[test]
fn connecting_fails_once_the_listener_is_gone() {
    let loopback = Loopback::default();
    loopback.enable();
    drop(loopback.listen("svc").unwrap());
    assert!(loopback.connect("svc").is_err());
}
