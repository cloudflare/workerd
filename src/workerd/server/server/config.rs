// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The service graph, built from the config in three passes.
//!
//! The passes: the actor configs of every worker (needed before any worker compiles, so bindings
//! to another worker's classes can be checked), then every service in config order, then the
//! links between them. The `Server` owns the graph for the run and resolves channel tokens and
//! debug-port requests into it.

use std::cell::Cell;
use std::cell::RefCell;
use std::collections::HashMap;
use std::collections::HashSet;
use std::rc::Rc;
use std::time::Duration;

use capnp::message::ReaderOptions;
use capnp::serialize::BufferSegments;
use hashlink::LinkedHashMap;
use kj_rs::KjMaybe;
use kj_rs::KjOwn;
use workerd_capnp::config;
use workerd_capnp::service;
use workerd_capnp::service_designator;
use workerd_capnp::worker;
use workerd_capnp::workflows_engine;

use crate::Result;
use crate::actor::ActorNamespace;
use crate::bindings::ActorConfig;
use crate::bindings::ActorConfigMap;
use crate::bindings::ActorConfigs;
use crate::bindings::Designator;
use crate::bindings::SPECIAL_SUBREQUEST_CHANNEL_COUNT;
use crate::bindings::WorkerLoaderDesignator;
use crate::bindings::Workflow;
use crate::bindings::len_u32;
use crate::bindings::workflow_namespace_key;
use crate::bridge::ffi;
use crate::channels::ActorClass;
use crate::channels::ActorClassChannel;
use crate::channels::ActorIdHandle;
use crate::channels::Channel;
use crate::channels::Frankenvalue;
use crate::channels::NewActor;
use crate::channels::PendingToken;
use crate::channels::RequestMetadata;
use crate::channels::ServerHandle;
use crate::channels::SubrequestChannel;
use crate::channels::TokenUsage;
use crate::channels::WorkerInterface;
use crate::listen::loopback::Loopback;
use crate::loader::WorkerLoaderNamespace;
use crate::services::DiskDirectoryService;
use crate::worker::LinkedChannels;
use crate::worker::WorkerService;

/// The C++ worker factory: everything the workers of one run share, owned for the run.
pub struct Factory {
    inner: KjOwn<ffi::WorkerFactory>,
    loopback: Loopback,
}

impl Factory {
    #[must_use]
    pub fn new(inner: KjOwn<ffi::WorkerFactory>) -> Self {
        Self {
            inner,
            loopback: Loopback::default(),
        }
    }

    /// The `loopback:` namespace of this server's sockets and external services.
    #[must_use]
    pub fn loopback(&self) -> &Loopback {
        &self.loopback
    }

    /// The factory, for the bridge functions.
    #[must_use]
    pub fn raw(&self) -> &ffi::WorkerFactory {
        &self.inner
    }

    /// The config message; `get_root::<config::Reader>()` gives the `Config`. Configs can
    /// legitimately be very large and are not malicious: the traversal limit is off.
    pub fn config(&self) -> Result<capnp::message::Reader<BufferSegments<&[u8]>>> {
        let options = *ReaderOptions::new().traversal_limit_in_words(None);
        capnp::serialize::read_message_from_flat_slice(
            &mut ffi::factory_config(&self.inner),
            options,
        )
        .map_err(capnp_error)
    }

    #[must_use]
    pub fn experimental(&self) -> bool {
        ffi::factory_experimental(&self.inner)
    }

    /// The reading of the factory's `kj::Timer`, which the server's own delays run on, as the
    /// time since the timer's origin.
    #[must_use]
    pub fn now(&self) -> Duration {
        Duration::from_nanos(ffi::factory_timer_now(&self.inner))
    }

    /// Completes `delay` from now on the factory's `kj::Timer`.
    pub async fn sleep(&self, delay: Duration) {
        let nanos = u64::try_from(delay.as_nanos()).unwrap_or(u64::MAX);
        // A timer's delay does not fail.
        let _ = ffi::factory_sleep(&self.inner, nanos).await;
    }
}

/// A malformed config message; the CLI checked the message, so this is a bug.
pub fn capnp_error(error: capnp::Error) -> crate::Error {
    let capnp::Error { extra, .. } = error;
    kj::failed!("{extra}")
}

/// A text field of the config, owned.
pub fn text(text: capnp::Result<capnp::text::Reader<'_>>) -> Result<String> {
    text.and_then(|text| text.to_string().map_err(capnp::Error::from))
        .map_err(capnp_error)
}

/// An optional text field of the config: `has` is its `has_*` accessor's answer.
pub fn optional_text(
    has: bool,
    get: capnp::Result<capnp::text::Reader<'_>>,
) -> Result<Option<String>> {
    if has { text(get).map(Some) } else { Ok(None) }
}

/// Where config errors and warnings go.
///
/// Every error is reported; once the services are built the server refuses to serve if there was
/// one, unless the reporter is `tolerant` (`--watch`, where someone is about to fix the config,
/// and the in-process server).
pub struct Reporter {
    error: Box<dyn Fn(String)>,
    warning: Box<dyn Fn(String)>,
    tolerant: bool,
    had_errors: Cell<bool>,
}

impl Reporter {
    #[must_use]
    pub fn new(error: Box<dyn Fn(String)>, warning: Box<dyn Fn(String)>, tolerant: bool) -> Self {
        Self {
            error,
            warning,
            tolerant,
            had_errors: Cell::new(false),
        }
    }

    pub fn error(&self, message: impl Into<String>) {
        self.had_errors.set(true);
        (self.error)(message.into());
    }

    /// Whether an error was reported that the server must not serve with.
    #[must_use]
    pub fn refuses(&self) -> bool {
        self.had_errors.get() && !self.tolerant
    }

    pub fn warning(&self, message: impl Into<String>) {
        (self.warning)(message.into());
    }

    /// An error of one service: `service <name>: <message>`.
    pub fn service_error(&self, service: &str, message: impl AsRef<str>) {
        self.error(format!("service {service}: {}", message.as_ref()));
    }

    pub fn service_warning(&self, service: &str, message: impl AsRef<str>) {
        self.warning(format!("service {service}: {}", message.as_ref()));
    }
}

