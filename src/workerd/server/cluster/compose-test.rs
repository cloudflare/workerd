// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use capnp::message::HeapAllocator;
use workerd_capnp::config;
use workerd_capnp::service;

use super::check_deployments;
use super::deployment_config;
use super::merge;
use super::read;
use crate::Error;
use crate::deploy::Deployment;
use crate::deploy::version_of;

#[derive(Clone, Copy, Default)]
struct Worker<'a> {
    unique_key: Option<&'a str>,
    /// A `fromEnvironment` binding's variable.
    env: Option<&'a str>,
    /// The same, wrapped.
    wrapped_env: Option<&'a str>,
    /// The source of an ES module.
    module: Option<&'a str>,
    /// The name of a Workflow.
    workflow: Option<&'a str>,
}

#[derive(Clone, Copy)]
enum Kind<'a> {
    Worker(Worker<'a>),
    Disk,
    Network,
}

const WORKER: Kind<'static> = Kind::Worker(Worker {
    unique_key: None,
    env: None,
    wrapped_env: None,
    module: None,
    workflow: None,
});

fn set_service(mut service: service::Builder<'_>, name: &str, kind: Kind<'_>) {
    service.set_name(name);
    match kind {
        Kind::Worker(spec) => {
            let mut worker = service.init_worker();
            worker.set_compatibility_date("2026-01-01");
            if let Some(source) = spec.module {
                let mut module = worker.reborrow().init_modules(1).get(0);
                module.set_name("app.js");
                module.set_es_module(source);
            }
            if let Some(key) = spec.unique_key {
                let mut ns = worker.reborrow().init_durable_object_namespaces(1).get(0);
                ns.set_class_name("Counter");
                ns.set_unique_key(key);
            }
            if let Some(name) = spec.workflow {
                let mut engine = worker.reborrow().init_workflows_engine();
                engine.reborrow().init_actor_class().set_name("engine");
                let mut workflow = engine.init_workflows(1).get(0);
                workflow.set_class_name("Flow");
                workflow.set_name(name);
            }
            let count = u32::from(spec.env.is_some()) + u32::from(spec.wrapped_env.is_some());
            let mut bindings = worker.init_bindings(count);
            let mut index = 0;
            if let Some(variable) = spec.env {
                let mut binding = bindings.reborrow().get(index);
                binding.set_name("SECRET");
                binding.set_from_environment(variable);
                index += 1;
            }
            if let Some(variable) = spec.wrapped_env {
                let mut binding = bindings.reborrow().get(index);
                binding.set_name("WRAPPED");
                let mut inner = binding.init_wrapped().init_inner_bindings(1).get(0);
                inner.set_name("INNER");
                inner.set_from_environment(variable);
            }
        }
        Kind::Disk => {
            service.init_disk().set_writable(true);
        }
        Kind::Network => {
            service.init_network();
        }
    }
}

