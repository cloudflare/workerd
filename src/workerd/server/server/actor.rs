// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Durable Object namespaces: the actors of one class of one worker, their storage, facets and
//! eviction. A worker service owns one namespace per class its config gives storage to.
//!
//! An [`ActorNamespace`] files its actors by key (the hex id of a durable actor, the name of an
//! ephemeral one) in [`ActorContainer`]s. A container outlives the actor it holds: an actor idle
//! for ten seconds is shut down, its WebSockets hibernated, and rebuilt by the next request; a
//! container with no clients and no access for seventy seconds is dropped by the namespace's
//! cleanup loop; a broken actor removes its container at once. Facets are containers under a root
//! container, sharing its storage and its last-access time.
//!
//! Ownership: the namespace owns its root containers and a container its facets. Everything that
//! points the other way (a facet to its parent; an actor's hooks and a background task to their
//! container) holds a `Weak`, so that a container dropped from its map goes away with its actor.
//! Whatever else holds a container's `Rc` keeps it from the cleanup loop: a stub's channel and a
//! request in flight (each a [`Client`]), and an eviction in progress.

use std::cell::Cell;
use std::cell::OnceCell;
use std::cell::RefCell;
use std::collections::HashMap;
use std::collections::hash_map::Entry;
use std::pin::Pin;
use std::rc::Rc;
use std::rc::Weak;
use std::time::Duration;

use cluster_logic::handoff::Heat;
use futures::FutureExt;
use futures::future::Either;
use futures::future::LocalBoxFuture;
use futures::future::RemoteHandle;
use futures::future::Shared;
use hashlink::LinkedHashMap;
use kj_rs::KjMaybe;
use kj_rs::KjOwn;
use kj_rs::KjRc;
use tokio::sync::Notify;
use tokio::sync::watch;
use tokio::time::Instant;
use worker::Interface;
use worker::PromisedInterface;

use crate::Result;
use crate::bindings::ActorConfig;
use crate::bridge::ffi;
use crate::channels::AbortReason;
use crate::channels::ActorClass;
use crate::channels::ActorHooks;
use crate::channels::ActorIdHandle;
use crate::channels::ActorNamespaceHandle;
use crate::channels::Channel;
use crate::channels::FacetStart;
use crate::channels::NewActor;
use crate::channels::PendingToken;
use crate::channels::Persistent;
use crate::channels::RequestMetadata;
use crate::channels::SubrequestChannel;
use crate::channels::TokenUsage;
use crate::channels::WorkerInterface;
use crate::channels::attach;
use crate::cluster::ClusterNode;
use crate::cluster::Committer;
use crate::cluster::Deadlines;
use crate::cluster::Owned;
use crate::cluster::Owner;
use crate::cluster::ResultTask;
use crate::cluster::RpcConnection;
use crate::cluster::is_moved;
use crate::cluster::moved;
use crate::cluster::not_delivered;
use crate::config::Factory;
use crate::config::invalid_config_error;
use crate::tasks::TaskHandle;

/// How long an actor stays up after its last request ends before it is shut down.
const IDLE_SHUTDOWN_DELAY: Duration = Duration::from_secs(10);
/// How long a container with no clients stays in its namespace after its last access.
const CONTAINER_EXPIRATION: Duration = Duration::from_secs(70);
/// How long a test eviction waits for the actor's requests to drain.
const EVICT_TIMEOUT: Duration = Duration::from_secs(30);
/// How long a test eviction waits before re-checking an actor that has no requests but is still
/// referenced (a request tearing down).
const EVICT_RETRY_DELAY: Duration = Duration::from_millis(1);

/// How often a handoff checks whether the object's requests ended.
const HANDOFF_POLL: Duration = Duration::from_millis(10);
/// How many objects a draining instance releases at once.
const HANDOFFS_AT_ONCE: usize = 32;
/// How many actors a stopping instance stops between turns of its event loop.
const STOPS_AT_ONCE: usize = 64;

const EVICT_TIMEOUT_MESSAGE: &str =
    "jsg.Error: Timed out waiting to evict Durable Object: it still has active references.";

/// The namespace of one Durable Object class: its actors and their shared storage. The runtime's
/// `ActorNamespace`.
pub struct ActorNamespace {
    this: Weak<Self>,
    factory: Rc<Factory>,
    /// The worker service the class belongs to, which a peer names to reach an actor.
    service: String,
    class_name: String,
    config: ActorConfig,
    actor_class: Rc<dyn ActorClass>,
    /// Whether the actors' self tokens may be stored: the `allow_irrevocable_stub_storage` flag
    /// of the worker whose class they are.
    persistent_self_tokens: Persistent,
    /// Opened by `link`; every actor of the namespace shares it. An ephemeral namespace gets
    /// in-memory storage that its actors, being ephemeral, never use.
    storage: OnceCell<KjOwn<ffi::ActorStorage>>,
    /// A broken actor removes its container; an idle one keeps it, minus the actor, so that the
    /// next request rebuilds the actor in place. The cleanup loop drops long-idle, clientless
    /// containers.
    actors: RefCell<LinkedHashMap<String, Rc<ActorContainer>>>,
    /// The cleanup loop, started by the first request; dropped with the namespace.
    cleanup_task: RefCell<Option<TaskHandle>>,
    /// Set in cluster mode for a namespace on disk: its actors are owned by one instance of
    /// the cluster at a time, and replicated.
    cluster: OnceCell<Rc<ClusterNode>>,
    /// The namespace's directory, once linked on disk.
    directory: OnceCell<String>,
    /// Actors being resolved in the cluster, each woken when its first resolution ends.
    resolving: RefCell<HashMap<String, Rc<Notify>>>,
    /// Set for the namespace of a worker that cannot handle requests (`set_broken`).
    broken: Cell<bool>,
    /// Actors a stop left as they were at its deadline: they end as the process exits.
    left_behind: RefCell<Vec<Rc<ActorContainer>>>,
}

/// The first resolution of an actor: its end, however it ends, wakes the requests that waited.
struct Resolving<'a> {
    pending: &'a RefCell<HashMap<String, Rc<Notify>>>,
    key: String,
}

impl Drop for Resolving<'_> {
    fn drop(&mut self) {
        if let Some(done) = self.pending.borrow_mut().remove(&self.key) {
            done.notify_waiters();
        }
    }
}

impl ActorNamespace {
    /// Builds the namespace of `class_name`, a class of a worker `factory` compiled.
    ///
    /// `actor_class` is the class every actor of the namespace is constructed from: the worker's
    /// own, with no props bound, or for a Workflow's namespace the engine's class with the
    /// Workflow's props. `persistent_self_tokens` is that class's worker's flag.
    pub fn new(
        factory: Rc<Factory>,
        service: String,
        class_name: String,
        config: ActorConfig,
        actor_class: Rc<dyn ActorClass>,
        persistent_self_tokens: Persistent,
    ) -> Rc<Self> {
        Rc::new_cyclic(|this| Self {
            this: Weak::clone(this),
            factory,
            service,
            class_name,
            config,
            actor_class,
            persistent_self_tokens,
            storage: OnceCell::new(),
            actors: RefCell::new(LinkedHashMap::new()),
            cleanup_task: RefCell::new(None),
            cluster: OnceCell::new(),
            directory: OnceCell::new(),
            resolving: RefCell::new(HashMap::new()),
            broken: Cell::new(false),
            left_behind: RefCell::new(Vec::new()),
        })
    }

    /// Fails every request for one of the namespace's actors before it is routed, so that the
    /// instance never claims an actor it cannot run, and declines dead peers' alarms.
    pub fn set_broken(&self) {
        self.broken.set(true);
    }

    /// Puts a durable namespace on disk under the cluster's ownership; before `link`.
    pub fn set_cluster(&self, cluster: Rc<ClusterNode>) {
        if let Some(unique_key) = self.unique_key() {
            cluster.add_namespace(unique_key, Weak::clone(&self.this));
        }
        let _ = self.cluster.set(cluster);
    }

    /// Schedules an alarm adopted from a dead peer: it claims the object when it fires.
    pub fn adopt_alarm(&self, actor: &str, name: Option<&str>, at_ms: u64) -> Result<()> {
        if self.broken.get() {
            return Err(kj::failed!(
                "the namespace cannot adopt alarms because its worker cannot handle requests"
            ));
        }
        ffi::actor_storage_set_alarm(self.storage()?, actor, name.unwrap_or_default(), at_ms)?;
        Ok(())
    }

    /// The cluster, for a namespace linked on disk; in-memory namespaces stay local.
    pub(crate) fn cluster(&self) -> Option<&Rc<ClusterNode>> {
        self.directory.get().and_then(|_| self.cluster.get())
    }

    /// The database of root actor `key`'s facet numbered `facet_id`, the root's own for none.
    fn sqlite_path(&self, key: &str, facet_id: Option<u32>) -> Result<String> {
        let directory = self
            .directory
            .get()
            .ok_or_else(|| kj::failed!("the namespace has no directory"))?;
        Ok(match facet_id {
            Some(id) => format!("{directory}/{key}.{id}.sqlite"),
            None => format!("{directory}/{key}.sqlite"),
        })
    }

    /// The container running the database `file`, `<key>.sqlite` or a facet's
    /// `<key>.<n>.sqlite`.
    fn database_container(&self, file: &str) -> Option<Rc<ActorContainer>> {
        fn find(c: &Rc<ActorContainer>, id: Option<u32>) -> Option<Rc<ActorContainer>> {
            if c.facet_id.get() == id {
                return Some(Rc::clone(c));
            }
            c.facets.borrow().values().find_map(|f| find(f, id))
        }
        let mut parts = file.split('.');
        let root = self.actors.borrow().get(parts.next()?).cloned()?;
        find(&root, parts.next().and_then(|n| n.parse().ok()))
    }