/// The CLI's substitutions for config values, by service or socket name.
#[derive(Default)]
pub struct Overrides {
    /// `--directory-path`: a disk service's path.
    pub directories: HashMap<String, String>,
    /// `--external-addr`: an external service's address.
    pub externals: HashMap<String, String>,
}

/// One named service of the config.
pub enum Service {
    Worker(Rc<WorkerService>),
    /// An external server or a network: a channel and nothing more. A service whose config was
    /// rejected (the error was reported) is an [`InvalidConfigChannel`], which stands in so that
    /// the rest of the config can be checked.
    Leaf(Rc<dyn Channel>),
    Disk(Rc<DiskDirectoryService>),
}

impl Service {
    /// The service as a channel: a worker's default entrypoint, or the leaf itself.
    #[must_use]
    pub fn channel(&self) -> Rc<dyn Channel> {
        match self {
            Self::Worker(worker) => worker.default_entrypoint(),
            Self::Leaf(channel) => Rc::clone(channel),
            Self::Disk(disk) => Rc::clone(disk) as Rc<dyn Channel>,
        }
    }

    #[must_use]
    pub fn as_worker(&self) -> Option<&Rc<WorkerService>> {
        match self {
            Self::Worker(worker) => Some(worker),
            _ => None,
        }
    }
}

/// The channel of a service whose config is invalid. workerd refuses to serve a config with
/// errors, so only its `start_request` can be reached (by another service's link stage).
pub struct InvalidConfigChannel;

impl Channel for InvalidConfigChannel {
    fn start_request(&self, metadata: KjOwn<RequestMetadata>) -> Result<KjOwn<WorkerInterface>> {
        let _ = metadata;
        Err(kj::failed!(
            "jsg.Error: Service cannot handle requests because its config is invalid."
        ))
    }

    fn token(&self, _usage: TokenUsage) -> Result<KjOwn<PendingToken>> {
        Err(kj::failed!(
            "a service with an invalid config has no channel token"
        ))
    }
}

/// The actor class of a designator the config got wrong.
pub struct InvalidConfigActorClass;

impl ActorClass for InvalidConfigActorClass {
    fn new_actor(&self, request: NewActor<'_>) -> Result<KjOwn<ffi::ActorHandle>> {
        let _ = request;
        Err(kj::failed!(
            "jsg.Error: Cannot instantiate Durable Object class because its config is invalid."
        ))
    }

    fn start_request(
        &self,
        metadata: KjOwn<RequestMetadata>,
        actor: &ffi::ActorHandle,
    ) -> Result<KjOwn<WorkerInterface>> {
        let _ = (metadata, actor);
        Err(kj::failed!(
            "an actor of an invalid config's class cannot exist"
        ))
    }

    fn require_allows_transfer(&self) -> Result<()> {
        Err(kj::failed!(
            "an invalid config's class cannot be transferred"
        ))
    }

    fn token(&self, _usage: TokenUsage) -> Result<KjOwn<PendingToken>> {
        Err(kj::failed!(
            "an invalid config's class has no channel token"
        ))
    }
}

/// The `workerLoader` namespaces: shared by id, or one per binding without an id.
#[derive(Default)]
struct WorkerLoaders {
    by_id: HashMap<String, Rc<WorkerLoaderNamespace>>,
    anonymous: Vec<Rc<WorkerLoaderNamespace>>,
}

/// The services by name, in config order.
///
/// A worker and its Durable Object namespaces refer to each other, a cycle only
/// `WorkerService::unlink` breaks, so dropping the services unlinks every worker: at teardown,
/// and when the graph is abandoned half built.
#[derive(Default)]
pub struct Services(LinkedHashMap<String, Service>);

impl Drop for Services {
    fn drop(&mut self) {
        for worker in self.0.values().filter_map(Service::as_worker) {
            worker.unlink();
        }
    }
}

/// The server: the service graph of one run.
pub struct Server {
    report: Reporter,
    services: Services,
    /// Durable namespaces by `uniqueKey`, for actor tokens.
    namespaces_by_unique_key: HashMap<String, Rc<ActorNamespace>>,
    worker_loaders: RefCell<WorkerLoaders>,
    /// Declared last, so that it is dropped last: the services (and their isolates) refer to it.
    factory: Rc<Factory>,
}

impl Server {
    #[must_use]
    pub fn factory(&self) -> &Rc<Factory> {
        &self.factory
    }

    #[must_use]
    pub fn report(&self) -> &Reporter {
        &self.report
    }

    /// What a `ServiceDesignator` resolves to; see [`lookup_service`].
    pub fn lookup(&self, designator: &Designator) -> Rc<dyn Channel> {
        lookup_service(&self.services.0, &self.report, designator)
    }

    /// Every service, in config order.
    pub fn services(&self) -> impl Iterator<Item = (&str, &Service)> {
        self.services
            .0
            .iter()
            .map(|(name, service)| (name.as_str(), service))
    }

    /// Every worker service, in config order.
    fn workers(&self) -> impl Iterator<Item = (&str, &Rc<WorkerService>)> {
        self.services
            .0
            .iter()
            .filter_map(|(name, service)| Some((name.as_str(), service.as_worker()?)))
    }

    /// Aborts every evictable actor. `reason` becomes the error of the actors' in-flight requests.
    pub fn abort_all_actors(&self, reason: Option<&crate::Error>) {
        for (_, worker) in self.workers() {
            for (_, namespace) in worker.namespaces() {
                if namespace.is_evictable() {
                    namespace.abort_all(reason);
                }
            }
        }
    }

    /// Aborts every evictable actor and deletes its storage.
    pub fn delete_all_actors(&self, reason: Option<&crate::Error>) -> Result<()> {
        for (_, worker) in self.workers() {
            for (_, namespace) in worker.namespaces() {
                if namespace.is_evictable() {
                    namespace.delete_all(reason)?;
                }
            }
        }
        Ok(())
    }
}

