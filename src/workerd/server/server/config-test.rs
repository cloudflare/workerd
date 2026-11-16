// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::cell::RefCell;

use super::*;

/// A reporter that collects into a shared list.
fn collecting_reporter() -> (Reporter, Rc<RefCell<Vec<String>>>) {
    let errors = Rc::new(RefCell::new(Vec::new()));
    let sink = Rc::clone(&errors);
    let report = Reporter::new(
        Box::new(move |error| sink.borrow_mut().push(error)),
        Box::new(|_warning| {}),
        true,
    );
    (report, errors)
}

/// The message a config `build` fills in, read back.
fn config_message(
    build: impl FnOnce(config::Builder<'_>),
) -> capnp::message::Reader<capnp::serialize::OwnedSegments> {
    let mut message = capnp::message::Builder::new_default();
    build(message.init_root::<config::Builder>());
    let words = capnp::serialize::write_message_to_words(&message);
    capnp::serialize::read_message(words.as_slice(), ReaderOptions::new()).unwrap()
}

fn actor_configs_of(
    build: impl FnOnce(config::Builder<'_>),
    experimental: bool,
) -> (Result<ActorConfigs>, Vec<String>) {
    let message = config_message(build);
    let config = message.get_root::<config::Reader>().unwrap();
    let (report, errors) = collecting_reporter();
    let result = collect_actor_configs(config, experimental, &report).map_err(capnp_error);
    let errors = errors.borrow().clone();
    (result, errors)
}

#[test]
fn actor_configs_read_durable_and_ephemeral_namespaces() {
    let (result, errors) = actor_configs_of(
        |config| {
            let mut service = config.init_services(1).get(0);
            service.set_name("main");
            let mut worker = service.init_worker();
            worker
                .reborrow()
                .get_durable_object_storage()
                .set_in_memory(());
            let mut namespaces = worker.init_durable_object_namespaces(2);
            let mut durable = namespaces.reborrow().get(0);
            durable.set_class_name("Counter");
            durable.set_unique_key("counter-key");
            durable.set_enable_sql(true);
            let mut ephemeral = namespaces.get(1);
            ephemeral.set_class_name("Cache");
            ephemeral.set_ephemeral_local(());
            ephemeral.set_prevent_eviction(true);
        },
        true,
    );
    assert!(errors.is_empty(), "{errors:?}");
    let configs = result.unwrap();
    let main = &configs["main"];
    assert_eq!(
        main.get("Counter"),
        Some(&ActorConfig::Durable {
            unique_key: "counter-key".to_owned(),
            evictable: true,
            enable_sql: true,
            workflow: None,
            container: None,
        })
    );
    assert_eq!(
        main.get("Cache"),
        Some(&ActorConfig::Ephemeral {
            evictable: false,
            enable_sql: false,
        })
    );
    // Config order is kept.
    let names: Vec<&String> = main.keys().collect();
    assert_eq!(names, ["Counter", "Cache"]);
}

#[test]
fn ephemeral_namespaces_need_experimental() {
    let (result, errors) = actor_configs_of(
        |config| {
            let mut service = config.init_services(1).get(0);
            service.set_name("main");
            let worker = service.init_worker();
            let mut ns = worker.init_durable_object_namespaces(1).get(0);
            ns.set_class_name("Cache");
            ns.set_ephemeral_local(());
        },
        false,
    );
    result.unwrap();
    assert_eq!(
        errors,
        [
            "Ephemeral objects (Durable Object namespaces with type 'ephemeralLocal') are an \
                 experimental feature which may change or go away in the future. You must run \
                 workerd with `--experimental` to use this feature."
        ]
    );
}

#[test]
fn bundled_modules_need_experimental() {
    let (result, errors) = actor_configs_of(
        |config| {
            let mut service = config.init_services(1).get(0);
            service.set_name("kv");
            let worker = service.init_worker();
            worker.init_modules(1).get(0).set_bundled("/workerd/kv.js");
        },
        false,
    );
    result.unwrap();
    assert_eq!(
        errors,
        [
            "Bundled modules are an experimental feature which may change or go away in the \
                 future. You must run workerd with `--experimental` to use this feature."
        ]
    );
}

#[test]
fn durable_classes_need_storage() {
    let (result, errors) = actor_configs_of(
        |config| {
            let mut service = config.init_services(1).get(0);
            service.set_name("main");
            let worker = service.init_worker();
            let mut ns = worker.init_durable_object_namespaces(1).get(0);
            ns.set_class_name("Counter");
            ns.set_unique_key("k");
        },
        false,
    );
    result.unwrap();
    assert_eq!(
        errors,
        [
            "Worker service \"main\" implements durable object classes but has \
                 `durableObjectStorage` set to `none`."
        ]
    );
}

#[test]
fn duplicate_service_names_are_reported() {
    let (result, errors) = actor_configs_of(
        |config| {
            let mut services = config.init_services(2);
            services.reborrow().get(0).set_name("dup");
            services.reborrow().get(0).set_unspecified(());
            services.reborrow().get(1).set_name("dup");
            services.get(1).init_network();
        },
        false,
    );
    let configs = result.unwrap();
    assert_eq!(configs.len(), 1);
    assert_eq!(errors, ["Config defines multiple services named \"dup\"."]);
}

#[test]
fn unique_key_modifier_is_unimplemented() {
    let (result, errors) = actor_configs_of(
        |config| {
            let mut service = config.init_services(1).get(0);
            service.set_name("main");
            service
                .init_worker()
                .set_durable_object_unique_key_modifier("mod");
        },
        false,
    );
    assert!(result.is_ok());
    assert_eq!(
        errors,
        ["service main: durableObjectUniqueKeyModifier is not implemented yet"]
    );
}

struct NullChannel;

impl Channel for NullChannel {
    fn start_request(&self, _metadata: KjOwn<RequestMetadata>) -> Result<KjOwn<WorkerInterface>> {
        Err(kj::failed!("null"))
    }

    fn token(&self, _usage: TokenUsage) -> Result<KjOwn<PendingToken>> {
        Err(kj::failed!("null"))
    }
}

fn designator(service: &str, entrypoint: Option<&str>) -> Designator {
    Designator {
        service: service.to_owned(),
        entrypoint: entrypoint.map(str::to_owned),
        props_json: None,
        error_context: "Worker \"w\"'s binding \"b\"".to_owned(),
    }
}

fn leaf_services() -> LinkedHashMap<String, Service> {
    let mut services = LinkedHashMap::new();
    services.insert("net".to_owned(), Service::Leaf(Rc::new(NullChannel)));
    services.insert(
        "bad".to_owned(),
        Service::Leaf(Rc::new(InvalidConfigChannel)),
    );
    services
}

#[test]
fn lookup_of_a_missing_service_is_reported() {
    let services = leaf_services();
    let (report, errors) = collecting_reporter();
    let channel = lookup_service(&services, &report, None, &designator("nope", None));
    assert!(channel.start_request(dummy_metadata()).is_err());
    assert_eq!(
        *errors.borrow(),
        [
            "Worker \"w\"'s binding \"b\" refers to a service \"nope\", but no such service \
                 is defined."
        ]
    );

    let (report, errors) = collecting_reporter();
    lookup_actor_class(&services, &report, None, &designator("nope", None));
    assert_eq!(
        *errors.borrow(),
        [
            "Worker \"w\"'s binding \"b\" refers to a service \"nope\", but no such service \
                 is defined."
        ]
    );
}

#[test]
fn lookup_of_a_leaf_with_an_entrypoint_is_reported() {
    let services = leaf_services();
    let (report, errors) = collecting_reporter();
    lookup_service(&services, &report, None, &designator("net", Some("ep")));
    assert_eq!(
        *errors.borrow(),
        [
            "Worker \"w\"'s binding \"b\" refers to service \"net\" with a named entrypoint \
                 \"ep\", but \"net\" is not a Worker, so does not have any named entrypoints."
        ]
    );

    let (report, errors) = collecting_reporter();
    lookup_service(&services, &report, None, &designator("net", None));
    assert!(errors.borrow().is_empty());
}

#[test]
fn lookup_of_a_leaf_as_an_actor_class_is_reported() {
    let services = leaf_services();
    let (report, errors) = collecting_reporter();
    lookup_actor_class(&services, &report, None, &designator("net", Some("C")));
    lookup_actor_class(&services, &report, None, &designator("net", None));
    assert_eq!(
        *errors.borrow(),
        [
            "Worker \"w\"'s binding \"b\" refers to service \"net\" with a named Durable \
                 Object entrypoint \"C\", but \"net\" is not a Worker, so does not have any named \
                 entrypoints.",
            "Worker \"w\"'s binding \"b\" refers to service \"net\" as a Durable Object \
                 class, but \"net\" is not a Worker, so cannot be used as a class.",
        ]
    );
}

#[test]
fn invalid_services_fail_requests_with_a_js_error() {
    let services = leaf_services();
    let (report, errors) = collecting_reporter();
    let channel = lookup_service(&services, &report, None, &designator("bad", None));
    assert!(errors.borrow().is_empty());
    let error = channel.start_request(dummy_metadata()).err().unwrap();
    assert_eq!(
        error.description(),
        "jsg.Error: Service cannot handle requests because its config is invalid."
    );
}

/// What a reporter's sink collected.
type Messages = Rc<RefCell<Vec<String>>>;

/// A strict reporter that knows the deployments "app" (service "net") and "gone" (skipped,
/// service "missing"), with what it reports as config errors and as deployment errors.
fn deployment_reporter() -> (Reporter, Messages, Messages) {
    cluster_reporter(Deployments::default())
}

/// A strict reporter with `deployments` plus the default "app" and "gone" deployments.
fn cluster_reporter(deployments: Deployments) -> (Reporter, Messages, Messages) {
    let errors = Rc::new(RefCell::new(Vec::new()));
    let logged = Rc::new(RefCell::new(Vec::new()));
    let (error_sink, log_sink) = (Rc::clone(&errors), Rc::clone(&logged));
    let warning_sink = Rc::clone(&logged);
    let mut deployments = Deployments {
        skipped: HashMap::from([("missing".to_owned(), "gone".to_owned())]),
        ..deployments
    };
    deployments
        .services
        .insert("net".to_owned(), "app".to_owned());
    let report = Reporter::new(
        Box::new(move |error| error_sink.borrow_mut().push(error)),
        Box::new(|_warning| {}),
        false,
    )
    .with_deployments(
        deployments,
        Box::new(move |error| log_sink.borrow_mut().push(error)),
        Box::new(move |warning| {
            warning_sink
                .borrow_mut()
                .push(format!("warning: {warning}"));
        }),
    );
    (report, errors, logged)
}

/// A config of the services `names`, all networks.
fn services_config(names: &[&str]) -> capnp::message::Reader<capnp::serialize::OwnedSegments> {
    config_message(|root| {
        let mut services = root.init_services(len_u32(names));
        for (i, name) in (0..).zip(names) {
            let mut service = services.reborrow().get(i);
            service.set_name(name);
            service.init_network();
        }
    })
}

#[test]
fn an_error_in_a_deployments_service_breaks_it_and_lets_the_server_serve() {
    let (report, errors, logged) = deployment_reporter();
    report.service_error("net", "bad");
    assert!(errors.borrow().is_empty());
    assert!(!report.refuses());
    assert!(report.is_broken("net"));
    report.report_broken();
    assert_eq!(
        *logged.borrow(),
        [
            "deployment \"app\": service net: bad",
            "deployment \"app\": service \"net\" cannot handle requests because of the errors \
             above.",
        ]
    );

    report.service_error("node", "bad");
    assert_eq!(*errors.borrow(), ["service node: bad"]);
    assert!(report.refuses());
    assert!(!report.is_broken("node"));
}

#[test]
fn a_reference_from_the_node_to_a_skipped_deployment_fails_without_refusing() {
    let services = leaf_services();
    let (report, errors, logged) = deployment_reporter();
    let channel = lookup_service(&services, &report, None, &designator("missing", None));
    assert!(channel.start_request(dummy_metadata()).is_err());
    lookup_actor_class(
        &services,
        &report,
        Some("node"),
        &designator("missing", None),
    );
    assert!(errors.borrow().is_empty());
    assert!(!report.refuses());
    let error = "deployment \"gone\": Worker \"w\"'s binding \"b\" refers to a service \
                 \"missing\", but no such service is defined. \"missing\" is defined by this \
                 deployment, which was skipped.";
    assert_eq!(*logged.borrow(), [error, error]);
}

#[test]
fn a_bad_reference_is_its_referrers_error_unless_the_node_makes_it_to_a_deployment() {
    let services = leaf_services();
    let (report, errors, logged) = deployment_reporter();
    // The node to a deployment's service: the deployment's error, which breaks nothing.
    lookup_service(
        &services,
        &report,
        Some("node"),
        &designator("net", Some("ep")),
    );
    assert!(errors.borrow().is_empty());
    assert_eq!(logged.borrow().len(), 1);
    assert!(
        logged.borrow()[0].starts_with("deployment \"app\": Worker \"w\"'s binding \"b\""),
        "{:?}",
        logged.borrow()
    );
    assert!(!report.is_broken("net"));

    // A deployment's service to anything: that service's error, which breaks it.
    lookup_service(&services, &report, Some("net"), &designator("nope", None));
    assert!(errors.borrow().is_empty());
    assert!(report.is_broken("net"));

    // The node to its own: the node's error.
    lookup_service(&services, &report, Some("node"), &designator("nope", None));
    assert_eq!(errors.borrow().len(), 1);
    assert!(report.refuses());
}

fn dummy_metadata() -> KjOwn<RequestMetadata> {
    ffi::new_request_metadata(kj_rs::KjMaybe::None, kj_rs::KjMaybe::None)
}

#[test]
fn a_reference_from_the_node_to_an_unknown_service_is_a_deployments_in_a_cluster() {
    let services = leaf_services();
    let (report, errors, logged) = cluster_reporter(Deployments {
        enabled: true,
        ..Deployments::default()
    });
    let message = services_config(&["node", "net"]);
    report
        .set_node_services(message.get_root::<config::Reader>().unwrap())
        .unwrap();
    let channel = lookup_service(&services, &report, None, &designator("nope", None));
    assert!(channel.start_request(dummy_metadata()).is_err());
    assert!(errors.borrow().is_empty());
    assert!(!report.refuses());
    assert_eq!(
        *logged.borrow(),
        [
            "cluster: Worker \"w\"'s binding \"b\" refers to a service \"nope\", but no such \
          service is defined. Neither the instance's config nor a deployment defines \"nope\"."
        ]
    );

    // A reference the node makes to its own service is still the node's error.
    lookup_service(
        &services,
        &report,
        Some("node"),
        &designator("node", Some("ep")),
    );
    assert_eq!(errors.borrow().len(), 1, "{:?}", errors.borrow());
    assert!(report.refuses());
}

#[test]
fn an_unknown_service_may_belong_to_a_deployment_that_could_not_be_read() {
    let services = leaf_services();
    let (report, errors, logged) = cluster_reporter(Deployments {
        enabled: true,
        unreadable: vec!["lost".to_owned(), "gone-too".to_owned()],
        ..Deployments::default()
    });
    let message = services_config(&["node"]);
    report
        .set_node_services(message.get_root::<config::Reader>().unwrap())
        .unwrap();
    lookup_service(&services, &report, Some("node"), &designator("nope", None));
    assert!(errors.borrow().is_empty());
    assert_eq!(
        *logged.borrow(),
        [
            "cluster: Worker \"w\"'s binding \"b\" refers to a service \"nope\", but no such \
          service is defined. Neither the instance's config nor a deployment that was read \
          defines \"nope\". It may belong to a deployment that could not be read: \"lost\", \
          \"gone-too\"."
        ]
    );
}

#[test]
fn a_deployments_warnings_and_broken_services_are_attributed_to_it() {
    let (report, errors, logged) = cluster_reporter(Deployments {
        services: HashMap::from([("net2".to_owned(), "app".to_owned())]),
        ..Deployments::default()
    });
    report.service_warning("net", "careful");
    report.service_error("net", "bad");
    report.service_error("net2", "worse");
    report.error_in("node", "not a deployment's");
    assert_eq!(*errors.borrow(), ["not a deployment's"]);
    report.report_broken();
    assert_eq!(
        *logged.borrow(),
        [
            "warning: deployment \"app\": service net: careful",
            "deployment \"app\": service net: bad",
            "deployment \"app\": service net2: worse",
            "deployment \"app\": services \"net\", \"net2\" cannot handle requests because of \
             the errors above.",
        ]
    );
}
