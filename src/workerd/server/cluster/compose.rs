// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Defines how deployments (`deploy.rs`) compose with a node's config.
//!
//! The composed config contains the node's own `Config` followed by the services of each accepted
//! deployment in catalog order. [`merge`] skips a deployment as a whole when it breaks a rule.
//!
//! - A deployment holds only `services`. Every other field of `Config` (sockets, the cluster
//!   block, extensions, V8 flags, autogates, logging) is the node's, and a deployment that sets
//!   one is skipped rather than trimmed. So is one that defines a `disk` service, whose path is
//!   the node's, or a service named `internet`, which would replace the node's implicit one for
//!   every worker.
//! - Service names are unique across the node's config and every deployment, and so are the keys
//!   of actor namespaces: Durable Object `uniqueKey`s and the key of each Workflow's namespace
//!   (`workflow_namespace_key`). A deployment refers to what the node provides, such as the disk
//!   service its `durableObjectStorage` names, by service name.
//! - A service with fields this build's schema does not know cannot be composed because merging
//!   would drop them. During [`merge`], one in the node's config fails the merge and one in a
//!   deployment causes that deployment to be skipped.
//! - A deployment has no `fromEnvironment` binding, wrapped ones included. It cannot read the
//!   node's environment.

use std::collections::HashMap;

use capnp::message::HeapAllocator;
use capnp::message::ReaderOptions;
use workerd_capnp::config;
use workerd_capnp::service;
use workerd_capnp::worker;
use workerd_capnp::worker::binding;

use crate::Error;
use crate::Result;
use crate::deploy::Deployment;

/// The implicit service the node defines unless its config does.
const INTERNET: &str = "internet";

/// Miniflare's own Workflow engine namespaces use this key prefix, which determines actor IDs and
/// the storage subdirectory; matching it lets both reach the same local instances.
///
/// TODO(cleanup): Make this configurable rather than hardcoding Miniflare's prefix; a configurable
///   key must keep this value for Miniflare.
const WORKFLOW_NAMESPACE_KEY_PREFIX: &str = "miniflare-workflows-";

/// The unique key of the namespace backing the Workflow `workflow_name`.
#[must_use]
pub fn workflow_namespace_key(workflow_name: &str) -> String {
    format!("{WORKFLOW_NAMESPACE_KEY_PREFIX}{workflow_name}")
}

/// `config`, a deployment's encoded `Config`, checked to hold only what a deployment may and
/// re-encoded canonically: the bytes whose hash is the deployment's version.
pub fn deployment_config(name: &str, config: &[u8]) -> Result<Vec<u8>> {
    let owner = Owner::Deployment(name.to_owned());
    let message = parse(&owner, config)?;
    let root = root(&owner, &message)?;
    check_text(&owner, root)?;
    let mut errors = Vec::new();
    check_deployment(&owner, root, &mut errors).map_err(|e| malformed(&owner, &e))?;
    refuse(&errors)?;
    let size = root.total_size().map_err(|e| malformed(&owner, &e))?;
    canonical(root, size).map_err(|e| malformed(&owner, &e))
}

/// Checks that `deployments` share no service name or actor namespace key, and that each holds
/// only what a deployment may.
pub fn check_deployments(deployments: &[&Deployment]) -> Result<()> {
    let mut names = Names::default();
    let mut errors = Vec::new();
    for deployment in deployments {
        let owner = Owner::Deployment(deployment.name.clone());
        let message = parse(&owner, &deployment.config)?;
        let root = root(&owner, &message)?;
        check_text(&owner, root)?;
        admit(&owner, root, &mut names, &mut errors).map_err(|e| malformed(&owner, &e))?;
    }
    refuse(&errors)
}

/// Contains the config a node serves and the result for every deployment.
#[derive(Debug)]
pub struct Composed {
    /// The node's encoded `Config` with every accepted deployment's services appended.
    pub config: Vec<u8>,
    /// The result for every deployment, in the order passed to [`merge`].
    pub deployments: Vec<Composition>,
}

/// Describes whether one deployment was accepted or skipped.
#[derive(Debug, PartialEq, Eq)]
pub struct Composition {
    /// The deployment's name.
    pub name: String,
    /// The deployment's version.
    pub version: String,
    /// The services this deployment contributes or leaves absent.
    ///
    /// For a skipped deployment, this lists only names absent from the composed config. A
    /// reference to one of them cannot be served.
    pub services: Vec<String>,
    /// The reasons it was skipped, one per line, or empty when the deployment was accepted.
    pub errors: Vec<String>,
}