impl Drop for Server {
    fn drop(&mut self) {
        // Actors may have background work, which aborting them cancels. The run's background
        // tasks (tasks.rs) go too: one may hold an actor, and so its class's worker.
        self.abort_all_actors(Some(&kj::disconnected!("Server shutting down.")));
        ffi::factory_clear_tasks(self.factory.raw());
        // The dynamic workers go before `services` unlinks the config's.
        let loaders = std::mem::take(&mut *self.worker_loaders.borrow_mut());
        for loader in loaders.by_id.values().chain(&loaders.anonymous) {
            loader.unlink();
        }
    }
}

/// The config's first pass: every worker's `durableObjectNamespaces`, checked.
///
/// Every service gets an entry, so that a binding to a class of a service that is not a worker
/// is a lookup failure rather than a missing entry.
pub fn collect_actor_configs(
    config: config::Reader<'_>,
    experimental: bool,
    report: &Reporter,
) -> capnp::Result<ActorConfigs> {
    // Every `uniqueKey` in the config, which no Workflow's namespace key may collide with.
    let mut durable_namespace_keys = HashSet::new();
    for service_conf in config.get_services()? {
        if let Ok(service::Which::Worker(worker_conf)) = service_conf.which() {
            for ns in worker_conf?.get_durable_object_namespaces()? {
                if let Ok(worker::durable_object_namespace::Which::UniqueKey(key)) = ns.which() {
                    durable_namespace_keys.insert(key?.to_string()?);
                }
            }
        }
    }
    let mut workflow_namespace_keys = HashSet::new();

    let mut actor_configs = ActorConfigs::new();
    for (service_index, service_conf) in (0..).zip(config.get_services()?) {
        let name = service_conf.get_name()?.to_string()?;
        let mut configs = ActorConfigMap::new();

        if let Ok(service::Which::Worker(worker_conf)) = service_conf.which() {
            let worker_conf = worker_conf?;
            let mut had_durable = false;
            for (namespace_index, ns) in (0..).zip(worker_conf.get_durable_object_namespaces()?) {
                let class_name = ns.get_class_name()?.to_string()?;
                match ns.which() {
                    Ok(worker::durable_object_namespace::Which::UniqueKey(unique_key)) => {
                        had_durable = true;
                        configs.insert(
                            class_name,
                            ActorConfig::Durable {
                                unique_key: unique_key?.to_string()?,
                                evictable: !ns.get_prevent_eviction(),
                                enable_sql: ns.get_enable_sql(),
                                workflow: None,
                                container: ns.has_container().then_some(ffi::ContainerRef {
                                    service_index,
                                    namespace_index,
                                }),
                            },
                        );
                    }
                    Ok(worker::durable_object_namespace::Which::EphemeralLocal(())) => {
                        if !experimental {
                            report.error(
                                "Ephemeral objects (Durable Object namespaces with type \
                                 'ephemeralLocal') are an experimental feature which may change \
                                 or go away in the future. You must run workerd with \
                                 `--experimental` to use this feature.",
                            );
                        }
                        configs.insert(
                            class_name,
                            ActorConfig::Ephemeral {
                                evictable: !ns.get_prevent_eviction(),
                                enable_sql: ns.get_enable_sql(),
                            },
                        );
                    }
                    Err(capnp::NotInSchema(_)) => report.error(format!(
                        "Encountered unknown DurableObjectNamespace type in service \"{name}\", \
                         class \"{class_name}\". Was the config compiled with a newer version of \
                         the schema?"
                    )),
                }
            }

            if worker_conf.has_workflows_engine() {
                collect_workflow_configs(
                    &name,
                    worker_conf.get_workflows_engine()?,
                    &durable_namespace_keys,
                    &mut workflow_namespace_keys,
                    &mut configs,
                    report,
                )?;
            }

            match worker_conf.get_durable_object_storage().which() {
                Ok(worker::durable_object_storage::Which::None(())) if had_durable => {
                    report.error(format!(
                        "Worker service \"{name}\" implements durable object classes but has \
                         `durableObjectStorage` set to `none`."
                    ));
                }
                Ok(_) => {}
                Err(capnp::NotInSchema(_)) => report.error(format!(
                    "Encountered unknown durableObjectStorage type in service \"{name}\". Was the \
                     config compiled with a newer version of the schema?"
                )),
            }

            // Meant for parameterized workers; nothing implements it, so nothing may set it.
            if worker_conf.has_durable_object_unique_key_modifier() {
                return Err(capnp::Error::failed(
                    "durableObjectUniqueKeyModifier is not implemented yet".to_owned(),
                ));
            }
        }

        if actor_configs.insert(name.clone(), configs).is_some() {
            report.error(format!(
                "Config defines multiple services named \"{name}\"."
            ));
        }
    }
    Ok(actor_configs)
}

