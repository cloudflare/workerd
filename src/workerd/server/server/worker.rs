// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! A worker service: a compiled worker, its entrypoints and actor classes, and the I/O channel
//! table its requests see.
//!
//! The service is built in two steps. `WorkerService::new` compiles the worker through the
//! factory and keeps, as a [`PendingLink`], every designator its bindings name; once every
//! service exists the server resolves those into a [`LinkedChannels`] table and calls `link`.
//! Until then the service starts no requests.

use std::cell::RefCell;
use std::collections::HashSet;
use std::ptr;
use std::rc::Rc;
use std::rc::Weak;

use hashlink::LinkedHashMap;
use kj_rs::KjMaybe;
use kj_rs::KjOwn;
use workerd_capnp::service_designator;
use workerd_capnp::worker;

use crate::Result;
use crate::actor::ActorNamespace;
use crate::bindings::ActorConfig;
use crate::bindings::ActorConfigMap;
use crate::bindings::ActorConfigs;
use crate::bindings::CompiledBindings;
use crate::bindings::Designator;
use crate::bindings::Exports;
use crate::bindings::LoopbackGlobals;
use crate::bindings::SPECIAL_SUBREQUEST_CHANNEL_COUNT;
use crate::bindings::compile_bindings;
use crate::bindings::len_u32;
use crate::bindings::loopback_globals;
use crate::bridge::ffi;
use crate::channels::ActorClass;
use crate::channels::ActorClassChannel;
use crate::channels::ActorIdHandle;
use crate::channels::Channel;
use crate::channels::ChannelFactory;
use crate::channels::DynamicSource;
use crate::channels::Frankenvalue;
use crate::channels::NewActor;
use crate::channels::PendingToken;
use crate::channels::Persistent;
use crate::channels::RequestMetadata;
use crate::channels::SubrequestChannel;
use crate::channels::Tail;
use crate::channels::TokenUsage;
use crate::channels::WorkerEntrypoint;
use crate::channels::WorkerInterface;
use crate::channels::WorkerInterfaceList;
use crate::channels::WorkerStub;
use crate::config::Factory;
use crate::config::Reporter;
use crate::config::Server;
use crate::config::capnp_error;
use crate::config::text;
use crate::loader::WorkerLoaderNamespace;
use crate::loader::dynamic_transfer_error;

/// What the link stage resolves for a worker: every designator its config names, and the loopback
/// channels `ctx.exports` was numbered with.
pub struct PendingLink {
    pub global_outbound: Designator,
    pub cache_api_outbound: Option<Designator>,
    pub bindings: CompiledBindings,
    pub loopback: LoopbackGlobals,
    pub tails: Vec<Designator>,
    pub streaming_tails: Vec<Designator>,
    /// `durableObjectStorage.localDisk`: the disk service whose directory holds the storage.
    pub storage: Option<String>,
    /// `accessBindingService`: the worker whose entrypoint receives the request's Access identity.
    pub access_binding: Option<Designator>,
}

/// The I/O channel table of a linked worker: what each channel number its bindings hold reaches.
/// One per worker, shared by every request. The runtime's `IoChannelFactory`.
///
/// A dynamic worker's `env` channels are the factory's own (worker-factory-impl.h); its table
/// here holds only the channels numbered after them, its `ctx.exports`, and `first_subrequest`
/// / `first_actor_class` say where it starts. A config worker's table starts at zero.
pub struct LinkedChannels {
    pub first_subrequest: u32,
    /// Indexed by channel number less `first_subrequest`: the two global-outbound slots, the
    /// bindings' channels, then the loopback entrypoints (and the access binding's entrypoint,
    /// if any).
    pub subrequest: Vec<Rc<dyn Channel>>,
    /// Indexed by actor channel; `None` when the binding's config was invalid.
    pub actor: Vec<Option<Rc<ActorNamespace>>>,
    pub first_actor_class: u32,
    /// Indexed by actor class channel less `first_actor_class`.
    pub actor_class: Vec<Rc<dyn ActorClass>>,
    pub cache: Option<Rc<dyn Channel>>,
    pub tails: Vec<Rc<dyn Channel>>,
    pub streaming_tails: Vec<Rc<dyn Channel>>,
    pub worker_loaders: Vec<Rc<WorkerLoaderNamespace>>,
    /// The subrequest channel whose props come from the request's Access blob.
    pub access_binding_channel: Option<u32>,
    pub has_debug_port: bool,
    pub server: Weak<Server>,
    /// How a dynamic worker unloads itself; a static worker aborting its isolate ends the
    /// process.
    pub abort_isolate: Option<Box<dyn Fn()>>,
}

