// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! A worker's config bindings compiled into the `Globals` message (compiled-bindings.capnp) and
//! its channel tables.
//!
//! [`compile_bindings`] interprets `Worker.bindings`: every value becomes a `Global`, and every
//! capability gets the next channel number of its kind. The channel tables record what each
//! number must resolve to; the link stage fills them once every service exists.
//! [`loopback_globals`] does the same for `ctx.exports`, numbering the worker's own entrypoints and
//! actor classes after the bindings' channels.

use std::collections::HashMap;
use std::env;

use base64::Engine as _;
use base64::engine::DecodePaddingMode;
use base64::engine::GeneralPurpose;
use base64::engine::GeneralPurposeConfig;
use capnp::message;
use capnp::message::HeapAllocator;
use capnp::struct_list;
use compiled_bindings_capnp::global;
use compiled_bindings_capnp::globals;
use hashlink::LinkedHashMap;
use rustls::pki_types::pem::PemObject;
use rustls::pki_types::pem::SectionKind;
use workerd_capnp::service_designator;
use workerd_capnp::worker;
use workerd_capnp::worker::binding;

/// The limits of a `durableObjectNamespace` binding's `retryPolicy`: the retries after the first
/// attempt, and the milliseconds from the call's start after which none may begin
/// (`api::UserDefinedRetryPolicy` in actor-call-retry.h).
const RETRY_MAX_CONFIGURABLE_ATTEMPTS: u32 = 10;
const RETRY_CONFIGURABLE_TIMEOUT_MS: std::ops::RangeInclusive<u32> = 500..=60_000;

/// `IoContext::SPECIAL_SUBREQUEST_CHANNEL_COUNT` (src/workerd/io/io-context.h): the subrequest
/// channels with a special meaning (the global outbound), ahead of the bindings' channels.
pub const SPECIAL_SUBREQUEST_CHANNEL_COUNT: u32 = 2;

/// A `ServiceDesignator` from the config: what a subrequest or actor-class channel resolves to.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Designator {
    pub service: String,
    pub entrypoint: Option<String>,
    /// `props.json`; none for `props.empty`.
    pub props_json: Option<String>,
    /// The prefix of the errors the link stage reports for this channel:
    /// `Worker "<worker>"'s binding "<binding>"`.
    pub error_context: String,
}

impl Designator {
    /// Reads a designator. An unrecognized `props` kind is reported to `errors` and read as empty.
    pub fn from_reader(
        reader: service_designator::Reader<'_>,
        error_context: String,
        errors: &mut Vec<String>,
    ) -> capnp::Result<Self> {
        let service = reader.get_name()?.to_string()?;
        let entrypoint = if reader.has_entrypoint() {
            Some(reader.get_entrypoint()?.to_string()?)
        } else {
            None
        };
        let props_json = match reader.get_props().which() {
            Ok(service_designator::props::Which::Empty(())) => None,
            Ok(service_designator::props::Which::Json(json)) => Some(json?.to_string()?),
            Err(capnp::NotInSchema(_)) => {
                errors.push(format!(
                    "{error_context} has unrecognized props type. Was the config compiled with a \
                     newer version of the schema?"
                ));
                None
            }
        };
        Ok(Self {
            service,
            entrypoint,
            props_json,
            error_context,
        })
    }
}

/// A Durable Object namespace binding's target: a class of `service`, or of the worker itself
/// when there is none.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ActorDesignator {
    pub service: Option<String>,
    pub class_name: String,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct WorkerLoaderDesignator {
    /// For error logging; the binding's name when the loader has no id, so not necessarily unique.
    pub name: String,
    /// Bindings with the same id share one loader.
    pub id: Option<String>,
}

/// A configured Workflow (`workflowsEngine.workflows`) the config's first pass found valid.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Workflow {
    pub name: String,
    pub class_name: String,
    pub binding_service: Designator,
}

/// A Durable Object namespace's storage: durable with its unique key, or ephemeral. What the
/// config's first pass learns from `durableObjectNamespaces` for every worker.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ActorConfig {
    Durable {
        unique_key: String,
        evictable: bool,
        enable_sql: bool,
        /// The Workflow a namespace was synthesized to back, rather than declared in
        /// `durableObjectNamespaces`: its actors are the engine's class, and its storage the
        /// Workflow's `bindingService` worker's, so the worker's own namespace setup skips it
        /// (`WorkerService::init_workflow_namespace`).
        workflow: Option<Box<Workflow>>,
        /// Where the config has the class's `container` options, if it has any.
        container: Option<crate::bridge::ffi::ContainerRef>,
    },
    Ephemeral {
        evictable: bool,
        enable_sql: bool,
    },
}