/// Composes a node config with deployments in their given order.
///
/// A deployment is skipped as a whole when it breaks a rule alone, against the node's config, or
/// against a deployment accepted before it. Nodes with the same config and catalog skip the same
/// deployments. Only an error in the node's own config fails the merge.
pub fn merge(node: &[u8], deployments: &[Deployment]) -> Result<Composed> {
    let node_message = parse(&Owner::Node, node)?;
    let node = root(&Owner::Node, &node_message)?;
    let mut names = Names::default();
    let mut errors = Vec::new();
    check_node(node, &mut names, &mut errors).map_err(|e| malformed(&Owner::Node, &e))?;
    refuse(&errors)?;

    let messages: Vec<_> = deployments
        .iter()
        .map(|deployment| {
            parse(
                &Owner::Deployment(deployment.name.clone()),
                &deployment.config,
            )
        })
        .collect();
    let mut merged = Vec::new();
    let mut compositions = Vec::with_capacity(deployments.len());
    for (deployment, message) in deployments.iter().zip(&messages) {
        let owner = Owner::Deployment(deployment.name.clone());
        let (services, errors) = match message {
            Ok(message) => match compose(&owner, message, &mut names) {
                Ok((root, services, errors)) => {
                    if errors.is_empty() {
                        merged.push(root);
                    }
                    (services, errors)
                }
                Err(error) => (Vec::new(), lines(&error)),
            },
            Err(error) => (Vec::new(), lines(error)),
        };
        compositions.push(Composition {
            name: deployment.name.clone(),
            version: deployment.version.clone(),
            services,
            errors,
        });
    }
    for skipped in compositions.iter_mut().filter(|c| !c.errors.is_empty()) {
        skipped
            .services
            .retain(|service| !names.services.contains_key(service));
    }
    let config = append(node, &merged)
        .map_err(|e| Error::Deploy(format!("could not merge the deployments: {e}")))?;
    Ok(Composed {
        config,
        deployments: compositions,
    })
}

/// Checks one deployment against itself and the names accepted before it.
///
/// Returns its config, service names, and broken rules. `names` gains the deployment's names only
/// when it breaks no rules.
fn compose<'a>(
    owner: &Owner,
    message: &'a Message,
    names: &mut Names,
) -> Result<(config::Reader<'a>, Vec<String>, Vec<String>)> {
    let root = root(owner, message)?;
    check_text(owner, root)?;
    let services = service_names(root).map_err(|e| malformed(owner, &e))?;
    let mut with_these = names.clone();
    let mut errors = Vec::new();
    admit(owner, root, &mut with_these, &mut errors).map_err(|e| malformed(owner, &e))?;
    if errors.is_empty() {
        *names = with_these;
    }
    Ok((root, services, errors))
}

fn service_names(config: config::Reader<'_>) -> capnp::Result<Vec<String>> {
    config
        .get_services()?
        .iter()
        .map(|service| text(service.get_name()))
        .collect()
}

fn lines(error: &Error) -> Vec<String> {
    error.to_string().lines().map(str::to_owned).collect()
}

/// Records the node's names, and reports its services with fields this build does not know.
fn check_node(
    node: config::Reader<'_>,
    names: &mut Names,
    errors: &mut Vec<String>,
) -> capnp::Result<()> {
    names.add(&Owner::Node, node, errors)?;
    known_fields(&Owner::Node, node, errors)
}

/// Checks `owner`'s deployment config and records its names in `names`.
fn admit(
    owner: &Owner,
    config: config::Reader<'_>,
    names: &mut Names,
    errors: &mut Vec<String>,
) -> capnp::Result<()> {
    check_deployment(owner, config, errors)?;
    names.add(owner, config, errors)
}