    /// The owner of the actor with `id`: a container here, holding its ownership, or a session
    /// with the instance that owns it. For a request a peer sent (`remote`), a draining instance
    /// is no hop to another: the peer is answered as `moved`, so that it finds the owner itself
    /// and its route cache stops pointing here.
    async fn resolve(&self, id: KjOwn<ActorIdHandle>, remote: bool) -> Result<Resolved> {
        if self.broken.get() {
            return Err(invalid_config_error());
        }
        let key = ffi::actor_id_key(&id);
        if let Some(container) = self.owned_container(&key) {
            return Ok(Resolved::Here(Client(container)));
        }
        let cluster = self
            .cluster()
            .ok_or_else(|| kj::failed!("not in a cluster"))?;
        // One cold resolution per actor: the rest wait for it, then find its container or its
        // cached route, rather than each reading and claiming the owner record in the bucket.
        let pending = self.resolving.borrow().get(&key).cloned();
        let _first = if let Some(done) = pending {
            done.notified().await;
            if let Some(container) = self.owned_container(&key) {
                return Ok(Resolved::Here(Client(container)));
            }
            None
        } else {
            let done = Rc::new(Notify::new());
            self.resolving.borrow_mut().insert(key.clone(), done);
            Some(Resolving {
                pending: &self.resolving,
                key: key.clone(),
            })
        };
        let unique_key = self.unique_key().unwrap_or_default().to_owned();
        let route = || cluster.route(&unique_key, &self.service, &self.class_name, &key);
        let mut found = route().await?;
        if let Owner::Releasing(epoch) = found {
            // Owned here but not running: this draining instance lets it go, then sends the
            // request on to whoever claims it.
            if cluster.release(&unique_key, &key, epoch).await {
                self.clear_storage(&key);
            }
            found = route().await?;
        }
        let draining = || moved(&format!("{unique_key}/{key}: this node is draining"));
        match found {
            Owner::Releasing(_) => Err(draining()),
            Owner::Remote(..) if remote && cluster.cluster.is_draining() => Err(draining()),
            Owner::Remote(connection, node) => Ok(Resolved::Remote(connection, node)),
            Owner::Here(epoch) => {
                // A concurrent request may have taken it meanwhile.
                if let Some(container) = self.owned_container(&key) {
                    return Ok(Resolved::Here(Client(container)));
                }
                // A container added once the handoff began might never be handed off.
                if cluster.cluster.is_draining() {
                    return Err(moved(&format!("{key}: this node is draining")));
                }
                let container = ActorContainer::new_root(key.clone(), self, id);
                let owned = Owned::new(Rc::clone(cluster), &unique_key, &key, epoch);
                *container.owned.borrow_mut() = Some(owned);
                let replaced = self.actors.borrow_mut().insert(key, Rc::clone(&container));
                drop(replaced);
                Ok(Resolved::Here(Client(container)))
            }
        }
    }

    /// Fails in a cluster once this instance's lease has lapsed. A node stalled past its lease may
    /// have been taken over: running its actors, even to read their databases, could act on stale
    /// data.
    fn require_lease(&self) -> Result<()> {
        if self
            .cluster()
            .is_some_and(|node| !node.cluster.lease_holds())
        {
            return Err(kj::failed!(
                "this instance's cluster lease in the bucket has lapsed"
            ));
        }
        Ok(())
    }

    /// Removes `container` from the map, unless a newer container has taken its key: one whose
    /// restore or shutdown ends after it was released and replaced must leave its successor.
    fn remove_container(&self, container: &ActorContainer) {
        let mut actors = self.actors.borrow_mut();
        if actors
            .get(&container.key)
            .is_some_and(|c| std::ptr::eq(Rc::as_ptr(c), container))
        {
            actors.remove(&container.key);
        }
    }

    /// Removes the databases of an object this instance handed off.
    fn clear_storage(&self, key: &str) {
        let cleared = self
            .storage()
            .and_then(|s| Ok(ffi::actor_storage_clear(s, key)?));
        if let Err(e) = cleared {
            tracing::warn!("removing {key}'s databases: {}", e.description());
        }
    }

    fn owned_container(&self, key: &str) -> Option<Rc<ActorContainer>> {
        let actors = self.actors.borrow();
        let container = actors.get(key)?;
        container
            .owned
            .borrow()
            .is_some()
            .then(|| Rc::clone(container))
    }

    fn storage(&self) -> Result<&ffi::ActorStorage> {
        self.storage.get().map(|storage| &**storage).ok_or_else(|| {
            kj::failed!(
                "Durable Object class \"{}\": link() has not been called",
                self.class_name
            )
        })
    }

    fn is_durable(&self) -> bool {
        matches!(self.config, ActorConfig::Durable { .. })
    }

    fn enable_sql(&self) -> bool {
        match &self.config {
            ActorConfig::Durable { enable_sql, .. } | ActorConfig::Ephemeral { enable_sql, .. } => {
                *enable_sql
            }
        }
    }

    /// The container of the actor with `id`, created if the namespace has none.
    fn container_for(&self, id: KjOwn<ActorIdHandle>) -> Rc<ActorContainer> {
        let key = ffi::actor_id_key(&id);
        let mut actors = self.actors.borrow_mut();
        if let Some(container) = actors.get(&key) {
            return Rc::clone(container);
        }
        let container = ActorContainer::new_root(key.clone(), self, id);
        actors.insert(key, Rc::clone(&container));
        container
    }

    /// Starts the cleanup loop if it is not running. A namespace with `preventEviction` never
    /// drops a container, so it runs none.
    fn ensure_cleanup_loop(&self) {
        if !self.is_evictable() || self.cleanup_task.borrow().is_some() {
            return;
        }
        let task = self.factory.spawn(cleanup_loop(Weak::clone(&self.this)));
        *self.cleanup_task.borrow_mut() = Some(task);
    }

    /// Removes and returns the containers the cleanup loop should drop: not accessed within
    /// `CONTAINER_EXPIRATION` and without clients.
    fn take_expired(&self, now: Duration) -> Vec<Rc<ActorContainer>> {
        let mut actors = self.actors.borrow_mut();
        let keys: Vec<String> = actors
            .iter()
            // Checking the access time first: it is cheaper than the client walk.
            .filter(|(_, container)| {
                now.saturating_sub(container.last_access.get()) > CONTAINER_EXPIRATION
                    && !container.has_clients()
            })
            .map(|(key, _)| key.clone())
            .collect();
        keys.iter().filter_map(|key| actors.remove(key)).collect()
    }
}

/// Drops containers the namespace has not touched in `CONTAINER_EXPIRATION`, that often.
async fn cleanup_loop(namespace: Weak<ActorNamespace>) {
    loop {
        let Some(ns) = namespace.upgrade() else {
            return;
        };
        let factory = Rc::clone(&ns.factory);
        let expired = ns.take_expired(factory.now());
        drop(ns);
        // Dropping a container shuts its actor down; that happens outside the map's borrow.
        drop(expired);
        factory.sleep(CONTAINER_EXPIRATION).await;
    }
}

impl ActorNamespace {
    /// The channel of the actor with `id`, started on first use. `persistent` is recorded on the
    /// channel: whether stubs to it may be stored.
    pub fn channel(&self, id: KjOwn<ActorIdHandle>, persistent: Persistent) -> Rc<dyn Channel> {
        self.channel_for(id, persistent, false)
    }

    /// The channel of the actor with `id` for a peer's or the debug port's request.
    pub fn peer_channel(&self, id: KjOwn<ActorIdHandle>) -> Rc<dyn Channel> {
        self.channel_for(id, false, true)
    }

    fn channel_for(
        &self,
        id: KjOwn<ActorIdHandle>,
        persistent: Persistent,
        remote: bool,
    ) -> Rc<dyn Channel> {
        if self.cluster().is_some() {
            return Rc::new(RoutedChannel {
                namespace: Weak::clone(&self.this),
                id,
                persistent,
                remote,
                bound: Rc::default(),
            });
        }
        Rc::new(ActorChannel {
            container: Client(self.container_for(id)),
            persistent,
        })
    }

    /// The channel of the ephemeral actor named `name`.
    pub fn channel_by_name(&self, name: &str, persistent: Persistent) -> Rc<dyn Channel> {
        self.channel(ffi::actor_id_from_name(name), persistent)
    }

    /// Whether `abortAllActors()`/`deleteAllActors()` may evict this namespace's actors
    /// (`preventEviction` unset).
    pub fn is_evictable(&self) -> bool {
        match &self.config {
            ActorConfig::Durable { evictable, .. } | ActorConfig::Ephemeral { evictable, .. } => {
                *evictable
            }
        }
    }

    /// `reason` becomes the error of the actors' in-flight requests.
    pub fn abort_all(&self, reason: Option<&crate::Error>) {
        let actors = std::mem::take(&mut *self.actors.borrow_mut());
        for container in actors.values() {
            container.abort(reason.cloned());
        }
    }

    /// Stops every actor that is left as this instance's handoff ends; peers take them over
    /// once it left the cluster. Any request still in flight is cancelled, as it could not
    /// commit once a peer owns the object. A later request that still reaches one here, through
    /// a stub or a peer that found it here, started nothing: it is answered as `moved`, so that
    /// it can follow the actor to its next owner. Those left at `until` are left as they are
    /// (`stop_all`).
    pub async fn release_all(&self, until: Instant) {
        self.stop_all(Some(until), |key, container| {
            let left = moved(&format!("{key}: this node is draining"));
            container.abort_then(container.cancelled(), Some(left));
        })
        .await;
    }

    /// Stops each actor with `stop`, a few at a time, letting the event loop run between them,
    /// as stopping one aborts it and closes its databases. The actors left once `until` passed
    /// are left as they are, for the process's exit to end.
    async fn stop_all(&self, until: Option<Instant>, stop: impl Fn(&str, &ActorContainer)) {
        let actors = std::mem::take(&mut *self.actors.borrow_mut());
        let mut actors = actors.into_iter().collect::<Vec<_>>().into_iter();
        while actors.len() > 0 {
            if until.is_some_and(|until| Instant::now() >= until) {
                let left = actors.map(|(_, container)| container);
                return self.left_behind.borrow_mut().extend(left);
            }
            for (key, container) in actors.by_ref().take(STOPS_AT_ONCE) {
                stop(&key, &container);
            }
            tokio::task::yield_now().await;
        }
    }

    /// The handoffs of this namespace's objects as this instance drains, each with its heat.
    /// Each object takes no new request but those of a handoff's work from now on (`Cause`).
    fn handoffs(
        &self,
        in_flight: Instant,
        slots: &Rc<tokio::sync::Semaphore>,
    ) -> Vec<(Heat, LocalBoxFuture<'static, ()>)> {
        let actors = self.actors.borrow();
        let handoffs = actors.values().map(|c| {
            *c.handoff.borrow_mut() = Some(watch::channel(false).0);
            let ns = Weak::clone(&self.this);
            let handoff = Rc::clone(c).hand_off(ns, in_flight, Rc::clone(slots));
            (c.heat.get(), handoff.boxed_local())
        });
        handoffs.collect()
    }

    /// Aborts every actor and deletes the namespace's storage and alarms.
    pub fn delete_all(&self, reason: Option<&crate::Error>) -> Result<()> {
        // The databases are reset while their connections are open, before the abort closes
        // them: Windows releases file locks late.
        for container in self.actors.borrow().values() {
            container.reset_storage();
        }
        self.abort_all(reason);
        match self.storage.get() {
            Some(storage) => Ok(ffi::actor_storage_delete_all(storage)?),
            None => Ok(()),
        }
    }