/// The first pass over one worker's `workflowsEngine`: checks it, and for every valid Workflow
/// synthesizes the `Durable` config of the namespace backing it, keyed by
/// [`workflow_namespace_key`]. What the Workflow names in other services (the engine's class, the
/// `bindingService`) is resolved by [`init_workflows`] once every service exists.
fn collect_workflow_configs(
    name: &str,
    engine: workflows_engine::Reader<'_>,
    durable_namespace_keys: &HashSet<String>,
    workflow_namespace_keys: &mut HashSet<String>,
    configs: &mut ActorConfigMap,
    report: &Reporter,
) -> capnp::Result<()> {
    let has_actor_class =
        engine.has_actor_class() && !engine.get_actor_class()?.get_name()?.is_empty();
    if !has_actor_class {
        report.error(format!(
            "Worker service \"{name}\"'s workflowsEngine is missing actorClass."
        ));
    }
    let mut classes = HashSet::new();
    let mut names = HashSet::new();
    for workflow in engine.get_workflows()? {
        let class_name = workflow.get_class_name()?.to_string()?;
        let workflow_name = workflow.get_name()?.to_string()?;
        let mut valid = has_actor_class;

        if class_name.is_empty() {
            report.error(format!(
                "Worker service \"{name}\" configures a Workflow without className."
            ));
            valid = false;
        } else if !classes.insert(class_name.clone()) {
            report.error(format!(
                "Worker service \"{name}\" configures multiple Workflows for class \
                 \"{class_name}\"."
            ));
            valid = false;
        }

        if workflow_name.is_empty() {
            report.error(format!(
                "Worker service \"{name}\" configures a Workflow without name."
            ));
            valid = false;
        } else if workflow_name.contains(['/', '\\']) {
            report.error(format!(
                "Worker service \"{name}\" configures Workflow name \"{workflow_name}\" \
                 containing a path separator."
            ));
            valid = false;
        } else if !names.insert(workflow_name.clone()) {
            report.error(format!(
                "Worker service \"{name}\" configures multiple Workflows named \
                 \"{workflow_name}\"."
            ));
            valid = false;
        }

        if !workflow.has_binding_service() || workflow.get_binding_service()?.get_name()?.is_empty()
        {
            report.error(format!(
                "Worker service \"{name}\"'s Workflow \"{workflow_name}\" is missing \
                 bindingService."
            ));
            valid = false;
        }

        if !valid {
            continue;
        }

        let key = workflow_namespace_key(&workflow_name);
        if configs.contains_key(&key) {
            report.error(format!(
                "Worker service \"{name}\"'s Workflow namespace conflicts with Durable Object \
                 class \"{key}\"."
            ));
            continue;
        }
        if durable_namespace_keys.contains(&key) {
            report.error(format!(
                "Workflow ActorNamespace key \"{key}\" conflicts with a Durable Object namespace \
                 unique key."
            ));
            continue;
        }
        if !workflow_namespace_keys.insert(key.clone()) {
            report.error(format!(
                "Workflow ActorNamespace key \"{key}\" is configured by more than one Worker."
            ));
            continue;
        }
        let mut errors = Vec::new();
        let binding_service = Designator::from_reader(
            workflow.get_binding_service()?,
            format!("Worker \"{name}\"'s Workflow \"{workflow_name}\"'s bindingService"),
            &mut errors,
        )?;
        for error in errors {
            report.service_error(name, error);
        }
        configs.insert(
            key.clone(),
            ActorConfig::Durable {
                unique_key: key,
                // Workflow actors must not be evicted mid-run, and their state is SQL-backed.
                evictable: false,
                enable_sql: true,
                workflow: Some(Box::new(Workflow {
                    name: workflow_name,
                    class_name,
                    binding_service,
                })),
                container: None,
            },
        );
    }
    Ok(())
}

/// The pass after every service exists: for each Workflow the first pass found valid, resolves
/// the engine's actor class and the Workflow's `bindingService`, builds the props that tell the
/// engine which class it runs, and creates the namespace the first pass synthesized as one served
/// by the engine's class (`WorkerService::init_workflow_namespace`).
fn init_workflows(
    config: config::Reader<'_>,
    services: &LinkedHashMap<String, Service>,
    actor_configs: &ActorConfigs,
    report: &Reporter,
    namespaces_by_unique_key: &mut HashMap<String, Rc<ActorNamespace>>,
) -> Result<()> {
    for service_conf in config.get_services().map_err(capnp_error)? {
        let Ok(service::Which::Worker(worker_conf)) = service_conf.which() else {
            continue;
        };
        let worker_conf = worker_conf.map_err(capnp_error)?;
        if !worker_conf.has_workflows_engine() {
            continue;
        }
        let name = text(service_conf.get_name())?;
        let Some(Service::Worker(app)) = services.get(&name) else {
            report.service_error(
                &name,
                format!("Worker service \"{name}\" could not initialize its workflowsEngine."),
            );
            continue;
        };
        let conf = worker_conf.get_workflows_engine().map_err(capnp_error)?;
        let Some(engine) = WorkflowEngine::resolve(&name, conf, app, services, report)? else {
            continue;
        };

        for (key, config) in actor_configs.get(&name).into_iter().flatten() {
            let ActorConfig::Durable {
                workflow: Some(workflow),
                ..
            } = config
            else {
                continue;
            };
            if let Some(namespace) = engine.init_workflow(workflow, key, config)? {
                namespaces_by_unique_key.insert(key.clone(), namespace);
            }
        }
    }
    Ok(())
}

/// One worker's `workflowsEngine`, resolved: the app worker whose Workflows it runs, the engine
/// worker and the name of its class.
struct WorkflowEngine<'a> {
    /// The app worker's service name, which the errors are reported under.
    name: &'a str,
    app: &'a Rc<WorkerService>,
    engine: &'a Rc<WorkerService>,
    engine_name: String,
    engine_class: Option<String>,
    services: &'a LinkedHashMap<String, Service>,
    report: &'a Reporter,
}

impl<'a> WorkflowEngine<'a> {
    /// Resolves `conf`'s `actorClass` to its worker, or none after reporting why it cannot be
    /// (nothing, after the first pass reported the class missing).
    fn resolve(
        name: &'a str,
        conf: workflows_engine::Reader<'_>,
        app: &'a Rc<WorkerService>,
        services: &'a LinkedHashMap<String, Service>,
        report: &'a Reporter,
    ) -> Result<Option<Self>> {
        if !conf.has_actor_class() {
            return Ok(None);
        }
        let designator = conf.get_actor_class().map_err(capnp_error)?;
        let engine_name = text(designator.get_name())?;
        if engine_name.is_empty() {
            return Ok(None);
        }
        if !matches!(
            designator.get_props().which(),
            Ok(service_designator::props::Which::Empty(()))
        ) {
            report.service_error(
                name,
                "workflowsEngine.actorClass must not specify props; Workflow props are supplied \
                 by the runtime.",
            );
            return Ok(None);
        }
        let Some(engine_service) = services.get(&engine_name) else {
            report.service_error(
                name,
                format!(
                    "workflowsEngine.actorClass refers to a service \"{engine_name}\", but no \
                     such service is defined."
                ),
            );
            return Ok(None);
        };
        let Service::Worker(engine) = engine_service else {
            report.service_error(
                name,
                format!(
                    "workflowsEngine.actorClass refers to service \"{engine_name}\", but it is \
                     not a Worker."
                ),
            );
            return Ok(None);
        };
        let engine_class = optional_text(designator.has_entrypoint(), designator.get_entrypoint())?;
        Ok(Some(Self {
            name,
            app,
            engine,
            engine_name,
            engine_class,
            services,
            report,
        }))
    }

