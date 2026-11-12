// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use capnp::message::HeapAllocator;
use capnp::message::ReaderSegments;
use workerd_capnp::config;
use workerd_capnp::service;

use super::Composed;
use super::Composition;
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
    Bucket,
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
        Kind::Bucket => {
            service.init_bucket().set_path("kv");
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

/// A node's config: a disk, a worker, a socket, structured logging and the cluster block.
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
        root.set_structured_logging(true);
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

/// The deployments `composed` skipped, by name, with their errors joined by newlines.
fn skipped(composed: &Composed) -> Vec<(&str, String)> {
    composed
        .deployments
        .iter()
        .filter(|c| !c.errors.is_empty())
        .map(|c| (c.name.as_str(), c.errors.join("\n")))
        .collect()
}

fn refusal(result: crate::Result<impl std::fmt::Debug>) -> String {
    match result {
        Err(Error::Deploy(message)) => message,
        other => panic!("expected a refusal, got {other:?}"),
    }
}

#[test]
fn merging_appends_the_deployments_services_and_keeps_the_nodes_config() {
    let a = deployment("a", &services(&[("app", WORKER)]));
    let b = deployment(
        "b",
        &services(&[("api", WORKER), ("egress", Kind::Network)]),
    );
    let merged = merge(&node(), &[a, b]).unwrap().config;
    assert_eq!(names(&merged), ["local", "main", "app", "api", "egress"]);

    let message = read(&merged).unwrap();
    let root = message.get_root::<config::Reader<'_>>().unwrap();
    let sockets = root.get_sockets().unwrap();
    assert_eq!(sockets.len(), 1);
    assert_eq!(
        sockets.get(0).get_service().unwrap().get_name().unwrap(),
        "app"
    );
    assert!(root.get_structured_logging());
    let cluster = root.get_cluster().unwrap();
    assert_eq!(cluster.get_bucket().unwrap(), "s3://bucket/prefix");
}

#[test]
fn the_merged_config_holds_nothing_unreachable() {
    let a = deployment("a", &services(&[("app", WORKER)]));
    let merged = merge(&node(), &[a]).unwrap().config;
    let message = read(&merged).unwrap();
    let root = message.get_root::<config::Reader<'_>>().unwrap();
    let words = root.total_size().unwrap().word_count;
    let segments = message.into_segments();
    assert_eq!(segments.len(), 1);
    // The root pointer, then what it reaches and nothing else.
    let segment = segments.get_segment(0).unwrap();
    assert_eq!(segment.len() / 8, usize::try_from(words).unwrap() + 1);
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
    let config = services(&[
        ("files", Kind::Disk),
        ("blobs", Kind::Bucket),
        ("internet", Kind::Network),
    ]);
    assert_eq!(
        refusal(deployment_config("app", &config)),
        "deployment \"app\" defines disk service \"files\". A deployment cannot define a disk. \
         Define the disk in the instance's config and refer to it by name.\n\
         deployment \"app\" defines bucket service \"blobs\". A deployment cannot define a \
         bucket service. Define it in the instance's config and refer to it by name.\n\
         deployment \"app\" defines service \"internet\". Only an instance's config may define \
         this service."
    );
}

#[test]
fn service_names_are_unique_across_the_node_and_its_deployments() {
    let a = deployment("a", &services(&[("main", WORKER), ("shared", WORKER)]));
    let b = deployment("b", &services(&[("shared", WORKER)]));
    // "a" is skipped, so "b" may define "shared".
    let composed = merge(&node(), &[a.clone(), b.clone()]).unwrap();
    assert_eq!(
        skipped(&composed),
        [(
            "a",
            "deployment \"a\" defines service \"main\". The same service is already defined by \
             the instance's config."
                .to_owned()
        )]
    );
    assert_eq!(names(&composed.config), ["local", "main", "shared"]);
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
    let composed = merge(&node(), &[a, b.clone(), c.clone()]).unwrap();
    assert_eq!(
        skipped(&composed),
        [
            (
                "a",
                "deployment \"a\" defines Durable Object namespace uniqueKey \"node-key\". The \
                 same Durable Object namespace uniqueKey is already defined by the instance's \
                 config."
                    .to_owned()
            ),
            (
                "c",
                "deployment \"c\" defines Durable Object namespace uniqueKey \"app-key\". The \
                 same Durable Object namespace uniqueKey is already defined by deployment \"b\"."
                    .to_owned()
            ),
        ]
    );
    assert_eq!(names(&composed.config), ["local", "main", "two"]);
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
        skipped(&merge(&node, &[a]).unwrap()),
        [(
            "a",
            "deployment \"a\" defines Workflow namespace key \"miniflare-workflows-orders\". The \
             same Workflow namespace key is already defined by the instance's config."
                .to_owned()
        )]
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
        refusal(merge(&newer(true), &[])),
        "the instance's config defines service \"\" with fields this workerd does not know."
    );
}

#[test]
fn a_deployment_cannot_read_the_environment() {
    let worker = Kind::Worker(Worker {
        env: Some("API_TOKEN"),
        wrapped_env: Some("WRAPPED_TOKEN"),
        ..Worker::default()
    });
    assert_eq!(
        refusal(deployment_config("a", &services(&[("app", worker)]))),
        "service \"app\" in deployment \"a\" has `fromEnvironment` binding \"SECRET\". A \
         deployment cannot read the instance's environment variables.\n\
         service \"app\" in deployment \"a\" has `fromEnvironment` binding \"INNER\". A \
         deployment cannot read the instance's environment variables."
    );
}

#[test]
fn the_nodes_own_config_may_read_the_environment() {
    let worker = Kind::Worker(Worker {
        env: Some("UNSET"),
        ..Worker::default()
    });
    let node = services(&[("main", worker)]);
    let merged = merge(&node, &[]).unwrap().config;
    assert_eq!(names(&merged), ["main"]);
}