    /// Test hook: shuts down every running actor of the namespace (hibernating or closing its
    /// WebSockets), keeping its container so that the next request rebuilds it. Nothing for a
    /// namespace with `preventEviction`.
    pub async fn evict_all_for_test(&self, hibernate: bool) -> Result<()> {
        if !self.is_evictable() {
            return Ok(());
        }
        // The containers stay in the map; a broken actor may remove its own mid-eviction,
        // which the eviction's own `Rc` survives.
        let containers: Vec<Rc<ActorContainer>> = self.actors.borrow().values().cloned().collect();
        let evictions = containers
            .iter()
            .map(|container| container.evict_tree_for_test_if_running(hibernate));
        futures::future::join_all(evictions)
            .await
            .into_iter()
            .collect::<Result<()>>()
    }

    /// Whether `link` has opened the namespace's storage.
    pub fn is_linked(&self) -> bool {
        self.storage.get().is_some()
    }

    /// Opens the namespace's storage. `storage_path` is the directory of the worker's
    /// `durableObjectStorage.localDisk` service; none for in-memory storage.
    pub fn link(&self, storage_path: Option<&str>) -> Result<()> {
        if self.is_linked() {
            return Err(kj::failed!("already called link()"));
        }
        let (path, unique_key) = match &self.config {
            ActorConfig::Durable { unique_key, .. } => {
                (storage_path.unwrap_or_default(), unique_key.as_str())
            }
            ActorConfig::Ephemeral { .. } => ("", ""),
        };
        if !path.is_empty() {
            let directory = format!("{path}/{unique_key}");
            if self.cluster.get().is_some() {
                // Composition rejects this in a deployment; this check also prevents any other
                // config from claiming a directory outside `path`.
                if !::cluster::compose::is_path_component(unique_key) {
                    return Err(kj::failed!(
                        "Durable Object namespace uniqueKey {unique_key:?} is not one path \
                         component"
                    ));
                }
                claim_directory(&directory)?;
            }
            let _ = self.directory.set(directory);
        }
        let storage = ffi::factory_new_actor_storage(
            self.factory.raw(),
            path,
            unique_key,
            Box::new(ActorNamespaceHandle(Weak::clone(&self.this))),
        )?;
        if self.cluster().is_some() {
            // An earlier process's alarms: its objects may live elsewhere now, and peers adopt
            // its alarms from the bucket.
            ffi::actor_storage_delete_all(&storage)?;
        }
        // Checked above; nothing else sets it.
        let _ = self.storage.set(storage);
        Ok(())
    }

    /// Whether this instance is handing off root actor `key`.
    pub fn hands_off(&self, key: &str) -> bool {
        self.actors
            .borrow()
            .get(key)
            .is_some_and(|container| container.is_handed_off())
    }

    /// The `uniqueKey` of a durable namespace; none for an ephemeral one.
    pub fn unique_key(&self) -> Option<&str> {
        match &self.config {
            ActorConfig::Durable { unique_key, .. } => Some(unique_key),
            ActorConfig::Ephemeral { .. } => None,
        }
    }

    /// Gives `metadata` a self token naming the root actor `id`, for `ctx.restore()`. An
    /// ephemeral namespace's actors have none.
    fn set_self_token(&self, metadata: &mut KjOwn<RequestMetadata>, id: &ActorIdHandle) {
        if let Some(unique_key) = self.unique_key() {
            ffi::request_metadata_set_actor_self_token(
                self.factory.raw(),
                metadata.as_mut(),
                unique_key,
                id,
                self.persistent_self_tokens,
            );
        }
    }

    /// Where the config has the class's `container` options, if it has any.
    fn container(&self) -> Option<&ffi::ContainerRef> {
        match &self.config {
            ActorConfig::Durable { container, .. } => container.as_ref(),
            ActorConfig::Ephemeral { .. } => None,
        }
    }

    /// Stops the namespace's actors ahead of shutdown so that none can race its containers'
    /// Docker cleanup. Nothing for a class without `container` options.
    /// Those left at `until` are left as they are (`stop_all`).
    pub async fn begin_container_cleanup(&self, until: Option<Instant>) {
        if self.container().is_some() {
            let reason = kj::disconnected!("Server shutting down.");
            self.stop_all(until, |_, container| container.abort(Some(reason.clone())))
                .await;
        }
    }
}

/// Marks a namespace's `directory` as the cluster's. The cluster replaces an
/// object's database with the bucket's copy, so a directory holding Durable
/// Objects from before cluster mode is refused rather than deleted.
fn claim_directory(directory: &str) -> Result<()> {
    let directory = std::path::Path::new(directory);
    let marker = directory.join(".cluster");
    if marker.exists() {
        return Ok(());
    }
    let databases = std::fs::read_dir(directory).into_iter().flatten().flatten();
    let held = databases.map(|e| e.file_name()).any(|name| {
        let name = name.to_string_lossy();
        name.ends_with(".sqlite") && name != "metadata.sqlite"
    });
    if held {
        return Err(kj::failed!(
            "{} holds Durable Objects from before cluster mode, which the cluster would \
             replace: move them away",
            directory.display()
        ));
    }
    std::fs::create_dir_all(directory)
        .and_then(|()| std::fs::write(&marker, ""))
        .map_err(|e| kj::failed!("{}: {e}", marker.display()))
}

impl ActorNamespaceHandle {
    pub(crate) fn actor_for_alarm(
        &self,
        id: KjOwn<ActorIdHandle>,
    ) -> Result<KjOwn<WorkerInterface>> {
        let namespace = self
            .0
            .upgrade()
            .ok_or_else(|| kj::disconnected!("the Durable Object namespace is gone"))?;
        let metadata = ffi::new_request_metadata(KjMaybe::None, KjMaybe::None);
        let Some(node) = namespace.cluster().map(Rc::clone) else {
            return namespace.container_for(id).start_request(metadata);
        };
        // A draining instance starts nothing: the alarm runs where the object goes, once peers
        // adopt this instance's alarms as it leaves, or the next owner opens the object.
        if node.cluster.is_draining() {
            return Ok(ffi::alarm_owned_elsewhere(ffi::AlarmElsewhere::Retry));
        }
        // Only an object's owner runs its alarm. One that moved since this instance scheduled
        // the alarm has it scheduled where it lives now, since opening an object schedules its
        // alarm, unless the owner died before indexing it: this instance fires it again until
        // the owner has outlived that (`Index::elsewhere`).
        let key = ffi::actor_id_key(&id);
        Ok(PromisedInterface::new(async move {
            match namespace.resolve(id, false).await? {
                Resolved::Here(client) => client.start_request(metadata),
                Resolved::Remote(_, owner) => {
                    let unique_key = namespace.unique_key().unwrap_or_default();
                    let again = node
                        .cluster
                        .alarm_elsewhere(unique_key, &key, &owner, cluster::now_ms())
                        .await
                        .unwrap_or(true);
                    let then = if again {
                        ffi::AlarmElsewhere::Retry
                    } else {
                        ffi::AlarmElsewhere::Done
                    };
                    Ok(ffi::alarm_owned_elsewhere(then))
                }
            }
        })
        .into_kj())
    }

    pub fn alarm_set(&self, actor: &str, name: &str, at_ms: u64) -> Box<ResultTask> {
        let (actor, name) = (actor.to_owned(), name.to_owned());
        Box::new(self.index_alarm(move |cluster, unique_key| async move {
            let name = (!name.is_empty()).then_some(name.as_str());
            let at_ms = (at_ms != 0).then_some(at_ms);
            cluster.arm_alarm(&unique_key, &actor, name, at_ms).await
        }))
    }

    pub fn alarm_opened(&self, actor: &str, name: &str, at_ms: u64) -> Box<ResultTask> {
        let (actor, name) = (actor.to_owned(), name.to_owned());
        Box::new(self.index_alarm(move |cluster, unique_key| async move {
            let name = (!name.is_empty()).then_some(name.as_str());
            cluster.opened_alarm(&unique_key, &actor, name, at_ms).await
        }))
    }

    /// A task running `index` with the cluster and the namespace's unique key; nothing outside
    /// a cluster.
    fn index_alarm<F: Future<Output = cluster::Result<()>> + 'static>(
        &self,
        index: impl FnOnce(Rc<cluster::Cluster>, String) -> F + 'static,
    ) -> ResultTask {
        let target = self.namespace().and_then(|ns| {
            let unique_key = ns.unique_key()?.to_owned();
            Some((Rc::clone(&ns.cluster()?.cluster), unique_key))
        });
        ResultTask(Box::pin(async move {
            let Some((cluster, unique_key)) = target else {
                return Ok(());
            };
            (index(cluster, unique_key).await).map_err(|e| kj::failed!("indexing an alarm: {e}"))
        }))
    }

    fn namespace(&self) -> Option<Rc<ActorNamespace>> {
        self.0.upgrade()
    }

    pub(crate) fn replicated(&self) -> bool {
        self.namespace().is_some_and(|ns| ns.cluster().is_some())
    }

    pub(crate) fn check_open(&self) -> Result<()> {
        self.namespace().map_or(Ok(()), |ns| ns.require_lease())
    }

    pub(crate) fn database_reset(&self, file: &str) {
        let Some(ns) = self.namespace() else {
            return;
        };
        // The facets beneath the database go with it: their connections close before their
        // files are removed.
        if let Some(container) = ns.database_container(file) {
            let names: Vec<String> = container.facets.borrow().keys().cloned().collect();
            for name in names {
                container.abort_facet(&name, kj::failed!("jsg.Error: Facet was deleted."));
            }
        }
        if let (Some(node), Some(unique_key)) = (ns.cluster(), ns.unique_key()) {
            node.cluster.database_reset(unique_key, file);
        }
    }

    pub(crate) fn wal_write(&self, file: &str, offset: u64, data: &[u8]) {
        if let Some(ns) = self.namespace()
            && let (Some(node), Some(unique_key)) = (ns.cluster(), ns.unique_key())
        {
            node.cluster.wal_write(unique_key, file, offset, data);
        }
    }
}

/// Where a request to a clustered actor goes.
enum Resolved {
    Here(Client),
    /// On the node named.
    Remote(Rc<RpcConnection>, String),
}

impl Resolved {
    fn is_down(&self) -> bool {
        matches!(self, Self::Remote(connection, _) if connection.is_down())
    }
}

/// A stub's channel to an actor in a clustered namespace. Its first request finds the owner;
/// the stub stays with that instance, as a local stub stays with its container, until the
/// instance answers that the object moved, or the connection to it is lost (`MovedHook`).
/// A failure before the request reaches that instance is marked as not delivered
/// (`not_delivered`): a peer that forwards the request answers it so, and its caller may retry.
struct RoutedChannel {
    namespace: Weak<ActorNamespace>,
    id: KjOwn<ActorIdHandle>,
    persistent: Persistent,
    /// Whether its requests come from a peer (or the debug port).
    remote: bool,
    bound: Rc<RefCell<Option<Rc<Resolved>>>>,
}