    /// Creates the namespace backing `workflow`, keyed `key`, on the app worker; or none after
    /// reporting why it cannot.
    fn init_workflow(
        &self,
        workflow: &Workflow,
        key: &str,
        config: &ActorConfig,
    ) -> Result<Option<Rc<ActorNamespace>>> {
        let (workflow_name, class_name) = (workflow.name.as_str(), workflow.class_name.as_str());
        if self.app.has_actor_class(key) {
            self.report.service_error(
                self.name,
                format!(
                    "Workflow \"{workflow_name}\" namespace \"{key}\" conflicts with an exported \
                     Durable Object class."
                ),
            );
            return Ok(None);
        }
        if !self.app.has_workflow_class(class_name) {
            self.report.service_error(
                self.name,
                format!(
                    "Workflow \"{workflow_name}\" refers to class \"{class_name}\", but the \
                     Worker exports no such WorkflowEntrypoint."
                ),
            );
            return Ok(None);
        }
        let Some(disk) = self.binding_storage(workflow_name, &workflow.binding_service) else {
            return Ok(None);
        };

        // The one engine class serves every Workflow, so a Workflow's identity travels in its
        // actors' props: `workflowClass` is a stub to the app worker's `WorkflowEntrypoint` (the
        // user code the engine runs), `workflowClassName` and `workflowName` name it. That is
        // why `workflowsEngine.actorClass` may carry no props of its own.
        let names = serde_json::json!({
            "workflowClassName": class_name,
            "workflowName": workflow_name,
        });
        let mut props = ffi::frankenvalue_from_json(&names.to_string());
        ffi::frankenvalue_set_service_stub(
            props.as_mut(),
            "workflowClass",
            SubrequestChannel::new(self.app.loopback_entrypoint(Some(class_name))?),
        );

        let engine_class = self.engine_class.as_deref();
        let Some(actor_class) = self.engine.actor_class(engine_class, Some(props), false) else {
            self.report.service_error(
                self.name,
                format!(
                    "workflowsEngine.actorClass refers to service \"{}\" with Durable Object \
                     entrypoint \"{}\", but no such class is exported.",
                    self.engine_name,
                    engine_class.unwrap_or("default")
                ),
            );
            return Ok(None);
        };
        // The namespace's storage is the `bindingService` worker's directory.
        let storage_path = match self.services.get(disk) {
            Some(Service::Disk(disk)) => disk.writable_path(),
            _ => None,
        };
        if storage_path.is_none() {
            self.report.service_error(
                self.name,
                format!(
                    "Workflow ActorNamespace \"{key}\" could not resolve its bindingService's \
                     durableObjectStorage.localDisk."
                ),
            );
        }
        // Restore-token persistence follows the engine's compatibility flags, not the app's.
        self.app
            .init_workflow_namespace(
                key,
                config.clone(),
                actor_class,
                storage_path,
                self.engine.persistent_self_tokens(),
            )
            .map(Some)
    }

    /// The `localDisk` storage service of the worker a Workflow's `bindingService` names (which
    /// the Workflow's namespace takes), once the worker is checked to export the stateless
    /// entrypoint the designator names; or none after reporting why it cannot serve.
    fn binding_storage(&self, workflow_name: &str, designator: &Designator) -> Option<&'a str> {
        let binding_name = &designator.service;
        let Some(binding_service) = self.services.get(binding_name) else {
            self.report.service_error(
                self.name,
                format!(
                    "Workflow \"{workflow_name}\"'s bindingService refers to a service \
                     \"{binding_name}\", but no such service is defined."
                ),
            );
            return None;
        };
        let Service::Worker(binding) = binding_service else {
            self.report.service_error(
                self.name,
                format!(
                    "Workflow \"{workflow_name}\"'s bindingService refers to service \
                     \"{binding_name}\", but it is not a Worker."
                ),
            );
            return None;
        };
        let entrypoint = designator.entrypoint.as_deref();
        if !binding.has_stateless_entrypoint(entrypoint) {
            self.report.service_error(
                self.name,
                format!(
                    "Workflow \"{workflow_name}\"'s bindingService Worker does not export \
                     WorkerEntrypoint \"{}\".",
                    entrypoint.unwrap_or("default")
                ),
            );
            return None;
        }
        let disk = binding.storage();
        if disk.is_none() {
            self.report.service_error(
                self.name,
                format!(
                    "Workflow \"{workflow_name}\"'s bindingService Worker must configure \
                     durableObjectStorage.localDisk; in-memory and absent storage are unsupported."
                ),
            );
        }
        disk
    }
}

/// What a `ServiceDesignator` resolves to, or the invalid-config stand-in after the error was
/// reported.
pub fn lookup_service(
    services: &LinkedHashMap<String, Service>,
    report: &Reporter,
    designator: &Designator,
) -> Rc<dyn Channel> {
    let Designator {
        service: target,
        entrypoint,
        props_json,
        error_context: context,
    } = designator;
    let Some(service) = services.get(target) else {
        report.error(format!(
            "{context} refers to a service \"{target}\", but no such service is defined."
        ));
        return Rc::new(InvalidConfigChannel);
    };
    let props = configured_props(props_json.as_deref());

    let Service::Worker(worker) = service else {
        if let Some(entrypoint) = entrypoint {
            report.error(format!(
                "{context} refers to service \"{target}\" with a named entrypoint \
                 \"{entrypoint}\", but \"{target}\" is not a Worker, so does not have any \
                 named entrypoints."
            ));
        } else if !ffi::frankenvalue_is_empty(&props) {
            report.error(format!(
                "{context} refers to service \"{target}\" and provides a `props` value, but \
                 \"{target}\" is not a Worker, so cannot accept `props`"
            ));
        }
        return service.channel();
    };
    if let Some(channel) = worker.entrypoint(entrypoint.as_deref(), Some(props), false) {
        channel
    } else if let Some(entrypoint) = entrypoint {
        report.error(format!(
            "{context} refers to service \"{target}\" with a named entrypoint \
             \"{entrypoint}\", but \"{target}\" has no such named entrypoint."
        ));
        Rc::new(InvalidConfigChannel)
    } else {
        report.error(format!(
            "{context} refers to service \"{target}\", but does not specify an \
             entrypoint, and the service does not have a default entrypoint."
        ));
        Rc::new(InvalidConfigChannel)
    }
}

