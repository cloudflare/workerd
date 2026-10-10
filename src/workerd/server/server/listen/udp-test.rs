// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

#[test]
fn a_datagram_that_fills_the_buffer_is_dropped_as_truncated() {
    let runtime = tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .unwrap();
    runtime.block_on(async {
        let receiver = UdpSocket::bind("127.0.0.1:0").await.unwrap();
        let sender = UdpSocket::bind("127.0.0.1:0").await.unwrap();
        let to = receiver.local_addr().unwrap();
        sender.send_to(b"truncated", to).await.unwrap();
        sender.send_to(b"full", to).await.unwrap();
        sender.send_to(b"fit", to).await.unwrap();

        let mut buffer = [0u8; 4];
        assert_eq!(
            receive_datagram(&receiver, &mut buffer).await.unwrap(),
            None
        );
        assert_eq!(
            receive_datagram(&receiver, &mut buffer).await.unwrap(),
            None
        );
        let (len, peer) = receive_datagram(&receiver, &mut buffer)
            .await
            .unwrap()
            .unwrap();
        assert_eq!(&buffer[..len], b"fit");
        assert_eq!(peer, sender.local_addr().unwrap());
    });
}
