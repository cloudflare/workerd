// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The JSON bodies of the Docker Engine API calls the container client makes, and of the
//! egress sidecar's (`proxy-everything`) configuration endpoint.
//!
//! Only the fields the client reads or writes are declared. A request leaves out an unset
//! `Option`, so Docker applies its own default, but sends every number and flag of a container
//! create request's structs, zero or false unless set (see [`HostConfig`]). A response field
//! that is absent or `null` reads as `None`, and fields not declared here are ignored.

use std::collections::BTreeMap;

use serde::Deserialize;
use serde::Serialize;
use serde_json::Value;

// =======================================================================================
// Containers

#[expect(
    clippy::struct_excessive_bools,
    reason = "Docker's container create request has these flags"
)]
#[derive(Debug, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase", default)]
pub struct ContainerCreateRequest {
    pub attach_stdin: bool,
    pub attach_stdout: bool,
    pub attach_stderr: bool,
    pub tty: bool,
    pub open_stdin: bool,
    pub stdin_once: bool,
    pub network_disabled: bool,
    /// Seconds Docker waits for the container to stop before it kills it.
    pub stop_timeout: u32,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub image: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub cmd: Option<Vec<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub entrypoint: Option<Vec<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub env: Option<Vec<String>>,
    /// Labels as key-value pairs.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub labels: Option<BTreeMap<String, String>>,
    /// Ports mapped to empty objects, as `{"39001/tcp": {}}`.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub exposed_ports: Option<BTreeMap<String, Value>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub host_config: Option<HostConfig>,
}

impl Default for ContainerCreateRequest {
    fn default() -> Self {
        Self {
            attach_stdin: false,
            attach_stdout: false,
            attach_stderr: false,
            tty: false,
            open_stdin: false,
            stdin_once: false,
            network_disabled: false,
            stop_timeout: 10,
            image: None,
            cmd: None,
            entrypoint: None,
            env: None,
            labels: None,
            exposed_ports: None,
            host_config: None,
        }
    }
}

/// A container's host settings.
///
/// The resource settings go out as zero or false rather than being left to Docker's defaults.
/// Most of them mean the same either way, but `MemorySwappiness: 0` turns swapping off on a
/// cgroup v1 host, where Docker's default inherits the host's swappiness.
#[expect(
    clippy::struct_excessive_bools,
    reason = "Docker's HostConfig has these flags"
)]
#[derive(Debug, Default, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase", default)]
pub struct HostConfig {
    pub memory: u32,
    pub memory_swap: u32,
    pub memory_reservation: u32,
    pub kernel_memory: u32,
    pub nano_cpus: u32,
    pub cpu_percent: u32,
    pub cpu_shares: u32,
    pub cpu_period: u32,
    pub cpu_realtime_period: u32,
    pub cpu_realtime_runtime: u32,
    pub cpu_quota: u32,
    #[serde(rename = "MaximumIOps")]
    pub maximum_iops: u32,
    #[serde(rename = "MaximumIOBps")]
    pub maximum_iobps: u32,
    pub blkio_weight: u16,
    pub memory_swappiness: u32,
    pub oom_kill_disable: bool,
    pub oom_score_adj: i32,
    pub pids_limit: i32,
    pub publish_all_ports: bool,
    pub privileged: bool,
    pub readonly_rootfs: bool,
    pub auto_remove: bool,
    pub shm_size: u32,
    /// Volume bindings, as `volume:path[:ro]`.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub binds: Option<Vec<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub pid_mode: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub dns: Option<Vec<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub cap_add: Option<Vec<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub restart_policy: Option<RestartPolicy>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub network_mode: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub devices: Option<Vec<DeviceMapping>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub security_opt: Option<Vec<String>>,
    /// `--add-host` entries, as `host:ip`.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub extra_hosts: Option<Vec<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub mounts: Option<Vec<Mount>>,
}

/// The extra privileges a container's config grants it (`ContainerOptions.privileges`).
#[derive(Debug, Default, Clone)]
pub struct Privileges {
    pub capabilities: Vec<String>,
    pub devices: Vec<Device>,
    pub security_opt: Vec<String>,
}

#[derive(Debug, Default, Clone)]
pub struct Device {
    pub path_on_host: String,
    pub path_in_container: String,
    pub cgroup_permissions: String,
}

impl Privileges {
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.capabilities.is_empty() && self.devices.is_empty() && self.security_opt.is_empty()
    }
}

impl HostConfig {
    /// Grants the application container `privileges`. This is the only writer of `CapAdd`,
    /// `Devices` and `SecurityOpt` for that container, so it replaces rather than appends.
    pub fn grant(&mut self, privileges: &Privileges) {
        if !privileges.capabilities.is_empty() {
            self.cap_add = Some(privileges.capabilities.clone());
        }
        if !privileges.devices.is_empty() {
            self.devices = Some(
                privileges
                    .devices
                    .iter()
                    .map(|device| DeviceMapping {
                        path_on_host: device.path_on_host.clone(),
                        path_in_container: device.path_in_container.clone(),
                        cgroup_permissions: device.cgroup_permissions.clone(),
                    })
                    .collect(),
            );
        }
        if !privileges.security_opt.is_empty() {
            self.security_opt = Some(privileges.security_opt.clone());
        }
    }
}