/// A compiled worker and what the service graph hangs off it.
pub struct WorkerService {
    factory: Rc<Factory>,
    /// The service's name in the config; none for a dynamic worker, which no token can name.
    service_name: Option<String>,
    worker: KjOwn<ffi::CompiledWorker>,
    /// The default export's handlers, if the worker has a default export.
    default_handlers: Option<HashSet<String>>,
    /// Named exports with their handlers, in export order (loopback channels follow it).
    /// Workflow classes are among them.
    named_entrypoints: LinkedHashMap<String, HashSet<String>>,
    /// Exported Durable Object classes, in export order.
    actor_classes: Vec<String>,
    /// Exported `WorkflowEntrypoint` classes, which `named_entrypoints` also lists (a Workflow
    /// class is a stateless entrypoint at runtime).
    workflow_classes: Vec<String>,
    /// `durableObjectStorage.localDisk`'s disk service, which a Workflow's namespace takes its
    /// storage from.
    storage: Option<String>,
    /// The `allow_irrevocable_stub_storage` flag: whether tokens to this worker may be stored.
    persistent_self_tokens: bool,
    pending: RefCell<Option<PendingLink>>,
    channels: RefCell<Option<Rc<LinkedChannels>>>,
    /// The namespaces of the classes the config gives storage to, by class name, in config
    /// order. Cleared by `unlink`: each namespace's class refers back to this service.
    namespaces: RefCell<LinkedHashMap<String, Rc<ActorNamespace>>>,
}

impl WorkerService {
    /// Compiles the worker `conf` (the config's service `service_index`, named `name`) and
    /// prepares its link. Config errors are reported, not returned: a worker with errors still
    /// exists so that the rest of the config's errors can be found.
    pub async fn new(
        factory: Rc<Factory>,
        name: &str,
        service_index: u32,
        conf: worker::Reader<'_>,
        actor_configs: &ActorConfigs,
        inbound_listeners: Vec<ffi::InboundListener>,
        report: &Reporter,
    ) -> Result<Rc<Self>> {
        let experimental = factory.experimental();
        let empty = ActorConfigMap::new();
        let local_actor_configs = actor_configs.get(name).unwrap_or(&empty);

        let mut bindings =
            compile_bindings(name, conf, actor_configs, local_actor_configs, experimental)
                .map_err(capnp_error)?;
        for error in std::mem::take(&mut bindings.errors) {
            report.service_error(name, error);
        }

        let access_blob_header = if !conf.has_access_blob_header() {
            None
        } else if experimental {
            Some(text(conf.get_access_blob_header())?)
        } else {
            report.service_error(
                name,
                format!(
                    "Worker \"{name}\" has accessBlobHeader configured but this is an experimental \
                     feature. You must run workerd with `--experimental` to use this feature."
                ),
            );
            None
        };

        let spec = ffi::WorkerSpec {
            name: name.to_owned(),
            inbound_listeners,
            globals: std::mem::take(&mut bindings.globals),
            access_blob_header: access_blob_header.into(),
        };
        let compiled = ffi::factory_new_worker(
            factory.raw(),
            &spec,
            KjMaybe::Some(service_index),
            KjMaybe::None,
        )
        .await?;

        let info = ffi::worker_info(&compiled);
        for error in info.errors {
            report.service_error(name, error);
        }
        for warning in info.warnings {
            report.service_warning(name, warning);
        }

        let (default_handlers, named_entrypoints) = split_entrypoints(info.entrypoints);
        let named_names: Vec<String> = named_entrypoints.keys().cloned().collect();
        let exports = Exports {
            has_default_entrypoint: default_handlers.is_some(),
            named_entrypoints: &named_names,
            workflow_classes: &info.workflow_classes,
            actor_classes: &info.actor_classes,
        };
        let loopback = loopback_globals(
            &exports,
            local_actor_configs,
            |class| bindings.workflow_binding_channels.get(class).copied(),
            len_u32(&bindings.subrequest),
            len_u32(&bindings.actors),
            len_u32(&bindings.actor_classes),
        )
        .map_err(capnp_error)?;
        ffi::worker_set_ctx_exports(&compiled, &loopback.globals)?;

        let pending = pending_link(name, conf, bindings, loopback, report).map_err(capnp_error)?;

        let this = Rc::new(Self {
            factory,
            service_name: Some(name.to_owned()),
            worker: compiled,
            default_handlers,
            named_entrypoints,
            actor_classes: info.actor_classes,
            workflow_classes: info.workflow_classes,
            storage: pending.storage.clone(),
            persistent_self_tokens: info.persistent_self_tokens,
            pending: RefCell::new(Some(pending)),
            channels: RefCell::new(None),
            namespaces: RefCell::new(LinkedHashMap::new()),
        });
        this.init_namespaces(local_actor_configs);
        Ok(this)
    }
}

/// The compiled worker's entrypoints, split into the default export's handlers and the named
/// exports' (in export order).
type SplitEntrypoints = (
    Option<HashSet<String>>,
    LinkedHashMap<String, HashSet<String>>,
);

