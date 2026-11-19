// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::future::Future;
use std::time::Duration;

use super::*;

fn run(test: impl Future<Output = ()>) {
    tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .unwrap()
        .block_on(async {
            tokio::time::timeout(Duration::from_secs(5), test)
                .await
                .expect("UDP test timed out");
        });
}

async fn round_trip(address: &str) {
    let listener = UdpSocket::bind(address).await.unwrap();
    let filter = PeerFilter::new(["local"], []).unwrap();
    let channel = UdpEgress::connect(&listener.local_addr().unwrap().to_string(), &filter)
        .await
        .unwrap();
    // Some platforms default to send buffers smaller than the largest UDP payload. Size both
    // sockets for the boundary test without relying on the host's defaults.
    for socket in [&listener, &channel.socket] {
        socket2::SockRef::from(socket)
            .set_send_buffer_size(65_536)
            .unwrap();
    }
    let mut buffer = vec![0; 65_536];

    // Empty packets, binary data and a maximum-sized packet retain their boundaries in both
    // directions. In particular, an empty packet is data rather than EOF.
    for payload in [vec![], vec![0, 255, 32], vec![42; 1500], vec![123; 65_507]] {
        channel.udp_send(&payload).await.unwrap();
        let (size, peer) = listener.recv_from(&mut buffer).await.unwrap();
        assert_eq!(&buffer[..size], payload);
        assert!(peer.ip().is_loopback());
        listener.send_to(&payload, peer).await.unwrap();
        let received = channel.udp_receive().await.unwrap();
        assert!(!received.ended);
        assert_eq!(received.data, payload);
    }

    // A socket's selected destination is also its only permitted source of replies.
    let stranger = UdpSocket::bind(address).await.unwrap();
    let to = channel.socket.local_addr().unwrap();
    stranger.send_to(b"unsolicited", to).await.unwrap();
    listener.send_to(b"reply", to).await.unwrap();
    assert_eq!(channel.udp_receive().await.unwrap().data, b"reply");
}

#[test]
fn ipv4_preserves_datagrams_and_filters_reply_sources() {
    run(round_trip("127.0.0.1:0"));
}

#[test]
fn ipv6_preserves_datagrams_and_filters_reply_sources() {
    run(round_trip("[::1]:0"));
}

#[test]
fn oversized_payloads_are_rejected_without_sending_or_poisoning_the_transport() {
    run(async {
        let listener = UdpSocket::bind("127.0.0.1:0").await.unwrap();
        let filter = PeerFilter::new(["local"], []).unwrap();
        let channel = UdpEgress::connect(&listener.local_addr().unwrap().to_string(), &filter)
            .await
            .unwrap();
        for size in [65_508, 131_072] {
            let error = channel.udp_send(&vec![42; size]).await.unwrap_err();
            assert!(
                error
                    .description()
                    .contains("UDP payload exceeds 65507 bytes.")
            );
        }
        channel.udp_send(b"after rejection").await.unwrap();
        let mut buffer = [0; 32];
        let (size, _) = listener.recv_from(&mut buffer).await.unwrap();
        assert_eq!(&buffer[..size], b"after rejection");
        assert!(matches!(
            listener.try_recv_from(&mut buffer),
            Err(error) if error.kind() == std::io::ErrorKind::WouldBlock
        ));
    });
}

#[test]
fn destination_filter_is_checked_before_a_socket_is_exposed() {
    run(async {
        for filter in [
            PeerFilter::new([], []).unwrap(),
            PeerFilter::new(["public"], []).unwrap(),
            PeerFilter::new(["local"], ["127.0.0.1/32"]).unwrap(),
        ] {
            let error = UdpEgress::connect("127.0.0.1:8125", &filter)
                .await
                .err()
                .expect("a denied UDP destination must fail");
            assert_eq!(
                error.description(),
                "jsg.Error: No permitted UDP destination address."
            );
        }
    });
}

#[test]
fn invalid_authority_fails_before_a_socket_is_exposed() {
    run(async {
        let filter = PeerFilter::new(["local"], []).unwrap();
        assert!(UdpEgress::connect("127.0.0.1", &filter).await.is_err());
    });
}