/// A configured designator's props, empty for `props.empty`: only a `ctx.exports` template goes
/// without props, and a configured binding is never one.
fn configured_props(props_json: Option<&str>) -> KjOwn<ffi::Frankenvalue> {
    props_json.map_or_else(ffi::frankenvalue_new, ffi::frankenvalue_from_json)
}

/// What a `ServiceDesignator` naming a Durable Object class resolves to, or the invalid-config
/// stand-in after the error was reported.
pub fn lookup_actor_class(
    services: &LinkedHashMap<String, Service>,
    report: &Reporter,
    designator: &Designator,
) -> Rc<dyn ActorClass> {
    let Designator {
        service: target,
        entrypoint,
        props_json,
        error_context: context,
    } = designator;
    let Some(service) = services.get(target) else {
        report.error(format!(
            "{context} refers to a service \"{target}\", but no such service is defined."
        ));
        return Rc::new(InvalidConfigActorClass);
    };
    let props = configured_props(props_json.as_deref());

    let Service::Worker(worker) = service else {
        if let Some(entrypoint) = entrypoint {
            report.error(format!(
                "{context} refers to service \"{target}\" with a named Durable Object \
                 entrypoint \"{entrypoint}\", but \"{target}\" is not a Worker, so does not \
                 have any named entrypoints."
            ));
        } else {
            report.error(format!(
                "{context} refers to service \"{target}\" as a Durable Object class, but \
                 \"{target}\" is not a Worker, so cannot be used as a class."
            ));
        }
        return Rc::new(InvalidConfigActorClass);
    };
    if let Some(class) = worker.actor_class(entrypoint.as_deref(), Some(props), false) {
        class
    } else if let Some(entrypoint) = entrypoint {
        report.error(format!(
            "{context} refers to service \"{target}\" with a Durable Object entrypoint \
             \"{entrypoint}\", but \"{target}\" has no such exported entrypoint class."
        ));
        Rc::new(InvalidConfigActorClass)
    } else {
        report.error(format!(
            "{context} refers to service \"{target}\", but does not specify an \
             entrypoint, and the service does export a Durable Object class as its \
             default entrypoint."
        ));
        Rc::new(InvalidConfigActorClass)
    }
}

impl Server {
    /// Builds the service graph: the actor configs, then every service in config order, then
    /// the links. Config errors go to `report`; the graph exists either way, so that the CLI can
    /// report every error before it refuses to serve.
    ///
    /// `inbound_listeners` are the sockets bound ahead of the services, by the service they
    /// serve, for `Worker::Api::getInboundListeners()`.
    pub async fn start(
        factory: Rc<Factory>,
        overrides: &Overrides,
        report: Reporter,
        mut inbound_listeners: HashMap<String, Vec<ffi::InboundListener>>,
    ) -> Result<Rc<Self>> {
        let message = factory.config()?;
        let config = message.get_root::<config::Reader>().map_err(capnp_error)?;

        // First pass: the actor configs, which bindings to other workers' classes are checked
        // against.
        let actor_configs =
            collect_actor_configs(config, factory.experimental(), &report).map_err(capnp_error)?;

        // Second pass: the services.
        let mut services = Services::default();
        let mut namespaces_by_unique_key = HashMap::new();
        let service_confs = config.get_services().map_err(capnp_error)?;
        for (index, service_conf) in (0..).zip(service_confs) {
            let name = text(service_conf.get_name())?;
            let service = make_service(
                &factory,
                &name,
                index,
                service_conf,
                &actor_configs,
                inbound_listeners.remove(&name).unwrap_or_default(),
                overrides,
                &report,
            )
            .await?;
            if let Service::Worker(worker) = &service {
                for (_, namespace) in worker.namespaces() {
                    if let Some(unique_key) = namespace.unique_key() {
                        namespaces_by_unique_key.insert(unique_key.to_owned(), namespace);
                    }
                }
            }
            // A service of the same name (the first pass reported it) is replaced.
            if let Some(Service::Worker(replaced)) = services.0.insert(name, service) {
                replaced.unlink();
            }
        }

        if !services.0.contains_key("internet") {
            let internet = crate::services::make_default_network_service(&factory)?;
            services
                .0
                .insert("internet".to_owned(), Service::Leaf(internet));
        }

        // Every service exists: wire each worker's Workflows to the engine that runs them.
        init_workflows(
            config,
            &services.0,
            &actor_configs,
            &report,
            &mut namespaces_by_unique_key,
        )?;

        let server = Rc::new(Self {
            report,
            services,
            namespaces_by_unique_key,
            worker_loaders: RefCell::new(WorkerLoaders::default()),
            factory,
        });
        let handle = Box::new(ServerHandle(Rc::downgrade(&server)));
        ffi::factory_set_server(server.factory.raw(), handle);

        // Third pass: the links.
        for (name, worker) in server.workers() {
            server.link_worker(name, worker)?;
        }
        Ok(server)
    }