fn split_entrypoints(entrypoints: Vec<ffi::EntrypointInfo>) -> SplitEntrypoints {
    let mut default_handlers = None;
    let mut named_entrypoints = LinkedHashMap::new();
    for entrypoint in entrypoints {
        let handlers: HashSet<String> = entrypoint.handlers.into_iter().collect();
        if entrypoint.is_default {
            default_handlers = Some(handlers);
        } else {
            named_entrypoints.insert(entrypoint.name, handlers);
        }
    }
    (default_handlers, named_entrypoints)
}

impl WorkerService {
    /// Compiles a dynamic worker from `source` and links it at once. The factory keeps the
    /// channels the source's `env`, global outbound and tails supply, and serves their numbers
    /// itself; the table here is the worker's own `ctx.exports`, numbered after them. A dynamic
    /// worker has no config, so no namespaces, no pending link and no name a token could carry;
    /// `name` is for error logs. Compilation errors fail the load rather than being reported.
    pub async fn new_dynamic(
        factory: Rc<Factory>,
        name: String,
        source: KjOwn<DynamicSource>,
        server: Weak<Server>,
        abort_isolate: Box<dyn Fn()>,
    ) -> Result<Rc<Self>> {
        let spec = ffi::WorkerSpec {
            name,
            inbound_listeners: Vec::new(),
            globals: Vec::new(),
            access_blob_header: KjMaybe::None,
        };
        let compiled =
            ffi::factory_new_worker(factory.raw(), &spec, KjMaybe::None, KjMaybe::Some(source))
                .await?;

        let info = ffi::worker_info(&compiled);
        if !info.errors.is_empty() {
            return Err(kj::failed!(
                "jsg.Error: Failed to start Worker:\n{}",
                info.errors.join("\n")
            ));
        }
        for warning in &info.warnings {
            tracing::warn!("{warning}");
        }

        let (default_handlers, named_entrypoints) = split_entrypoints(info.entrypoints);
        let named_names: Vec<String> = named_entrypoints.keys().cloned().collect();
        let exports = Exports {
            has_default_entrypoint: default_handlers.is_some(),
            named_entrypoints: &named_names,
            workflow_classes: &info.workflow_classes,
            actor_classes: &info.actor_classes,
        };
        // `ctx.exports` is numbered after the env's channels, which the factory serves.
        let loopback = loopback_globals(
            &exports,
            &ActorConfigMap::new(),
            |_| None,
            info.env_subrequest_channels,
            0,
            info.env_actor_classes,
        )
        .map_err(capnp_error)?;
        ffi::worker_set_ctx_exports(&compiled, &loopback.globals)?;

        let this = Rc::new(Self {
            factory,
            service_name: None,
            worker: compiled,
            default_handlers,
            named_entrypoints,
            actor_classes: info.actor_classes,
            workflow_classes: info.workflow_classes,
            storage: None,
            persistent_self_tokens: info.persistent_self_tokens,
            pending: RefCell::new(None),
            channels: RefCell::new(None),
            namespaces: RefCell::new(LinkedHashMap::new()),
        });

        let subrequest = loopback
            .subrequest_entrypoints
            .iter()
            .map(|entrypoint| this.loopback_entrypoint(entrypoint.as_deref()))
            .collect::<Result<Vec<_>>>()?;
        let actor_class = loopback
            .actor_classes
            .iter()
            .map(|class_name| this.loopback_actor_class(class_name))
            .collect::<Result<Vec<_>>>()?;
        this.link(
            LinkedChannels {
                first_subrequest: info.env_subrequest_channels + SPECIAL_SUBREQUEST_CHANNEL_COUNT,
                subrequest,
                actor: Vec::new(),
                first_actor_class: info.env_actor_classes,
                actor_class,
                cache: None,
                tails: Vec::new(),
                streaming_tails: Vec::new(),
                worker_loaders: Vec::new(),
                access_binding_channel: None,
                has_debug_port: false,
                server,
                abort_isolate: Some(abort_isolate),
            },
            None,
        )?;
        Ok(this)
    }
}

fn designators(
    list: capnp::struct_list::Reader<'_, service_designator::Owned>,
    context: &str,
    errors: &mut Vec<String>,
) -> capnp::Result<Vec<Designator>> {
    list.iter()
        .map(|reader| Designator::from_reader(reader, context.to_owned(), errors))
        .collect()
}

