// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The server's container clients, one per actor whose class has `container` options, by
//! container id.
//!
//! A client lives while something holds it: its actor, a process or port it handed out, or a
//! `setInactivityTimeout()` timer. An actor that starts again while its client lives gets the
//! same client. Once a client shuts down (dropped, or the server shutting down), Docker removes
//! its container in the background; a new client for the container cancels that removal and
//! waits for it to have ended before it talks to Docker.

use std::cell::Cell;
use std::cell::RefCell;
use std::collections::HashMap;
use std::rc::Rc;
use std::rc::Weak;

use futures::FutureExt;
use futures::future::AbortHandle;
use futures::future::LocalBoxFuture;
use futures::future::Shared;
use futures::future::abortable;
use futures::future::join_all;
use workerd_capnp::config;
use workerd_capnp::service;
use workerd_capnp::worker;

use super::api::Device;
use super::api::Privileges;
use super::client::ClientConfig;
use super::client::ContainerClient;
use crate::Result;
use crate::bridge::ffi;
use crate::config::Factory;
use crate::config::capnp_error;
use crate::config::text;

pub type Cleanup = Shared<LocalBoxFuture<'static, ()>>;

fn done() -> Cleanup {
    futures::future::ready(()).boxed_local().shared()
}

/// What the registry needs of a client.
pub trait Client {
    /// Begins the client's shutdown, which hands the registry its cleanup
    /// ([`Registry::cleanup_started`]). Once.
    fn shutdown(&self);
}

impl Client for ContainerClient {
    fn shutdown(&self) {
        Self::shutdown(self);
    }
}

struct Entry<T> {
    client: Weak<T>,
    /// Docker removing the container of the last client that shut down.
    cleanup: Cleanup,
    /// Cancels `cleanup`.
    abort: Option<AbortHandle>,
}

/// Clients by container id, and the cleanup each one left behind.
pub struct Registry<T> {
    entries: RefCell<HashMap<String, Entry<T>>>,
    /// No client may be made once the server shuts down.
    shutdown_started: Cell<bool>,
}

/// The server's registry.
pub type Containers = Registry<ContainerClient>;

impl<T> Default for Registry<T> {
    fn default() -> Self {
        Self {
            entries: RefCell::default(),
            shutdown_started: Cell::new(false),
        }
    }
}

impl Containers {
    /// The client of the container of actor `actor_key` (`<uniqueKey>-<actorId>`) of the
    /// class at `container` in the config: the live one, else a new one.
    ///
    /// # Errors
    ///
    /// The server shutting down, or a worker without a Docker engine or interceptor image.
    pub fn client(
        &self,
        factory: &Rc<Factory>,
        container: &ffi::ContainerRef,
        actor_key: &str,
    ) -> Result<Rc<ContainerClient>> {
        self.check_open()?;
        let message = factory.config()?;
        let config = message
            .get_root::<config::Reader<'_>>()
            .map_err(capnp_error)?;
        let worker_conf = match config
            .get_services()
            .map_err(capnp_error)?
            .get(container.service_index)
            .which()
            .map_err(|e| capnp_error(e.into()))?
        {
            service::Which::Worker(worker) => worker.map_err(capnp_error)?,
            _ => kj::fail_require!("a container's service must be a worker"),
        };
        let options = worker_conf
            .get_durable_object_namespaces()
            .map_err(capnp_error)?
            .get(container.namespace_index)
            .get_container()
            .map_err(capnp_error)?;
        let docker = match worker_conf
            .get_container_engine()
            .which()
            .map_err(|e| capnp_error(e.into()))?
        {
            worker::container_engine::Which::LocalDocker(docker) => docker.map_err(capnp_error)?,
            worker::container_engine::Which::None(()) => kj::fail_require!(
                "dockerPath must be defined to enable containers on this Durable Object."
            ),
        };

        // Unique per namespace and actor across the machine, and per session if there is one.
        let session = text(docker.get_session_id())?;
        let id = if session.is_empty() {
            format!("workerd-{actor_key}")
        } else {
            format!("workerd-{session}-{actor_key}")
        };
        if let Some(client) = self.live(&id) {
            return Ok(client);
        }
        // Everything that can fail comes before the previous client's removal is cancelled.
        let image = text(options.get_image_name())?;
        let privileges = privileges(options.get_privileges().map_err(capnp_error)?)?;
        let config = ClientConfig {
            docker_path: text(docker.get_socket_path())?,
            id: id.clone(),
            image: (!image.is_empty()).then_some(image),
            egress_interceptor_image: text(docker.get_container_egress_interceptor_image())?,
            session_id: (!session.is_empty()).then_some(session),
            privileges,
        };
        if !docker.has_container_egress_interceptor_image() {
            kj::fail_require!("containerEgressInterceptorImage must be configured for containers.");
        }
        self.make(&id, |previous| {
            ContainerClient::new(factory, config, previous)
        })
    }
}

