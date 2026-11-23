// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! A port of the container (`getTcpPort()`).
//!
//! Each `connect()` is a CONNECT to the sidecar's ingress, which forwards it to the port inside
//! the container's namespace (`X-Dst-Addr`). The C++ shim pumps the tunnel to and from the
//! Worker's `ByteStream`s.

use std::rc::Rc;

use kj_rs::KjOwn;

use super::client::ContainerClient;
use super::client::js_error;
use super::docker::Upgrade;
use crate::Result;
use crate::bridge::ffi;

/// `rpc::Container::Port`. It keeps its client alive.
pub struct Port {
    client: Rc<ContainerClient>,
    port: u16,
}

impl ContainerClient {
    pub async fn tcp_port(&self, port: u16) -> Result<Port> {
        self.queued_turns().await;
        let Some(client) = self.rc() else {
            kj::fail_require!("container client dropped during getTcpPort()");
        };
        Ok(Port { client, port })
    }
}

impl Port {
    /// A tunnel to the port.
    pub async fn connect(&self) -> Result<KjOwn<ffi::AsyncIoStream>> {
        let Some(ingress) = self.client.ingress_port.get() else {
            return Err(js_error(
                "connect(): Container ingress proxy is not running.",
            ));
        };
        let destination = format!("127.0.0.1:{}", self.port);
        let sidecar = self.client.docker.at(format!("127.0.0.1:{ingress}"));
        match sidecar
            .connect(&destination, &[("X-Dst-Addr", &destination)])
            .await?
        {
            Upgrade::Hijacked(tunnel) => Ok(kj_hyper::into_kj_stream(tunnel)),
            Upgrade::Refused { status: 400, .. } => Err(kj::disconnected!(
                "jsg.Error: Container is not listening to port {}",
                self.port
            )),
            Upgrade::Refused {
                status,
                status_text,
                body,
            } => Err(js_error(format_args!(
                "Connecting to container port through proxy-everything failed: [{status}] \
                 {status_text} {body}"
            ))),
        }
    }
}