/// Everything the link stage needs from `conf`, beyond the bindings.
fn pending_link(
    name: &str,
    conf: worker::Reader<'_>,
    bindings: CompiledBindings,
    loopback: LoopbackGlobals,
    report: &Reporter,
) -> capnp::Result<PendingLink> {
    let mut errors = Vec::new();
    let global_outbound = Designator::from_reader(
        conf.get_global_outbound()?,
        format!("Worker \"{name}\"'s globalOutbound"),
        &mut errors,
    )?;
    let cache_api_outbound = if conf.has_cache_api_outbound() {
        Some(Designator::from_reader(
            conf.get_cache_api_outbound()?,
            format!("Worker \"{name}\"'s cacheApiOutbound"),
            &mut errors,
        )?)
    } else {
        None
    };
    let tails = designators(
        conf.get_tails()?,
        &format!("Worker \"{name}\"'s tails"),
        &mut errors,
    )?;
    let streaming_tails = designators(
        conf.get_streaming_tails()?,
        &format!("Worker \"{name}\"'s streaming tails"),
        &mut errors,
    )?;
    // In-memory storage has no disk; an unknown kind was reported by the config's first pass.
    let storage = match conf.get_durable_object_storage().which() {
        Ok(worker::durable_object_storage::Which::LocalDisk(disk)) => Some(disk?.to_string()?),
        _ => None,
    };
    let access_binding = if conf.has_access_binding_service() {
        Some(Designator::from_reader(
            conf.get_access_binding_service()?,
            "Worker accessBindingService".to_owned(),
            &mut errors,
        )?)
    } else {
        None
    };
    for error in errors {
        report.service_error(name, error);
    }
    Ok(PendingLink {
        global_outbound,
        cache_api_outbound,
        bindings,
        loopback,
        tails,
        streaming_tails,
        storage,
        access_binding,
    })
}

impl WorkerService {
    /// One namespace per class the config gives storage to. A class without a matching export
    /// gets a namespace anyway: calls to it fail at runtime, not at startup. A Workflow's
    /// namespace is another worker's class over another worker's storage, created once every
    /// service exists (`init_workflow_namespace`).
    fn init_namespaces(self: &Rc<Self>, configs: &ActorConfigMap) {
        let mut namespaces = self.namespaces.borrow_mut();
        for (class_name, config) in configs {
            if let ActorConfig::Durable {
                workflow: Some(_), ..
            } = config
            {
                continue;
            }
            if !self.actor_classes.contains(class_name) {
                tracing::warn!(
                    "A DurableObjectNamespace in the config referenced the class \"{class_name}\", \
                     but no such Durable Object class is exported from the worker. Please make \
                     sure the class name matches, it is exported, and the class extends \
                     'DurableObject'. Attempts to call to this Durable Object class will fail at \
                     runtime, but historically this was not a startup-time error. Future versions \
                     of workerd may make this a startup-time error."
                );
            }
            let actor_class: Rc<dyn ActorClass> = Rc::new(ActorClassImpl {
                worker: Rc::clone(self),
                class_name: class_name.clone(),
                props: None,
                persistent: false,
            });
            let namespace = ActorNamespace::new(
                Rc::clone(&self.factory),
                class_name.clone(),
                config.clone(),
                actor_class,
                self.persistent_self_tokens,
            );
            namespaces.insert(class_name.clone(), namespace);
        }
    }

    /// Creates the namespace backing one configured Workflow, keyed by `key`, its unique key.
    /// Its actors are `actor_class`, the engine's class with the Workflow's props bound, and its
    /// storage is in `storage_path`, the directory of the Workflow's `bindingService` worker.
    /// `persistent_self_tokens` is the engine worker's.
    pub fn init_workflow_namespace(
        &self,
        key: &str,
        config: ActorConfig,
        actor_class: Rc<dyn ActorClass>,
        storage_path: Option<&str>,
        persistent_self_tokens: Persistent,
    ) -> Result<Rc<ActorNamespace>> {
        let namespace = ActorNamespace::new(
            Rc::clone(&self.factory),
            key.to_owned(),
            config,
            actor_class,
            persistent_self_tokens,
        );
        namespace.link(storage_path)?;
        self.namespaces
            .borrow_mut()
            .insert(key.to_owned(), Rc::clone(&namespace));
        Ok(namespace)
    }

    /// The factory the worker was compiled by, for its namespaces' storage, containers and
    /// tokens.
    #[must_use]
    pub fn factory(&self) -> &Rc<Factory> {
        &self.factory
    }

    /// The service's config name, which restores a stub to the worker. A dynamic worker has
    /// none, so it cannot be reached from another worker as a stub: nothing could reload it from
    /// a token.
    pub fn transferable_name(&self) -> Result<&str> {
        self.service_name
            .as_deref()
            .ok_or_else(dynamic_transfer_error)
    }

    /// Whether channel tokens to this worker may be stored (`allow_irrevocable_stub_storage`).
    #[must_use]
    pub fn persistent_self_tokens(&self) -> Persistent {
        self.persistent_self_tokens
    }

    /// Whether the worker exports the Durable Object class `name`.
    #[must_use]
    pub fn has_actor_class(&self, name: &str) -> bool {
        self.actor_classes.iter().any(|class| class == name)
    }

    /// Whether the worker exports the `WorkflowEntrypoint` class `name`.
    #[must_use]
    pub fn has_workflow_class(&self, name: &str) -> bool {
        self.workflow_classes.iter().any(|class| class == name)
    }