/// A deployment as a tool other than workerd may publish it: `config` as it is.
fn unchecked(name: &str, config: Vec<u8>) -> Deployment {
    Deployment {
        name: name.to_owned(),
        version: version_of(&config),
        config,
    }
}

#[test]
fn a_deployment_that_breaks_a_rule_is_skipped_as_a_whole() {
    let first = deployment("a", &services(&[("x", WORKER), ("y", WORKER)]));
    // "y" clashes with "a"; "z" is only "b"'s.
    let clashing = deployment("b", &services(&[("y", WORKER), ("z", WORKER)]));
    let env = Kind::Worker(Worker {
        env: Some("TOKEN"),
        ..Worker::default()
    });
    let reading = unchecked("c", services(&[("reads-env", env)]));
    let malformed = unchecked("d", b"not a message".to_vec());
    let last = deployment("e", &services(&[("w", WORKER)]));
    let composed = merge(
        &node(),
        &[
            first.clone(),
            clashing.clone(),
            reading.clone(),
            malformed,
            last,
        ],
    )
    .unwrap();
    assert_eq!(names(&composed.config), ["local", "main", "x", "y", "w"]);
    assert_eq!(
        composed.deployments[..3],
        [
            Composition {
                name: "a".to_owned(),
                version: first.version,
                services: vec!["x".to_owned(), "y".to_owned()],
                errors: Vec::new(),
            },
            Composition {
                name: "b".to_owned(),
                version: clashing.version,
                services: vec!["z".to_owned()],
                errors: vec![
                    "deployment \"b\" defines service \"y\". The same service is already \
                     defined by deployment \"a\"."
                        .to_owned()
                ],
            },
            Composition {
                name: "c".to_owned(),
                version: reading.version,
                services: vec!["reads-env".to_owned()],
                errors: vec![
                    "service \"reads-env\" in deployment \"c\" has `fromEnvironment` binding \
                     \"SECRET\". A deployment cannot read the instance's environment variables."
                        .to_owned()
                ],
            },
        ]
    );
    let malformed = &composed.deployments[3];
    assert!(malformed.services.is_empty());
    assert!(
        malformed.errors[0].starts_with("deployment \"d\" is not a valid config: "),
        "{:?}",
        malformed.errors
    );
    assert!(composed.deployments[4].errors.is_empty());
}

#[test]
fn a_deployment_with_fields_of_a_newer_schema_is_skipped() {
    let a = unchecked("a", newer(true));
    let composed = merge(&node(), &[a]).unwrap();
    assert_eq!(
        skipped(&composed),
        [(
            "a",
            "deployment \"a\" defines service \"\" with fields this workerd does not know."
                .to_owned()
        )]
    );
    assert_eq!(names(&composed.config), ["local", "main"]);
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
    let merged = merge(&node(), &[a]).unwrap().config;
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

/// A deployment of one worker whose module's source is not UTF-8.
fn not_utf8() -> Vec<u8> {
    let worker = Kind::Worker(Worker {
        module: Some("PLACEHOLDER"),
        ..Worker::default()
    });
    let mut config = services(&[("app", worker)]);
    let at = config
        .windows(11)
        .position(|w| w == b"PLACEHOLDER")
        .unwrap();
    config[at] = 0xff;
    config
}

#[test]
fn a_deployment_with_text_that_is_not_utf8_is_refused() {
    let error = "deployment \"a\" has text that is not valid UTF-8 at \
                 Config.services[0].worker.modules[0].esModule.";
    assert_eq!(refusal(deployment_config("a", &not_utf8())), error);
    let a = Deployment {
        name: "a".to_owned(),
        version: version_of(&not_utf8()),
        config: not_utf8(),
    };
    assert_eq!(refusal(check_deployments(&[&a])), error);
}

#[test]
fn a_deployment_with_text_that_is_not_utf8_or_a_unique_key_outside_its_directory_is_skipped() {
    let escaping = Kind::Worker(Worker {
        unique_key: Some("../../x"),
        ..Worker::default()
    });
    let deployments = [
        unchecked("a", not_utf8()),
        unchecked("b", services(&[("escapes", escaping)])),
        deployment("c", &services(&[("w", WORKER)])),
    ];
    let composed = merge(&node(), &deployments).unwrap();
    assert_eq!(names(&composed.config), ["local", "main", "w"]);
    let skipped = skipped(&composed);
    assert_eq!(skipped.len(), 2, "{skipped:?}");
    assert_eq!(
        skipped[0],
        (
            "a",
            "deployment \"a\" has text that is not valid UTF-8 at \
             Config.services[0].worker.modules[0].esModule."
                .to_owned()
        )
    );
    assert_eq!(skipped[1].0, "b");
    assert!(
        skipped[1].1.contains("uniqueKey \"../../x\""),
        "{skipped:?}"
    );
}

#[test]
fn a_deployments_unique_keys_are_path_components() {
    for key in ["", ".", "..", "../../x", "a/b", "a\\b", "a\0b"] {
        let worker = Kind::Worker(Worker {
            unique_key: Some(key),
            ..Worker::default()
        });
        let message = refusal(deployment_config("a", &services(&[("app", worker)])));
        assert!(
            message.starts_with(&format!(
                "service \"app\" in deployment \"a\" has Durable Object namespace uniqueKey \
                 {key:?}."
            )),
            "{message}"
        );
    }
    let worker = Kind::Worker(Worker {
        unique_key: Some("app-Counter.v2"),
        ..Worker::default()
    });
    assert!(deployment_config("a", &services(&[("app", worker)])).is_ok());
}
