// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The Docker client of one Durable Object's container: the methods of `rpc::Container`
//! (io/container.capnp), which the C++ shim in `factory/worker-factory-container.c++` forwards
//! here.
//!
//! The container runs in the network namespace of a sidecar (`<name>-proxy`, the configured
//! `containerEgressInterceptorImage`), which publishes the container's ports on the host through
//! its ingress port and sends the container's outbound connections to the egress listener
//! ([`super::egress`]).
//!
//! Methods that change the container take turns, in the order they were called; `monitor()` and
//! `getTcpPort()` wait for the turns already queued without taking one. A client made while
//! Docker is still removing its predecessor's container (`pending_cleanup`) waits for that
//! before its first `status()`.
//!
//! Errors a Worker sees are `jsg.Error`s, whose text the tests match.

use std::cell::Cell;
use std::cell::RefCell;
use std::fmt::Display;
use std::fmt::Write as _;
use std::future::Future;
use std::pin::pin;
use std::rc::Rc;
use std::rc::Weak;
use std::sync::Arc;
use std::sync::atomic::AtomicBool;
use std::sync::atomic::Ordering;
use std::time::Duration;
use std::time::SystemTime;
use std::time::UNIX_EPOCH;

use futures::FutureExt;
use futures::future::Either;
use futures::future::LocalBoxFuture;
use futures::future::Shared;
use futures::future::join_all;
use futures::future::select;
use http::Method;
use percent_encoding::AsciiSet;
use percent_encoding::NON_ALPHANUMERIC;
use percent_encoding::utf8_percent_encode;
use serde_json::Value;

use super::api;
use super::docker::Api;
use super::docker::Body;
use super::docker::MAX_JSON_RESPONSE_SIZE;
use super::egress;
use super::egress::Egress;
use super::egress::Protocol;
use super::tar::tar_with_file;
use crate::Error;
use crate::Result;
use crate::bridge::ffi;
use crate::config::Factory;

// =======================================================================================
// Constants

/// The port the sidecar's ingress listens on inside its namespace, published on the host.
pub const SIDECAR_INGRESS_PORT: u16 = 39001;
const SIDECAR_DNS_SERVERS: [&str; 2] = ["1.1.1.1", "8.8.8.8"];

pub const SNAPSHOT_VOLUME_PREFIX: &str = "workerd-snap-";
const SNAPSHOT_CLONE_VOLUME_PREFIX: &str = "workerd-snap-clone-";
pub const CONTAINER_SNAPSHOT_IMAGE_PREFIX: &str = "workerd-container-snap-";
const SNAPSHOT_VOLUME_CREATED_AT_LABEL: &str = "dev.workerd.snapshot-created-at";
/// Labels a container with its session, if there is one, so the session's supervisor can find
/// it.
const SESSION_LABEL: &str = "dev.workerd.session";

/// Images a Worker can name by alias, pulled on first use.
const CONTAINER_IMAGE_ALIASES: [(&str, &str); 1] = [(
    "cloudflare/debian-trixie",
    "docker.io/library/node:24.20.0-trixie-slim@\
     sha256:a747ad80c8a161b650d79a6da9c422005b91148b18b8d2c669eb5a0b7c07e600",
)];

const SNAPSHOT_STALE_AGE: Duration = Duration::from_hours(30 * 24);
/// The most a snapshot's tar archive may hold in memory while it is created.
const MAX_SNAPSHOT_TAR_SIZE: usize = 1024 * 1024 * 1024;

/// What the container sees of where it runs, as in production but made up.
const DEFAULT_ENV: [&str; 6] = [
    "CLOUDFLARE_COUNTRY_A2=XX",
    "CLOUDFLARE_DEPLOYMENT_ID=xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx",
    "CLOUDFLARE_LOCATION=loc01",
    "CLOUDFLARE_REGION=REGN",
    "CLOUDFLARE_APPLICATION_ID=xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx",
    "CLOUDFLARE_DURABLE_OBJECT_ID=\
     xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
];

const CLOUDFLARE_CA_DIR: &str = "/etc";
const CLOUDFLARE_CA_FILENAME: &str = "cloudflare/certs/cloudflare-containers-ca.crt";

/// Docker's polling of an exec's state: how often, and how many times before giving up.
pub const EXEC_POLL_INTERVAL: Duration = Duration::from_millis(50);
pub const EXEC_POLL_ATTEMPTS: usize = 20;

/// Whether this process has looked for stale snapshot volumes, which it does once.
static STALE_SNAPSHOT_VOLUME_CHECK_SCHEDULED: AtomicBool = AtomicBool::new(false);

// =======================================================================================
// Helpers

/// An error a Worker sees as an `Error` with `message`.
pub fn js_error(message: impl Display) -> Error {
    kj::failed!("jsg.Error: {message}")
}

/// The characters `encodeURIComponent()` leaves alone.
const URI_COMPONENT: &AsciiSet = &NON_ALPHANUMERIC
    .remove(b'-')
    .remove(b'_')
    .remove(b'.')
    .remove(b'!')
    .remove(b'~')
    .remove(b'*')
    .remove(b'\'')
    .remove(b'(')
    .remove(b')');

/// `text` as `encodeURIComponent()` encodes it.
#[must_use]
pub fn encode_uri_component(text: &str) -> String {
    utf8_percent_encode(text, URI_COMPONENT).to_string()
}

/// The image an alias names, else `image`.
#[must_use]
pub fn resolve_image(image: &str) -> &str {
    CONTAINER_IMAGE_ALIASES
        .iter()
        .find(|(alias, _)| *alias == image)
        .map_or(image, |(_, resolved)| resolved)
}

/// The alias of an image, else `image`.
#[must_use]
pub fn image_alias(image: &str) -> &str {
    CONTAINER_IMAGE_ALIASES
        .iter()
        .find(|(_, resolved)| *resolved == image)
        .map_or(image, |(alias, _)| alias)
}

/// The name Docker's kill endpoint takes for signal `signo`; unknown numbers kill.
#[must_use]
pub fn signal_name(signo: u32) -> &'static str {
    const NAMES: [&str; 31] = [
        "SIGHUP",
        "SIGINT",
        "SIGQUIT",
        "SIGILL",
        "SIGTRAP",
        "SIGABRT",
        "SIGBUS",
        "SIGFPE",
        "SIGKILL",
        "SIGUSR1",
        "SIGSEGV",
        "SIGUSR2",
        "SIGPIPE",
        "SIGALRM",
        "SIGTERM",
        "SIGSTKFLT",
        "SIGCHLD",
        "SIGCONT",
        "SIGSTOP",
        "SIGTSTP",
        "SIGTTIN",
        "SIGTTOU",
        "SIGURG",
        "SIGXCPU",
        "SIGXFSZ",
        "SIGVTALRM",
        "SIGPROF",
        "SIGWINCH",
        "SIGIO",
        "SIGPWR",
        "SIGSYS",
    ];
    usize::try_from(signo)
        .ok()
        .and_then(|n| n.checked_sub(1))
        .and_then(|i| NAMES.get(i))
        .copied()
        .unwrap_or("SIGKILL")
}