    /// Whether the worker exports a plain stateless entrypoint: the `WorkerEntrypoint` named
    /// `name`, or the default export when `name` is none. A Workflow class is not one: a
    /// `WorkflowEntrypoint` cannot be, for one, another Workflow's `bindingService`.
    #[must_use]
    pub fn has_stateless_entrypoint(&self, name: Option<&str>) -> bool {
        match name {
            Some(name) => {
                self.named_entrypoints.contains_key(name) && !self.has_workflow_class(name)
            }
            None => self.has_default_entrypoint(),
        }
    }

    /// The disk service of the worker's `localDisk` Durable Object storage, which a Workflow's
    /// `bindingService` worker must have to provide the Workflow's storage.
    #[must_use]
    pub fn storage(&self) -> Option<&str> {
        self.storage.as_deref()
    }

    #[must_use]
    pub fn has_default_entrypoint(&self) -> bool {
        self.default_handlers.is_some()
    }

    /// Whether `entrypoint` (the default when `None`) exports `handler`, e.g. `fetch` or `test`.
    #[must_use]
    pub fn has_handler(&self, entrypoint: Option<&str>, handler: &str) -> bool {
        let handlers = match entrypoint {
            Some(name) => self.named_entrypoints.get(name),
            None => self.default_handlers.as_ref(),
        };
        handlers.is_some_and(|handlers| handlers.contains(handler))
    }

    /// The named entrypoints, in export order.
    pub fn entrypoint_names(&self) -> impl Iterator<Item = &str> {
        self.named_entrypoints.keys().map(String::as_str)
    }

    /// The namespace of `class_name`, if the config gives the class storage.
    #[must_use]
    pub fn namespace(&self, class_name: &str) -> Option<Rc<ActorNamespace>> {
        self.namespaces.borrow().get(class_name).cloned()
    }

    /// Every namespace, in config order.
    #[must_use]
    pub fn namespaces(&self) -> Vec<(String, Rc<ActorNamespace>)> {
        self.namespaces
            .borrow()
            .iter()
            .map(|(name, namespace)| (name.clone(), Rc::clone(namespace)))
            .collect()
    }

    /// The channel of an entrypoint (the default when `name` is `None`), with `props` bound.
    /// `None` when there is no such named entrypoint. A worker without a default export still
    /// yields a channel for the default entrypoint, whose requests fail: it is not a config
    /// error, and configs depend on it.
    #[must_use]
    pub fn entrypoint(
        self: &Rc<Self>,
        name: Option<&str>,
        props: Option<KjOwn<Frankenvalue>>,
        persistent: Persistent,
    ) -> Option<Rc<dyn Channel>> {
        let entrypoint = if let Some(name) = name {
            // A Durable Object class named as an entrypoint is accepted, with no handlers:
            // calls to it fail at runtime.
            if !self.named_entrypoints.contains_key(name)
                && !self.actor_classes.iter().any(|class| class == name)
            {
                return None;
            }
            Some(name.to_owned())
        } else {
            None
        };
        Some(Rc::new(EntrypointChannel {
            worker: Rc::clone(self),
            entrypoint,
            props,
            persistent,
        }))
    }

    /// The default entrypoint's channel, with no props: the worker as a service.
    #[must_use]
    pub fn default_entrypoint(self: &Rc<Self>) -> Rc<dyn Channel> {
        Rc::new(EntrypointChannel {
            worker: Rc::clone(self),
            entrypoint: None,
            props: None,
            persistent: false,
        })
    }

    /// The `ctx.exports` channel of an entrypoint: the template its props specialize.
    pub fn loopback_entrypoint(self: &Rc<Self>, name: Option<&str>) -> Result<Rc<dyn Channel>> {
        let exists = match name {
            Some(name) => self.named_entrypoints.contains_key(name),
            None => self.default_handlers.is_some(),
        };
        if !exists {
            return Err(kj::failed!(
                "getLoopbackEntrypoint() called for entrypoint that doesn't exist"
            ));
        }
        Ok(Rc::new(EntrypointChannel {
            worker: Rc::clone(self),
            entrypoint: name.map(str::to_owned),
            props: None,
            persistent: false,
        }))
    }

    /// The Durable Object class `name` with `props` bound; `None` when the worker exports no
    /// such class (or `name` is `None`: a default export is never a class).
    #[must_use]
    pub fn actor_class(
        self: &Rc<Self>,
        name: Option<&str>,
        props: Option<KjOwn<Frankenvalue>>,
        persistent: Persistent,
    ) -> Option<Rc<dyn ActorClass>> {
        let class_name = self
            .actor_classes
            .iter()
            .find(|class| Some(class.as_str()) == name)?;
        Some(Rc::new(ActorClassImpl {
            worker: Rc::clone(self),
            class_name: class_name.clone(),
            props,
            persistent,
        }))
    }