/// One worker's namespaces by class name, in config order.
pub type ActorConfigMap = LinkedHashMap<String, ActorConfig>;

/// Every worker's namespaces, by service name.
pub type ActorConfigs = HashMap<String, ActorConfigMap>;

/// The result of compiling a worker's bindings.
#[derive(Debug, Default)]
pub struct CompiledBindings {
    /// The encoded `Globals` message (compiled-bindings.capnp), in words: the worker's `env`.
    pub globals: Vec<u64>,
    /// Subrequest channels: index `i` is channel `i + SPECIAL_SUBREQUEST_CHANNEL_COUNT`. After
    /// the bindings' channels come one per configured Workflow, to its `bindingService`.
    pub subrequest: Vec<Designator>,
    /// The subrequest channel of each configured Workflow's `bindingService`, by the Workflow's
    /// class name: the inner fetcher of the Workflow's `ctx.exports` binding.
    pub workflow_binding_channels: HashMap<String, u32>,
    /// Actor channels, indexed by channel number.
    pub actors: Vec<ActorDesignator>,
    /// Actor-class channels, indexed by channel number.
    pub actor_classes: Vec<Designator>,
    /// Worker-loader channels, indexed by channel number.
    pub worker_loaders: Vec<WorkerLoaderDesignator>,
    pub has_debug_port: bool,
    /// Config errors, without the `service <name>: ` prefix (the caller adds it).
    pub errors: Vec<String>,
}

/// What the worker exports, as its compilation reported them, in that order.
#[derive(Clone, Copy, Debug)]
pub struct Exports<'a> {
    pub has_default_entrypoint: bool,
    /// Every named entrypoint, workflow classes included.
    pub named_entrypoints: &'a [String],
    pub workflow_classes: &'a [String],
    pub actor_classes: &'a [String],
}

/// The `ctx.exports` globals and the loopback channels they use, in the order the link stage
/// appends them to the bindings' tables.
#[derive(Debug, Default, PartialEq, Eq)]
pub struct LoopbackGlobals {
    /// The encoded `Globals` message: the worker's `ctx.exports`.
    pub globals: Vec<u64>,
    /// The subrequest channels after the bindings': the entrypoint each resolves to, none for the
    /// default.
    pub subrequest_entrypoints: Vec<Option<String>>,
    /// The actor-class channels after the bindings': one per exported class.
    pub actor_classes: Vec<String>,
    /// The actor channels after the bindings': the exported classes that have a namespace.
    pub actor_namespaces: Vec<String>,
}

/// Compiles `worker`'s bindings. `local_actor_configs` are `worker_name`'s own.
///
/// A malformed message fails; a binding the config gets wrong is reported in `errors` and left
/// out of the globals. A `parameter` binding is not implemented and fails the compilation.
pub fn compile_bindings(
    worker_name: &str,
    worker: worker::Reader<'_>,
    actor_configs: &ActorConfigs,
    local_actor_configs: &ActorConfigMap,
    experimental: bool,
) -> capnp::Result<CompiledBindings> {
    let mut compiler = Compiler {
        worker_name,
        service_worker_script: worker.has_service_worker_script(),
        actor_configs,
        local_actor_configs,
        experimental,
        out: CompiledBindings::default(),
    };
    let mut globals = Vec::new();
    for binding in worker.get_bindings()? {
        if let Some(global) = compiler.global(binding)? {
            globals.push(global);
        }
    }
    let mut out = compiler.out;
    out.globals = encode_globals(&globals)?;
    // After the bindings' channels, one subrequest channel per Workflow, to its `bindingService`.
    for config in local_actor_configs.values() {
        if let ActorConfig::Durable {
            workflow: Some(workflow),
            ..
        } = config
        {
            let channel = len_u32(&out.subrequest) + SPECIAL_SUBREQUEST_CHANNEL_COUNT;
            out.subrequest.push(workflow.binding_service.clone());
            out.workflow_binding_channels
                .insert(workflow.class_name.clone(), channel);
        }
    }
    Ok(out)
}