#[derive(Debug, Default, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct RestartPolicy {
    /// `no`, `always`, `unless-stopped` or `on-failure`.
    pub name: String,
    #[serde(default)]
    pub maximum_retry_count: u32,
}

#[derive(Debug, Default, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct DeviceMapping {
    pub path_on_host: String,
    pub path_in_container: String,
    pub cgroup_permissions: String,
}

#[derive(Debug, Default, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct Mount {
    #[serde(rename = "Type")]
    pub kind: String,
    pub source: String,
    pub target: String,
    #[serde(default)]
    pub read_only: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub volume_options: Option<VolumeOptions>,
}

#[derive(Debug, Default, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct VolumeOptions {
    pub no_copy: bool,
}

#[derive(Debug, Default, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct ContainerCreateResponse {
    #[serde(default)]
    pub id: String,
}

/// The answer to `/containers/<id>/wait`.
#[derive(Debug, Default, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct ContainerWaitResponse {
    #[serde(default)]
    pub status_code: i32,
}

#[derive(Debug, Default, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct ContainerInspectResponse {
    pub id: Option<String>,
    pub args: Option<Vec<String>>,
    pub state: Option<ContainerState>,
    pub network_settings: Option<NetworkSettings>,
    pub config: Option<ContainerConfig>,
}

#[derive(Debug, Default, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct ContainerState {
    /// `created`, `running`, `paused`, `restarting`, `removing`, `exited` or `dead`.
    pub status: Option<String>,
    pub running: Option<bool>,
}

#[derive(Debug, Default, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct NetworkSettings {
    /// Each exposed port (`39001/tcp`) to the host bindings it is published on, or `null`.
    pub ports: Option<BTreeMap<String, Value>>,
}

#[derive(Debug, Default, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct ContainerConfig {
    pub image: Option<String>,
}

// =======================================================================================
// Exec

#[derive(Debug, Default, Serialize)]
#[serde(rename_all = "PascalCase")]
pub struct ExecCreateRequest {
    #[serde(skip_serializing_if = "Option::is_none")]
    pub attach_stdin: Option<bool>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub attach_stdout: Option<bool>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub attach_stderr: Option<bool>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub tty: Option<bool>,
    pub cmd: Vec<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub env: Option<Vec<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub working_dir: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub user: Option<String>,
}

#[derive(Debug, Default, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct ExecCreateResponse {
    #[serde(default)]
    pub id: String,
}

#[derive(Debug, Default, Serialize)]
#[serde(rename_all = "PascalCase")]
pub struct ExecStartRequest {
    #[serde(skip_serializing_if = "Option::is_none")]
    pub detach: Option<bool>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub tty: Option<bool>,
    /// `[height, width]`.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub console_size: Option<[u16; 2]>,
}

#[derive(Debug, Default, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct ExecInspectResponse {
    /// A number once the process has exited, else `null`.
    pub exit_code: Option<Value>,
    #[serde(default)]
    pub running: bool,
    #[serde(default)]
    pub pid: u32,
}

// =======================================================================================
// Images, volumes and networks

#[derive(Debug, Default, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct ImageInspectResponse {
    #[serde(default)]
    pub size: u64,
}

#[derive(Debug, Default, Serialize)]
#[serde(rename_all = "PascalCase")]
pub struct VolumeCreateRequest {
    pub name: String,
    pub labels: BTreeMap<String, String>,
}

/// The `filters` query parameter of `/volumes`.
#[derive(Debug, Default, Serialize)]
pub struct VolumeListFilters {
    pub name: Vec<String>,
}

#[derive(Debug, Default, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct VolumeListResponse {
    pub volumes: Option<Vec<Volume>>,
}

#[derive(Debug, Default, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct Volume {
    #[serde(default)]
    pub name: String,
    pub labels: Option<Value>,
}

#[derive(Debug, Default, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct NetworkInspectResponse {
    pub name: Option<String>,
    #[serde(rename = "IPAM")]
    pub ipam: Option<Ipam>,
}

#[derive(Debug, Default, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct Ipam {
    pub config: Option<Vec<IpamConfig>>,
}

#[derive(Debug, Default, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct IpamConfig {
    #[serde(default)]
    pub subnet: String,
    #[serde(default)]
    pub gateway: String,
}

// =======================================================================================
// proxy-everything

/// The body of the sidecar's `PUT /egress`.
#[derive(Debug, Default, Serialize)]
pub struct EgressConfig {
    pub port: u16,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub dns: Option<EgressDns>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub internet: Option<EgressInternet>,
}

#[derive(Debug, Default, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct EgressDns {
    pub allow_hostnames: Vec<String>,
}

/// General internet access; `enabled` is left out when it is unset.
#[derive(Debug, Default, Serialize)]
pub struct EgressInternet {
    #[serde(skip_serializing_if = "Option::is_none")]
    pub enabled: Option<bool>,
}

#[cfg(test)]
#[path = "api-test.rs"]
mod tests;