/// Encodes `node` with the services from `deployments` appended after its own.
fn append(node: config::Reader<'_>, deployments: &[config::Reader<'_>]) -> capnp::Result<Vec<u8>> {
    let mut lists = vec![node.get_services()?];
    for deployment in deployments {
        lists.push(deployment.get_services()?);
    }
    let total = lists.iter().map(capnp::struct_list::Reader::len).sum();
    let mut out = capnp::message::Builder::new_default();
    let mut root = out.init_root::<config::Builder<'_>>();
    copy_all_but_services(node, root.reborrow())?;
    let mut services = root.init_services(total);
    for (index, service) in (0..total).zip(lists.iter().flat_map(|list| list.iter())) {
        services.set_with_caveats(index, service)?;
    }
    Ok(capnp::serialize::write_message_to_words(&out))
}

/// Copies every field except `services` from `from` into the empty `Config` `to`.
///
/// Copying the whole config and then replacing its services would leave an unreachable copy of
/// the original services in the message. Fields this build's schema does not know are omitted;
/// the server would ignore them.
fn copy_all_but_services(from: config::Reader<'_>, to: config::Builder<'_>) -> capnp::Result<()> {
    let from: capnp::dynamic_struct::Reader<'_> =
        capnp::dynamic_value::Reader::from(from).downcast();
    let mut to: capnp::dynamic_struct::Builder<'_> =
        capnp::dynamic_value::Builder::from(to).downcast();
    for field in from.get_schema().get_fields()? {
        if field.get_proto().get_name()?.to_str()? != "services" && from.has(field)? {
            to.set(field, from.get(field)?)?;
        }
    }
    Ok(())
}

/// Who defines a service: the node's own config, or a deployment.
enum Owner {
    Node,
    Deployment(String),
}

impl std::fmt::Display for Owner {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Node => write!(f, "the instance's config"),
            Self::Deployment(name) => write!(f, "deployment \"{name}\""),
        }
    }
}

/// The names deployments must not share, and who defined each.
#[derive(Clone, Default)]
struct Names {
    services: HashMap<String, String>,
    /// Durable Object `uniqueKey`s and Workflow namespace keys: the server makes an actor
    /// namespace of each, and two of one key would share it.
    namespace_keys: HashMap<String, String>,
}

impl Names {
    /// Records `config`'s service names and actor namespace keys.
    ///
    /// When a deployment repeats a name already defined by the node, another deployment, or
    /// itself, this records an error. The server reports duplicates within the node's config.
    fn add(
        &mut self,
        owner: &Owner,
        config: config::Reader<'_>,
        errors: &mut Vec<String>,
    ) -> capnp::Result<()> {
        let who = owner.to_string();
        let mut record = |map: &mut HashMap<String, String>, what: &str, name: String| {
            if let Some(before) = map.get(&name) {
                if matches!(owner, Owner::Deployment(_)) {
                    errors.push(defined_twice(what, &name, before, &who));
                }
            } else {
                map.insert(name, who.clone());
            }
        };
        // TODO(someday): Scope a Cap'n Proto deployment's Durable Object `uniqueKey`s and all
        //   Workflow namespace keys to the deployment, as `workerd deploy` does for a wrangler
        //   project's Durable Objects.
        for service in config.get_services()? {
            record(&mut self.services, "service", text(service.get_name())?);
            let Ok(service::Which::Worker(worker)) = service.which() else {
                continue;
            };
            let worker = worker?;
            for namespace in worker.get_durable_object_namespaces()? {
                if let Ok(worker::durable_object_namespace::Which::UniqueKey(key)) =
                    namespace.which()
                {
                    let what = "Durable Object namespace uniqueKey";
                    record(&mut self.namespace_keys, what, text(key)?);
                }
            }
            if worker.has_workflows_engine() {
                for workflow in worker.get_workflows_engine()?.get_workflows()? {
                    let key = workflow_namespace_key(&text(workflow.get_name())?);
                    record(&mut self.namespace_keys, "Workflow namespace key", key);
                }
            }
        }
        Ok(())
    }
}

fn defined_twice(what: &str, name: &str, before: &str, now: &str) -> String {
    if before == now {
        format!("{now} defines {what} \"{name}\" more than once.")
    } else {
        format!("{now} defines {what} \"{name}\". The same {what} is already defined by {before}.")
    }
}

