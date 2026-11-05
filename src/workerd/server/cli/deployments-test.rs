// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::io::Read;
use std::io::Write;
use std::net::TcpListener;

use workerd_capnp::config;

use super::Failure;
use super::add;
use crate::config::Config;

/// A config of one service and, given `endpoint`, a cluster whose bucket is there.
fn node(endpoint: Option<&str>) -> Config {
    let mut message = capnp::message::Builder::new_default();
    let mut root = message.init_root::<config::Builder<'_>>();
    let mut service = root.reborrow().init_services(1).get(0);
    service.set_name("main");
    service.init_network();
    if let Some(endpoint) = endpoint {
        let mut cluster = root.init_cluster();
        cluster.set_bucket("s3://bucket/prefix");
        let mut options = cluster.init_bucket_options(3);
        options.set(0, format!("aws_endpoint={endpoint}"));
        options.set(1, "aws_access_key_id=key");
        options.set(2, "aws_secret_access_key=secret");
    }
    Config::from_message_bytes(&capnp::serialize::write_message_to_words(&message)).unwrap()
}

#[test]
fn without_a_cluster_the_config_is_unchanged() {
    let config = node(None);
    let bytes = config.to_bytes();
    assert_eq!(add(config, None, None).unwrap().0.to_bytes(), bytes);
}

#[test]
fn without_a_catalog_the_config_is_unchanged() {
    // A bucket that holds nothing.
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let endpoint = format!("http://{}", listener.local_addr().unwrap());
    let bucket = std::thread::spawn(move || {
        let (mut stream, _) = listener.accept().unwrap();
        let mut request = [0; 4096];
        let _ = stream.read(&mut request).unwrap();
        let response = "HTTP/1.1 404 Not Found\r\ncontent-length: 0\r\nconnection: close\r\n\r\n";
        stream.write_all(response.as_bytes()).unwrap();
        String::from_utf8_lossy(&request).into_owned()
    });
    let config = node(Some(&endpoint));
    let bytes = config.to_bytes();
    let (config, deployments) = add(config, None, None).unwrap();
    assert_eq!(config.to_bytes(), bytes);
    // The node's references to services it does not define are still deployment references.
    assert!(deployments.enabled);
    let request = bucket.join().unwrap();
    assert!(
        request.starts_with("GET ") && request.contains("/prefix/v1/deploy/catalog.json "),
        "{request}"
    );
}

#[test]
fn a_bucket_that_cannot_be_read_is_not_a_config_error() {
    // A port nothing listens on.
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let endpoint = format!("http://{}", listener.local_addr().unwrap());
    drop(listener);
    match add(node(Some(&endpoint)), None, None) {
        Err(Failure::Remote(errors)) => assert!(
            errors[0].starts_with("cluster: could not load deployments from s3://bucket/prefix: "),
            "{errors:?}"
        ),
        other => panic!("expected a remote failure, got {:?}", other.map(|_| ())),
    }
}

#[test]
fn a_public_cluster_address_is_refused_before_the_bucket_is_read() {
    // A port nothing listens on: reading the bucket would fail differently.
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let endpoint = format!("http://{}", listener.local_addr().unwrap());
    drop(listener);
    for (address, advertise) in [(Some("8.8.8.8:7000"), None), (None, Some("8.8.8.8:7000"))] {
        match add(node(Some(&endpoint)), address, advertise) {
            Err(Failure::Config(errors)) => assert!(
                errors[0].starts_with("cluster address 8.8.8.8 is public"),
                "{errors:?}"
            ),
            other => panic!("expected a config failure, got {:?}", other.map(|_| ())),
        }
    }
}

#[test]
fn a_wildcard_cluster_address_without_an_advertised_one_is_refused_before_the_bucket_is_read() {
    // A port nothing listens on: reading the bucket would fail differently.
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let endpoint = format!("http://{}", listener.local_addr().unwrap());
    drop(listener);
    for address in ["0.0.0.0:7000", "[::]:7000"] {
        match add(node(Some(&endpoint)), Some(address), None) {
            Err(Failure::Config(errors)) => assert!(
                errors[0].starts_with(&format!("cluster address {address} is a wildcard")),
                "{errors:?}"
            ),
            other => panic!("expected a config failure, got {:?}", other.map(|_| ())),
        }
    }
    // With an address to advertise, the bucket is read.
    match add(
        node(Some(&endpoint)),
        Some("0.0.0.0:7000"),
        Some("10.0.0.1:7000"),
    ) {
        Err(Failure::Remote(_)) => {}
        other => panic!("expected a remote failure, got {:?}", other.map(|_| ())),
    }
}
