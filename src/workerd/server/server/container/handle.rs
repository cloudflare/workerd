// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The bridge's view of a client, a process and a port.
//!
//! The C++ shim (`factory/worker-factory-container.c++`) calls these with the parameters it
//! unpacked from Cap'n Proto. Each async method borrows its handle, which the shim keeps alive
//! while the call runs.

use std::rc::Rc;

use kj_rs::KjMaybe;
use kj_rs::KjOwn;

use super::client::ContainerClient;
use super::client::Source;
use super::client::StartParams;
use super::egress::Protocol;
use super::exec::ExecParams;
use super::exec::Process;
use super::port::Port;
use crate::Result;
use crate::bridge::ffi;

/// A container's client, as `rpc::Container`.
pub struct ContainerHandle(pub Rc<ContainerClient>);

fn labels(labels: &[ffi::ContainerLabel]) -> Vec<(String, String)> {
    labels
        .iter()
        .map(|label| (label.name.clone(), label.value.clone()))
        .collect()
}

fn to_labels(labels: Vec<(String, String)>) -> Vec<ffi::ContainerLabel> {
    labels
        .into_iter()
        .map(|(name, value)| ffi::ContainerLabel { name, value })
        .collect()
}

impl ContainerHandle {
    pub async fn status(&self) -> Result<bool> {
        self.0.status().await
    }

    pub async fn start(&self, params: &ffi::ContainerStart) -> Result<()> {
        let source = match params.source {
            ffi::ContainerSource::Image => Source::Image(params.source_text.clone()),
            ffi::ContainerSource::Snapshot => Source::ContainerSnapshot(params.source_text.clone()),
            _ => Source::Default,
        };
        self.0
            .start(StartParams {
                entrypoint: params.has_entrypoint.then(|| params.entrypoint.clone()),
                environment: params.has_environment.then(|| params.environment.clone()),
                enable_internet: params.enable_internet,
                labels: labels(&params.labels),
                directory_snapshots: params
                    .directory_snapshots
                    .iter()
                    .map(|s| (s.snapshot_id.clone(), s.restore_path.clone()))
                    .collect(),
                source,
                pid_namespace: params.pid_namespace,
            })
            .await
    }

    pub async fn monitor(&self) -> Result<i32> {
        self.0.monitor().await
    }

    pub async fn destroy(&self) -> Result<()> {
        self.0.destroy().await
    }

    pub async fn signal(&self, signo: u32) -> Result<()> {
        self.0.signal(signo).await
    }

    pub async fn exec(
        &self,
        params: &ffi::ContainerExec,
        out: KjMaybe<KjOwn<ffi::ExecOutput>>,
        err: KjMaybe<KjOwn<ffi::ExecOutput>>,
    ) -> Result<Box<ContainerProcess>> {
        let process = self
            .0
            .exec(
                ExecParams {
                    cmd: params.cmd.clone(),
                    env: params.has_env.then(|| params.env.clone()),
                    working_directory: params
                        .has_working_directory
                        .then(|| params.working_directory.clone()),
                    user: params.has_user.then(|| params.user.clone()),
                    combined_output: params.combined_output,
                    pty: params.pty.then_some((params.pty_cols, params.pty_rows)),
                },
                out.into(),
                err.into(),
            )
            .await?;
        Ok(Box::new(ContainerProcess(process)))
    }

    pub async fn tcp_port(&self, port: u16) -> Result<Box<ContainerPort>> {
        Ok(Box::new(ContainerPort(self.0.tcp_port(port).await?)))
    }

    pub async fn set_inactivity_timeout(&self, duration_ms: i64) -> Result<()> {
        self.0.set_inactivity_timeout(duration_ms).await
    }

    pub async fn set_egress(
        &self,
        protocol: ffi::EgressProtocol,
        host_port: &str,
        token: &[u8],
    ) -> Result<()> {
        let protocol = match protocol {
            ffi::EgressProtocol::Https => Protocol::Https,
            ffi::EgressProtocol::Tcp => Protocol::Tcp,
            _ => Protocol::Http,
        };
        self.0.set_egress(protocol, host_port, token).await
    }

    pub async fn snapshot_directory(&self, dir: &str) -> Result<ffi::ContainerDirectorySnapshot> {
        let snapshot = self.0.snapshot_directory(dir).await?;
        Ok(ffi::ContainerDirectorySnapshot {
            id: snapshot.id,
            size: snapshot.size,
            dir: snapshot.dir,
        })
    }

    pub async fn snapshot_container(&self) -> Result<ffi::ContainerImageSnapshot> {
        let snapshot = self.0.snapshot_container().await?;
        Ok(ffi::ContainerImageSnapshot {
            id: snapshot.id,
            size: snapshot.size,
        })
    }

    pub async fn inspect(&self) -> Result<ffi::ContainerInspect> {
        Ok(match self.0.inspect().await? {
            Some(inspected) => ffi::ContainerInspect {
                started: true,
                labels: to_labels(inspected.labels),
                image: inspected.image,
            },
            None => ffi::ContainerInspect {
                started: false,
                labels: Vec::new(),
                image: String::new(),
            },
        })
    }

    pub async fn set_labels(&self, labels: &[ffi::ContainerLabel]) -> Result<()> {
        self.0.set_labels(self::labels(labels)).await
    }
}

/// A process, as `rpc::Container::ProcessHandle`.
pub struct ContainerProcess(Process);

impl ContainerProcess {
    pub fn pid(&self) -> i32 {
        self.0.pid()
    }

    pub async fn wait(&self) -> Result<i32> {
        self.0.wait().await
    }

    pub fn stdin_writer(&self) -> Result<KjOwn<ffi::AsyncIoStream>> {
        self.0.stdin_writer()
    }

    pub async fn kill(&self, signo: u32) -> Result<()> {
        self.0.kill(signo).await
    }

    pub async fn resize(&self, cols: u16, rows: u16) -> Result<()> {
        self.0.resize(cols, rows).await
    }
}

/// A port, as `rpc::Container::Port`.
pub struct ContainerPort(Port);

impl ContainerPort {
    pub async fn connect(&self) -> Result<KjOwn<ffi::AsyncIoStream>> {
        self.0.connect().await
    }
}