/// Miniflare's own Workflow engine namespaces use this key prefix, which determines actor IDs and
/// the storage subdirectory; matching it lets both reach the same local instances.
///
/// TODO(cleanup): Make this configurable rather than hardcoding Miniflare's prefix; a configurable
///   key must keep this value for Miniflare.
pub const WORKFLOW_NAMESPACE_KEY_PREFIX: &str = "miniflare-workflows-";

/// The unique key of the namespace backing the Workflow `workflow_name`.
#[must_use]
pub fn workflow_namespace_key(workflow_name: &str) -> String {
    format!("{WORKFLOW_NAMESPACE_KEY_PREFIX}{workflow_name}")
}

/// The `ctx.exports` globals, numbered after the `subrequest_channels`, `actor_channels` and
/// `actor_class_channels` the worker's `env` took.
///
/// Every exported entrypoint gets a loopback stub, except a Workflow class: a configured Workflow
/// is a wrapped binding (`cloudflare-internal:workflows-api` over a fetcher to the Workflow's
/// `bindingService` channel, which `workflow_binding_channel` gives for its class), and an
/// exported `WorkflowEntrypoint` the config does not list under `workflowsEngine.workflows` has
/// no entry. Every exported actor class gets a loopback namespace when `local_actor_configs`
/// gives the class one, and a loopback actor class otherwise.
pub fn loopback_globals(
    exports: &Exports<'_>,
    local_actor_configs: &ActorConfigMap,
    workflow_binding_channel: impl Fn(&str) -> Option<u32>,
    subrequest_channels: u32,
    actor_channels: u32,
    actor_class_channels: u32,
) -> capnp::Result<LoopbackGlobals> {
    let mut out = LoopbackGlobals::default();
    let mut globals = Vec::new();

    let mut next_subrequest = subrequest_channels + SPECIAL_SUBREQUEST_CHANNEL_COUNT;
    if exports.has_default_entrypoint {
        globals.push(new_global("default", |mut global| {
            global.set_loopback_service_stub(next_subrequest);
        }));
        next_subrequest += 1;
        out.subrequest_entrypoints.push(None);
    }
    for name in exports.named_entrypoints {
        if exports.workflow_classes.contains(name) {
            if let Some(channel) = workflow_binding_channel(name) {
                globals.push(new_global(name, |global| {
                    let mut wrapped = global.init_wrapped();
                    wrapped.set_module_name("cloudflare-internal:workflows-api");
                    wrapped.set_entrypoint("default");
                    let mut fetcher = wrapped.init_inner_bindings(1).get(0);
                    fetcher.set_name("fetcher");
                    fetcher.set_fetcher(channel);
                }));
            }
            continue;
        }
        out.subrequest_entrypoints.push(Some(name.clone()));
        globals.push(new_global(name, |mut global| {
            global.set_loopback_service_stub(next_subrequest);
        }));
        next_subrequest += 1;
    }

    let mut next_actor = actor_channels;
    for (class_channel, class_name) in (actor_class_channels..).zip(exports.actor_classes) {
        out.actor_classes.push(class_name.clone());
        let config = local_actor_configs.get(class_name);
        if config.is_some() {
            out.actor_namespaces.push(class_name.clone());
        }
        globals.push(new_global(class_name, |mut global| match config {
            Some(ActorConfig::Durable { unique_key, .. }) => {
                let mut namespace = global.init_loopback_durable_actor_namespace();
                namespace.set_actor_channel(next_actor);
                namespace.set_unique_key(unique_key.as_str());
                namespace.set_class_channel(class_channel);
            }
            Some(ActorConfig::Ephemeral { .. }) => {
                let mut namespace = global.init_loopback_ephemeral_actor_namespace();
                namespace.set_actor_channel(next_actor);
                namespace.set_class_channel(class_channel);
            }
            None => global.set_loopback_actor_class(class_channel),
        }));
        if config.is_some() {
            next_actor += 1;
        }
    }

    out.globals = encode_globals(&globals)?;
    Ok(out)
}

/// One `Global` in a message of its own, so that a list of globals can be sized once it is known
/// which bindings compiled.
type GlobalMessage = message::Builder<HeapAllocator>;