/// A random version 4 UUID, in its canonical form.
#[must_use]
pub fn random_uuid() -> String {
    let mut bytes: [u8; 16] = rand::random();
    bytes[6] = (bytes[6] & 0x0f) | 0x40;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;
    let hex = bytes.iter().fold(String::with_capacity(32), |mut hex, b| {
        let _ = write!(hex, "{b:02x}");
        hex
    });
    format!(
        "{}-{}-{}-{}-{}",
        &hex[0..8],
        &hex[8..12],
        &hex[12..16],
        &hex[16..20],
        &hex[20..32]
    )
}

/// A snapshot id, which must be a UUID in canonical (lowercase) form.
///
/// # Errors
///
/// Anything else.
pub fn parse_snapshot_id(id: &str) -> Result<String> {
    let bytes = id.as_bytes();
    let canonical = bytes.len() == 36
        && bytes.iter().enumerate().all(|(i, &b)| match i {
            8 | 13 | 18 | 23 => b == b'-',
            _ => b.is_ascii_digit() || (b'a'..=b'f').contains(&b),
        })
        && bytes.iter().any(|&b| b != b'0' && b != b'-');
    if !canonical {
        return Err(js_error(format_args!("Invalid snapshot ID{id}")));
    }
    Ok(id.to_owned())
}

/// An absolute path for a snapshot, normalized as `kj::Path` evaluates one: `.` and empty
/// components dropped, `..` removing the one before.
///
/// # Errors
///
/// A relative path, a NUL, or a `..` above the root.
pub fn parse_absolute_path(path: &str) -> Result<String> {
    let Some(relative) = path.strip_prefix('/') else {
        return Err(js_error(format_args!(
            "Snapshot path must be absolute, got: {path}"
        )));
    };
    if path.contains('\0') {
        return Err(js_error("Snapshot path must not contain null bytes"));
    }
    let invalid = |why: String| {
        js_error(format_args!(
            "Snapshot path contains invalid components: {path}; {why}"
        ))
    };
    if relative.starts_with('/') {
        return Err(invalid(format!(
            "expected !path.startsWith(\"/\"); expected a relative path, got absolute; \
             path = {relative}"
        )));
    }
    let mut parts: Vec<&str> = Vec::new();
    for part in relative.split('/') {
        match part {
            "" | "." => {}
            ".." => {
                if parts.pop().is_none() {
                    return Err(invalid(
                        "expected parts.size() > 0; can't use \"..\" to break out of starting \
                         directory"
                            .to_owned(),
                    ));
                }
            }
            part => parts.push(part),
        }
    }
    Ok(format!("/{}", parts.join("/")))
}

/// The host port published for a container port, from the value of its `Ports` entry.
fn published_host_port(value: &Value) -> Result<Option<u16>> {
    if value.is_null() {
        return Ok(None);
    }
    let Some(bindings) = value.as_array() else {
        return Err(js_error("Malformed ContainerInspect port mapping response"));
    };
    let Some(binding) = bindings.first() else {
        return Ok(None);
    };
    let Some(binding) = binding.as_object() else {
        return Err(js_error("Malformed ContainerInspect port binding response"));
    };
    let Some(port) = binding.get("HostPort") else {
        kj::fail_require!("Malformed ContainerInspect port binding response: missing HostPort");
    };
    let Some(port) = port.as_str() else {
        return Err(js_error("Malformed ContainerInspect port binding response"));
    };
    match port.parse::<u16>() {
        Ok(port) => Ok(Some(port)),
        Err(_) => kj::fail_require!("Malformed ContainerInspect host port"),
    }
}

/// The time a snapshot volume's label says it was created, in Unix seconds.
fn snapshot_created_at(labels: Option<&Value>) -> Option<i64> {
    labels?
        .as_object()?
        .get(SNAPSHOT_VOLUME_CREATED_AT_LABEL)?
        .as_str()?
        .parse()
        .ok()
}

fn unix_seconds_now() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |since| since.as_secs())
}

/// Runs `cleanup` on drop unless disarmed, so on failure and cancellation alike.
pub struct Defer<F: FnOnce()>(Option<F>);

impl<F: FnOnce()> Defer<F> {
    pub fn new(cleanup: F) -> Self {
        Self(Some(cleanup))
    }

    pub fn disarm(&mut self) {
        self.0 = None;
    }
}

impl<F: FnOnce()> Drop for Defer<F> {
    fn drop(&mut self) {
        if let Some(cleanup) = self.0.take() {
            cleanup();
        }
    }
}

/// Spawns `task` on the factory, logging how it fails, as the factory's task set does.
pub fn spawn_logged(factory: &Factory, task: impl Future<Output = Result<()>> + 'static) {
    factory.spawn_detached(async move {
        if let Err(e) = task.await {
            tracing::error!("{}", e.description());
        }
    });
}

// =======================================================================================
// Docker operations that outlive a client

/// Removes a container and waits for Docker to have removed it.
pub async fn remove_container(docker: &Api, name: &str) -> Result<()> {
    let response = docker
        .json(
            Method::DELETE,
            &format!("/containers/{name}?force=true"),
            None,
        )
        .await?;
    // 404: already gone. 409: removal is in progress, which is fine for teardown.
    if !matches!(response.status, 204 | 404 | 409) {
        kj::fail_require!("Removing a container failed with: {}", response.text());
    }
    if matches!(response.status, 204 | 409) {
        let response = docker
            .json(
                Method::POST,
                &format!("/containers/{name}/wait?condition=removed"),
                None,
            )
            .await?;
        // 404: the container went before the wait was processed.
        if !matches!(response.status, 200 | 404) {
            kj::fail_require!(
                "Waiting for container removal failed with: {}{}",
                response.status,
                response.text()
            );
        }
    }
    Ok(())
}

async fn delete_volume_logged(docker: &Api, name: &str) -> Result<()> {
    let response = docker
        .json(Method::DELETE, &format!("/volumes/{name}"), None)
        .await?;
    if !matches!(response.status, 204 | 404) {
        tracing::warn!(
            "failed to delete volume; volumeName = {name}; status = {}; body = {}",
            response.status,
            response.text()
        );
    }
    Ok(())
}

/// Deletes snapshot clone volumes, logging those that fail.
async fn delete_volumes(docker: &Api, volumes: Vec<String>) {
    join_all(volumes.iter().map(|name| async move {
        if let Err(e) = delete_volume_logged(docker, name).await {
            tracing::warn!(
                "failed to delete volume; volumeName = {name}; e = {}",
                e.description()
            );
        }
    }))
    .await;
}

async fn warn_about_stale_snapshot_volumes(docker: Api) -> Result<()> {
    let filters = api::VolumeListFilters {
        name: vec![SNAPSHOT_VOLUME_PREFIX.to_owned()],
    };
    let filters = serde_json::to_string(&filters).map_err(|e| kj::failed!("{e}"))?;
    let response = docker
        .get(&format!(
            "/volumes?filters={}",
            encode_uri_component(&filters)
        ))
        .await?;
    if response.status != 200 {
        return Ok(());
    }
    let list: api::VolumeListResponse = response.json()?;
    let now = unix_seconds_now();
    let stale: Vec<String> = list
        .volumes
        .unwrap_or_default()
        .into_iter()
        .filter(|volume| {
            snapshot_created_at(volume.labels.as_ref()).is_some_and(|created| {
                let age = i128::from(now) - i128::from(created);
                age >= i128::from(SNAPSHOT_STALE_AGE.as_secs())
            })
        })
        .map(|volume| volume.name)
        .collect();
    if !stale.is_empty() {
        tracing::warn!(
            "the following snapshot volumes were created 30+ days ago and may be stale; {}",
            stale.join(", ")
        );
    }
    Ok(())
}