    /// The `ctx.exports` class of `name`: the template its props specialize.
    pub fn loopback_actor_class(self: &Rc<Self>, name: &str) -> Result<Rc<dyn ActorClass>> {
        self.actor_class(Some(name), None, false).ok_or_else(|| {
            kj::failed!("getLoopbackActorClass() called for actor class that doesn't exist")
        })
    }
}

impl WorkerService {
    /// The designators awaiting the link stage. Taken once, by the server's third pass.
    pub fn take_pending_link(&self) -> Option<PendingLink> {
        self.pending.borrow_mut().take()
    }

    /// Installs the channel table and opens the namespaces' storage. `storage_path` is the
    /// directory of the worker's `localDisk` storage service, if any. A Workflow's namespace
    /// opened its own when it was created (`init_workflow_namespace`).
    pub fn link(&self, channels: LinkedChannels, storage_path: Option<&str>) -> Result<()> {
        if self.channels.borrow().is_some() {
            return Err(kj::failed!("already called link()"));
        }
        *self.channels.borrow_mut() = Some(Rc::new(channels));
        for namespace in self.namespaces.borrow().values() {
            if !namespace.is_linked() {
                namespace.link(storage_path)?;
            }
        }
        Ok(())
    }

    /// Cancels the worker's background work, then drops the namespaces and the channel table
    /// (the factory's part of it included), so that the graph's reference cycles are gone before
    /// the services are. The background work goes first: it holds the channels and the actors.
    /// Each is taken out of its cell before it is dropped, so that nothing a destructor reaches
    /// finds the cell borrowed.
    pub fn unlink(&self) {
        ffi::worker_unlink(&self.worker);
        let namespaces = std::mem::take(&mut *self.namespaces.borrow_mut());
        let channels = self.channels.borrow_mut().take();
        drop((namespaces, channels));
    }

    fn channels(&self) -> Result<Rc<LinkedChannels>> {
        self.channels
            .borrow()
            .clone()
            .ok_or_else(|| kj::failed!("link() has not been called"))
    }

    /// Starts a request on `entrypoint` (the default when `None`), on `actor` if the entrypoint
    /// is a Durable Object class. `is_tracer` marks a request that is itself a tail worker's,
    /// which gets no tail workers of its own.
    pub fn start_request(
        &self,
        entrypoint: Option<&str>,
        props: Option<KjOwn<Frankenvalue>>,
        actor: Option<&ffi::ActorHandle>,
        metadata: KjOwn<RequestMetadata>,
        is_tracer: bool,
    ) -> Result<KjOwn<WorkerInterface>> {
        let channels = self.channels()?;

        // The test event is not traced; a test of span tracing can still tail a request the
        // test makes.
        let mut tails = Vec::new();
        if entrypoint != Some("test") {
            for tail in &channels.tails {
                if let Some(worker) = self.start_tail(tail, is_tracer)? {
                    tails.push(Tail {
                        streaming: false,
                        worker: Some(worker),
                    });
                }
            }
            for tail in &channels.streaming_tails {
                if let Some(worker) = self.start_tail(tail, is_tracer)? {
                    tails.push(Tail {
                        streaming: true,
                        worker: Some(worker),
                    });
                }
            }
        }

        ffi::worker_start_request(
            &self.worker,
            entrypoint.into(),
            props.into(),
            actor.into(),
            Box::new(ChannelFactory(channels)),
            metadata,
            WorkerInterfaceList::new(tails),
        )
        .map_err(Into::into)
    }

    /// Starts a tail worker for one of this worker's requests. A tail that is an entrypoint of
    /// this same worker is started as a tracer, and not at all when the request being tailed is
    /// already a tracer's: a worker tailing itself must not recurse. Only the direct
    /// self-reference is caught; a cycle through another worker is not.
    fn start_tail(
        &self,
        tail: &Rc<dyn Channel>,
        is_tracer: bool,
    ) -> Result<Option<KjOwn<WorkerInterface>>> {
        let metadata = ffi::new_request_metadata(KjMaybe::None, KjMaybe::None);
        match tail.worker_entrypoint() {
            Some(target) if ptr::eq(target.worker, self) => {
                if is_tracer {
                    return Ok(None);
                }
                let props = target.props.map(ffi::frankenvalue_clone);
                self.start_request(target.entrypoint, props, None, metadata, true)
                    .map(Some)
            }
            _ => tail.start_request(metadata).map(Some),
        }
    }

    /// The token of `entrypoint` with `props` bound, as the runtime encodes it.
    fn token(
        &self,
        entrypoint: Option<&str>,
        props: Option<&Frankenvalue>,
        persistent: Persistent,
        usage: TokenUsage,
    ) -> Result<KjOwn<PendingToken>> {
        Ok(ffi::factory_encode_subrequest_token(
            self.factory.raw(),
            self.transferable_name()?,
            entrypoint.into(),
            props.into(),
            persistent,
            usage,
        )?)
    }
}

