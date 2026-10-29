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
use std::rc::Rc;
use std::rc::Weak;
use std::time::Duration;

use futures::FutureExt;
use futures::future::Either;
use futures::future::LocalBoxFuture;
use futures::future::Shared;
use hashlink::LinkedHashMap;
use kj_rs::KjMaybe;
use kj_rs::KjOwn;
use kj_rs::KjRc;
use tokio::sync::Notify;
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
use crate::cluster::Owned;
use crate::cluster::Owner;
use crate::cluster::ResultTask;
use crate::cluster::RpcConnection;
use crate::config::Factory;
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
        })
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
        ffi::actor_storage_set_alarm(self.storage()?, actor, name.unwrap_or_default(), at_ms);
        Ok(())
    }

    /// The cluster, for a namespace linked on disk; in-memory namespaces stay local.
    pub(crate) fn cluster(&self) -> Option<&Rc<ClusterNode>> {
        self.directory.get().and_then(|_| self.cluster.get())
    }

    /// The database of root actor `key`.
    fn sqlite_path(&self, key: &str) -> Result<String> {
        let directory = self
            .directory
            .get()
            .ok_or_else(|| kj::failed!("the namespace has no directory"))?;
        Ok(format!("{directory}/{key}.sqlite"))
    }

    /// The owner of the actor with `id`: a container here, holding its ownership, or a session
    /// with the instance that owns it.
    async fn resolve(&self, id: KjOwn<ActorIdHandle>) -> Result<Resolved> {
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
        match cluster
            .route(&unique_key, &self.service, &self.class_name, &key)
            .await?
        {
            Owner::Remote(connection, node) => Ok(Resolved::Remote(connection, node)),
            Owner::Here(epoch) => {
                // A concurrent request may have taken it meanwhile.
                if let Some(container) = self.owned_container(&key) {
                    return Ok(Resolved::Here(Client(container)));
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
        if self.cluster().is_some() {
            return Rc::new(RoutedChannel {
                namespace: Weak::clone(&self.this),
                id,
                persistent,
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

    /// The `uniqueKey` of a durable namespace; none for an ephemeral one.
    pub fn unique_key(&self) -> Option<&str> {
        match &self.config {
            ActorConfig::Durable { unique_key, .. } => Some(unique_key),
            ActorConfig::Ephemeral { .. } => None,
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
    pub fn begin_container_cleanup(&self) {
        if self.container().is_some() {
            self.abort_all(Some(&kj::disconnected!("Server shutting down.")));
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
        // Only an object's owner runs its alarm. One that moved since this instance scheduled
        // the alarm has it scheduled where it lives now, since opening an object schedules its
        // alarm, unless the owner died before indexing it: this instance fires it again until
        // the owner has outlived that (`Index::elsewhere`).
        let key = ffi::actor_id_key(&id);
        Ok(PromisedInterface::new(async move {
            match namespace.resolve(id).await? {
                Resolved::Here(client) => client.start_request(metadata),
                Resolved::Remote(_, owner) => {
                    let unique_key = namespace.unique_key().unwrap_or_default();
                    let again = node
                        .cluster
                        .alarm_elsewhere(unique_key, &key, &owner, cluster::now_ms())
                        .await
                        .unwrap_or(true);
                    Ok(ffi::alarm_owned_elsewhere(again))
                }
            }
        })
        .into_kj())
    }

    pub fn alarm_set(&self, actor: &str, name: &str, at_ms: u64) -> Box<ResultTask> {
        let target = self.namespace().and_then(|ns| {
            let unique_key = ns.unique_key()?.to_owned();
            Some((Rc::clone(&ns.cluster()?.cluster), unique_key))
        });
        let (actor, name) = (actor.to_owned(), name.to_owned());
        Box::new(ResultTask(Box::pin(async move {
            let Some((cluster, unique_key)) = target else {
                return Ok(());
            };
            let name = (!name.is_empty()).then_some(name.as_str());
            let at_ms = (at_ms != 0).then_some(at_ms);
            cluster
                .arm_alarm(&unique_key, &actor, name, at_ms)
                .await
                .map_err(|e| kj::failed!("indexing an alarm: {e}"))
        })))
    }

    fn namespace(&self) -> Option<Rc<ActorNamespace>> {
        self.0.upgrade()
    }

    pub(crate) fn replicated(&self) -> bool {
        self.namespace().is_some_and(|ns| ns.cluster().is_some())
    }

    pub(crate) fn check_open(&self) -> Result<()> {
        match self.namespace().as_ref().and_then(|ns| ns.cluster()) {
            Some(node) if !node.cluster.lease_holds() => Err(kj::failed!(
                "this instance's cluster lease in the bucket has lapsed"
            )),
            _ => Ok(()),
        }
    }

    pub(crate) fn database_reset(&self, root_key: &str) {
        if let Some(ns) = self.namespace()
            && let (Some(node), Some(unique_key)) = (ns.cluster(), ns.unique_key())
        {
            node.cluster.database_reset(unique_key, root_key);
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

/// A stub's channel to an actor in a clustered namespace. Its first request finds the owner;
/// the stub stays with that instance, as a local stub stays with its container.
struct RoutedChannel {
    namespace: Weak<ActorNamespace>,
    id: KjOwn<ActorIdHandle>,
    persistent: Persistent,
    bound: Rc<RefCell<Option<Rc<Resolved>>>>,
}

impl Channel for RoutedChannel {
    fn start_request(
        &self,
        mut metadata: KjOwn<RequestMetadata>,
    ) -> Result<KjOwn<WorkerInterface>> {
        ffi::request_metadata_set_from_persistent_stub(metadata.as_mut(), self.persistent);
        let namespace = self
            .namespace
            .upgrade()
            .ok_or_else(|| kj::disconnected!("the Durable Object namespace is gone"))?;
        let id = ffi::actor_id_clone(&self.id);
        let bound = Rc::clone(&self.bound);
        Ok(PromisedInterface::new(async move {
            let resolved = bound.borrow().clone();
            let resolved = if let Some(resolved) = resolved {
                resolved
            } else {
                let resolved = Rc::new(namespace.resolve(id).await?);
                Rc::clone(bound.borrow_mut().get_or_insert(resolved))
            };
            match &*resolved {
                Resolved::Here(container) => container.start_request(metadata),
                Resolved::Remote(connection, _) => Ok(attach(
                    connection.start_request(metadata),
                    Rc::clone(connection),
                )),
            }
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
            match namespace.resolve(ffi::actor_id_clone(&self.id)).await? {
                Resolved::Here(container) => container.evict_for_test(hibernate).await,
                Resolved::Remote(..) => Err(kj::failed!(
                    "jsg.Error: Cannot evict a Durable Object another instance owns."
                )),
            }
        })
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
    /// A clustered root's ownership, for as long as the container is in its namespace's map.
    owned: RefCell<Option<Owned>>,
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
            owned: RefCell::new(None),
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
        self.require_not_broken()?;
        if !self.is_running() {
            if let Some(pending) = self.pending_class() {
                pending.await?;
                self.require_not_broken()?;
            }
            let class = self.class()?;
            class.when_ready().await?;
            self.require_not_broken()?;
            self.prepare_storage().await?;
            // A concurrent request may have started the actor meanwhile.
            if !self.is_running() {
                self.start(&class)?;
            }
        }
        self.actor
            .borrow()
            .clone()
            .ok_or_else(|| kj::failed!("the actor did not start"))
    }

    /// In cluster mode, readies the actor's database before the actor opens it: it is taken over
    /// (recovered from earlier epochs). An actor whose takeover fails leaves its namespace, so the
    /// next request claims anew.
    async fn prepare_storage(&self) -> Result<()> {
        let ns = self.namespace()?;
        if ns.cluster().is_none() {
            return Ok(());
        }
        let root_path = ns.sqlite_path(&self.root_key)?;
        if self.is_root() {
            let Some(restore) = self.owned.borrow().as_ref().map(|o| o.restore(root_path)) else {
                return Err(kj::failed!("a clustered actor without ownership"));
            };
            if let Err(e) = restore.await {
                self.abort(Some(e.clone()));
                ns.actors.borrow_mut().remove(&self.key);
                return Err(e);
            }
        } else {
            // TODO(cluster): facets, each a log of its own in the root's epoch.
            return Err(kj::failed!(
                "jsg.Error: Facets are not supported in cluster mode."
            ));
        }
        Ok(())
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
        let running = self.actor.borrow().clone();
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
        // A node stalled past its lease may have been taken over: serving
        // even a read from its database could return stale data.
        if ns.cluster().is_some_and(|node| !node.cluster.lease_holds()) {
            return Err(kj::failed!(
                "this instance's cluster lease in the bucket has lapsed"
            ));
        }
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
                if let Some(unique_key) = ns.unique_key() {
                    ffi::request_metadata_set_actor_self_token(
                        ns.factory.raw(),
                        metadata.as_mut(),
                        unique_key,
                        id,
                        ns.persistent_self_tokens,
                    );
                }
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
        if !ns.is_evictable() {
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
                    ns.actors.borrow_mut().remove(&self.key);
                }
            }
        }
        drop(manager);
        self.release(actor.as_deref());
        drop(actor);
    }

    /// Closes a clustered actor's database and gives up its ownership, so nothing writes behind
    /// the next owner.
    fn release(&self, actor: Option<&KjOwn<ffi::ActorHandle>>) {
        if let Some(actor) = actor
            && self
                .namespace
                .upgrade()
                .is_some_and(|ns| ns.cluster().is_some())
        {
            ffi::actor_close_storage(actor);
        }
        drop(self.owned.borrow_mut().take());
    }

    /// Aborts the actor and its facets; every request on them fails with `reason`. Without one
    /// the actors are shut down, failing nothing in flight, and later requests fail for an
    /// unknown reason. The caller removes the container from any map that could route traffic to
    /// it: at most call sites the map is at hand, and `abort` on a facet's tree leaves the facets
    /// filed under their parent.
    fn abort(&self, reason: Option<crate::Error>) {
        if self.broken_reason.borrow().is_some() {
            return;
        }
        let actor = self.actor.borrow_mut().take();
        if let Some(actor) = &actor {
            ffi::actor_abort(actor, &AbortReason(reason.clone()));
        }
        for facet in self.facets.borrow().values() {
            facet.abort(reason.clone());
        }
        self.cancel_on_broken();
        self.cancel_shutdown();
        *self.manager.borrow_mut() = None;
        *self.broken_reason.borrow_mut() = Some(
            reason.unwrap_or_else(|| kj::failed!("jsg.Error: Actor aborted for unknown reason.")),
        );
        self.release(actor.as_deref());
        drop(actor);
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
                self.cancel_shutdown();
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

    /// Shuts the actor down, keeping its storage. Returns false without evicting if the actor
    /// acquired a strong reference meanwhile (a request raced in): an idle shutdown is cancelled
    /// by a new request, but a test eviction is not cancellable, so it relies on that re-check.
    /// The only suspension is the wait for the isolate lock that hibernating WebSockets takes;
    /// the shutdown and the clearing of the slot after it are one step, so no request finds a
    /// shut-down actor in the slot. The on-broken monitor is cancelled only once the shutdown
    /// is committed, so that an early `false` leaves the actor watched.
    async fn try_evict(&self, reason: &str, hibernate: bool) -> Result<bool> {
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
        *self.actor.borrow_mut() = None;
        if !hibernate {
            *self.manager.borrow_mut() = None;
        }
        Ok(true)
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
        self.release(actor.as_deref());
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
        )
        .map_err(Into::into)
    }

    fn clone_facet(&self, src: &str, dst: &str) -> Result<()> {
        let ns = self.namespace()?;
        if ns.cluster().is_some() {
            // TODO(cluster): clone in the bucket's terms.
            return Err(kj::failed!(
                "jsg.Error: Cloning facets is not supported in cluster mode."
            ));
        }
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
        )
        .map_err(Into::into)
    }
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
