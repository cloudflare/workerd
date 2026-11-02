// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Connected UDP for the standalone network service. DNS and the configured peer filter
//! run before a socket is exposed to the Worker. One send/receive is one datagram.

use tokio::net::UdpSocket;

use super::network::PeerFilter;
use crate::Result;
use crate::bridge::ffi::UdpDatagram;

pub struct UdpEgress {
    socket: UdpSocket,
}

impl UdpEgress {
    pub async fn connect(authority: &str, filter: &PeerFilter) -> Result<Self> {
        let peers = tokio::net::lookup_host(authority)
            .await
            .map_err(|error| kj_hyper::io_kj_error(&error))?;
        for peer in peers {
            if !filter.allows(peer.ip()) {
                continue;
            }
            let local = if peer.is_ipv4() {
                "0.0.0.0:0"
            } else {
                "[::]:0"
            };
            let socket = UdpSocket::bind(local)
                .await
                .map_err(|error| kj_hyper::io_kj_error(&error))?;
            socket
                .connect(peer)
                .await
                .map_err(|error| kj_hyper::io_kj_error(&error))?;
            return Ok(Self { socket });
        }
        Err(kj::failed!(
            "jsg.Error: No permitted UDP destination address."
        ))
    }

    pub async fn udp_receive(&self) -> Result<UdpDatagram> {
        // Larger than any ordinary UDP payload, so a received packet cannot be silently cut.
        let mut data = vec![0; 65536];
        let size = self
            .socket
            .recv(&mut data)
            .await
            .map_err(|error| kj_hyper::io_kj_error(&error))?;
        data.truncate(size);
        Ok(UdpDatagram { ended: false, data })
    }

    pub async fn udp_send(&self, data: &[u8]) -> Result<()> {
        if data.len() > 65507 {
            kj::fail_require!("UDP payload exceeds 65507 bytes.");
        }
        let written = self
            .socket
            .send(data)
            .await
            .map_err(|error| kj_hyper::io_kj_error(&error))?;
        if written != data.len() {
            kj::fail_require!("Partial UDP send.");
        }
        Ok(())
    }
}