// =======================================================================================
// The client

/// What a client is made of, from the worker's `containerEngine` and the class's `container`
/// options.
pub struct ClientConfig {
    pub docker_path: String,
    /// The registry's key: `workerd-[<session>-]<uniqueKey>-<actorId>`.
    pub id: String,
    pub image: Option<String>,
    pub egress_interceptor_image: String,
    pub session_id: Option<String>,
    pub privileges: api::Privileges,
}

/// What `start()` is given.
pub struct StartParams {
    pub entrypoint: Option<Vec<String>>,
    pub environment: Option<Vec<String>>,
    pub enable_internet: bool,
    pub labels: Vec<(String, String)>,
    pub directory_snapshots: Vec<(String, String)>,
    pub source: Source,
    /// The `containers_pid_namespace` compatibility flag.
    pub pid_namespace: bool,
}

/// Where `start()` takes the container's image from.
pub enum Source {
    /// The config's image: the `containerSnapshotId` member of the union, unset.
    Default,
    Image(String),
    ContainerSnapshot(String),
}

/// A directory snapshot.
pub struct DirectorySnapshot {
    pub id: String,
    pub size: u64,
    pub dir: String,
}

/// A container snapshot.
pub struct ContainerSnapshot {
    pub id: String,
    pub size: u64,
}

/// What `inspect()` answers for a running container.
pub struct Inspected {
    pub labels: Vec<(String, String)>,
    pub image: String,
}

struct InspectResponse {
    running: bool,
    image: String,
}

struct RestoreMount {
    restore_path: String,
    source_volume: String,
    clone_volume: String,
}

pub struct ContainerClient {
    factory: Rc<Factory>,
    this: Weak<Self>,
    pub(super) docker: Api,
    /// The registry's key.
    id: String,
    /// URI-encoded, as every endpoint takes it.
    pub(super) name: String,
    sidecar_name: String,
    image: Option<String>,
    egress_interceptor_image: String,
    session_id: Option<String>,
    privileges: api::Privileges,
    /// Docker removing the container of the client before this one.
    pending_cleanup: Shared<LocalBoxFuture<'static, ()>>,
    /// Shared only so that a turn can be moved into a task (`OwnedMutexGuard`).
    turns: Arc<tokio::sync::Mutex<()>>,
    egress: Rc<Egress>,
    started: Cell<bool>,
    sidecar_started: Cell<bool>,
    listener_started: Cell<bool>,
    ca_cert_injected: Cell<bool>,
    /// In the order they were set.
    labels: RefCell<Vec<(String, String)>>,
    snapshot_clones: RefCell<Vec<String>>,
    ca_cert: RefCell<Option<String>>,
    listener: RefCell<Option<egress::Listener>>,
    egress_port: Cell<u16>,
    /// The sidecar's ingress port, published on the host.
    pub(super) ingress_port: Cell<Option<u16>>,
    shutdown_started: Cell<bool>,
}

impl ContainerClient {
    #[must_use]
    pub fn new(
        factory: &Rc<Factory>,
        config: ClientConfig,
        pending_cleanup: Shared<LocalBoxFuture<'static, ()>>,
    ) -> Rc<Self> {
        let docker = Api::new(config.docker_path, factory.loopback());
        if !STALE_SNAPSHOT_VOLUME_CHECK_SCHEDULED.swap(true, Ordering::Relaxed) {
            let docker = docker.clone();
            factory.spawn_detached(async move {
                if let Err(e) = warn_about_stale_snapshot_volumes(docker).await {
                    tracing::warn!(
                        "failed to inspect snapshot volumes for staleness; e = {}",
                        e.description()
                    );
                }
            });
        }
        Rc::new_cyclic(|this| Self {
            factory: Rc::clone(factory),
            this: Weak::clone(this),
            docker,
            name: encode_uri_component(&config.id),
            sidecar_name: encode_uri_component(&format!("{}-proxy", config.id)),
            id: config.id,
            image: config.image,
            egress_interceptor_image: config.egress_interceptor_image,
            session_id: config.session_id,
            privileges: config.privileges,
            pending_cleanup,
            turns: Arc::new(tokio::sync::Mutex::new(())),
            egress: Rc::default(),
            started: Cell::new(false),
            sidecar_started: Cell::new(false),
            listener_started: Cell::new(false),
            ca_cert_injected: Cell::new(false),
            labels: RefCell::default(),
            snapshot_clones: RefCell::default(),
            ca_cert: RefCell::default(),
            listener: RefCell::default(),
            egress_port: Cell::new(0),
            ingress_port: Cell::new(None),
            shutdown_started: Cell::new(false),
        })
    }

    pub(super) fn factory(&self) -> &Rc<Factory> {
        &self.factory
    }

    /// Another reference to this client, for work that keeps it alive.
    pub(super) fn rc(&self) -> Option<Rc<Self>> {
        self.this.upgrade()
    }

    /// Spawns `task` with a reference to this client, logging how it fails.
    fn spawn_with_self<F, Fut>(&self, task: F)
    where
        F: FnOnce(Rc<Self>) -> Fut,
        Fut: Future<Output = Result<()>> + 'static,
    {
        if let Some(this) = self.rc() {
            spawn_logged(&self.factory, task(this));
        }
    }

    /// Waits for the turns queued before this one, then holds the turn until it is dropped.
    pub(super) async fn turn(&self) -> tokio::sync::OwnedMutexGuard<()> {
        Arc::clone(&self.turns).lock_owned().await
    }

    /// Waits for the turns queued so far, without taking one.
    pub(super) async fn queued_turns(&self) {
        drop(self.turns.lock().await);
    }

    /// Stops the egress listener and removes the container, its clone volumes and its sidecar,
    /// in that order, in the background; the registry keeps the cleanup for the client after
    /// this one to wait for. Once.
    pub fn shutdown(&self) {
        if self.shutdown_started.replace(true) {
            return;
        }
        self.stop_egress_listener();
        let docker = self.docker.clone();
        let name = self.name.clone();
        let sidecar_name = self.sidecar_name.clone();
        let volumes = self.snapshot_clones.take();
        let cleanup = async move {
            let _ = remove_container(&docker, &name).await;
            delete_volumes(&docker, volumes).await;
            let _ = remove_container(&docker, &sidecar_name).await;
        }
        .boxed_local();
        let cleanup = self.factory.containers().cleanup_started(&self.id, cleanup);
        self.factory.spawn_detached(cleanup);
    }

    // -------------------------------------------------------------------------------------
    // Docker

    async fn inspect_container(&self) -> Result<Option<InspectResponse>> {
        let response = self
            .docker
            .get(&format!("/containers/{}/json", self.name))
            .await?;
        if response.status == 404 {
            return Ok(None);
        }
        if response.status != 200 {
            return Err(js_error("Container inspect failed"));
        }
        let root: api::ContainerInspectResponse = response.json()?;
        let status = root
            .state
            .and_then(|state| state.status)
            .ok_or_else(|| js_error("Malformed ContainerInspect response"))?;
        // A restarting container (its restart policy) is still alive to the Worker: start()
        // refuses to start another and destroy() cleans it up.
        let running = status == "running" || status == "restarting";
        let image = root
            .config
            .and_then(|config| config.image)
            .map(|image| image_alias(&image).to_owned())
            .unwrap_or_default();
        Ok(Some(InspectResponse { running, image }))
    }