impl Channel for RoutedChannel {
    fn start_request(
        &self,
        mut metadata: KjOwn<RequestMetadata>,
    ) -> Result<KjOwn<WorkerInterface>> {
        ffi::request_metadata_set_from_persistent_stub(metadata.as_mut(), self.persistent);
        let namespace = self.namespace.upgrade().ok_or_else(|| {
            not_delivered(kj::disconnected!("the Durable Object namespace is gone"))
        })?;
        let id = ffi::actor_id_clone(&self.id);
        let key = ffi::actor_id_key(&self.id);
        let bound = Rc::clone(&self.bound);
        let remote = self.remote;
        // A draining peer sent the request on here: a route back there predates its handoff.
        if remote
            && let Some(by) = Option::<String>::from(ffi::request_metadata_forwarded_by(&metadata))
            && let (Some(node), Some(unique_key)) = (namespace.cluster(), namespace.unique_key())
        {
            node.cluster.forget_route(unique_key, &key, &by);
        }
        // What made the request (`Cause`): as the peer says, or the work of the actor making it
        // here.
        if !remote {
            ffi::stamp_current_actor_cause(metadata.as_mut());
        }
        let cause = Cause::of_request(&metadata);
        let counted = remote.then(|| namespace.cluster().map(|node| node.peer_request()));
        Ok(PromisedInterface::new(async move {
            // A peer's request that waited for a handoff is sent on to where the object went, so
            // that it does not outlast its caller's retries. It binds nothing, so that the peer's
            // next request is answered `moved` and finds the object itself (`resolve`). Any request
            // a draining instance sends to a peer names it, so that the peer does not route it back
            // here on a route from before the handoff.
            let mut forward = false;
            let resolved = loop {
                // A resolution over a session known to be down would fail unsent: resolve again.
                let live = bound
                    .borrow()
                    .clone()
                    .filter(|resolved| !resolved.is_down());
                let resolved = if let Some(resolved) = live.filter(|_| !forward) {
                    resolved
                } else {
                    let resolved = namespace.resolve(ffi::actor_id_clone(&id), remote && !forward);
                    let resolved = Rc::new(resolved.await.map_err(not_delivered)?);
                    let mut bound = bound.borrow_mut();
                    match &*bound {
                        _ if forward => resolved,
                        Some(other) if !other.is_down() => Rc::clone(other),
                        _ => Rc::clone(bound.insert(resolved)),
                    }
                };
                let Resolved::Here(container) = &*resolved else {
                    break resolved;
                };
                let Some(mut handoff) = container
                    .handoff()
                    .filter(|_| !cause.admitted_by(&namespace))
                else {
                    let mut heat = container.heat.get();
                    heat.hit(cluster::now_ms());
                    container.heat.set(heat);
                    break resolved;
                };
                // Nothing ran: once the object is gone, the request follows it.
                let _ = handoff.wait_for(|done| *done).await;
                forget(&bound, &resolved);
                forward = remote;
            };
            if matches!(&*resolved, Resolved::Remote(..))
                && let Some(node) = namespace.cluster()
                && node.cluster.is_draining()
            {
                ffi::request_metadata_set_forwarded_by(metadata.as_mut(), node.cluster.node());
            }
            let hook = MovedHook {
                namespace: Rc::downgrade(&namespace),
                key,
                bound,
                tried: Rc::clone(&resolved),
            };
            match &*resolved {
                Resolved::Here(container) => match container.start_request(metadata) {
                    Ok(interface) => {
                        let interface = attach(interface, container.count_cause(cause));
                        Ok(ffi::worker_interface_watch_moved(interface, Box::new(hook)))
                    }
                    Err(e) => {
                        if is_moved(&e) {
                            hook.moved();
                        }
                        Err(e)
                    }
                },
                Resolved::Remote(connection, _) => Ok(ffi::worker_interface_watch_peer(
                    attach(connection.start_request(metadata), Rc::clone(connection)),
                    Box::new(hook),
                )),
            }
            .map(|interface| attach(interface, counted))
        })
        .into_kj())
    }

    fn token(&self, usage: TokenUsage) -> Result<KjOwn<PendingToken>> {
        let namespace = self
            .namespace
            .upgrade()
            .ok_or_else(|| kj::disconnected!("the Durable Object namespace is gone"))?;
        let unique_key = namespace
            .unique_key()
            .ok_or_else(|| kj::failed!("only durable actors have channel tokens"))?;
        Ok(ffi::factory_encode_actor_token(
            namespace.factory.raw(),
            unique_key,
            &self.id,
            self.persistent,
            usage,
        )?)
    }

    fn evict_for_test(&self, hibernate: bool) -> LocalBoxFuture<'_, Result<()>> {
        Box::pin(async move {
            let namespace = self
                .namespace
                .upgrade()
                .ok_or_else(|| kj::disconnected!("the Durable Object namespace is gone"))?;
            match namespace
                .resolve(ffi::actor_id_clone(&self.id), false)
                .await?
            {
                Resolved::Here(container) => container.evict_for_test(hibernate).await,
                Resolved::Remote(..) => Err(kj::failed!(
                    "jsg.Error: Cannot evict a Durable Object another instance owns."
                )),
            }
        })
    }
}

/// What a stub's request does when the instance it reached answers that the object moved, or
/// the connection to it is lost.
///
/// The stub forgets that resolution, so that the stub's next request, or a retry of this one,
/// resolves the object again. On a move the cluster also forgets its route there.
pub struct MovedHook {
    namespace: Weak<ActorNamespace>,
    key: String,
    bound: Rc<RefCell<Option<Rc<Resolved>>>>,
    /// The resolution the request used.
    tried: Rc<Resolved>,
}

/// Clears a channel's resolution if it is still `tried`.
fn forget(bound: &RefCell<Option<Rc<Resolved>>>, tried: &Rc<Resolved>) {
    let mut bound = bound.borrow_mut();
    if bound.as_ref().is_some_and(|b| Rc::ptr_eq(b, tried)) {
        *bound = None;
    }
}

impl MovedHook {
    pub fn moved(&self) {
        self.lost();
        if let Resolved::Remote(_, node) = &*self.tried
            && let Some(namespace) = self.namespace.upgrade()
            && let (Some(cluster), Some(unique_key)) = (namespace.cluster(), namespace.unique_key())
        {
            cluster.cluster.forget_route(unique_key, &self.key, node);
        }
    }

    /// The connection to the instance was lost.
    pub fn lost(&self) {
        forget(&self.bound, &self.tried);
    }
}

// =====================================================================================
// Containers

/// The class an actor is constructed from and its id: known up front for a root actor, resolved
/// through the parent actor's `FacetStart` for a facet.
enum ClassState {
    Ready {
        class: Rc<dyn ActorClass>,
        id: KjOwn<ActorIdHandle>,
    },
    /// Resolving. Every waiter awaits a clone; the future itself stores `Ready` into the
    /// container when it completes.
    Pending(Shared<LocalBoxFuture<'static, Result<()>>>),
}

/// The slot of one actor: its key in the namespace (or in its parent's facets) and the actor
/// while it runs.
///
/// The container also holds what survives the actor's eviction: its hibernation manager, its
/// facets, and the broken reason once it broke.
pub struct ActorContainer {
    this: Weak<Self>,
    key: String,
    namespace: Weak<ActorNamespace>,
    /// None for a root actor.
    parent: Option<Weak<Self>>,
    /// The key of the root actor whose storage this one shares: its own for a root.
    root_key: String,
    /// Shared by the whole facet tree: the namespace expires the tree as one.
    last_access: Rc<Cell<Duration>>,
    class_and_id: RefCell<ClassState>,
    /// The running actor: `None` until the first request, and again after an eviction. The `Rc`
    /// is for the on-broken monitor and a request being started, which borrow the handle; it
    /// counts no request, so it does not block the actor's shutdown.
    actor: RefCell<Option<Rc<KjOwn<ffi::ActorHandle>>>>,
    /// The hibernation manager of the evicted actor, for the actor that replaces it.
    manager: RefCell<Option<KjRc<ffi::HibernationManager>>>,
    /// Set once the actor broke or was aborted; every later use of the container fails with it.
    broken_reason: RefCell<Option<crate::Error>>,
    facets: RefCell<LinkedHashMap<String, Rc<Self>>>,
    /// A facet's number within the root's storage, once looked up. The root has none.
    facet_id: Cell<Option<u32>>,
    /// Whether a request holds the actor, per the actor's `ActorHooks`.
    active: Cell<bool>,
    /// Test evictions waiting for the actor to become inactive.
    idle: Notify,
    on_broken_task: RefCell<Option<TaskHandle>>,
    /// The idle shutdown, armed when the last request ends and cancelled by the next.
    shutdown_task: RefCell<Option<TaskHandle>>,
    /// A committed hook-bearing retirement. The container owns the task even if every eviction
    /// caller goes away; requests and concurrent evictions wait for its shared result.
    retirement: RefCell<Option<Shared<RemoteHandle<Result<()>>>>>,
    /// A clustered root's ownership, for as long as the container is in its namespace's map.
    owned: RefCell<Option<Owned>>,
    /// Set as a clustered root is handed off (`hand_off`), and true once it is gone.
    handoff: RefCell<Option<watch::Sender<bool>>>,
    /// How busy a clustered root has been: a draining instance hands off the busiest first.
    heat: Cell<Heat>,
    /// What made a clustered root's requests in flight through its stubs, which the requests its
    /// work makes carry on (`Cause`).
    causes: RefCell<Causes>,
}

impl ActorContainer {
    fn new_root(key: String, ns: &ActorNamespace, id: KjOwn<ActorIdHandle>) -> Rc<Self> {
        let class = Rc::clone(&ns.actor_class);
        Rc::new_cyclic(|this| {
            Self::init(
                Weak::clone(this),
                key.clone(),
                Weak::clone(&ns.this),
                None,
                key,
                Rc::new(Cell::new(ns.factory.now())),
                ClassState::Ready { class, id },
            )
        })
    }

    /// A facet of `parent`, whose class and id `start` resolves.
    fn new_facet(name: String, parent: &Self, start: KjOwn<FacetStart>) -> Rc<Self> {
        Rc::new_cyclic(|this| {
            let pending = resolve_facet_start(Weak::clone(this), start)
                .boxed_local()
                .shared();
            Self::init(
                Weak::clone(this),
                name,
                Weak::clone(&parent.namespace),
                Some(Weak::clone(&parent.this)),
                parent.root_key.clone(),
                Rc::clone(&parent.last_access),
                ClassState::Pending(pending),
            )
        })
    }

