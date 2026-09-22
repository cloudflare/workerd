// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Basic Tokio runtime tests that also verify its scheduler, timer, and I/O reactor under `TSan`.

use std::net::Ipv4Addr;
use std::time::Duration;

fn runtime() -> tokio::runtime::Runtime {
    tokio::runtime::Builder::new_multi_thread()
        .worker_threads(4)
        .enable_all()
        .build()
        .expect("failed to create Tokio runtime")
}

#[test]
fn runtime_executes_spawned_tasks() {
    let result = runtime().block_on(async {
        tokio::spawn(async { 42 })
            .await
            .expect("spawned task panicked")
    });

    assert_eq!(result, 42);
}

#[test]
fn runtime_drives_timers() {
    let result = runtime().block_on(async {
        tokio::time::timeout(
            Duration::from_secs(5),
            tokio::time::sleep(Duration::from_millis(1)),
        )
        .await
    });

    assert!(result.is_ok(), "Tokio timer did not complete");
}

#[test]
fn runtime_publishes_io_registration_to_the_reactor() {
    runtime().block_on(async {
        let listener = tokio::net::TcpListener::bind((Ipv4Addr::LOCALHOST, 0))
            .await
            .expect("failed to bind TCP listener");
        let address = listener
            .local_addr()
            .expect("listener has no local address");

        let client = tokio::spawn(async move {
            let stream = tokio::net::TcpStream::connect(address)
                .await
                .expect("failed to connect to TCP listener");
            loop {
                stream
                    .writable()
                    .await
                    .expect("TCP stream did not become writable");
                match stream.try_write(&[1]) {
                    Ok(1) => break,
                    Ok(written) => panic!("wrote {written} bytes instead of one"),
                    Err(error) if error.kind() == std::io::ErrorKind::WouldBlock => {}
                    Err(error) => panic!("failed to write to TCP stream: {error}"),
                }
            }
        });

        let (stream, _) = listener
            .accept()
            .await
            .expect("failed to accept connection");
        let mut byte = [0];
        loop {
            stream
                .readable()
                .await
                .expect("TCP stream did not become readable");
            match stream.try_read(&mut byte) {
                Ok(1) => break,
                Ok(read) => panic!("read {read} bytes instead of one"),
                Err(error) if error.kind() == std::io::ErrorKind::WouldBlock => {}
                Err(error) => panic!("failed to read from TCP stream: {error}"),
            }
        }
        assert_eq!(byte, [1]);

        client.await.expect("TCP client task panicked");
    });
}