/// One entrypoint of a worker, with props bound or, for a `ctx.exports` template, not yet.
///
/// `persistent` is set only for a channel that came from `ctx.exports` in a worker with
/// `allow_irrevocable_stub_storage`, or was restored from a token that recorded that.
struct EntrypointChannel {
    worker: Rc<WorkerService>,
    /// `None` is the default entrypoint.
    entrypoint: Option<String>,
    props: Option<KjOwn<Frankenvalue>>,
    persistent: Persistent,
}

impl Channel for EntrypointChannel {
    fn start_request(
        &self,
        mut metadata: KjOwn<RequestMetadata>,
    ) -> Result<KjOwn<WorkerInterface>> {
        // A restored persistent stub tells the target so that it re-verifies that it still allows
        // persistent stubs; a bit already set by an outer hop is kept.
        ffi::request_metadata_set_from_persistent_stub(metadata.as_mut(), self.persistent);
        // A template called without props runs with empty props.
        let props = self.props.as_deref().map(ffi::frankenvalue_clone);
        self.worker
            .start_request(self.entrypoint.as_deref(), props, None, metadata, false)
    }

    fn require_allows_transfer(&self) -> Result<()> {
        self.worker.transferable_name().map(drop)
    }

    fn token(&self, usage: TokenUsage) -> Result<KjOwn<PendingToken>> {
        self.worker.token(
            self.entrypoint.as_deref(),
            self.props.as_deref(),
            self.persistent,
            usage,
        )
    }

    /// Specializes a `ctx.exports` template; a channel that already has props cannot be.
    fn for_props(
        &self,
        props: KjOwn<Frankenvalue>,
        persistent: Persistent,
    ) -> Result<Rc<dyn Channel>> {
        if self.props.is_some() {
            return Err(kj::failed!("can't override props for this service"));
        }
        Ok(Rc::new(Self {
            worker: Rc::clone(&self.worker),
            entrypoint: self.entrypoint.clone(),
            props: Some(props),
            persistent,
        }))
    }

    fn worker_entrypoint(&self) -> Option<WorkerEntrypoint<'_>> {
        Some(WorkerEntrypoint {
            worker: &self.worker,
            entrypoint: self.entrypoint.as_deref(),
            props: self.props.as_deref(),
        })
    }
}

/// One Durable Object class of a worker, with props bound or, for a `ctx.exports` template, not
/// yet.
struct ActorClassImpl {
    worker: Rc<WorkerService>,
    class_name: String,
    props: Option<KjOwn<Frankenvalue>>,
    persistent: Persistent,
}

impl ActorClass for ActorClassImpl {
    fn new_actor(&self, request: NewActor<'_>) -> Result<KjOwn<ffi::ActorHandle>> {
        // A template used without props constructs the actor with empty props.
        let props = self.props.as_deref().map(ffi::frankenvalue_clone);
        ffi::worker_new_actor(
            &self.worker.worker,
            &self.class_name,
            props.into(),
            request.id,
            request.storage,
            &request.spec,
            request.hooks,
            request.hibernation_manager.into(),
            request.container.into(),
        )
        .map_err(Into::into)
    }

    /// Props are not passed per request: the actor was constructed with them.
    fn start_request(
        &self,
        metadata: KjOwn<RequestMetadata>,
        actor: &ffi::ActorHandle,
    ) -> Result<KjOwn<WorkerInterface>> {
        self.worker
            .start_request(Some(&self.class_name), None, Some(actor), metadata, false)
    }

    fn require_allows_transfer(&self) -> Result<()> {
        self.worker.transferable_name().map(drop)
    }

    /// A template (no props) is not serializable: `ctx.exports` classes must be specialized
    /// before they can be sent anywhere.
    fn token(&self, usage: TokenUsage) -> Result<KjOwn<PendingToken>> {
        let service_name = self.worker.transferable_name()?;
        let props = self
            .props
            .as_deref()
            .ok_or_else(|| kj::failed!("an unspecialized loopback actor class has no token"))?;
        Ok(ffi::factory_encode_actor_class_token(
            self.worker.factory.raw(),
            service_name,
            &self.class_name,
            Some(props).into(),
            self.persistent,
            usage,
        )?)
    }

    fn for_props(
        &self,
        props: KjOwn<Frankenvalue>,
        persistent: Persistent,
    ) -> Result<Rc<dyn ActorClass>> {
        if self.props.is_some() {
            return Err(kj::failed!("can't override props for this actor class"));
        }
        Ok(Rc::new(Self {
            worker: Rc::clone(&self.worker),
            class_name: self.class_name.clone(),
            props: Some(props),
            persistent,
        }))
    }
}

impl LinkedChannels {
    fn subrequest(&self, channel: u32) -> Result<&Rc<dyn Channel>> {
        channel
            .checked_sub(self.first_subrequest)
            .and_then(|index| self.subrequest.get(index as usize))
            .ok_or_else(|| kj::failed!("invalid subrequest channel number"))
    }