    fn init(
        this: Weak<Self>,
        key: String,
        namespace: Weak<ActorNamespace>,
        parent: Option<Weak<Self>>,
        root_key: String,
        last_access: Rc<Cell<Duration>>,
        class_and_id: ClassState,
    ) -> Self {
        Self {
            this,
            key,
            namespace,
            parent,
            root_key,
            last_access,
            class_and_id: RefCell::new(class_and_id),
            actor: RefCell::new(None),
            manager: RefCell::new(None),
            broken_reason: RefCell::new(None),
            facets: RefCell::new(LinkedHashMap::new()),
            facet_id: Cell::new(None),
            active: Cell::new(false),
            idle: Notify::new(),
            on_broken_task: RefCell::new(None),
            shutdown_task: RefCell::new(None),
            retirement: RefCell::new(None),
            owned: RefCell::new(None),
            handoff: RefCell::new(None),
            heat: Cell::new(Heat::default()),
            causes: RefCell::new(Causes::default()),
        }
    }
}

impl ActorContainer {
    fn namespace(&self) -> Result<Rc<ActorNamespace>> {
        self.namespace
            .upgrade()
            .ok_or_else(|| kj::disconnected!("the Durable Object namespace is gone"))
    }

    /// The parent of a facet. An error for a facet whose parent is gone; `None` for a root.
    fn parent(&self) -> Result<Option<Rc<Self>>> {
        match &self.parent {
            None => Ok(None),
            Some(parent) => parent
                .upgrade()
                .map(Some)
                .ok_or_else(|| kj::disconnected!("the facet's parent actor is gone")),
        }
    }

    fn is_root(&self) -> bool {
        self.parent.is_none()
    }

    /// The root of the facet tree; a facet whose parent is gone stands for it.
    fn root(self: &Rc<Self>) -> Rc<Self> {
        let mut container = Rc::clone(self);
        while let Ok(Some(parent)) = container.parent() {
            container = parent;
        }
        container
    }

    /// The object's name in a `Cause`: its namespace's hash and its root's key. None outside a
    /// durable namespace.
    fn cause_name(&self) -> Option<String> {
        let ns = self.namespace.upgrade()?;
        let unique_key = ns.unique_key()?;
        Some(format!(
            "{}/{}",
            cluster::namespace_hash(unique_key),
            self.root_key
        ))
    }

    /// What made the requests a root's work makes: the root itself, being handed off or not,
    /// and what made its requests in flight.
    fn cause(&self) -> Cause {
        let causes = self.causes.borrow();
        let own = self.cause_name();
        let mut objects: Vec<String> = own.iter().cloned().collect();
        let others = causes
            .objects
            .keys()
            .filter(|object| Some(*object) != own.as_ref());
        objects.extend(others.take(MAX_CAUSES - objects.len()).cloned());
        Cause {
            from_handoff: self.handoff.borrow().is_some() || causes.from_handoff > 0,
            objects,
        }
    }

    /// Counts `cause` among those of the root's requests in flight while the returned guard
    /// lives.
    fn count_cause(self: &Rc<Self>, cause: Cause) -> CountedCause {
        let mut causes = self.causes.borrow_mut();
        causes.from_handoff += usize::from(cause.from_handoff);
        for object in &cause.objects {
            *causes.objects.entry(object.clone()).or_default() += 1;
        }
        CountedCause {
            container: Rc::downgrade(self),
            cause,
        }
    }

    /// Whether the object is being handed off.
    fn is_handed_off(&self) -> bool {
        self.handoff.borrow().is_some()
    }

    fn require_not_broken(&self) -> Result<()> {
        match &*self.broken_reason.borrow() {
            Some(reason) => Err(reason.clone()),
            None => Ok(()),
        }
    }

    /// The pending class resolution of a facet, if it has not resolved yet.
    fn pending_class(&self) -> Option<Shared<LocalBoxFuture<'static, Result<()>>>> {
        match &*self.class_and_id.borrow() {
            ClassState::Pending(pending) => Some(pending.clone()),
            ClassState::Ready { .. } => None,
        }
    }

    fn class(&self) -> Result<Rc<dyn ActorClass>> {
        match &*self.class_and_id.borrow() {
            ClassState::Ready { class, .. } => Ok(Rc::clone(class)),
            ClassState::Pending(_) => Err(kj::failed!("the facet's class is not resolved yet")),
        }
    }

    fn id(&self) -> Result<KjOwn<ActorIdHandle>> {
        match &*self.class_and_id.borrow() {
            ClassState::Ready { id, .. } => Ok(ffi::actor_id_clone(id)),
            ClassState::Pending(_) => Err(kj::failed!("the facet's id is not resolved yet")),
        }
    }

    fn update_access_time(&self) {
        if let Some(ns) = self.namespace.upgrade() {
            self.last_access.set(ns.factory.now());
        }
    }

    /// Whether anything but its map holds this container or one of its facets: a stub's
    /// channel, a request in flight, an eviction in progress.
    fn has_clients(&self) -> bool {
        Weak::strong_count(&self.this) > 1
            || self
                .facets
                .borrow()
                .values()
                .any(|facet| facet.has_clients())
    }

    /// Whether a running actor exists.
    fn is_running(&self) -> bool {
        self.actor.borrow().is_some()
    }

    /// Keeps the running actor's hibernation manager for the actor that replaces it.
    fn save_manager(&self) {
        if let Some(actor) = &*self.actor.borrow()
            && let Some(manager) = Option::from(ffi::actor_hibernation_manager(actor))
        {
            *self.manager.borrow_mut() = Some(manager);
        }
    }

    /// Spawns a background task of the container; none once its namespace is gone.
    fn spawn(&self, future: impl Future<Output = ()> + 'static) -> Option<TaskHandle> {
        Some(self.namespace.upgrade()?.factory.spawn(future))
    }

    fn cancel_on_broken(&self) {
        drop(self.on_broken_task.borrow_mut().take());
    }

    fn cancel_shutdown(&self) {
        drop(self.shutdown_task.borrow_mut().take());
    }

    /// The running actor, started first if it is not running.
    async fn get_actor(&self) -> Result<Rc<KjOwn<ffi::ActorHandle>>> {
        loop {
            let retirement = self.retirement.borrow().clone();
            if let Some(retirement) = retirement {
                // A request that the onHibernate handler makes to its own object waits here too,
                // until the handler's wall-clock budget cuts it off. A successor may have started
                // and begun retiring while this waited.
                retirement.await?;
                continue;
            }
            self.require_not_broken()?;
            if let Some(actor) = self.actor.borrow().clone() {
                return Ok(actor);
            }
            if let Some(pending) = self.pending_class() {
                pending.await?;
                self.require_not_broken()?;
            }
            let class = self.class()?;
            class.when_ready().await?;
            self.require_not_broken()?;
            self.prepare_storage().await?;
            self.require_not_broken()?;
            if self.retirement.borrow().is_some() {
                continue;
            }
            // A concurrent request may have started the actor meanwhile.
            if !self.is_running() {
                self.start(&class)?;
            }
            return self
                .actor
                .borrow()
                .clone()
                .ok_or_else(|| kj::failed!("the actor did not start"));
        }
    }

    /// In cluster mode, readies the actor's database before the actor opens it: it is taken over
    /// (recovered from earlier epochs). An actor whose takeover fails leaves its namespace, so the
    /// next request claims anew.
    async fn prepare_storage(&self) -> Result<()> {
        let ns = self.namespace()?;
        let Some(node) = ns.cluster() else {
            return Ok(());
        };
        if self.is_root() {
            let layout = Rc::new(Databases {
                ns: Rc::clone(&ns),
                root_key: self.root_key.clone(),
            });
            let restoring = self
                .owned
                .borrow()
                .as_ref()
                .map(|o| (o.restore(layout), o.retire()));
            let Some((restore, retire)) = restoring else {
                return Err(kj::failed!("a clustered actor without ownership"));
            };
            if let Err(e) = restore.await {
                self.abort(Some(e.clone()));
                ns.remove_container(self);
                return Err(e);
            }
            // Released meanwhile: its ownership went before the restore opened the object here.
            // Unless another container serves the object now, what the restore opened goes too.
            if self.broken_reason.borrow().is_some() && ns.owned_container(&self.key).is_none() {
                retire.run();
            }
        } else {
            // A facet runs once its root has taken the object over: its database is a lane of
            // the root's log.
            let path = ns.sqlite_path(&self.root_key, self.facet_id()?)?;
            let lane = self.lane_prefix()? + cluster::DB;
            let unique_key = ns.unique_key().unwrap_or_default();
            node.cluster
                .open_lane(unique_key, &self.root_key, &path, &lane)
                .map_err(|e| kj::failed!("{e}"))?;
        }
        Ok(())
    }

    /// The prefix of the lanes of this actor's subtree in its object's log: empty for the root.
    fn lane_prefix(&self) -> Result<String> {
        Ok(match self.parent()? {
            Some(parent) => parent.lane_prefix()? + &cluster::facet_prefix([self.key.as_str()]),
            None => String::new(),
        })
    }

    /// Constructs the actor and starts watching it for breakage.
    fn start(&self, class: &Rc<dyn ActorClass>) -> Result<()> {
        let ns = self.namespace()?;
        let id = self.id()?;
        let storage = ns.storage()?;
        let spec = ffi::ActorStorageSpec {
            durable: ns.is_durable(),
            enable_sql: ns.enable_sql(),
            root_key: self.root_key.clone(),
            facet_id: self.facet_id()?.into(),
            replicated: ns.cluster().is_some(),
        };
        let actor = Rc::new(class.new_actor(NewActor {
            id,
            storage,
            spec,
            hooks: Box::new(ActorHooks(Weak::clone(&self.this))),
            container: ns.container(),
            hibernation_manager: self.manager.borrow_mut().take(),
        })?);
        let monitor = self.spawn(monitor_on_broken(
            Weak::clone(&self.this),
            ffi::actor_on_broken(&actor),
        ));
        *self.on_broken_task.borrow_mut() = monitor;
        *self.actor.borrow_mut() = Some(actor);
        Ok(())
    }

    /// Starts a request on the actor, starting the actor first if it is not running. The request
    /// is a client of the container from here until its interface is dropped.
    pub fn start_request(
        self: &Rc<Self>,
        metadata: KjOwn<RequestMetadata>,
    ) -> Result<KjOwn<WorkerInterface>> {
        self.require_not_broken()?;
        if let Some(ns) = self.namespace.upgrade() {
            ns.ensure_cleanup_loop();
        }
        let client = Client(Rc::clone(self));
        let running = if self.retirement.borrow().is_none() {
            self.actor.borrow().clone()
        } else {
            None
        };
        if let Some(actor) = running {
            return client.request_on(metadata, &actor);
        }
        Ok(PromisedInterface::new(async move {
            let actor = client.get_actor().await?;
            client.request_on(metadata, &actor)
        })
        .into_kj())
    }
}