    /// Resolves everything `worker`'s config names into its channel table.
    fn link_worker(self: &Rc<Self>, name: &str, worker: &Rc<WorkerService>) -> Result<()> {
        let Some(pending) = worker.take_pending_link() else {
            return Err(kj::failed!("service \"{name}\" was already linked"));
        };
        let report = &self.report;
        let services = &self.services.0;
        let lookup = |designator: &Designator| lookup_service(services, report, designator);
        let lookup_class =
            |designator: &Designator| lookup_actor_class(services, report, designator);

        // Both special channels ("next" and "null") reach the global outbound; the difference is
        // a legacy artifact.
        let global = lookup(&pending.global_outbound);
        let mut subrequest: Vec<Rc<dyn Channel>> = Vec::new();
        for _ in 0..SPECIAL_SUBREQUEST_CHANNEL_COUNT {
            subrequest.push(Rc::clone(&global));
        }
        subrequest.extend(pending.bindings.subrequest.iter().map(lookup));
        // The loopback channels, in the order `ctx.exports` was numbered with.
        for entrypoint in &pending.loopback.subrequest_entrypoints {
            subrequest.push(worker.loopback_entrypoint(entrypoint.as_deref())?);
        }
        // The access binding's entrypoint is a template: its props are each request's identity.
        let mut access_binding_channel = None;
        if let Some(designator) = &pending.access_binding {
            let target = &designator.service;
            match services.get(target) {
                Some(Service::Worker(target_worker)) => {
                    access_binding_channel = Some(len_u32(&subrequest));
                    subrequest
                        .push(target_worker.loopback_entrypoint(designator.entrypoint.as_deref())?);
                }
                Some(_) => report.error(format!(
                    "Worker accessBindingService refers to service \"{target}\", but it is not a \
                     Worker."
                )),
                None => report.error(format!(
                    "Worker accessBindingService refers to a service \"{target}\", but no such \
                     service is defined."
                )),
            }
        }

        let mut actor_class: Vec<Rc<dyn ActorClass>> = Vec::new();
        actor_class.extend(pending.bindings.actor_classes.iter().map(lookup_class));
        for class_name in &pending.loopback.actor_classes {
            actor_class.push(worker.loopback_actor_class(class_name)?);
        }

        // A namespace binding whose service or class does not exist was reported by the
        // bindings compiler; its channel stays empty.
        let mut actor: Vec<Option<Rc<ActorNamespace>>> = Vec::new();
        for designator in &pending.bindings.actors {
            let target = match &designator.service {
                Some(service) => services.get(service).and_then(Service::as_worker),
                None => Some(worker),
            };
            actor.push(target.and_then(|target| target.namespace(&designator.class_name)));
        }
        for class_name in &pending.loopback.actor_namespaces {
            actor.push(worker.namespace(class_name));
        }

        let cache = pending.cache_api_outbound.as_ref().map(lookup);

        let storage_path = self.storage_path(name, pending.storage.as_deref());

        let tails = pending.tails.iter().map(lookup).collect();
        let streaming_tails = pending.streaming_tails.iter().map(lookup).collect();

        let worker_loaders = pending
            .bindings
            .worker_loaders
            .iter()
            .map(|designator| self.worker_loader(designator))
            .collect();

        worker.link(
            LinkedChannels {
                first_subrequest: 0,
                first_actor_class: 0,
                subrequest,
                actor,
                actor_class,
                cache,
                tails,
                streaming_tails,
                worker_loaders,
                access_binding_channel,
                has_debug_port: pending.bindings.has_debug_port,
                server: Rc::downgrade(self),
                abort_isolate: None,
            },
            storage_path.as_deref(),
        )
    }

    /// The directory a worker's Durable Objects persist to: the writable path of its
    /// `durableObjectStorage` disk service. `None` for in-memory storage, and after reporting a
    /// disk service that is missing, read-only, or not a disk.
    fn storage_path(&self, name: &str, disk_name: Option<&str>) -> Option<String> {
        let disk_name = disk_name?;
        match self.services.0.get(disk_name) {
            Some(Service::Disk(disk)) => {
                let path = disk.writable_path();
                if path.is_none() {
                    self.report.service_error(
                        name,
                        format!(
                            "durableObjectStorage config refers to the disk service \
                             \"{disk_name}\", but that service is defined read-only."
                        ),
                    );
                }
                path.map(str::to_owned)
            }
            Some(_) => {
                self.report.service_error(
                    name,
                    format!(
                        "durableObjectStorage config refers to the service \"{disk_name}\", \
                         but that service is not a local disk service."
                    ),
                );
                None
            }
            None => {
                self.report.service_error(
                    name,
                    format!(
                        "durableObjectStorage config refers to a service \"{disk_name}\", but \
                         no such service is defined."
                    ),
                );
                None
            }
        }
    }

    /// The loader namespace of one `workerLoader` binding: shared with every binding of the same
    /// id, or the binding's own when it has none.
    fn worker_loader(
        self: &Rc<Self>,
        designator: &WorkerLoaderDesignator,
    ) -> Rc<WorkerLoaderNamespace> {
        let mut loaders = self.worker_loaders.borrow_mut();
        if let Some(id) = &designator.id {
            return Rc::clone(loaders.by_id.entry(id.clone()).or_insert_with(|| {
                WorkerLoaderNamespace::new(Rc::downgrade(self), designator.name.clone())
            }));
        }
        let loader = WorkerLoaderNamespace::new(Rc::downgrade(self), designator.name.clone());
        loaders.anonymous.push(Rc::clone(&loader));
        loader
    }
}

/// A service whose construction failed is a config error: reported, and replaced by a service
/// that fails every request.
fn invalid_on_error(name: &str, report: &Reporter, service: Result<Service>) -> Service {
    match service {
        Ok(service) => service,
        Err(error) => {
            report.service_error(name, error.description());
            Service::Leaf(Rc::new(InvalidConfigChannel))
        }
    }
}

/// The second pass for one service: what its config says it serves.
#[expect(
    clippy::too_many_arguments,
    reason = "everything one service takes from the config pass"
)]
async fn make_service(
    factory: &Rc<Factory>,
    name: &str,
    index: u32,
    conf: service::Reader<'_>,
    actor_configs: &ActorConfigs,
    inbound_listeners: Vec<ffi::InboundListener>,
    overrides: &Overrides,
    report: &Reporter,
) -> Result<Service> {
    match conf.which() {
        Ok(service::Which::Unspecified(())) => {
            report.error(format!(
                "Service named \"{name}\" does not specify what to serve."
            ));
            Ok(Service::Leaf(Rc::new(InvalidConfigChannel)))
        }
        Ok(service::Which::External(external)) => {
            let external = external.map_err(capnp_error)?;
            let address = overrides.externals.get(name).map(String::as_str);
            Ok(invalid_on_error(
                name,
                report,
                crate::services::make_external_service(name, external, address, factory)
                    .map(Service::Leaf),
            ))
        }
        Ok(service::Which::Network(network)) => {
            let network = network.map_err(capnp_error)?;
            Ok(invalid_on_error(
                name,
                report,
                crate::services::make_network_service(network, factory).map(Service::Leaf),
            ))
        }
        Ok(service::Which::Worker(worker)) => {
            let worker = worker.map_err(capnp_error)?;
            WorkerService::new(
                Rc::clone(factory),
                name,
                index,
                worker,
                actor_configs,
                inbound_listeners,
                report,
            )
            .await
            .map(Service::Worker)
        }
        Ok(service::Which::Disk(disk)) => {
            let disk = disk.map_err(capnp_error)?;
            let path = overrides.directories.get(name).map(String::as_str);
            Ok(invalid_on_error(
                name,
                report,
                crate::services::make_disk_directory_service(name, disk, path, factory)
                    .map(Service::Disk),
            ))
        }
        Err(capnp::NotInSchema(_)) => {
            report.error(format!(
                "Service named \"{name}\" has unrecognized type. Was the config compiled with a \
                 newer version of the schema?"
            ));
            Ok(Service::Leaf(Rc::new(InvalidConfigChannel)))
        }
    }
}