fn new_global(name: &str, value: impl FnOnce(global::Builder<'_>)) -> GlobalMessage {
    let mut message = message::Builder::new_default();
    let mut global = message.init_root::<global::Builder>();
    global.set_name(name);
    value(global);
    message
}

/// A `Globals` message of `globals`, encoded.
fn encode_globals(globals: &[GlobalMessage]) -> capnp::Result<Vec<u64>> {
    let mut message = message::Builder::new_default();
    let list = message
        .init_root::<globals::Builder>()
        .init_globals(len_u32(globals));
    set_globals(list, globals)?;
    Ok(capnp::serialize::write_message_to_words(&message)
        .chunks_exact(8)
        .map(|chunk| u64::from_ne_bytes(chunk.try_into().unwrap_or_default()))
        .collect())
}

fn set_globals(
    mut list: struct_list::Builder<'_, global::Owned>,
    globals: &[GlobalMessage],
) -> capnp::Result<()> {
    for (index, global) in (0..).zip(globals) {
        list.set_with_caveats(index, global.get_root_as_reader::<global::Reader>()?)?;
    }
    Ok(())
}

/// A table's length as the next channel number or a list length: there is one entry per config
/// binding, and a capnp list is shorter than 2^32.
pub fn len_u32<T>(items: &[T]) -> u32 {
    items.len() as u32
}

/// The state of one worker's compilation.
struct Compiler<'a> {
    worker_name: &'a str,
    /// Whether the worker is a service-worker-syntax script, whose Wasm bindings are modules.
    service_worker_script: bool,
    actor_configs: &'a ActorConfigs,
    local_actor_configs: &'a ActorConfigMap,
    experimental: bool,
    out: CompiledBindings,
}