impl Client {
    /// Starts this client's request on `actor`, the container's running actor.
    fn request_on(
        self,
        mut metadata: KjOwn<RequestMetadata>,
        actor: &ffi::ActorHandle,
    ) -> Result<KjOwn<WorkerInterface>> {
        let ns = self.namespace()?;
        ns.require_lease()?;
        let inner = {
            let state = self.class_and_id.borrow();
            let ClassState::Ready { class, id } = &*state else {
                return Err(kj::failed!("the facet's class is not resolved yet"));
            };
            if self.is_root() {
                // A root actor's requests get a self token that names the actor itself, and
                // must not take one from the caller: a caller-supplied factory could read and
                // manipulate the parameters of the actor's own `restore()`. The token names the
                // namespace and id only, so that the request holds no reference back to the
                // container that would block eviction. A facet is only ever called by its
                // parent, or through a restored channel the parent set up, which brings its own
                // trusted factory.
                ns.set_self_token(&mut metadata, id);
            }
            class.start_request(metadata, actor)?
        };
        // The interface is a client of the container while it lives. It does not keep the actor
        // active: the actor's own reference accounting does that, so that an interface living on
        // with an open WebSocket leaves the actor free to hibernate.
        Ok(attach(inner, self))
    }
}

impl ActorContainer {
    /// The actor's first request started: it cancels the pending idle shutdown.
    fn active(&self) {
        self.active.set(true);
        self.cancel_shutdown();
    }

    /// The actor's last request ended: arms the idle shutdown.
    fn inactive(&self) {
        self.active.set(false);
        self.update_access_time();
        self.idle.notify_waiters();
        let Some(ns) = self.namespace.upgrade() else {
            return;
        };
        if !ns.is_evictable() || self.retirement.borrow().is_some() {
            return;
        }
        let factory = Rc::clone(&ns.factory);
        let container = Weak::clone(&self.this);
        let task = self.spawn(async move {
            factory.sleep(IDLE_SHUTDOWN_DELAY).await;
            if let Some(container) = container.upgrade() {
                container.shutdown_after_idle().await;
            }
        });
        *self.shutdown_task.borrow_mut() = task;
    }
}

impl ActorContainer {
    /// Evicts the actor after `IDLE_SHUTDOWN_DELAY` without requests: hibernates its WebSockets
    /// and shuts it down. The container keeps its storage, facets and hibernation manager, so
    /// the next request rebuilds the actor as it was.
    async fn shutdown_after_idle(&self) {
        match self
            .try_evict("broken.dropped; Actor freed due to inactivity", true)
            .await
        {
            Ok(true) => {}
            // Something still holds the actor although its requests have all ended: the
            // request accounting is off. Keeping the actor is the safe choice; dropping it
            // could leave two instances of the same actor alive.
            Ok(false) => tracing::error!(
                "Detected internal bug in hibernation: Durable Object has strong references \
                 when hibernation timeout expired."
            ),
            Err(error) => tracing::error!(
                "shutting down an idle Durable Object failed: {}",
                error.description()
            ),
        }
    }

    /// The actor broke: `reason` becomes every later call's error, the facets are aborted and
    /// the container leaves its map, dropping the hibernation manager (which disconnects the
    /// hibernated WebSockets).
    fn broken(self: &Rc<Self>, reason: &crate::Error) {
        *self.broken_reason.borrow_mut() = Some(reason.clone());
        let facets = std::mem::take(&mut *self.facets.borrow_mut());
        for facet in facets.values() {
            facet.abort(Some(reason.clone()));
        }
        drop(facets);
        // The monitor is the task running this, and ends with it; its handle has nothing left to
        // cancel.
        drop(self.on_broken_task.borrow_mut().take());
        self.cancel_shutdown();
        // Hollow the container out: a stub still holding it must not keep these alive.
        // `get_actor()` fails from now on, so nothing recreates the actor.
        let actor = self.actor.borrow_mut().take();
        let manager = self.manager.borrow_mut().take();
        match self.parent.as_ref().and_then(Weak::upgrade) {
            Some(parent) => {
                parent.facets.borrow_mut().remove(&self.key);
            }
            None => {
                if let Some(ns) = self.namespace.upgrade() {
                    ns.remove_container(self);
                }
            }
        }
        drop(manager);
        drop(self.release(actor.as_deref()));
        drop(actor);
    }

    /// Closes a clustered actor's database and takes its ownership for the caller to give up,
    /// so nothing writes behind the next owner.
    fn release(&self, actor: Option<&KjOwn<ffi::ActorHandle>>) -> Option<Owned> {
        if let Some(actor) = actor
            && self
                .namespace
                .upgrade()
                .is_some_and(|ns| ns.cluster().is_some())
        {
            ffi::actor_close_storage(actor);
        }
        self.owned.borrow_mut().take()
    }

    /// Aborts the actor and its facets; every request on them fails with `reason`. Without one
    /// the actors are shut down, failing nothing in flight except a running onHibernate handler:
    /// that is canceled with an error, which the evictions and requests waiting for it receive.
    /// Later requests fail for an unknown reason. The caller removes the container from any map
    /// that could route traffic to it: at most call sites the map is at hand, and `abort` on a
    /// facet's tree leaves the facets filed under their parent.
    fn abort(&self, reason: Option<crate::Error>) {
        self.abort_then(reason, None);
    }

    /// `abort`, with `later`, if given, the error of every request after it.
    fn abort_then(&self, reason: Option<crate::Error>, later: Option<crate::Error>) {
        drop(self.stop(reason, later));
    }

    /// `abort_then`, which gives the ownership the actor held to the caller.
    fn stop(&self, reason: Option<crate::Error>, later: Option<crate::Error>) -> Option<Owned> {
        if self.broken_reason.borrow().is_some() {
            return None;
        }
        let actor = self.actor.borrow_mut().take();
        if let Some(actor) = &actor {
            // Only an error cancels a committed hook, which must stop before ownership is released.
            let actor_reason = reason.clone().or_else(|| {
                self.retirement.borrow().as_ref().map(|_| {
                    kj::disconnected!(
                        "the Durable Object was shut down during its onHibernate handler"
                    )
                })
            });
            ffi::actor_abort(actor, &AbortReason(actor_reason));
        }
        for facet in self.facets.borrow().values() {
            facet.abort(reason.clone());
        }
        self.cancel_on_broken();
        self.cancel_shutdown();
        *self.manager.borrow_mut() = None;
        *self.broken_reason.borrow_mut() = Some(
            later
                .or(reason)
                .unwrap_or_else(|| kj::failed!("jsg.Error: Actor aborted for unknown reason.")),
        );
        let owned = self.release(actor.as_deref());
        drop(actor);
        owned
    }

    /// Whether this actor or a facet of it has a request in flight.
    fn tree_active(&self) -> bool {
        self.active.get() || self.facets.borrow().values().any(|f| f.tree_active())
    }

    /// Set while the object is handed off; true once it is gone.
    fn handoff(&self) -> Option<watch::Receiver<bool>> {
        self.handoff.borrow().as_ref().map(watch::Sender::subscribe)
    }

    /// The error of a request still in flight as the object stops for its handoff: it has
    /// reached the object, so it fails as one that may have run, and is not sent again.
    fn cancelled(&self) -> Option<crate::Error> {
        self.tree_active().then(|| {
            kj::disconnected!(
                "{}: this node is draining, and the request outlasted its handoff",
                self.key
            )
        })
    }

    /// Hands a clustered root off as this instance drains, once `handoffs` marked it. Its
    /// requests in flight may finish until `in_flight`, and are cancelled then. The object then
    /// stops and, with one of the `slots`, its Docker container stops and its ownership is
    /// released to the next claim anywhere (`Owned::hand_off`).
    async fn hand_off(
        self: Rc<Self>,
        ns: Weak<ActorNamespace>,
        in_flight: Instant,
        slots: Rc<tokio::sync::Semaphore>,
    ) {
        // However the handoff ends, the requests waiting for it go on.
        let _done = HandedOff(&self);
        while self.tree_active() && Instant::now() < in_flight {
            tokio::time::sleep(HANDOFF_POLL).await;
        }
        let later = moved(&format!("{}: this node is draining", self.key));
        let owned = self.stop(self.cancelled(), Some(later));
        if let Some(ns) = ns.upgrade() {
            ns.remove_container(&self);
            let Ok(_slot) = slots.acquire().await else {
                return;
            };
            let unique_key = ns.unique_key().unwrap_or_default();
            if ns.container().is_some()
                && let Err(e) =
                    ffi::factory_stop_container(ns.factory.raw(), unique_key, &self.key).await
            {
                let e = crate::Error::from(e);
                tracing::warn!("stopping {}'s container: {}", self.key, e.description());
            }
            if let Some(owned) = owned
                && owned.hand_off().await
            {
                ns.clear_storage(&self.key);
            }
        }
    }

    /// Resets the actor's SQLite database while its connection is open, ahead of an abort that
    /// closes it.
    fn reset_storage(&self) {
        if let Some(actor) = &*self.actor.borrow() {
            ffi::actor_reset_storage(actor);
        }
    }