/// Channel tokens and debug-port requests, resolved into the graph.
impl ServerHandle {
    pub(crate) fn resolve_entrypoint(
        &self,
        service_name: &str,
        entrypoint: KjMaybe<&str>,
        props: KjOwn<Frankenvalue>,
        persistent: bool,
    ) -> Result<Box<SubrequestChannel>> {
        let entrypoint: Option<&str> = entrypoint.into();
        self.server()?
            .stub_worker(service_name)?
            .entrypoint(entrypoint, Some(props), persistent)
            .map(SubrequestChannel::new)
            .ok_or_else(|| {
                kj::failed!(
                    "jsg.Error: Stub refers to a an entrypoint of the target service that doesn't \
                     exist: {}",
                    entrypoint.unwrap_or("default")
                )
            })
    }

    pub(crate) fn resolve_actor_class(
        &self,
        service_name: &str,
        class_name: KjMaybe<&str>,
        props: KjOwn<Frankenvalue>,
        persistent: bool,
    ) -> Result<Box<ActorClassChannel>> {
        let class_name: Option<&str> = class_name.into();
        self.server()?
            .stub_worker(service_name)?
            .actor_class(class_name, Some(props), persistent)
            .map(ActorClassChannel::new)
            .ok_or_else(|| {
                kj::failed!(
                    "jsg.Error: Stub refers to a an entrypoint of the target service that doesn't \
                     exist: {}",
                    class_name.unwrap_or("default")
                )
            })
    }

    pub(crate) fn resolve_actor(
        &self,
        unique_key: &str,
        id: KjOwn<ActorIdHandle>,
        persistent: bool,
    ) -> Result<Box<SubrequestChannel>> {
        let server = self.server()?;
        let namespace = server
            .namespaces_by_unique_key
            .get(unique_key)
            .ok_or_else(|| {
                kj::failed!(
                    "couldn't deserialize actor stub pointing at unknown namespace; namespaceKey = \
                 {unique_key}"
                )
            })?;
        Ok(SubrequestChannel::new(namespace.channel(id, persistent)))
    }

    /// The debug port's `getEntrypoint`: any service, a worker's by entrypoint. Its errors name
    /// what the request asked for, where a channel token's name the stub.
    pub(crate) fn resolve_debug_entrypoint(
        &self,
        service_name: &str,
        entrypoint: KjMaybe<&str>,
        props: KjMaybe<KjOwn<Frankenvalue>>,
    ) -> Result<Box<SubrequestChannel>> {
        let entrypoint: Option<&str> = entrypoint.into();
        let props: Option<KjOwn<Frankenvalue>> = props.into();
        let server = self.server()?;
        let service = server.debug_service(service_name)?;
        let Some(worker) = service.as_worker() else {
            if entrypoint.is_some() {
                return Err(kj::failed!(
                    "jsg.Error: Worker does not support named entrypoints"
                ));
            }
            return Ok(SubrequestChannel::new(match props {
                Some(props) => service.channel().for_props(props, false)?,
                None => service.channel(),
            }));
        };
        worker
            .entrypoint(
                entrypoint,
                Some(props.unwrap_or_else(ffi::frankenvalue_new)),
                false,
            )
            .map(SubrequestChannel::new)
            .ok_or_else(|| {
                kj::failed!(
                    "jsg.Error: Worker does not export an entrypoint named \"{}\"",
                    entrypoint.unwrap_or("(default)")
                )
            })
    }

    pub(crate) fn resolve_debug_actor(
        &self,
        service_name: &str,
        class_name: &str,
        actor_id: &str,
    ) -> Result<Box<SubrequestChannel>> {
        let server = self.server()?;
        let service = server.debug_service(service_name)?;
        let worker = service
            .as_worker()
            .ok_or_else(|| kj::failed!("jsg.Error: Worker does not support Durable Objects"))?;
        let namespace = worker.namespace(class_name).ok_or_else(|| {
            kj::failed!(
                "jsg.Error: Worker does not export a Durable Object class named \"{class_name}\""
            )
        })?;
        Ok(SubrequestChannel::new(match namespace.unique_key() {
            Some(_) => namespace.channel(ffi::actor_id_from_hex(actor_id)?, false),
            None => namespace.channel_by_name(actor_id, false),
        }))
    }
}

impl Server {
    /// The service a debug-port request names.
    fn debug_service(&self, service_name: &str) -> Result<&Service> {
        self.services
            .0
            .get(service_name)
            .ok_or_else(|| kj::failed!("jsg.Error: Worker \"{service_name}\" not found"))
    }

    /// The worker a channel token names.
    fn stub_worker(&self, service_name: &str) -> Result<&Rc<WorkerService>> {
        let service = self.services.0.get(service_name).ok_or_else(|| {
            kj::failed!("jsg.Error: Stub refers to a service that doesn't exist: {service_name}")
        })?;
        service.as_worker().ok_or_else(|| {
            kj::failed!("jsg.Error: Stub refers to a service that is not a Worker: {service_name}")
        })
    }
}

#[cfg(test)]
#[path = "config-test.rs"]
mod tests;