/// Checks a deployment's own content.
///
/// It may set only `services`. It may not define a `disk` service, a service named `internet`,
/// or a `fromEnvironment` binding.
fn check_deployment(
    owner: &Owner,
    config: config::Reader<'_>,
    errors: &mut Vec<String>,
) -> capnp::Result<()> {
    let node_fields = [
        ("sockets", config.has_sockets()),
        ("v8Flags", config.has_v8_flags()),
        ("extensions", config.has_extensions()),
        ("autogates", config.has_autogates()),
        ("structuredLogging", config.get_structured_logging()),
        ("logging", config.has_logging()),
        ("cluster", config.has_cluster()),
    ];
    let set: Vec<_> = node_fields
        .iter()
        .filter(|(_, set)| *set)
        .map(|(name, _)| format!("`{name}`"))
        .collect();
    if !set.is_empty() {
        errors.push(format!(
            "{owner} sets {}. A deployment may contain only `services`. Put other settings in \
             the instance's config.",
            set.join(", ")
        ));
    } else if !only_services(config)? {
        // A field of a newer schema.
        errors.push(format!(
            "{owner} sets fields other than `services`. A deployment may contain only \
             `services`."
        ));
    }
    known_fields(owner, config, errors)?;

    for service in config.get_services()? {
        let name = text(service.get_name())?;
        if name == INTERNET {
            errors.push(format!(
                "{owner} defines service \"{INTERNET}\". Only an instance's config may define \
                 this service."
            ));
        }
        match service.which() {
            Ok(service::Which::Disk(_)) => errors.push(format!(
                "{owner} defines disk service \"{name}\". A deployment cannot define a disk. \
                 Define the disk in the instance's config and refer to it by name."
            )),
            Ok(service::Which::Bucket(_)) => errors.push(format!(
                "{owner} defines bucket service \"{name}\". A deployment cannot define a \
                 bucket service. Define it in the instance's config and refer to it by name."
            )),
            Ok(service::Which::Worker(worker)) => {
                let worker = worker?;
                let context = format!("service \"{name}\" in {owner}");
                environment(&context, worker.get_bindings()?, errors)?;
                for namespace in worker.get_durable_object_namespaces()? {
                    if let Ok(worker::durable_object_namespace::Which::UniqueKey(key)) =
                        namespace.which()
                    {
                        let key = text(key)?;
                        if !is_path_component(&key) {
                            errors.push(format!(
                                "{context} has Durable Object namespace uniqueKey {key:?}. A \
                                 uniqueKey names a directory on the instance's disk, so it must \
                                 be one path component: not empty, not \".\" or \"..\", and \
                                 without \"/\", \"\\\", or NUL."
                            ));
                        }
                    }
                }
            }
            _ => {}
        }
    }
    Ok(())
}

/// Whether `name` is one path component: what a directory named after it needs.
#[must_use]
pub fn is_path_component(name: &str) -> bool {
    !name.is_empty() && name != "." && name != ".." && !name.contains(['/', '\\', '\0'])
}

/// Refuses deployment `owner`'s `config` when one of its texts is not UTF-8. The server reads
/// every text it uses as UTF-8.
fn check_text(owner: &Owner, config: config::Reader<'_>) -> Result<()> {
    let value = capnp::dynamic_value::Reader::from(config);
    match invalid_text(value, "Config").map_err(|e| malformed(owner, &e))? {
        Some(path) => Err(Error::Deploy(format!(
            "{owner} has text that is not valid UTF-8 at {path}."
        ))),
        None => Ok(()),
    }
}

/// The path, below `path`, of the first text in `value` that is not UTF-8.
fn invalid_text(
    value: capnp::dynamic_value::Reader<'_>,
    path: &str,
) -> capnp::Result<Option<String>> {
    use capnp::dynamic_value::Reader;
    match value {
        Reader::Text(text) => Ok(text.to_str().is_err().then(|| path.to_owned())),
        Reader::List(list) => {
            for (index, item) in list.iter().enumerate() {
                if let Some(found) = invalid_text(item?, &format!("{path}[{index}]"))? {
                    return Ok(Some(found));
                }
            }
            Ok(None)
        }
        Reader::Struct(value) => {
            let schema = value.get_schema();
            let fields = schema
                .get_non_union_fields()?
                .into_iter()
                .chain(value.which()?);
            for field in fields {
                if !value.has(field)? {
                    continue;
                }
                let name = field.get_proto().get_name()?.to_str()?;
                if let Some(found) = invalid_text(value.get(field)?, &format!("{path}.{name}"))? {
                    return Ok(Some(found));
                }
            }
            Ok(None)
        }
        _ => Ok(None),
    }
}