    /// The sidecar's published ingress port, if it is running.
    async fn inspect_sidecar(&self) -> Result<Option<u16>> {
        let response = self
            .docker
            .get(&format!("/containers/{}/json", self.sidecar_name))
            .await?;
        if response.status == 404 {
            return Ok(None);
        }
        if response.status != 200 {
            return Err(js_error("Sidecar container inspect failed"));
        }
        let root: api::ContainerInspectResponse = response.json()?;
        let running = root
            .state
            .and_then(|state| state.status)
            .is_some_and(|status| status == "running" || status == "restarting");
        if !running {
            return Ok(None);
        }
        let key = format!("{SIDECAR_INGRESS_PORT}/tcp");
        let port = match root
            .network_settings
            .and_then(|settings| settings.ports)
            .and_then(|mut ports| ports.remove(&key))
        {
            Some(value) => published_host_port(&value)?,
            None => None,
        };
        match port {
            Some(port) => Ok(Some(port)),
            None => kj::fail_require!("running sidecar missing ingress host port"),
        }
    }

    /// The bridge network's first IPAM gateway and subnet.
    async fn bridge_ipam(&self) -> Result<api::IpamConfig> {
        let response = self.docker.get("/networks/bridge").await?;
        if response.status == 200 {
            let root: api::NetworkInspectResponse = response.json()?;
            if let Some(config) = root
                .ipam
                .and_then(|ipam| ipam.config)
                .and_then(|configs| configs.into_iter().next())
            {
                return Ok(config);
            }
        }
        Err(js_error(format_args!(
            "Failed to get bridge. Status: {}, Body: {}",
            response.status,
            response.text()
        )))
    }

    /// Whether the daemon gives the bridge an IPv6 subnet (`"ipv6": true` in daemon.json).
    async fn daemon_ipv6_enabled(&self) -> Result<bool> {
        let response = self.docker.get("/networks/bridge").await?;
        if response.status != 200 {
            return Ok(false);
        }
        let root: api::NetworkInspectResponse = response.json()?;
        Ok(root
            .ipam
            .and_then(|ipam| ipam.config)
            .unwrap_or_default()
            .iter()
            .any(|config| config.subnet.contains(':')))
    }

    /// The sidecar's API, on its ingress port.
    fn sidecar(&self, port: u16) -> Api {
        self.docker.at(format!("127.0.0.1:{port}"))
    }

    async fn update_sidecar_egress(
        &self,
        ingress_port: u16,
        config: &api::EgressConfig,
    ) -> Result<String> {
        let body = serde_json::to_string(config).map_err(|e| kj::failed!("{e}"))?;
        let response = self
            .sidecar(ingress_port)
            .json(Method::PUT, "/egress", Some(body))
            .await?;
        if (200..300).contains(&response.status) {
            return Ok(String::new());
        }
        Ok(format!("{} {}", response.status, response.text()))
    }

    async fn update_sidecar_egress_port(&self, ingress_port: u16) -> Result<()> {
        let config = api::EgressConfig {
            port: self.egress_port.get(),
            ..api::EgressConfig::default()
        };
        let failure = self.update_sidecar_egress(ingress_port, &config).await?;
        if !failure.is_empty() {
            return Err(js_error(format_args!(
                "Updating sidecar egress port failed with: {failure}"
            )));
        }
        Ok(())
    }

    async fn update_sidecar_egress_config(&self, ingress_port: u16) -> Result<()> {
        let config = api::EgressConfig {
            port: self.egress_port.get(),
            dns: Some(api::EgressDns {
                allow_hostnames: self.egress.mappings.borrow().dns_allow_hostnames(),
            }),
            internet: self
                .egress
                .internet
                .get()
                .map(|enabled| api::EgressInternet {
                    enabled: Some(enabled),
                }),
        };
        let failure = self.update_sidecar_egress(ingress_port, &config).await?;
        if !failure.is_empty() {
            return Err(js_error(format_args!(
                "Updating sidecar egress config failed with: {failure}"
            )));
        }
        Ok(())
    }

    async fn read_ca_cert(&self) -> Result<()> {
        let Some(port) = self.ingress_port.get() else {
            kj::fail_require!("Cannot read CA cert: sidecar ingress port not known");
        };
        let response = self.sidecar(port).get("/ca").await?;
        if response.status != 200 {
            return Err(js_error(format_args!(
                "Failed to read CA cert from sidecar: {} {}",
                response.status,
                response.text()
            )));
        }
        *self.ca_cert.borrow_mut() = Some(response.text());
        Ok(())
    }

    async fn write_file_to_container(
        &self,
        dir: &str,
        filename: &str,
        content: &[u8],
    ) -> Result<()> {
        let tar = tar_with_file(filename, content)?;
        let endpoint = format!(
            "/containers/{}/archive?path={}",
            self.name,
            encode_uri_component(dir)
        );
        let response = self
            .docker
            .send(Method::PUT, &endpoint, Some(Body::tar(tar)), usize::MAX)
            .await?;
        if response.status != 200 {
            return Err(js_error(format_args!(
                "Failed to write file {dir}/{filename} to container [{}] {}",
                response.status,
                response.text()
            )));
        }
        Ok(())
    }

    async fn inject_ca_cert(&self) -> Result<()> {
        if self.ca_cert_injected.replace(true) {
            return Ok(());
        }
        let mut reset = Defer::new(|| self.ca_cert_injected.set(false));
        if self.ca_cert.borrow().is_none() {
            self.read_ca_cert().await?;
        }
        let Some(cert) = self.ca_cert.borrow().clone() else {
            kj::fail_require!("CA cert not read from sidecar yet");
        };
        self.write_file_to_container(CLOUDFLARE_CA_DIR, CLOUDFLARE_CA_FILENAME, cert.as_bytes())
            .await?;
        reset.disarm();
        Ok(())
    }

    fn with_session_label(&self, request: &mut api::ContainerCreateRequest) {
        if let Some(session) = &self.session_id {
            request.labels = Some([(SESSION_LABEL.to_owned(), session.clone())].into());
        }
    }

    /// Pulls an aliased image Docker does not have yet.
    async fn ensure_system_image(&self, requested: &str) -> Result<()> {
        let image = resolve_image(requested);
        if image == requested {
            return Ok(());
        }
        let inspect_endpoint = format!("/images/{}/json", encode_uri_component(image));
        let inspected = self.docker.get(&inspect_endpoint).await?;
        if inspected.status == 200 {
            return Ok(());
        }
        if inspected.status != 404 {
            return Err(js_error(format_args!(
                "Failed to inspect system image '{requested}': [{}] {}",
                inspected.status,
                inspected.text()
            )));
        }
        let pulled = self
            .docker
            .json(
                Method::POST,
                &format!("/images/create?fromImage={}", encode_uri_component(image)),
                None,
            )
            .await?;
        if pulled.status != 200 {
            return Err(js_error(format_args!(
                "Failed to pull system image '{requested}': [{}] {}",
                pulled.status,
                pulled.text()
            )));
        }
        let available = self.docker.get(&inspect_endpoint).await?;
        if available.status != 200 {
            return Err(js_error(format_args!(
                "Failed to pull system image '{requested}': Docker did not make the image \
                 available: [{}] {}. Pull response: {}",
                available.status,
                available.text(),
                pulled.text()
            )));
        }
        Ok(())
    }