/// An encoded `Config` of `services`, after which `extra` sets more of it.
fn build(services: &[(&str, Kind<'_>)], extra: impl FnOnce(config::Builder<'_>)) -> Vec<u8> {
    let mut message = capnp::message::Builder::new_default();
    let mut root = message.init_root::<config::Builder<'_>>();
    let mut list = root.reborrow().init_services(services.len() as u32);
    for (index, (name, kind)) in services.iter().enumerate() {
        set_service(list.reborrow().get(index as u32), name, *kind);
    }
    extra(root);
    capnp::serialize::write_message_to_words(&message)
}

fn services(services: &[(&str, Kind<'_>)]) -> Vec<u8> {
    build(services, |_| {})
}

fn deployment(name: &str, config: &[u8]) -> Deployment {
    let config = deployment_config(name, config).unwrap();
    Deployment {
        name: name.to_owned(),
        version: version_of(&config),
        config,
    }
}

/// A node's config: a disk, a worker, a socket and the cluster block.
fn node() -> Vec<u8> {
    let worker = Kind::Worker(Worker {
        unique_key: Some("node-key"),
        ..Worker::default()
    });
    build(&[("local", Kind::Disk), ("main", worker)], |mut root| {
        let mut socket = root.reborrow().init_sockets(1).get(0);
        socket.set_name("http");
        socket.set_address("*:8080");
        socket.init_service().set_name("app");
        root.init_cluster().set_bucket("s3://bucket/prefix");
    })
}

fn names(config: &[u8]) -> Vec<String> {
    let message = read(config).unwrap();
    let root = message.get_root::<config::Reader<'_>>().unwrap();
    let services = root.get_services().unwrap();
    services
        .iter()
        .map(|s| s.get_name().unwrap().to_string().unwrap())
        .collect()
}

fn refusal(result: crate::Result<impl std::fmt::Debug>) -> String {
    match result {
        Err(Error::Deploy(message)) => message,
        other => panic!("expected a refusal, got {other:?}"),
    }
}

const fn all_set(_: &str) -> bool {
    true
}

#[test]
fn merging_appends_the_deployments_services_and_keeps_the_nodes_config() {
    let a = deployment("a", &services(&[("app", WORKER)]));
    let b = deployment(
        "b",
        &services(&[("api", WORKER), ("egress", Kind::Network)]),
    );
    let merged = merge(&node(), &[a, b], &all_set).unwrap();
    assert_eq!(names(&merged), ["local", "main", "app", "api", "egress"]);

    let message = read(&merged).unwrap();
    let root = message.get_root::<config::Reader<'_>>().unwrap();
    let sockets = root.get_sockets().unwrap();
    assert_eq!(sockets.len(), 1);
    assert_eq!(
        sockets.get(0).get_service().unwrap().get_name().unwrap(),
        "app"
    );
    let cluster = root.get_cluster().unwrap();
    assert_eq!(cluster.get_bucket().unwrap(), "s3://bucket/prefix");
}

#[test]
fn a_deployment_may_contain_only_services() {
    let config = build(&[("app", WORKER)], |mut root| {
        root.reborrow().init_sockets(1).get(0).set_name("http");
        root.reborrow().init_cluster();
        root.set_structured_logging(true);
    });
    assert_eq!(
        refusal(deployment_config("app", &config)),
        "deployment \"app\" sets `sockets`, `structuredLogging`, `cluster`. A deployment may \
         contain only `services`. Put other settings in the instance's config."
    );
    let config = build(&[("app", WORKER)], |root| {
        root.init_extensions(0);
    });
    assert!(refusal(deployment_config("app", &config)).contains("sets `extensions`"));
}

#[test]
fn a_deployment_defines_no_disk_and_no_internet() {
    let config = services(&[("files", Kind::Disk), ("internet", Kind::Network)]);
    assert_eq!(
        refusal(deployment_config("app", &config)),
        "deployment \"app\" defines disk service \"files\". A deployment cannot define a disk. \
         Define the disk in the instance's config and refer to it by name.\n\
         deployment \"app\" defines service \"internet\". Only an instance's config may define \
         this service."
    );
}

#[test]
fn service_names_are_unique_across_the_node_and_its_deployments() {
    let a = deployment("a", &services(&[("main", WORKER), ("shared", WORKER)]));
    let b = deployment("b", &services(&[("shared", WORKER)]));
    assert_eq!(
        refusal(merge(&node(), &[a.clone(), b.clone()], &all_set)),
        "deployment \"a\" defines service \"main\". The same service is already defined by the \
         instance's config.\n\
         deployment \"b\" defines service \"shared\". The same service is already defined by \
         deployment \"a\"."
    );
    assert_eq!(
        refusal(check_deployments(&[&a, &b])),
        "deployment \"b\" defines service \"shared\". The same service is already defined by \
         deployment \"a\"."
    );
    let twice = deployment("c", &services(&[("x", WORKER), ("x", WORKER)]));
    assert_eq!(
        refusal(check_deployments(&[&twice])),
        "deployment \"c\" defines service \"x\" more than once."
    );
}

#[test]
fn durable_object_unique_keys_are_unique_across_the_node_and_its_deployments() {
    let keyed = |key| {
        Kind::Worker(Worker {
            unique_key: Some(key),
            ..Worker::default()
        })
    };
    let a = deployment("a", &services(&[("one", keyed("node-key"))]));
    let b = deployment("b", &services(&[("two", keyed("app-key"))]));
    let c = deployment("c", &services(&[("three", keyed("app-key"))]));
    assert_eq!(
        refusal(merge(&node(), &[a, b.clone(), c.clone()], &all_set)),
        "deployment \"a\" defines Durable Object namespace uniqueKey \"node-key\". The same \
         Durable Object namespace uniqueKey is already defined by the instance's config.\n\
         deployment \"c\" defines Durable Object namespace uniqueKey \"app-key\". The same \
         Durable Object namespace uniqueKey is already defined by deployment \"b\"."
    );
    assert!(check_deployments(&[&b]).is_ok());
    assert!(check_deployments(&[&b, &c]).is_err());
    let twice = deployment("d", &services(&[("x", keyed("k")), ("y", keyed("k"))]));
    assert_eq!(
        refusal(check_deployments(&[&twice])),
        "deployment \"d\" defines Durable Object namespace uniqueKey \"k\" more than once."
    );
}

#[test]
fn workflow_namespace_keys_are_unique_across_the_node_and_its_deployments() {
    let flow = |name| {
        Kind::Worker(Worker {
            workflow: Some(name),
            ..Worker::default()
        })
    };
    let a = deployment("a", &services(&[("one", flow("orders"))]));
    let b = deployment("b", &services(&[("two", flow("orders"))]));
    assert_eq!(
        refusal(check_deployments(&[&a, &b])),
        "deployment \"b\" defines Workflow namespace key \"miniflare-workflows-orders\". The \
         same Workflow namespace key is already defined by deployment \"a\"."
    );
    let keyed = Kind::Worker(Worker {
        unique_key: Some("miniflare-workflows-orders"),
        ..Worker::default()
    });
    let c = deployment("c", &services(&[("three", keyed)]));
    assert_eq!(
        refusal(check_deployments(&[&a, &c])),
        "deployment \"c\" defines Durable Object namespace uniqueKey \
         \"miniflare-workflows-orders\". The same Durable Object namespace uniqueKey is already \
         defined by deployment \"a\"."
    );
    let node = services(&[("main", flow("orders"))]);
    assert_eq!(
        refusal(merge(&node, &[a], &all_set)),
        "deployment \"a\" defines Workflow namespace key \"miniflare-workflows-orders\". The \
         same Workflow namespace key is already defined by the instance's config."
    );
}

/// An encoded `Config` as a newer schema could write it: one word of data past what this
/// build's schema defines, set, in the root or, given `in_service`, in its one service.
fn newer(in_service: bool) -> Vec<u8> {
    use capnp::traits::HasStructSize;
    let config = <config::Builder<'_> as HasStructSize>::STRUCT_SIZE;
    let service = <service::Builder<'_> as HasStructSize>::STRUCT_SIZE;
    let struct_pointer = |offset: u32, data: u16, pointers: u16| {
        u64::from(offset << 2) | (u64::from(data) << 32) | (u64::from(pointers) << 48)
    };
    let zeros = |count: u16| std::iter::repeat_n(0, count.into());
    let mut words = Vec::new();
    if in_service {
        words.push(struct_pointer(0, config.data, config.pointers));
        words.extend(zeros(config.data));
        // `services`, an inline composite list placed after the root's pointers.
        let size = u32::from(service.data + 1 + service.pointers);
        let offset = u32::from(config.pointers - 1);
        words.push(u64::from((offset << 2) | 1) | (u64::from((size << 3) | 7) << 32));
        words.extend(zeros(config.pointers - 1));
        words.push(struct_pointer(1, service.data + 1, service.pointers));
        words.extend(zeros(service.data));
        words.push(1);
        words.extend(zeros(service.pointers));
    } else {
        words.push(struct_pointer(0, config.data + 1, config.pointers));
        words.extend(zeros(config.data));
        words.push(1);
        words.extend(zeros(config.pointers));
    }
    let mut bytes = 0u32.to_le_bytes().to_vec();
    bytes.extend(u32::try_from(words.len()).unwrap().to_le_bytes());
    bytes.extend(words.iter().flat_map(|word| word.to_le_bytes()));
    bytes
}

#[test]
fn fields_of_a_newer_schema_are_refused() {
    assert_eq!(
        refusal(deployment_config("app", &newer(false))),
        "deployment \"app\" sets fields other than `services`. A deployment may contain only \
         `services`."
    );
    assert_eq!(
        refusal(deployment_config("app", &newer(true))),
        "deployment \"app\" defines service \"\" with fields this workerd does not know."
    );
    assert_eq!(
        refusal(merge(&newer(true), &[], &all_set)),
        "the instance's config defines service \"\" with fields this workerd does not know."
    );
}

#[test]
fn a_deployments_environment_variables_must_be_set() {
    let worker = Kind::Worker(Worker {
        env: Some("API_TOKEN"),
        wrapped_env: Some("WRAPPED_TOKEN"),
        ..Worker::default()
    });
    let a = deployment("a", &services(&[("app", worker)]));
    assert_eq!(
        refusal(merge(&node(), std::slice::from_ref(&a), &|_| false)),
        "service \"app\" in deployment \"a\" has binding \"SECRET\" for environment variable \
         \"API_TOKEN\". This instance does not set that variable.\n\
         service \"app\" in deployment \"a\" has binding \"INNER\" for environment variable \
         \"WRAPPED_TOKEN\". This instance does not set that variable."
    );
    let is_set = |variable: &str| variable == "API_TOKEN" || variable == "WRAPPED_TOKEN";
    assert!(merge(&node(), &[a], &is_set).is_ok());
    // Publishing checks no environment: each node has its own.
    let unset = services(&[("app", worker)]);
    assert!(deployment_config("a", &unset).is_ok());
}

#[test]
fn the_nodes_own_environment_variables_may_be_unset() {
    let worker = Kind::Worker(Worker {
        env: Some("UNSET"),
        ..Worker::default()
    });
    let node = services(&[("main", worker)]);
    let merged = merge(&node, &[], &|_| false).unwrap();
    assert_eq!(names(&merged), ["main"]);
}

#[test]
fn a_deployments_config_is_canonical() {
    let config = services(&[("app", WORKER), ("api", WORKER)]);
    // The same content in many segments.
    let mut message = capnp::message::Builder::new(
        HeapAllocator::new()
            .first_segment_words(1)
            .max_segment_words(4),
    );
    let source = read(&config).unwrap();
    message
        .set_root(source.get_root::<config::Reader<'_>>().unwrap())
        .unwrap();
    assert!(message.get_segments_for_output().len() > 1);
    let segmented = capnp::serialize::write_message_to_words(&message);
    assert_ne!(segmented, config);

    let canonical = deployment_config("app", &config).unwrap();
    assert_eq!(deployment_config("app", &segmented).unwrap(), canonical);
    assert_eq!(deployment_config("app", &canonical).unwrap(), canonical);
}

#[test]
fn a_large_deployment_is_checked_and_merged() {
    // Far larger than a message's first segment, as a deployment with its modules is.
    let source = "x".repeat(1 << 20);
    let worker = Kind::Worker(Worker {
        module: Some(&source),
        ..Worker::default()
    });
    let config = services(&[("app", worker)]);
    let a = deployment("a", &config);
    assert!(a.config.len() > 1 << 20);
    let merged = merge(&node(), &[a], &all_set).unwrap();
    assert_eq!(names(&merged), ["local", "main", "app"]);
}

#[test]
fn a_malformed_config_is_refused() {
    let message = refusal(deployment_config("app", b"not a message"));
    assert!(
        message.starts_with("deployment \"app\" is not a valid config: "),
        "{message}"
    );
}