/// Reports every `fromEnvironment` binding in `bindings`, including those inside wrapped bindings.
fn environment(
    context: &str,
    bindings: capnp::struct_list::Reader<'_, binding::Owned>,
    errors: &mut Vec<String>,
) -> capnp::Result<()> {
    for binding in bindings {
        match binding.which() {
            Ok(binding::Which::FromEnvironment(_)) => {
                let name = text(binding.get_name())?;
                errors.push(format!(
                    "{context} has `fromEnvironment` binding \"{name}\". A deployment cannot \
                     read the instance's environment variables."
                ));
            }
            Ok(binding::Which::Wrapped(wrapped)) => {
                environment(context, wrapped?.get_inner_bindings()?, errors)?;
            }
            _ => {}
        }
    }
    Ok(())
}

/// Whether `config` is its services and nothing else, fields of newer schemas included.
fn only_services(config: config::Reader<'_>) -> capnp::Result<bool> {
    let mut services = capnp::message::Builder::new_default();
    services
        .init_root::<config::Builder<'_>>()
        .set_services(config.get_services()?)?;
    let services = services.get_root_as_reader::<config::Reader<'_>>()?;
    Ok(canonical(services, services.total_size()?)? == canonical(config, config.total_size()?)?)
}

/// Reports each of `config`'s services that has fields of a newer schema. Merging copies a
/// service into a list of this build's services, which keeps only the fields this build knows.
fn known_fields(
    owner: &Owner,
    config: config::Reader<'_>,
    errors: &mut Vec<String>,
) -> capnp::Result<()> {
    for service in config.get_services()? {
        let mut message = capnp::message::Builder::new_default();
        let mut copy = message.init_root::<config::Builder<'_>>().init_services(1);
        copy.set_with_caveats(0, service)?;
        let copy = copy.into_reader().get(0);
        if canonical(copy, copy.total_size()?)? != canonical(service, service.total_size()?)? {
            let name = text(service.get_name())?;
            errors.push(format!(
                "{owner} defines service \"{name}\" with fields this workerd does not know."
            ));
        }
    }
    Ok(())
}

/// `value`, of size `size`, as a canonical message.
fn canonical<T: capnp::traits::Owned>(
    value: impl capnp::traits::SetterInput<T>,
    size: capnp::MessageSize,
) -> capnp::Result<Vec<u8>> {
    // A canonical message is one segment, which must have room for all of it and the root.
    let words = u32::try_from(size.word_count + 1)
        .map_err(|_| capnp::Error::failed("the config is too large".into()))?;
    let allocator = HeapAllocator::new().first_segment_words(words);
    let mut out = capnp::message::Builder::new(allocator);
    out.set_root_canonical(value)?;
    Ok(capnp::serialize::write_message_to_words(&out))
}

type Message = capnp::message::Reader<capnp::serialize::OwnedSegments>;

/// Reads `bytes` as `owner`'s encoded config.
fn parse(owner: &Owner, bytes: &[u8]) -> Result<Message> {
    read(bytes).map_err(|e| malformed(owner, &e))
}

/// Reads `owner`'s `Config` root from `message`.
fn root<'a>(owner: &Owner, message: &'a Message) -> Result<config::Reader<'a>> {
    message.get_root().map_err(|e| malformed(owner, &e))
}

/// An encoded message. Configs can legitimately be very large, so there is no traversal limit.
fn read(bytes: &[u8]) -> capnp::Result<Message> {
    let mut bytes = bytes;
    let options = *ReaderOptions::new().traversal_limit_in_words(None);
    capnp::serialize::read_message(&mut bytes, options)
}

fn text(text: capnp::Result<capnp::text::Reader<'_>>) -> capnp::Result<String> {
    Ok(text?.to_str()?.to_owned())
}

fn malformed(owner: &Owner, error: &capnp::Error) -> Error {
    Error::Deploy(format!("{owner} is not a valid config: {error}"))
}

fn refuse(errors: &[String]) -> Result<()> {
    if errors.is_empty() {
        Ok(())
    } else {
        Err(Error::Deploy(errors.join("\n")))
    }
}

#[cfg(test)]
#[path = "compose-test.rs"]
mod tests;