    async fn create_container(
        &self,
        image: &str,
        params: &StartParams,
        mounts: &[RestoreMount],
    ) -> Result<()> {
        let mut request = api::ContainerCreateRequest {
            image: Some(resolve_image(image).to_owned()),
            cmd: params.entrypoint.clone(),
            ..api::ContainerCreateRequest::default()
        };
        self.with_session_label(&mut request);
        let mut env = params.environment.clone().unwrap_or_default();
        env.extend(DEFAULT_ENV.iter().map(|&e| e.to_owned()));
        request.env = Some(env);

        let mut host_config = api::HostConfig {
            // A restart policy keeps the container out of an ambiguous "exited" state.
            restart_policy: Some(api::RestartPolicy {
                name: "on-failure".to_owned(),
                maximum_retry_count: 0,
            }),
            network_mode: Some(format!("container:{}", self.sidecar_name)),
            // Without containers_pid_namespace the container shares the host's PID namespace.
            pid_mode: (!params.pid_namespace).then(|| "host".to_owned()),
            ..api::HostConfig::default()
        };
        if !mounts.is_empty() {
            host_config.mounts = Some(
                mounts
                    .iter()
                    .map(|mount| api::Mount {
                        kind: "volume".to_owned(),
                        source: mount.clone_volume.clone(),
                        target: mount.restore_path.clone(),
                        read_only: false,
                        volume_options: Some(api::VolumeOptions { no_copy: true }),
                    })
                    .collect(),
            );
        }
        if !self.privileges.is_empty() {
            let devices: Vec<String> = self
                .privileges
                .devices
                .iter()
                .map(|device| {
                    format!(
                        "{{ pathOnHost = {}, pathInContainer = {}, cgroupPermissions = {} }}",
                        device.path_on_host, device.path_in_container, device.cgroup_permissions
                    )
                })
                .collect();
            tracing::warn!(
                "Creating a container with explicitly configured Docker privileges. \
                 Capabilities, devices, and security options may grant host-level access.; \
                 containerName = {}; privileges.capabilities = {:?}; devices = {:?}; \
                 privileges.securityOpt = {:?}",
                self.name,
                self.privileges.capabilities,
                devices,
                self.privileges.security_opt
            );
        }
        host_config.grant(&self.privileges);
        request.host_config = Some(host_config);
        let body = serde_json::to_string(&request).map_err(|e| kj::failed!("{e}"))?;

        self.ensure_system_image(image).await?;
        let endpoint = format!("/containers/create?name={}", self.name);
        let mut response = self
            .docker
            .json(Method::POST, &endpoint, Some(body.clone()))
            .await?;
        // 409: a container with the name exists. It is removed and the create retried, a few
        // times, since Docker may take a moment to release the name.
        for _ in 0..3 {
            if response.status != 409 {
                break;
            }
            remove_container(&self.docker, &self.name).await?;
            self.factory.sleep(Duration::from_millis(100)).await;
            response = self
                .docker
                .json(Method::POST, &endpoint, Some(body.clone()))
                .await?;
        }
        match response.status {
            201 => Ok(()),
            404 => Err(js_error(format_args!(
                "No such image available named {image}"
            ))),
            409 => Err(js_error("Container already exists")),
            status => Err(js_error(format_args!(
                "Create container failed with [{status}] {}",
                response.text()
            ))),
        }
    }

    async fn start_container(&self) -> Result<()> {
        // Docker fails a start without a body.
        let response = self
            .docker
            .json(
                Method::POST,
                &format!("/containers/{}/start", self.name),
                Some(String::new()),
            )
            .await?;
        match response.status {
            204 => Ok(()),
            304 => Err(js_error("Container already started")),
            _ => Err(js_error(format_args!(
                "Starting container failed with: {}",
                response.text()
            ))),
        }
    }

    async fn kill_container(&self, signo: u32) -> Result<()> {
        let endpoint = format!(
            "/containers/{}/kill?signal={}",
            self.name,
            signal_name(signo)
        );
        let response = self.docker.json(Method::POST, &endpoint, None).await?;
        // 409: the container is not running.
        if matches!(response.status, 204 | 409) {
            return Ok(());
        }
        Err(js_error(format_args!(
            "Stopping container failed with: {}",
            response.text()
        )))
    }

    /// Removes the container, then its clone volumes. Nothing if there is no container.
    async fn destroy_container(&self) -> Result<()> {
        remove_container(&self.docker, &self.name).await?;
        let volumes = self.snapshot_clones.take();
        delete_volumes(&self.docker, volumes).await;
        Ok(())
    }

    async fn create_sidecar(&self, subnet: &str) -> Result<()> {
        let ipv6 = self.daemon_ipv6_enabled().await?;
        let mut cmd = vec![
            "--http-egress-port".to_owned(),
            self.egress_port.get().to_string(),
            "--http-ingress-address".to_owned(),
            format!("0.0.0.0:{SIDECAR_INGRESS_PORT}"),
            "--docker-gateway-cidr".to_owned(),
            subnet.to_owned(),
            "--dns-enabled".to_owned(),
            "--tls-intercept".to_owned(),
        ];
        if !ipv6 {
            cmd.push("--disable-ipv6".to_owned());
        }
        let mut request = api::ContainerCreateRequest {
            image: Some(self.egress_interceptor_image.clone()),
            cmd: Some(cmd),
            exposed_ports: Some(
                [(
                    format!("{SIDECAR_INGRESS_PORT}/tcp"),
                    Value::Object(serde_json::Map::new()),
                )]
                .into(),
            ),
            host_config: Some(api::HostConfig {
                publish_all_ports: true,
                network_mode: Some("bridge".to_owned()),
                dns: Some(SIDECAR_DNS_SERVERS.iter().map(|&s| s.to_owned()).collect()),
                extra_hosts: Some(vec!["host.docker.internal:host-gateway".to_owned()]),
                // iptables and TPROXY.
                cap_add: Some(vec!["NET_ADMIN".to_owned()]),
                ..api::HostConfig::default()
            }),
            ..api::ContainerCreateRequest::default()
        };
        self.with_session_label(&mut request);
        let body = serde_json::to_string(&request).map_err(|e| kj::failed!("{e}"))?;
        let response = self
            .docker
            .json(
                Method::POST,
                &format!("/containers/create?name={}", self.sidecar_name),
                Some(body),
            )
            .await?;
        match response.status {
            // 409: it exists already.
            201 | 409 => Ok(()),
            404 => Err(js_error(format_args!(
                "No such image available named {}. Please ensure the container egress \
                 interceptor image is built and available.",
                self.egress_interceptor_image
            ))),
            status => Err(js_error(format_args!(
                "Failed to create the networking sidecar [{status}] {}",
                response.text()
            ))),
        }
    }

