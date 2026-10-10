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
    let (result, _) = actor_configs_of(
        |config| {
            let mut service = config.init_services(1).get(0);
            service.set_name("main");
            service
                .init_worker()
                .set_durable_object_unique_key_modifier("mod");
        },
        false,
    );
    let error = result.err().unwrap();
    assert_eq!(
        error.description(),
        "durableObjectUniqueKeyModifier is not implemented yet"
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
    let channel = lookup_service(&services, &report, &designator("nope", None));
    assert!(channel.start_request(dummy_metadata()).is_err());
    assert_eq!(
        *errors.borrow(),
        [
            "Worker \"w\"'s binding \"b\" refers to a service \"nope\", but no such service \
                 is defined."
        ]
    );

    let (report, errors) = collecting_reporter();
    lookup_actor_class(&services, &report, &designator("nope", None));
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
    lookup_service(&services, &report, &designator("net", Some("ep")));
    assert_eq!(
        *errors.borrow(),
        [
            "Worker \"w\"'s binding \"b\" refers to service \"net\" with a named entrypoint \
                 \"ep\", but \"net\" is not a Worker, so does not have any named entrypoints."
        ]
    );

    let (report, errors) = collecting_reporter();
    lookup_service(&services, &report, &designator("net", None));
    assert!(errors.borrow().is_empty());
}

#[test]
fn lookup_of_a_leaf_as_an_actor_class_is_reported() {
    let services = leaf_services();
    let (report, errors) = collecting_reporter();
    lookup_actor_class(&services, &report, &designator("net", Some("C")));
    lookup_actor_class(&services, &report, &designator("net", None));
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
    let channel = lookup_service(&services, &report, &designator("bad", None));
    assert!(errors.borrow().is_empty());
    let error = channel.start_request(dummy_metadata()).err().unwrap();
    assert_eq!(
        error.description(),
        "jsg.Error: Service cannot handle requests because its config is invalid."
    );
}

fn dummy_metadata() -> KjOwn<RequestMetadata> {
    ffi::new_request_metadata(kj_rs::KjMaybe::None, kj_rs::KjMaybe::None)
}