    /// Test hook: evicts the actor now, bypassing the idle delay. Fails if the actor is not
    /// running (never started, or already evicted). Waits for in-flight requests to drain first.
    fn evict_for_test(self: &Rc<Self>, hibernate: bool) -> LocalBoxFuture<'static, Result<()>> {
        let this = Rc::clone(self);
        Box::pin(async move {
            if !this.namespace()?.is_evictable() {
                return Err(kj::failed!(
                    "jsg.Error: Cannot evict Durable Object: its namespace has preventEviction set."
                ));
            }
            if !this.is_running() {
                return Err(kj::failed!(
                    "jsg.Error: Cannot evict Durable Object: it is not currently running."
                ));
            }
            this.evict_when_idle(hibernate).await
        })
    }

    /// Test hook: evicts the actor and its facets, those that are running; the bulk eviction
    /// must not fail on actors that are not.
    fn evict_tree_for_test_if_running(
        self: &Rc<Self>,
        hibernate: bool,
    ) -> LocalBoxFuture<'static, Result<()>> {
        let this = Rc::clone(self);
        Box::pin(async move {
            // Each eviction holds its own `Rc`: a broken actor may leave the map meanwhile.
            let facets: Vec<Rc<Self>> = this.facets.borrow().values().cloned().collect();
            let mut evictions: Vec<LocalBoxFuture<'static, Result<()>>> = facets
                .iter()
                .map(|facet| facet.evict_tree_for_test_if_running(hibernate))
                .collect();
            if this.is_running() {
                let this = Rc::clone(&this);
                evictions.push(Box::pin(
                    async move { this.evict_when_idle(hibernate).await },
                ));
            }
            futures::future::join_all(evictions)
                .await
                .into_iter()
                .collect::<Result<()>>()
        })
    }

    /// Waits for the actor to be idle, then evicts it. No live request is ever aborted, so while
    /// requests are in flight, or a just-ended one still holds the actor during its teardown,
    /// this polls; a fixed deadline keeps a request that never ends from hanging the test.
    async fn evict_when_idle(&self, hibernate: bool) -> Result<()> {
        let factory = Rc::clone(&self.namespace()?.factory);
        let deadline = factory.now() + EVICT_TIMEOUT;
        loop {
            if self
                .try_evict("broken.dropped; Actor evicted by test", hibernate)
                .await?
            {
                return Ok(());
            }
            let now = factory.now();
            if now >= deadline {
                return Err(kj::failed!("{EVICT_TIMEOUT_MESSAGE}"));
            }
            if self.active.get() {
                let idle = std::pin::pin!(self.idle.notified());
                let timeout = std::pin::pin!(factory.sleep(deadline.saturating_sub(now)));
                if let Either::Right(_) = futures::future::select(idle, timeout).await {
                    return Err(kj::failed!("{EVICT_TIMEOUT_MESSAGE}"));
                }
            } else {
                factory.sleep(EVICT_RETRY_DELAY).await;
            }
        }
    }

    /// Starts a root's hook only once it has no requests. Once committed, retirement belongs to
    /// the container, so canceling a test eviction or the idle timer cannot interrupt it.
    async fn try_evict(&self, reason: &str, hibernate: bool) -> Result<bool> {
        let retirement = self.retirement.borrow().clone();
        if let Some(retirement) = retirement {
            retirement.await?;
            return Ok(true);
        }
        let current = self.actor.borrow().clone();
        // The handler may be read only while the actor is idle.
        if self.is_root()
            && current
                .as_ref()
                .is_some_and(|actor| ffi::actor_is_shared(actor))
        {
            return Ok(false);
        }
        if let Some(actor) = current
            && self.is_root()
            && ffi::actor_has_on_hibernate_handler(&actor)
        {
            let ns = self.namespace()?;
            ns.require_lease()?;
            let class = self.class()?;
            let mut metadata = ffi::new_request_metadata(KjMaybe::None, KjMaybe::None);
            ns.set_self_token(&mut metadata, &*self.id()?);
            let container = Weak::clone(&self.this);
            let reason = reason.to_owned();
            let retirement = ns
                .factory
                .spawn(async move {
                    retire_actor(container, actor, class, metadata, reason, hibernate).await
                })
                .shared();
            *self.retirement.borrow_mut() = Some(retirement.clone());
            self.cancel_shutdown();
            retirement.await?;
            return Ok(true);
        }
        self.finish_eviction(reason, hibernate).await
    }

    /// Shuts the actor down, keeping its storage. Returns false without evicting if the actor
    /// acquired a strong reference meanwhile (a request raced in): an idle shutdown is cancelled
    /// by a new request, but a test eviction is not cancellable, so it relies on that re-check.
    /// The only suspension is the wait for the isolate lock that hibernating WebSockets takes;
    /// the shutdown and the clearing of the slot after it are one step, so no request finds a
    /// shut-down actor in the slot. The on-broken monitor is cancelled only once the shutdown
    /// is committed, so that an early `false` leaves the actor watched.
    async fn finish_eviction(&self, reason: &str, hibernate: bool) -> Result<bool> {
        let current = self.actor.borrow().clone();
        if let Some(actor) = &current {
            if hibernate {
                self.save_manager();
            }
            let lock = if hibernate && self.manager.borrow().is_some() {
                Some(ffi::actor_lock(actor).await?)
            } else {
                None
            };
            // The slot may have changed while the lock was awaited.
            match &*self.actor.borrow() {
                Some(now) if Rc::ptr_eq(now, actor) => {}
                Some(_) => return Ok(false),
                None => return Ok(true),
            }
            if !ffi::actor_shutdown(actor, reason, lock.into())? {
                return Ok(false);
            }
        }
        self.cancel_on_broken();
        self.cancel_shutdown();
        *self.actor.borrow_mut() = None;
        if !hibernate {
            *self.manager.borrow_mut() = None;
        }
        Ok(true)
    }
}

async fn retire_actor(
    container: Weak<ActorContainer>,
    actor: Rc<KjOwn<ffi::ActorHandle>>,
    class: Rc<dyn ActorClass>,
    metadata: KjOwn<RequestMetadata>,
    reason: String,
    hibernate: bool,
) -> Result<()> {
    let hook = class.run_on_hibernate(metadata, &actor).await;
    let container = container
        .upgrade()
        .ok_or_else(|| kj::disconnected!("the retiring actor's container is gone"))?;
    let result = async {
        hook?;
        container.require_not_broken()?;
        if !container.finish_eviction(&reason, hibernate).await? {
            return Err(kj::failed!("a retiring actor acquired a strong reference"));
        }
        Ok(())
    }
    .await;
    // An already-broken container has left its map; breaking it again could remove a replacement.
    if let Err(error) = &result
        && container.require_not_broken().is_ok()
    {
        ffi::actor_abort(&actor, &AbortReason(Some(error.clone())));
        container.broken(error);
    }
    // Close the old actor's storage before releasing requests to its successor.
    drop(actor);
    drop(container.retirement.borrow_mut().take());
    result
}

/// What made a request, for the handoffs it may reach: whether the work of an object being
/// handed off made it, directly or through other objects' requests, and the objects whose work
/// made it. An object being handed off admits a request that may be part of a handoff's work
/// (`admitted_by`), so that no object's work waits on another's handoff, nor on its own; the
/// others wait for it to be gone, so that new work does not keep it from leaving.
///
/// The requests an actor makes share its context, and so cannot tell which of its requests in
/// flight they serve: each carries what made any of them. While an object serves a request of a
/// handoff's work, every request it makes is admitted.
#[derive(Clone, Default)]
struct Cause {
    from_handoff: bool,
    /// Named `<namespace hash>/<root key>` (`ActorContainer::cause_name`); at most `MAX_CAUSES`.
    objects: Vec<String>,
}

/// The most objects a request names as having made it. A request made through more names only
/// some, and waits for the handoff of an object that only the others lead back to.
const MAX_CAUSES: usize = 32;

impl Cause {
    fn of_request(metadata: &RequestMetadata) -> Self {
        let mut objects = ffi::request_metadata_caused_by(metadata);
        objects.truncate(MAX_CAUSES);
        Self {
            from_handoff: ffi::request_metadata_from_handoff(metadata),
            objects,
        }
    }

    fn stamp(&self, metadata: Pin<&mut RequestMetadata>) {
        ffi::request_metadata_set_cause(metadata, self.from_handoff, self.objects.clone());
    }

    /// Whether an object `namespace` hands off takes the request: the work of an object being
    /// handed off made it, on this instance or, as the request says, on another.
    fn admitted_by(&self, namespace: &ActorNamespace) -> bool {
        self.from_handoff
            || namespace
                .cluster()
                .is_some_and(|node| self.objects.iter().any(|object| node.hands_off(object)))
    }
}

/// The causes of a root's requests in flight, each counted once per request.
#[derive(Default)]
struct Causes {
    from_handoff: usize,
    objects: HashMap<String, usize>,
}

/// Counts a request's `Cause` among its object's until the request ends.
struct CountedCause {
    container: Weak<ActorContainer>,
    cause: Cause,
}

impl Drop for CountedCause {
    fn drop(&mut self) {
        let Some(container) = self.container.upgrade() else {
            return;
        };
        let mut causes = container.causes.borrow_mut();
        causes.from_handoff -= usize::from(self.cause.from_handoff);
        for object in &self.cause.objects {
            if let Entry::Occupied(mut count) = causes.objects.entry(object.clone()) {
                *count.get_mut() -= 1;
                if *count.get() == 0 {
                    count.remove();
                }
            }
        }
    }
}

/// Wakes the requests waiting for a handoff once it ends.
struct HandedOff<'a>(&'a ActorContainer);

impl Drop for HandedOff<'_> {
    fn drop(&mut self) {
        if let Some(done) = &*self.0.handoff.borrow() {
            done.send_replace(true);
        }
    }
}

/// Watches the actor; when it breaks, the container records the reason and leaves its map. The
/// task holds no reference to the actor: a cancelled task's future is dropped only at its next
/// poll, and the actor must close its storage when the container drops it (a deleted facet's
/// files are removed in the same turn, which Windows refuses while they are open).
async fn monitor_on_broken(container: Weak<ActorContainer>, broken: KjOwn<ffi::ActorBroken>) {
    // `broken` only ever rejects; an actor that never breaks leaves this task to be
    // cancelled with the container.
    let reason: crate::Error = match ffi::actor_broken(broken).await {
        Ok(()) => kj::failed!("actor.onBroken() resolved normally?"),
        Err(error) => error.into(),
    };
    if let Some(container) = container.upgrade() {
        container.broken(&reason);
    }
}

/// Resolves a facet's class and id through its parent's `FacetStart` and stores them in the
/// facet's container.
async fn resolve_facet_start(
    container: Weak<ActorContainer>,
    start: KjOwn<FacetStart>,
) -> Result<()> {
    let ffi::FacetStartInfo { id, actor_class } = ffi::facet_start_resolve(start).await?;
    if let Some(container) = container.upgrade() {
        let class = actor_class.0;
        *container.class_and_id.borrow_mut() = ClassState::Ready { class, id };
    }
    Ok(())
}

impl Drop for ActorContainer {
    fn drop(&mut self) {
        for facet in self.facets.get_mut().values() {
            facet.abort(None);
        }
        let actor = self.actor.get_mut().take();
        if let Some(actor) = &actor {
            ffi::actor_abort(actor, &AbortReason(None));
        }
        drop(self.release(actor.as_deref()));
    }
}

// =====================================================================================
// Facets

impl ActorContainer {
    /// The facet `name`, created with `start` if new. An existing facet's `start` is dropped
    /// unused.
    fn facet_container(&self, name: &str, start: KjOwn<FacetStart>) -> Rc<Self> {
        let mut facets = self.facets.borrow_mut();
        if let Some(facet) = facets.get(name) {
            return Rc::clone(facet);
        }
        let facet = Self::new_facet(name.to_owned(), self, start);
        facets.insert(name.to_owned(), Rc::clone(&facet));
        facet
    }

    /// This facet's number within the root's storage; `None` for the root, and for a facet of
    /// an in-memory namespace, whose storage numbers nothing. Looked up (and allocated if new) on
    /// first use.
    fn facet_id(&self) -> Result<Option<u32>> {
        if let Some(id) = self.facet_id.get() {
            return Ok(Some(id));
        }
        let Some(parent) = self.parent()? else {
            return Ok(None);
        };
        let ns = self.namespace()?;
        let id: Option<u32> = ffi::actor_storage_facet_id(
            ns.storage()?,
            &self.root_key,
            parent.facet_id()?.into(),
            &self.key,
        )?
        .into();
        self.facet_id.set(id);
        Ok(id)
    }