    async fn start_sidecar(&self) -> Result<()> {
        let response = self
            .docker
            .json(
                Method::POST,
                &format!("/containers/{}/start", self.sidecar_name),
                Some(String::new()),
            )
            .await?;
        if matches!(response.status, 204 | 304) {
            return Ok(());
        }
        Err(js_error(format_args!(
            "Starting network sidecar container failed with: {}{}",
            response.status,
            response.text()
        )))
    }

    async fn create_volume(&self, name: &str, session_id: Option<&str>) -> Result<()> {
        let mut labels = std::collections::BTreeMap::new();
        labels.insert(
            SNAPSHOT_VOLUME_CREATED_AT_LABEL.to_owned(),
            unix_seconds_now().to_string(),
        );
        if let Some(session) = session_id {
            labels.insert(SESSION_LABEL.to_owned(), session.to_owned());
        }
        let request = api::VolumeCreateRequest {
            name: name.to_owned(),
            labels,
        };
        let body = serde_json::to_string(&request).map_err(|e| kj::failed!("{e}"))?;
        let response = self
            .docker
            .json(Method::POST, "/volumes/create", Some(body))
            .await?;
        // 201: created. 200: it existed.
        if matches!(response.status, 200 | 201) {
            return Ok(());
        }
        Err(js_error(format_args!(
            "Failed to create Docker volume '{name}': {} {}",
            response.status,
            response.text()
        )))
    }

    async fn delete_volume(docker: Api, name: String) -> Result<()> {
        let response = docker
            .json(Method::DELETE, &format!("/volumes/{name}"), None)
            .await?;
        if matches!(response.status, 204 | 404) {
            return Ok(());
        }
        Err(js_error(format_args!(
            "Failed to delete Docker volume '{name}': {} {}",
            response.status,
            response.text()
        )))
    }

    async fn inspect_image_size(&self, image: &str) -> Result<u64> {
        let response = self
            .docker
            .get(&format!("/images/{}/json", encode_uri_component(image)))
            .await?;
        if response.status != 200 {
            return Err(js_error(format_args!(
                "Failed to inspect Docker image '{image}': {} {}",
                response.status,
                response.text()
            )));
        }
        let root: api::ImageInspectResponse = response.json()?;
        Ok(root.size)
    }

    async fn delete_image(docker: Api, image: String) -> Result<()> {
        let response = docker
            .json(
                Method::DELETE,
                &format!("/images/{}?noprune=true", encode_uri_component(&image)),
                None,
            )
            .await?;
        if matches!(response.status, 200 | 404) {
            return Ok(());
        }
        Err(js_error(format_args!(
            "Failed to delete Docker image '{image}': {} {}",
            response.status,
            response.text()
        )))
    }

    async fn delete_temp_container(docker: Api, id: String) -> Result<()> {
        let response = docker
            .json(
                Method::DELETE,
                &format!("/containers/{id}?force=true"),
                None,
            )
            .await?;
        if matches!(response.status, 204 | 404) {
            return Ok(());
        }
        kj::fail_require!(
            "Failed to delete temp container; tempContainerId = {id}; status = {}; body = {}",
            response.status,
            response.text()
        );
    }

    /// Creates a container of the sidecar image (only its filesystem is used) that mounts a
    /// volume.
    async fn create_temp_container_with_volume(
        &self,
        volume: &str,
        mount_path: &str,
    ) -> Result<String> {
        let mut request = api::ContainerCreateRequest {
            image: Some(self.egress_interceptor_image.clone()),
            host_config: Some(api::HostConfig {
                binds: Some(vec![format!("{volume}:{mount_path}")]),
                ..api::HostConfig::default()
            }),
            ..api::ContainerCreateRequest::default()
        };
        self.with_session_label(&mut request);
        let body = serde_json::to_string(&request).map_err(|e| kj::failed!("{e}"))?;
        let response = self
            .docker
            .json(Method::POST, "/containers/create", Some(body))
            .await?;
        if response.status != 201 {
            return Err(js_error(format_args!(
                "Failed to create temp container for volume '{volume}': {} {}",
                response.status,
                response.text()
            )));
        }
        let created: api::ContainerCreateResponse = response.json()?;
        Ok(created.id)
    }

    /// Copies a snapshot volume into a new clone volume, which the container mounts.
    async fn clone_snapshot(&self, mount: &RestoreMount) -> Result<()> {
        self.create_volume(&mount.clone_volume, self.session_id.as_deref())
            .await?;
        let mut delete_clone = Defer::new(|| {
            let (docker, volume) = (self.docker.clone(), mount.clone_volume.clone());
            self.spawn_with_self(move |_this| async move {
                let _ = Self::delete_volume(docker, volume).await;
                Ok(())
            });
        });

        let mut request = api::ContainerCreateRequest {
            image: Some(self.egress_interceptor_image.clone()),
            // `/bin/cp -a /src/. /dst/` puts the snapshot's contents in the clone volume.
            entrypoint: Some(vec!["/bin/cp".to_owned()]),
            cmd: Some(vec![
                "-a".to_owned(),
                "/src/.".to_owned(),
                "/dst/".to_owned(),
            ]),
            host_config: Some(api::HostConfig {
                binds: Some(vec![
                    format!("{}:/src:ro", mount.source_volume),
                    format!("{}:/dst", mount.clone_volume),
                ]),
                ..api::HostConfig::default()
            }),
            ..api::ContainerCreateRequest::default()
        };
        self.with_session_label(&mut request);
        let body = serde_json::to_string(&request).map_err(|e| kj::failed!("{e}"))?;
        let created = self
            .docker
            .json(Method::POST, "/containers/create", Some(body))
            .await?;
        if created.status != 201 {
            return Err(js_error(format_args!(
                "Failed to create snapshot clone helper container for volume '{}': {} {}",
                mount.source_volume,
                created.status,
                created.text()
            )));
        }
        let helper = created.json::<api::ContainerCreateResponse>()?.id;
        let mut delete_helper = Defer::new(|| {
            let (docker, helper) = (self.docker.clone(), helper.clone());
            self.spawn_with_self(move |_this| async move {
                let _ = Self::delete_temp_container(docker, helper).await;
                Ok(())
            });
        });

        let started = self
            .docker
            .json(
                Method::POST,
                &format!("/containers/{helper}/start"),
                Some(String::new()),
            )
            .await?;
        if started.status != 204 {
            return Err(js_error(format_args!(
                "Failed to start snapshot clone helper container '{helper}': {} {}",
                started.status,
                started.text()
            )));
        }
        let waited = self
            .docker
            .json(
                Method::POST,
                &format!("/containers/{helper}/wait?condition=not-running"),
                None,
            )
            .await?;
        if waited.status != 200 {
            return Err(js_error(format_args!(
                "Failed waiting for snapshot clone helper container '{helper}': {} {}",
                waited.status,
                waited.text()
            )));
        }
        // A non-zero exit means the copy failed, and the clone is incomplete.
        let status = waited.json::<api::ContainerWaitResponse>()?.status_code;
        if status != 0 {
            return Err(js_error(format_args!(
                "Snapshot clone helper container '{helper}' exited with status {status}"
            )));
        }

        Self::delete_temp_container(self.docker.clone(), helper.clone()).await?;
        delete_helper.disarm();
        delete_clone.disarm();
        self.snapshot_clones
            .borrow_mut()
            .push(mount.clone_volume.clone());
        Ok(())
    }