impl Compiler<'_> {
    fn error(&mut self, error: String) {
        self.out.errors.push(error);
    }

    /// The binding's `Global`, or none after reporting why it has none.
    fn global(&mut self, binding: binding::Reader<'_>) -> capnp::Result<Option<GlobalMessage>> {
        let name = binding.get_name()?.to_str()?;
        let error_context = format!("Worker \"{}\"'s binding \"{name}\"", self.worker_name);
        let mut message = message::Builder::new_default();
        let mut global = message.init_root::<global::Builder>();
        global.set_name(name);
        let Ok(which) = binding.which() else {
            self.error(format!(
                "{error_context}has unrecognized type. Was the config compiled with a newer \
                 version of the schema?"
            ));
            return Ok(None);
        };
        Ok(self
            .value(which, name, error_context, global)?
            .then_some(message))
    }

    /// Writes the binding's value into `global`; false after reporting why it has none.
    fn value(
        &mut self,
        which: binding::WhichReader<'_>,
        name: &str,
        error_context: String,
        mut global: global::Builder<'_>,
    ) -> capnp::Result<bool> {
        use binding::Which;

        match which {
            Which::Unspecified(()) => {
                self.error(format!(
                    "{error_context} does not specify any binding value."
                ));
                return Ok(false);
            }
            Which::Parameter(_) => {
                return Err(capnp::Error::unimplemented(
                    "TODO(beta): parameters".to_owned(),
                ));
            }
            Which::Text(text) => global.set_text(text?),
            Which::Data(data) => global.set_data(data?),
            Which::Json(json) => global.set_json(json?),
            Which::WasmModule(_) => {
                // A service worker script's Wasm bindings are compiled with its modules.
                if !self.service_worker_script {
                    self.error(format!(
                        "{error_context} is a Wasm binding, but Wasm bindings are not allowed in \
                         modules-based scripts. Use Wasm modules instead."
                    ));
                }
                return Ok(false);
            }
            Which::CryptoKey(key) => return self.crypto_key(name, key?, global.init_crypto_key()),
            Which::Service(designator) => {
                let channel = self.subrequest_channel(designator?, error_context)?;
                global.set_fetcher(channel);
            }
            Which::DurableObjectNamespace(designator) => {
                return self.actor_namespace(designator?, &error_context, global);
            }
            Which::KvNamespace(designator) => {
                let channel = self.subrequest_channel(designator?, error_context)?;
                global.set_kv_namespace(channel);
            }
            Which::R2Bucket(designator) => self.r2_bucket(designator?, error_context, global)?,
            Which::Obsolete0(_) => {
                self.error(format!("{error_context} uses an obsolete binding type."));
                return Ok(false);
            }
            Which::Queue(designator) => {
                let channel = self.subrequest_channel(designator?, error_context)?;
                global.set_queue(channel);
            }
            Which::Wrapped(wrapped) => return self.wrapped(wrapped?, global),
            Which::FromEnvironment(variable) => match env::var_os(variable?.to_str()?) {
                Some(value) => global.set_text(&*value.to_string_lossy()),
                None => global.set_json("null"),
            },
            Which::AnalyticsEngine(designator) => {
                if !self.experimental_feature("AnalyticsEngine") {
                    return Ok(false);
                }
                self.analytics_engine(designator?, error_context, global)?;
            }
            Which::Hyperdrive(hyperdrive) => self.hyperdrive(hyperdrive, error_context, global)?,
            Which::UnsafeEval(()) => {
                if !self.experimental_feature("Unsafe eval") {
                    return Ok(false);
                }
                global.set_unsafe_eval(());
            }
            Which::MemoryCache(cache) => {
                if !self.experimental_feature("MemoryCache") {
                    return Ok(false);
                }
                return self.memory_cache(cache, global);
            }
            Which::DurableObjectClass(designator) => {
                if !self.experimental_feature("Durable Object class") {
                    return Ok(false);
                }
                let channel = len_u32(&self.out.actor_classes);
                let designator =
                    Designator::from_reader(designator?, error_context, &mut self.out.errors)?;
                self.out.actor_classes.push(designator);
                global.set_actor_class(channel);
            }
            Which::WorkerLoader(loader) => {
                if !self.experimental_feature("Worker loader") {
                    return Ok(false);
                }
                self.worker_loader(loader, name, global)?;
            }
            Which::WorkerdDebugPort(()) => {
                if !self.experimental_feature("workerdDebugPort") {
                    return Ok(false);
                }
                self.out.has_debug_port = true;
                global.set_workerd_debug_port(());
            }
        }
        Ok(true)
    }

    /// Whether `--experimental` is on; reports the `feature` binding's error when it is not.
    fn experimental_feature(&mut self, feature: &str) -> bool {
        if !self.experimental {
            self.error(format!(
                "{feature} bindings are an experimental feature which may change or go away in \
                 the future. You must run workerd with `--experimental` to use this feature."
            ));
        }
        self.experimental
    }

    /// Writes a wrapped binding. Its inner bindings are compiled the way top-level ones are,
    /// taking channels from the same tables; when one has no global, neither has the wrapper, and
    /// the channels taken so far stay taken.
    fn wrapped(
        &mut self,
        wrapped: binding::wrapped_binding::Reader<'_>,
        global: global::Builder<'_>,
    ) -> capnp::Result<bool> {
        let mut inner = Vec::new();
        for binding in wrapped.get_inner_bindings()? {
            match self.global(binding)? {
                Some(global) => inner.push(global),
                None => return Ok(false),
            }
        }
        let mut out = global.init_wrapped();
        out.set_module_name(wrapped.get_module_name()?);
        out.set_entrypoint(wrapped.get_entrypoint()?);
        set_globals(out.init_inner_bindings(len_u32(&inner)), &inner)?;
        Ok(true)
    }

    fn r2_bucket(
        &mut self,
        designator: service_designator::Reader<'_>,
        error_context: String,
        global: global::Builder<'_>,
    ) -> capnp::Result<()> {
        let channel = self.subrequest_channel(designator, error_context)?;
        let mut bucket = global.init_r2_bucket();
        bucket.set_channel(channel);
        bucket.set_bucket(designator.get_name()?);
        Ok(())
    }

    fn analytics_engine(
        &mut self,
        designator: service_designator::Reader<'_>,
        error_context: String,
        global: global::Builder<'_>,
    ) -> capnp::Result<()> {
        let channel = self.subrequest_channel(designator, error_context)?;
        let mut engine = global.init_analytics_engine();
        engine.set_channel(channel);
        engine.set_dataset(designator.get_name()?);
        Ok(())
    }

    fn hyperdrive(
        &mut self,
        hyperdrive: binding::hyperdrive::Reader<'_>,
        error_context: String,
        global: global::Builder<'_>,
    ) -> capnp::Result<()> {
        let channel = self.subrequest_channel(hyperdrive.get_designator()?, error_context)?;
        let mut out = global.init_hyperdrive();
        out.set_channel(channel);
        out.set_database(hyperdrive.get_database()?);
        out.set_user(hyperdrive.get_user()?);
        out.set_password(hyperdrive.get_password()?);
        out.set_scheme(hyperdrive.get_scheme()?);
        Ok(())
    }

    /// Writes a memory cache binding; false after reporting that it has no limits.
    fn memory_cache(
        &mut self,
        cache: binding::memory_cache::Reader<'_>,
        global: global::Builder<'_>,
    ) -> capnp::Result<bool> {
        if !cache.has_limits() {
            self.error(
                "MemoryCache bindings must specify limits. Please update the binding in the \
                 worker configuration and try again."
                    .to_owned(),
            );
            return Ok(false);
        }
        let mut out = global.init_memory_cache();
        // Bindings with the same id share one cache; a binding without gets its own.
        if cache.has_id() {
            out.set_cache_id(cache.get_id()?);
        }
        let limits = cache.get_limits()?;
        out.set_max_keys(limits.get_max_keys());
        out.set_max_value_size(limits.get_max_value_size());
        out.set_max_total_value_size(limits.get_max_total_value_size());
        Ok(true)
    }

    /// Writes a worker loader binding, taking the next worker-loader channel.
    fn worker_loader(
        &mut self,
        loader: binding::worker_loader::Reader<'_>,
        name: &str,
        mut global: global::Builder<'_>,
    ) -> capnp::Result<()> {
        let id = if loader.has_id() {
            Some(loader.get_id()?.to_string()?)
        } else {
            None
        };
        let channel = len_u32(&self.out.worker_loaders);
        self.out.worker_loaders.push(WorkerLoaderDesignator {
            name: id.clone().unwrap_or_else(|| name.to_owned()),
            id,
        });
        global.set_worker_loader(channel);
        Ok(())
    }

    /// Takes the next subrequest channel for `designator`.
    fn subrequest_channel(
        &mut self,
        designator: service_designator::Reader<'_>,
        error_context: String,
    ) -> capnp::Result<u32> {
        let channel = len_u32(&self.out.subrequest) + SPECIAL_SUBREQUEST_CHANNEL_COUNT;
        let designator = Designator::from_reader(designator, error_context, &mut self.out.errors)?;
        self.out.subrequest.push(designator);
        Ok(channel)
    }

    /// Writes a Durable Object namespace binding, taking the next actor channel, when the class
    /// has a namespace; reports the error otherwise.
    fn actor_namespace(
        &mut self,
        designator: binding::durable_object_namespace_designator::Reader<'_>,
        error_context: &str,
        mut global: global::Builder<'_>,
    ) -> capnp::Result<bool> {
        let class_name = designator.get_class_name()?.to_str()?;
        let service = if designator.has_service_name() {
            Some(designator.get_service_name()?.to_str()?)
        } else {
            None
        };
        let retry_policy = if designator.has_retry_policy() {
            let policy = designator.get_retry_policy()?;
            if policy.get_max_attempts() > RETRY_MAX_CONFIGURABLE_ATTEMPTS
                || !RETRY_CONFIGURABLE_TIMEOUT_MS.contains(&policy.get_timeout_ms())
            {
                self.error(format!(
                    "{error_context} has a Durable Object retry policy outside the system limits."
                ));
                return Ok(false);
            }
            Some(policy)
        } else {
            None
        };
        let actor_configs = self.actor_configs;
        let config = if let Some(service) = service {
            let Some(classes) = actor_configs.get(service) else {
                self.error(format!(
                    "{error_context} refers to a service \"{service}\", but no such service \
                     is defined."
                ));
                return Ok(false);
            };
            let Some(config) = classes.get(class_name) else {
                self.error(format!(
                    "{error_context} refers to a Durable Object namespace named \
                     \"{class_name}\" in service \"{service}\", but no such Durable Object \
                     namespace is defined by that service."
                ));
                return Ok(false);
            };
            config
        } else {
            let Some(config) = self.local_actor_configs.get(class_name) else {
                self.error(format!(
                    "{error_context} refers to a Durable Object namespace named \
                     \"{class_name}\", but no such Durable Object namespace is defined by \
                     this Worker."
                ));
                return Ok(false);
            };
            config
        };

        let channel = len_u32(&self.out.actors);
        self.out.actors.push(ActorDesignator {
            service: service.map(str::to_owned),
            class_name: class_name.to_owned(),
        });
        match config {
            ActorConfig::Durable { unique_key, .. } => {
                let mut namespace = global.init_durable_actor_namespace();
                namespace.set_actor_channel(channel);
                namespace.set_unique_key(unique_key.as_str());
                if let Some(policy) = retry_policy {
                    namespace.set_retry_policy(policy)?;
                }
            }
            ActorConfig::Ephemeral { .. } => global.set_ephemeral_actor_namespace(channel),
        }
        Ok(true)
    }

    /// Writes a `CryptoKey` binding: the key material in the format `importKey()` takes, the
    /// algorithm as JSON, and the usages. False after reporting an invalid key.
    fn crypto_key(
        &mut self,
        name: &str,
        key: binding::crypto_key::Reader<'_>,
        mut out: global::crypto_key::Builder<'_>,
    ) -> capnp::Result<bool> {
        use binding::crypto_key::Which;

        let Ok(which) = key.which() else {
            self.error(format!(
                "Encountered unknown CryptoKey type for binding \"{name}\". Was the config \
                 compiled with a newer version of the schema?"
            ));
            return Ok(false);
        };
        match which {
            Which::Raw(raw) => {
                out.set_format("raw");
                out.reborrow().init_key_data().set_bytes(raw?);
            }
            Which::Hex(hex) => {
                out.set_format("raw");
                let Ok(bytes) = data_encoding::HEXLOWER_PERMISSIVE.decode(hex?.as_bytes()) else {
                    self.error(format!(
                        "CryptoKey binding \"{name}\" contained invalid hex."
                    ));
                    return Ok(false);
                };
                out.reborrow().init_key_data().set_bytes(&bytes);
            }
            Which::Base64(base64) => {
                out.set_format("raw");
                let mut text = base64?.as_bytes().to_vec();
                text.retain(|c| !c.is_ascii_whitespace());
                let Ok(bytes) = BASE64.decode(text) else {
                    self.error(format!(
                        "CryptoKey binding \"{name}\" contained invalid base64."
                    ));
                    return Ok(false);
                };
                out.reborrow().init_key_data().set_bytes(&bytes);
            }
            Which::Pkcs8(pem) => {
                out.set_format("pkcs8");
                let Some(der) = self.pem_key(name, pem?, SectionKind::PrivateKey) else {
                    return Ok(false);
                };
                out.reborrow().init_key_data().set_bytes(&der);
            }
            Which::Spki(pem) => {
                out.set_format("spki");
                let Some(der) = self.pem_key(name, pem?, SectionKind::PublicKey) else {
                    return Ok(false);
                };
                out.reborrow().init_key_data().set_bytes(&der);
            }
            Which::Jwk(jwk) => {
                out.set_format("jwk");
                out.reborrow().init_key_data().set_json(jwk?);
            }
        }

        let Ok(algorithm) = key.get_algorithm().which() else {
            self.error(format!(
                "Encountered unknown CryptoKey algorithm type for binding \"{name}\". Was the \
                 config compiled with a newer version of the schema?"
            ));
            return Ok(false);
        };
        match algorithm {
            binding::crypto_key::algorithm::Which::Name(name) => {
                let quoted = serde_json::to_string(name?.to_str()?)
                    .map_err(|error| capnp::Error::failed(error.to_string()))?;
                out.set_algorithm(quoted.as_str());
            }
            binding::crypto_key::algorithm::Which::Json(json) => out.set_algorithm(json?),
        }

        out.set_extractable(key.get_extractable());
        // An absent list stays absent: capnp copies one as a list of Void, which does not read
        // back as a list of `Usage`.
        if key.has_usages() {
            out.set_usages(key.get_usages()?)?;
        }
        Ok(true)
    }

    /// The DER of the first PEM section in `pem`, which must be of `kind`, or none after
    /// reporting the error.
    fn pem_key(
        &mut self,
        name: &str,
        pem: capnp::text::Reader<'_>,
        kind: SectionKind,
    ) -> Option<Vec<u8>> {
        match <(SectionKind, Vec<u8>)>::from_pem_slice(pem.as_bytes()) {
            Ok((found, der)) if found == kind => return Some(der),
            Ok((found, _)) => self.error(format!(
                "CryptoKey binding \"{name}\" contained wrong PEM type, expected {kind:?} but \
                 got {found:?}."
            )),
            Err(_) => self.error(format!(
                "CryptoKey binding \"{name}\" contained invalid PEM format."
            )),
        }
        None
    }
}

/// Base64 as `kj::decodeBase64()` accepts it: padding is optional.
const BASE64: GeneralPurpose = GeneralPurpose::new(
    &base64::alphabet::STANDARD,
    GeneralPurposeConfig::new()
        .with_decode_padding_mode(DecodePaddingMode::Indifferent)
        .with_decode_allow_trailing_bits(true),
);

#[cfg(test)]
#[path = "bindings-test.rs"]
mod tests;