    fn namespace(&self, channel: u32) -> Result<&Rc<ActorNamespace>> {
        self.actor
            .get(channel as usize)
            .ok_or_else(|| kj::failed!("invalid actor channel number"))?
            .as_ref()
            .ok_or_else(|| kj::failed!("jsg.Error: Actor namespace configuration was invalid."))
    }
}

impl ChannelFactory {
    /// With props, the channel must be a `ctx.exports` template to specialize.
    pub(crate) fn subrequest_channel(
        &self,
        channel: u32,
        props: KjMaybe<KjOwn<Frankenvalue>>,
        persistent: bool,
    ) -> Result<Box<SubrequestChannel>> {
        let target = self.0.subrequest(channel)?;
        Ok(SubrequestChannel::new(match Option::from(props) {
            Some(props) => target.for_props(props, persistent)?,
            None => Rc::clone(target),
        }))
    }

    pub(crate) fn global_actor(
        &self,
        channel: u32,
        id: KjOwn<ActorIdHandle>,
        persistent: bool,
    ) -> Result<Box<SubrequestChannel>> {
        let namespace = self.0.namespace(channel)?;
        // The bindings compiler only makes durable bindings to durable namespaces.
        if namespace.unique_key().is_none() {
            return Err(kj::failed!(
                "expected a durable namespace on this actor channel"
            ));
        }
        Ok(SubrequestChannel::new(namespace.channel(id, persistent)))
    }

    pub(crate) fn colo_local_actor(
        &self,
        channel: u32,
        id: &str,
    ) -> Result<Box<SubrequestChannel>> {
        let namespace = self.0.namespace(channel)?;
        if namespace.unique_key().is_some() {
            return Err(kj::failed!(
                "expected an ephemeral namespace on this actor channel"
            ));
        }
        Ok(SubrequestChannel::new(namespace.channel_by_name(id, false)))
    }

    pub(crate) fn actor_class(
        &self,
        channel: u32,
        props: KjMaybe<KjOwn<Frankenvalue>>,
        persistent: bool,
    ) -> Result<Box<ActorClassChannel>> {
        let class = channel
            .checked_sub(self.0.first_actor_class)
            .and_then(|index| self.0.actor_class.get(index as usize))
            .ok_or_else(|| kj::failed!("invalid actor class channel number"))?;
        Ok(ActorClassChannel::new(match Option::from(props) {
            Some(props) => class.for_props(props, persistent)?,
            None => Rc::clone(class),
        }))
    }

    pub(crate) fn cache_channel(&self) -> Result<Box<SubrequestChannel>> {
        let cache = self.0.cache.clone();
        Ok(SubrequestChannel::new(cache.ok_or_else(|| {
            kj::failed!("jsg.Error: No Cache was configured")
        })?))
    }

    pub(crate) fn access_binding_channel(&self) -> KjMaybe<u32> {
        self.0.access_binding_channel.into()
    }

    pub(crate) fn abort_all_actors(&self, reason: KjMaybe<&ffi::Exception>) {
        let reason = Option::<&ffi::Exception>::from(reason).map(crate::Error::from);
        if let Some(server) = self.0.server.upgrade() {
            server.abort_all_actors(reason.as_ref());
        }
    }

    pub(crate) fn delete_all_actors(&self, reason: KjMaybe<&ffi::Exception>) -> Result<()> {
        let reason = Option::<&ffi::Exception>::from(reason).map(crate::Error::from);
        match self.0.server.upgrade() {
            Some(server) => server.delete_all_actors(reason.as_ref()),
            None => Ok(()),
        }
    }

    pub(crate) async fn evict_all_actors_for_test(&self, hibernate: bool) -> Result<()> {
        let namespaces = self.0.actor.iter().flatten();
        let evictions = namespaces.map(|namespace| namespace.evict_all_for_test(hibernate));
        futures::future::try_join_all(evictions).await?;
        Ok(())
    }

    /// A dynamic worker unloads. A static worker cannot be replaced, so the call fails; the
    /// runtime treats that failure as fatal and the process ends.
    pub(crate) fn abort_isolate(&self, reason: &str) -> Result<()> {
        match &self.0.abort_isolate {
            Some(abort) => {
                abort();
                Ok(())
            }
            None => Err(kj::failed!(
                "abortIsolate() called, terminating process; reason = {reason}"
            )),
        }
    }

    pub(crate) fn load_isolate(
        &self,
        loader_channel: u32,
        name: KjMaybe<&str>,
        source: KjOwn<DynamicSource>,
    ) -> Result<Box<WorkerStub>> {
        let loader = self
            .0
            .worker_loaders
            .get(loader_channel as usize)
            .ok_or_else(|| kj::failed!("invalid worker loader channel number"))?;
        Ok(Box::new(WorkerStub(loader.load(name.into(), source)?)))
    }

    pub(crate) fn has_debug_port(&self) -> bool {
        self.0.has_debug_port
    }
}