    // -------------------------------------------------------------------------------------
    // The sidecar and the egress listener

    async fn ensure_egress_listener(&self) -> Result<()> {
        if self.listener_started.replace(true) {
            return Ok(());
        }
        let mut stop = Defer::new(|| self.stop_egress_listener());
        let ipam = self.bridge_ipam().await?;
        // Docker Desktop on macOS routes `host-gateway` to the host's loopback.
        let address = if cfg!(target_os = "macos") {
            "127.0.0.1".to_owned()
        } else {
            ipam.gateway
        };
        let listener = egress::listen(&self.factory, Rc::clone(&self.egress), &address, 0).await?;
        self.egress_port.set(listener.port);
        *self.listener.borrow_mut() = Some(listener);
        stop.disarm();
        Ok(())
    }

    fn stop_egress_listener(&self) {
        drop(self.listener.borrow_mut().take());
        self.listener_started.set(false);
    }

    async fn ensure_sidecar(&self) -> Result<()> {
        // The sidecar's HTTP server gets this many chances to take the egress config.
        const ATTEMPTS: usize = 10;
        if self.sidecar_started.replace(true) {
            return Ok(());
        }
        let mut reset = Defer::new(|| {
            self.sidecar_started.set(false);
            self.ingress_port.set(None);
        });

        // The sidecar must be a fresh one: a workerd restart may have lost track of one.
        remove_container(&self.docker, &self.sidecar_name).await?;
        let ipam = self.bridge_ipam().await?;
        self.create_sidecar(&ipam.subnet).await?;
        self.start_sidecar().await?;
        let Some(port) = self.inspect_sidecar().await? else {
            kj::fail_require!("started sidecar not running");
        };
        self.ingress_port.set(Some(port));

        // The sidecar's HTTP server is ready once it takes the egress config.
        for attempt in 0.. {
            let update = pin!(self.update_sidecar_egress_config(port));
            let timeout = pin!(self.factory.sleep(Duration::from_secs(2)));
            let result = match select(update, timeout).await {
                Either::Left((result, _)) => result,
                Either::Right(_) => Err(kj::overloaded!("operation timed out")),
            };
            match result {
                Ok(()) => break,
                Err(e) if attempt + 1 >= ATTEMPTS => return Err(e),
                Err(_) => self.factory.sleep(Duration::from_millis(200)).await,
            }
        }

        self.read_ca_cert().await?;
        reset.disarm();
        Ok(())
    }

    // -------------------------------------------------------------------------------------
    // rpc::Container

    pub async fn status(&self) -> Result<bool> {
        self.pending_cleanup.clone().await;
        let _turn = self.turn().await;

        let running = self
            .inspect_container()
            .await?
            .is_some_and(|info| info.running);
        self.started.set(running);
        if !running {
            self.labels.borrow_mut().clear();
        }
        self.sidecar_started.set(false);
        self.ingress_port.set(None);

        if running {
            // A container that outlived workerd keeps its sidecar: recover its ingress port and
            // point it at this listener.
            let Some(port) = self.inspect_sidecar().await? else {
                kj::fail_require!(
                    "Recovered running container without a running networking sidecar"
                );
            };
            self.sidecar_started.set(true);
            self.ingress_port.set(Some(port));
            self.ensure_egress_listener().await?;
            self.update_sidecar_egress_port(port).await?;
            self.read_ca_cert().await?;
        }
        Ok(running)
    }

    pub async fn set_labels(&self, labels: Vec<(String, String)>) -> Result<()> {
        let _turn = self.turn().await;
        if !self.started.get() {
            return Err(js_error("setLabels() requires a running container."));
        }
        *self.labels.borrow_mut() = dedupe_labels(labels);
        Ok(())
    }

    pub async fn inspect(&self) -> Result<Option<Inspected>> {
        let _turn = self.turn().await;
        Ok(match self.inspect_container().await? {
            Some(info) if info.running => Some(Inspected {
                labels: self.labels.borrow().clone(),
                image: info.image,
            }),
            _ => None,
        })
    }

    pub async fn start(&self, params: StartParams) -> Result<()> {
        let turn = self.turn().await;

        let snapshot_image;
        let image = match &params.source {
            Source::Image(image) => {
                if image.is_empty() {
                    return Err(js_error("Container image reference cannot be empty."));
                }
                Some(image.as_str())
            }
            Source::ContainerSnapshot(id) => {
                if id.is_empty() {
                    return Err(js_error("Container snapshot ID cannot be empty."));
                }
                let id = parse_snapshot_id(id)?;
                snapshot_image = format!("{CONTAINER_SNAPSHOT_IMAGE_PREFIX}{id}");
                // Checked before the sidecar and the volumes are set up.
                self.inspect_image_size(&snapshot_image).await?;
                Some(snapshot_image.as_str())
            }
            Source::Default => self.image.as_deref(),
        };
        let Some(image) = image else {
            return Err(js_error(
                "Container.start() requires an image or container snapshot.",
            ));
        };

        self.egress.internet.set(Some(params.enable_internet));
        *self.labels.borrow_mut() = dedupe_labels(params.labels.clone());

        // A start that fails removes the container, then the volumes it cloned for it. The
        // removal keeps the turn, so a start() called meanwhile waits for it rather than having
        // its container removed.
        let _destroy = Defer::new(move || {
            if !self.started.get() {
                self.spawn_with_self(|this| async move {
                    let _turn = turn;
                    this.destroy_container().await
                });
            }
        });

        let mut mounts = Vec::with_capacity(params.directory_snapshots.len());
        for (id, restore_path) in &params.directory_snapshots {
            let id = parse_snapshot_id(id)?;
            let restore_path = parse_absolute_path(restore_path)?;
            if restore_path == "/" {
                return Err(js_error(
                    "Directory snapshot cannot be restored to root directory.",
                ));
            }
            let source_volume = format!("{SNAPSHOT_VOLUME_PREFIX}{id}");
            let found = self
                .docker
                .get(&format!("/volumes/{source_volume}"))
                .await?;
            if found.status != 200 {
                return Err(js_error(format_args!(
                    "Snapshot '{id}' not found (volume '{source_volume}' does not exist)"
                )));
            }
            mounts.push(RestoreMount {
                restore_path,
                source_volume,
                clone_volume: format!("{SNAPSHOT_CLONE_VOLUME_PREFIX}{}", random_uuid()),
            });
        }
        for mount in &mounts {
            self.clone_snapshot(mount).await?;
        }

        self.ensure_egress_listener().await?;
        self.ensure_sidecar().await?;
        // Pushed on every start: internet access and the DNS allow-list may have changed since
        // the sidecar was made.
        if let Some(port) = self.ingress_port.get() {
            self.update_sidecar_egress_config(port).await?;
        }

        self.ca_cert_injected.set(false);
        self.create_container(image, &params, &mounts).await?;
        let has_https = self.egress.mappings.borrow().has_https();
        if has_https {
            self.inject_ca_cert().await?;
        }
        self.start_container().await?;
        self.started.set(true);
        Ok(())
    }