impl<T: Client> Registry<T> {
    fn check_open(&self) -> Result<()> {
        if self.shutdown_started.get() {
            kj::fail_require!(
                "cannot acquire a container client after graceful shutdown has begun"
            );
        }
        Ok(())
    }

    /// The client of container `id`, if one lives.
    fn live(&self, id: &str) -> Option<Rc<T>> {
        self.entries
            .borrow()
            .get(id)
            .and_then(|entry| entry.client.upgrade())
    }

    /// The removal of container `id` by the client before the next one, which that client waits
    /// for. Docker may still be running it, which would race the next client: it is cancelled,
    /// so it ends at once.
    fn take_previous(&self, id: &str) -> Cleanup {
        let mut entries = self.entries.borrow_mut();
        let entry = entries.entry(id.to_owned()).or_insert_with(|| Entry {
            client: Weak::new(),
            cleanup: done(),
            abort: None,
        });
        if let Some(abort) = entry.abort.take() {
            abort.abort();
        }
        entry.cleanup.clone()
    }

    /// A new client of container `id`, from `new`, which gets the previous client's cleanup
    /// to wait for. None is made once shutdown has begun.
    fn make(&self, id: &str, new: impl FnOnce(Cleanup) -> Rc<T>) -> Result<Rc<T>> {
        self.check_open()?;
        let client = new(self.take_previous(id));
        if let Some(entry) = self.entries.borrow_mut().get_mut(id) {
            entry.client = Rc::downgrade(&client);
        }
        Ok(client)
    }

    /// Client `id` began to shut down, and `cleanup` removes its container: the actor's next
    /// incarnation gets a new client, which waits for it. The cleanup to run.
    pub fn cleanup_started(&self, id: &str, cleanup: LocalBoxFuture<'static, ()>) -> Cleanup {
        let (cleanup, abort) = abortable(cleanup);
        let cleanup = cleanup.map(|_| ()).boxed_local().shared();
        if let Some(entry) = self.entries.borrow_mut().get_mut(id) {
            entry.client = Weak::new();
            entry.cleanup = cleanup.clone();
            entry.abort = Some(abort);
        }
        cleanup
    }

    /// Shuts down every client and resolves once Docker has removed their containers. No
    /// client can be made afterwards.
    pub async fn shutdown(&self) {
        self.shutdown_started.set(true);
        self.shut_down_matching(|_| true).await;
    }

    /// Shuts down the client of root actor `actor` in the namespace `unique_key` and resolves
    /// once Docker has removed its container.
    pub async fn stop(&self, unique_key: &str, actor: &str) {
        // Container ids end in `<uniqueKey>-<actorId>`.
        let suffix = format!("-{unique_key}-{actor}");
        self.shut_down_matching(|id| id.ends_with(&suffix)).await;
    }

    async fn shut_down_matching(&self, matches: impl Fn(&str) -> bool) {
        let clients: Vec<Rc<T>> = self
            .entries
            .borrow()
            .iter()
            .filter(|(id, _)| matches(id))
            .filter_map(|(_, entry)| entry.client.upgrade())
            .collect();
        // A client's shutdown replaces its entry's cleanup.
        for client in &clients {
            client.shutdown();
        }
        drop(clients);
        let cleanups: Vec<Cleanup> = self
            .entries
            .borrow()
            .iter()
            .filter(|(id, _)| matches(id))
            .map(|(_, entry)| entry.cleanup.clone())
            .collect();
        join_all(cleanups).await;
    }
}

/// A container's privileges from the config.
fn privileges(
    privileges: worker::durable_object_namespace::container_options::container_privileges::Reader<
        '_,
    >,
) -> Result<Privileges> {
    let texts = |list: capnp::Result<capnp::text_list::Reader<'_>>| -> Result<Vec<String>> {
        list.map_err(capnp_error)?.iter().map(text).collect()
    };
    Ok(Privileges {
        capabilities: texts(privileges.get_capabilities())?,
        devices: privileges
            .get_devices()
            .map_err(capnp_error)?
            .iter()
            .map(|device| {
                Ok(Device {
                    path_on_host: text(device.get_path_on_host())?,
                    path_in_container: text(device.get_path_in_container())?,
                    cgroup_permissions: text(device.get_cgroup_permissions())?,
                })
            })
            .collect::<Result<_>>()?,
        security_opt: texts(privileges.get_security_opt())?,
    })
}

#[cfg(test)]
#[path = "registry-test.rs"]
mod tests;