    /// Aborts the facet `name` with `reason` and drops it, if it is running.
    fn abort_facet(&self, name: &str, reason: crate::Error) {
        let facet = self.facets.borrow_mut().remove(name);
        if let Some(facet) = facet {
            facet.abort(Some(reason));
        }
    }

    /// Whether a stub to this actor may be serialized: facets and ephemeral actors cannot be.
    fn require_transferable(&self) -> Result<()> {
        if !self.is_root() {
            return Err(kj::failed!(
                "jsg.DOMException(DataCloneError): Stubs pointing to Durable Object facets are \
                 not serializable."
            ));
        }
        if !self.namespace()?.is_durable() {
            return Err(kj::failed!(
                "jsg.DOMException(DataCloneError): Stubs pointing to ephemeral objects are not \
                 serializable."
            ));
        }
        Ok(())
    }

    /// The channel token that restores a stub to this actor, as the runtime encodes it. Only a
    /// root actor has one, and a root's class and id are known from the start.
    fn token(&self, usage: TokenUsage, persistent: Persistent) -> Result<KjOwn<PendingToken>> {
        self.require_transferable()?;
        let ns = self.namespace()?;
        let unique_key = ns
            .unique_key()
            .ok_or_else(|| kj::failed!("only durable actors have channel tokens"))?;
        let id = self.id()?;
        Ok(ffi::factory_encode_actor_token(
            ns.factory.raw(),
            unique_key,
            &id,
            persistent,
            usage,
        )?)
    }
}

impl ActorContainer {
    fn depth(&self) -> u32 {
        match self.parent.as_ref().and_then(Weak::upgrade) {
            Some(parent) => 1 + parent.depth(),
            None => 0,
        }
    }

    fn delete_facet(&self, name: &str) -> Result<()> {
        self.abort_facet(name, kj::failed!("jsg.Error: Facet was deleted."));
        let ns = self.namespace()?;
        ffi::actor_storage_delete_facet(
            ns.storage()?,
            &self.root_key,
            self.facet_id()?.into(),
            name,
        )?;
        if let Some(node) = ns.cluster() {
            let prefix = self.lane_prefix()? + &cluster::facet_prefix([name]);
            let unique_key = ns.unique_key().unwrap_or_default();
            node.cluster
                .delete_lanes(unique_key, &self.root_key, &prefix)
                .map_err(|e| kj::failed!("{e}"))?;
        }
        Ok(())
    }

    fn clone_facet(&self, src: &str, dst: &str) -> Result<()> {
        let ns = self.namespace()?;
        // Replacing a facet implies aborting it.
        self.abort_facet(dst, kj::failed!("jsg.Error: Facet was cloned-over."));
        if src == dst {
            // Cloning a facet onto itself replaces it with an exact copy of its own data: the
            // abort matches `delete(dst)`, and the storage stays as it is.
            return Ok(());
        }
        ffi::actor_storage_clone_facet(
            ns.storage()?,
            &self.root_key,
            self.facet_id()?.into(),
            src,
            dst,
        )?;
        if let Some(node) = ns.cluster() {
            let prefix = self.lane_prefix()?;
            let [src, dst] = [src, dst].map(|n| prefix.clone() + &cluster::facet_prefix([n]));
            let unique_key = ns.unique_key().unwrap_or_default();
            let layout = Databases {
                ns: Rc::clone(&ns),
                root_key: self.root_key.clone(),
            };
            node.cluster
                .clone_lanes(unique_key, &self.root_key, &src, &dst, &layout)
                .map_err(|e| kj::failed!("{e}"))?;
        }
        Ok(())
    }
}

/// Where a clustered object's databases are: the root's, and each facet's beside it as the
/// root's facet index numbers it.
struct Databases {
    ns: Rc<ActorNamespace>,
    root_key: String,
}

impl Databases {
    fn place(&self, lane: &str) -> Result<String> {
        let path = cluster::facet_path(lane).ok_or_else(|| kj::failed!("a lane named {lane}"))?;
        let storage = self.ns.storage()?;
        let mut id = None;
        for name in &path {
            id = ffi::actor_storage_facet_id(storage, &self.root_key, id.into(), name)?.into();
        }
        self.ns.sqlite_path(&self.root_key, id)
    }
}

impl cluster::Layout for Databases {
    fn place(&self, lane: &str) -> cluster::Result<std::path::PathBuf> {
        let path = Self::place(self, lane).map_err(|e| layout_error(&e))?;
        Ok(path.into())
    }

    fn clear(&self) -> cluster::Result<()> {
        let storage = self.ns.storage().map_err(|e| layout_error(&e))?;
        ffi::actor_storage_clear(storage, &self.root_key)
            .map_err(|e| layout_error(&crate::Error::from(e)))
    }
}

fn layout_error(e: &crate::Error) -> cluster::Error {
    cluster::Error::Replica(e.description().to_owned())
}

impl ActorHooks {
    pub(crate) fn committer(&self) -> Result<Box<Committer>> {
        // Only a clustered actor's storage asks; see `ActorStorageSpec::replicated`.
        let container = self.container()?;
        let ns = container
            .namespace
            .upgrade()
            .ok_or_else(|| kj::disconnected!("shut down"))?;
        let node = ns
            .cluster()
            .ok_or_else(|| kj::failed!("not in a cluster"))?;
        Ok(Box::new(Committer {
            cluster: Rc::clone(&node.cluster),
            namespace: ns.unique_key().unwrap_or_default().to_owned(),
            object: container.root_key.clone(),
        }))
    }

    fn container(&self) -> Result<Rc<ActorContainer>> {
        self.0
            .upgrade()
            .ok_or_else(|| kj::disconnected!("the actor is gone"))
    }

    pub(crate) fn stamp_cause(&self, metadata: Pin<&mut RequestMetadata>) {
        if let Ok(container) = self.container() {
            container.root().cause().stamp(metadata);
        }
    }

    /// Starts a request the actor raises for itself (an alarm, a hibernated WebSocket's event),
    /// restarting the actor if it was evicted.
    pub(crate) fn start_request(
        &self,
        metadata: KjOwn<RequestMetadata>,
    ) -> Result<KjOwn<WorkerInterface>> {
        self.container()?.start_request(metadata)
    }

    pub(crate) fn depth(&self) -> u32 {
        self.0.upgrade().map_or(0, |container| container.depth())
    }

    pub(crate) fn facet(
        &self,
        name: &str,
        start: KjOwn<FacetStart>,
    ) -> Result<Box<SubrequestChannel>> {
        Ok(SubrequestChannel::new(Rc::new(ActorChannel {
            container: Client(self.container()?.facet_container(name, start)),
            persistent: false,
        })))
    }

    pub(crate) fn abort_facet(&self, name: &str, reason: &ffi::Exception) {
        if let Some(container) = self.0.upgrade() {
            container.abort_facet(name, reason.into());
        }
    }

    pub(crate) fn delete_facet(&self, name: &str) -> Result<()> {
        self.container()?.delete_facet(name)
    }

    /// Whether the cluster replicates the actor's storage.
    pub(crate) fn replicated(&self) -> bool {
        self.0
            .upgrade()
            .and_then(|c| c.namespace.upgrade())
            .is_some_and(|ns| ns.cluster().is_some())
    }

    pub(crate) fn clone_facet(&self, src: &str, dst: &str) -> Result<()> {
        self.container()?.clone_facet(src, dst)
    }

    pub(crate) fn active(&self) {
        if let Some(container) = self.0.upgrade() {
            container.active();
        }
    }

    pub(crate) fn inactive(&self) {
        if let Some(container) = self.0.upgrade() {
            container.inactive();
        }
    }
}

// =====================================================================================
// Channels

/// A client's hold on a container: a stub's channel, or a request from its start until its
/// interface is dropped. The container's expiration is timed from when one last went away.
struct Client(Rc<ActorContainer>);

impl std::ops::Deref for Client {
    type Target = Rc<ActorContainer>;

    fn deref(&self) -> &Self::Target {
        &self.0
    }
}

impl Drop for Client {
    fn drop(&mut self) {
        self.0.update_access_time();
    }
}

/// A stub's channel to an actor.
struct ActorChannel {
    container: Client,
    /// Whether the channel was restored from a stored stub, or may be stored.
    persistent: Persistent,
}

impl Channel for ActorChannel {
    fn start_request(
        &self,
        mut metadata: KjOwn<RequestMetadata>,
    ) -> Result<KjOwn<WorkerInterface>> {
        ffi::request_metadata_set_from_persistent_stub(metadata.as_mut(), self.persistent);
        self.container.start_request(metadata)
    }

    fn require_allows_transfer(&self) -> Result<()> {
        self.container.require_transferable()
    }

    fn token(&self, usage: TokenUsage) -> Result<KjOwn<PendingToken>> {
        self.container.token(usage, self.persistent)
    }

    fn evict_for_test(&self, hibernate: bool) -> LocalBoxFuture<'_, Result<()>> {
        self.container.evict_for_test(hibernate)
    }
}

/// Hands off the objects of `namespaces` as this instance drains, and answers whether it
/// stopped them all.
///
/// Every object stops once its requests in flight finished, or at the in-flight deadline,
/// cancelling them. Then a few at a time, the busiest first, are released, until the handoff's
/// deadline. What is left then is stopped where it is (`ActorNamespace::release_all`), for peers
/// to take over once this instance left the cluster, until the release deadline. Objects still
/// running then must keep this instance a member: a peer that took one over would run it
/// alongside them.
pub async fn hand_off_all(namespaces: &[Rc<ActorNamespace>], deadlines: Deadlines) -> bool {
    let slots = Rc::new(tokio::sync::Semaphore::new(HANDOFFS_AT_ONCE));
    let mut handoffs: Vec<_> = namespaces
        .iter()
        .flat_map(|ns| ns.handoffs(deadlines.in_flight, &slots))
        .map(Some)
        .collect();
    let heats: Vec<Heat> = handoffs.iter().flatten().map(|(heat, _)| *heat).collect();
    let order = cluster_logic::handoff::busiest_first(&heats, cluster::now_ms());
    let ordered: Vec<_> = order
        .into_iter()
        .filter_map(|i| handoffs[i].take())
        .map(|(_, handoff)| handoff)
        .collect();
    // All at once: what each waits for first is its requests in flight, not a slot.
    let all = futures::future::join_all(ordered);
    let _ = tokio::time::timeout_at(deadlines.handoff, all).await;
    for ns in namespaces {
        ns.release_all(deadlines.release).await;
    }
    namespaces
        .iter()
        .all(|ns| ns.left_behind.borrow().is_empty())
}