    pub async fn monitor(&self) -> Result<i32> {
        // A start() in progress finishes first.
        self.queued_turns().await;
        // One that failed left no container to wait for.
        if !self.started.get() {
            return Err(js_error("Container failed to start"));
        }

        let result = async {
            let response = self
                .docker
                .json(
                    Method::POST,
                    &format!("/containers/{}/wait", self.name),
                    None,
                )
                .await?;
            if response.status != 200 {
                return Err(js_error(format_args!(
                    "Monitoring container failed with: {} {}",
                    response.status,
                    response.text()
                )));
            }
            Ok(response.json::<api::ContainerWaitResponse>()?.status_code)
        }
        .await;

        {
            let _turn = self.turn().await;
            self.started.set(false);
            self.labels.borrow_mut().clear();
        }
        result
    }

    pub async fn destroy(&self) -> Result<()> {
        let _turn = self.turn().await;
        // The sidecar stays for the next start(), which saves seconds of network setup; the
        // client's shutdown removes it.
        self.destroy_container().await?;
        self.started.set(false);
        self.labels.borrow_mut().clear();
        Ok(())
    }

    pub async fn signal(&self, signo: u32) -> Result<()> {
        let _turn = self.turn().await;
        self.kill_container(signo).await
    }

    pub async fn set_inactivity_timeout(&self, duration_ms: i64) -> Result<()> {
        let _turn = self.turn().await;
        if duration_ms <= 0 {
            return Err(js_error(format_args!(
                "setInactivityTimeout() requires durationMs > 0, got {duration_ms}"
            )));
        }
        // The client lives at least until the timeout, if nothing else holds it.
        let delay = Duration::from_millis(duration_ms.unsigned_abs());
        self.spawn_with_self(move |this| async move {
            this.factory.sleep(delay).await;
            drop(this);
            Ok(())
        });
        Ok(())
    }

    pub async fn set_egress(
        &self,
        protocol: Protocol,
        host_port: &str,
        token: &[u8],
    ) -> Result<()> {
        let _turn = self.turn().await;
        let (destination, port) = egress::parse_host_port(host_port)?;
        let default_port = match protocol {
            Protocol::Http => 80,
            Protocol::Https => 443,
            // Every port.
            Protocol::Tcp => 0,
        };
        let port = port.unwrap_or(default_port);

        self.ensure_egress_listener().await?;
        if self.started.get() {
            match protocol {
                // The sidecar is made only for a running container.
                Protocol::Http | Protocol::Tcp => self.ensure_sidecar().await?,
                Protocol::Https => self.inject_ca_cert().await?,
            }
        }

        let channel = ffi::factory_decode_egress_token(self.factory.raw(), token)?;
        self.egress
            .mappings
            .borrow_mut()
            .upsert(destination, port, protocol, Rc::new(channel));
        if let Some(port) = self.ingress_port.get() {
            self.update_sidecar_egress_config(port).await?;
        }
        Ok(())
    }

    pub async fn snapshot_directory(&self, dir: &str) -> Result<DirectorySnapshot> {
        let _turn = self.turn().await;
        let dir = parse_absolute_path(dir)?;
        if !self.started.get() {
            return Err(js_error(
                "snapshotDirectory() requires a running container.",
            ));
        }
        let id = random_uuid();

        // `<dir>/.` makes Docker archive the directory's contents without the directory, so a
        // snapshot can be restored elsewhere.
        let archive_path = if dir == "/" {
            "/.".to_owned()
        } else {
            format!("{dir}/.")
        };
        let tar = self
            .docker
            .send(
                Method::GET,
                &format!(
                    "/containers/{}/archive?path={}",
                    self.name,
                    encode_uri_component(&archive_path)
                ),
                None,
                MAX_SNAPSHOT_TAR_SIZE,
            )
            .await?;
        if tar.status == 404 {
            return Err(js_error(format_args!(
                "snapshotDirectory(): directory not found in container: {dir}"
            )));
        }
        if tar.status != 200 {
            return Err(js_error(format_args!(
                "snapshotDirectory(): failed to read directory '{dir}' from container: {}",
                tar.status
            )));
        }
        let size = tar.body.len() as u64;

        let volume = format!("{SNAPSHOT_VOLUME_PREFIX}{id}");
        // Snapshots outlive the session: the volume has no session label.
        self.create_volume(&volume, None).await?;
        let mut delete_volume = Defer::new(|| {
            let (docker, volume) = (self.docker.clone(), volume.clone());
            self.spawn_with_self(move |_this| async move {
                let _ = Self::delete_volume(docker, volume).await;
                Ok(())
            });
        });

        // The archive goes into the volume through a container that mounts it at /mnt.
        let temp = self
            .create_temp_container_with_volume(&volume, "/mnt")
            .await?;
        let _delete_temp = Defer::new(|| {
            let (docker, temp) = (self.docker.clone(), temp.clone());
            self.spawn_with_self(move |_this| Self::delete_temp_container(docker, temp));
        });
        let stored = self
            .docker
            .send(
                Method::PUT,
                &format!("/containers/{temp}/archive?path=/mnt"),
                Some(Body::tar(tar.body)),
                MAX_JSON_RESPONSE_SIZE,
            )
            .await?;
        if stored.status != 200 {
            return Err(js_error(format_args!(
                "snapshotDirectory(): failed to store snapshot in volume '{volume}': {}",
                stored.status
            )));
        }

        delete_volume.disarm();
        tracing::info!(
            "created snapshot volume; volumeName = {volume}; dir = {dir}; tarSize = {size}"
        );
        Ok(DirectorySnapshot { id, size, dir })
    }

    pub async fn snapshot_container(&self) -> Result<ContainerSnapshot> {
        let _turn = self.turn().await;
        if !self.started.get() {
            return Err(js_error(
                "snapshotContainer() requires a running container.",
            ));
        }
        let id = random_uuid();
        let image = format!("{CONTAINER_SNAPSHOT_IMAGE_PREFIX}{id}");

        let response = self
            .docker
            .json(
                Method::POST,
                &format!(
                    "/commit?container={}&pause=true&repo={}",
                    self.name,
                    encode_uri_component(&image)
                ),
                Some(String::new()),
            )
            .await?;
        if response.status != 201 {
            return Err(js_error(format_args!(
                "Failed to commit container to image '{image}': {} {}",
                response.status,
                response.text()
            )));
        }
        let mut delete_image = Defer::new(|| {
            let (docker, image) = (self.docker.clone(), image.clone());
            self.spawn_with_self(move |_this| async move {
                let _ = Self::delete_image(docker, image).await;
                Ok(())
            });
        });
        let size = self.inspect_image_size(&image).await?;
        delete_image.disarm();
        Ok(ContainerSnapshot { id, size })
    }

    pub(super) fn is_started(&self) -> bool {
        self.started.get()
    }
}

impl Drop for ContainerClient {
    fn drop(&mut self) {
        self.shutdown();
    }
}

/// Labels as a map keeps them: in the order first set, a later value replacing an earlier one.
fn dedupe_labels(labels: Vec<(String, String)>) -> Vec<(String, String)> {
    let mut result: Vec<(String, String)> = Vec::with_capacity(labels.len());
    for (name, value) in labels {
        match result.iter_mut().find(|(existing, _)| *existing == name) {
            Some(entry) => entry.1 = value,
            None => result.push((name, value)),
        }
    }
    result
}

#[cfg(test)]
#[path = "client-test.rs"]
mod tests;
