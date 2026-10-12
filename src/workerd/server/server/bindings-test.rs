// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use capnp::message::ReaderOptions;
use workerd_capnp::worker::binding::crypto_key::Usage;

use super::*;

/// A `Worker` message whose `bindings` `build` fills in.
fn worker_message(
    binding_count: u32,
    build: impl FnOnce(struct_list::Builder<'_, binding::Owned>),
) -> message::Builder<HeapAllocator> {
    let mut message = message::Builder::new_default();
    let worker = message.init_root::<worker::Builder>();
    build(worker.init_bindings(binding_count));
    message
}

fn compile(
    message: &message::Builder<HeapAllocator>,
    actor_configs: &ActorConfigs,
    experimental: bool,
) -> CompiledBindings {
    compile_bindings(
        "main",
        message.get_root_as_reader::<worker::Reader>().unwrap(),
        actor_configs,
        &actor_configs["main"],
        experimental,
    )
    .unwrap()
}

/// Every global of an encoded `Globals` message, in capnp's text format.
fn rendered(words: &[u64]) -> Vec<String> {
    let bytes: Vec<u8> = words.iter().flat_map(|word| word.to_ne_bytes()).collect();
    let message =
        capnp::serialize::read_message(&mut bytes.as_slice(), ReaderOptions::new()).unwrap();
    let globals = message.get_root::<globals::Reader>().unwrap();
    let globals = globals.get_globals().unwrap();
    globals.iter().map(|global| format!("{global:?}")).collect()
}

fn no_actors() -> ActorConfigs {
    ActorConfigs::from([("main".to_owned(), ActorConfigMap::new())])
}

fn durable(unique_key: &str) -> ActorConfig {
    ActorConfig::Durable {
        unique_key: unique_key.to_owned(),
        evictable: false,
        enable_sql: true,
        workflow: None,
        container: None,
    }
}

const EPHEMERAL: ActorConfig = ActorConfig::Ephemeral {
    evictable: true,
    enable_sql: false,
};

fn designator(service: &str) -> Designator {
    Designator {
        service: service.to_owned(),
        entrypoint: None,
        props_json: None,
        error_context: String::new(),
    }
}

/// Designators compare without their error context.
fn without_context(designators: &[Designator]) -> Vec<Designator> {
    designators
        .iter()
        .map(|designator| Designator {
            error_context: String::new(),
            ..designator.clone()
        })
        .collect()
}

#[test]
fn subrequest_channels_start_after_the_special_ones() {
    let message = worker_message(6, |mut bindings| {
        let mut service = bindings.reborrow().get(0);
        service.set_name("SVC");
        let mut designator = service.init_service();
        designator.set_name("target");
        designator.set_entrypoint("ep");
        designator.init_props().set_json("{\"x\":1}");
        let mut kv = bindings.reborrow().get(1);
        kv.set_name("KV");
        kv.init_kv_namespace().set_name("kv-svc");
        let mut r2 = bindings.reborrow().get(2);
        r2.set_name("R2");
        r2.init_r2_bucket().set_name("r2-svc");
        let mut queue = bindings.reborrow().get(3);
        queue.set_name("Q");
        queue.init_queue().set_name("q-svc");
        let mut engine = bindings.reborrow().get(4);
        engine.set_name("AE");
        engine.init_analytics_engine().set_name("ae-svc");
        let mut hyperdrive = bindings.get(5);
        hyperdrive.set_name("HD");
        let mut hyperdrive = hyperdrive.init_hyperdrive();
        hyperdrive.reborrow().init_designator().set_name("hd-svc");
        hyperdrive.set_database("db");
        hyperdrive.set_user("user");
        hyperdrive.set_password("pw");
        hyperdrive.set_scheme("postgres");
    });
    let compiled = compile(&message, &no_actors(), true);
    assert_eq!(compiled.errors, Vec::<String>::new());
    assert_eq!(
        rendered(&compiled.globals),
        [
            "(name = \"SVC\", fetcher = 2)",
            "(name = \"KV\", kvNamespace = 3)",
            "(name = \"R2\", r2Bucket = (channel = 4, bucket = \"r2-svc\"))",
            "(name = \"Q\", queue = 5)",
            "(name = \"AE\", analyticsEngine = (channel = 6, dataset = \"ae-svc\"))",
            "(name = \"HD\", hyperdrive = (channel = 7, database = \"db\", user = \"user\", \
                 password = \"pw\", scheme = \"postgres\"))",
        ]
    );
    assert_eq!(
        compiled.subrequest,
        [
            Designator {
                service: "target".to_owned(),
                entrypoint: Some("ep".to_owned()),
                props_json: Some("{\"x\":1}".to_owned()),
                error_context: "Worker \"main\"'s binding \"SVC\"".to_owned(),
            },
            Designator {
                error_context: "Worker \"main\"'s binding \"KV\"".to_owned(),
                ..designator("kv-svc")
            },
            Designator {
                error_context: "Worker \"main\"'s binding \"R2\"".to_owned(),
                ..designator("r2-svc")
            },
            Designator {
                error_context: "Worker \"main\"'s binding \"Q\"".to_owned(),
                ..designator("q-svc")
            },
            Designator {
                error_context: "Worker \"main\"'s binding \"AE\"".to_owned(),
                ..designator("ae-svc")
            },
            Designator {
                error_context: "Worker \"main\"'s binding \"HD\"".to_owned(),
                ..designator("hd-svc")
            },
        ]
    );
}

#[test]
fn durable_object_namespaces() {
    let mut actor_configs = no_actors();
    let local = actor_configs.get_mut("main").unwrap();
    local.insert("Counter".to_owned(), durable("key-1"));
    local.insert("Cache".to_owned(), EPHEMERAL);
    actor_configs.insert(
        "other".to_owned(),
        ActorConfigMap::from_iter([("Remote".to_owned(), durable("key-2"))]),
    );
    let message = worker_message(3, |mut bindings| {
        let mut counter = bindings.reborrow().get(0);
        counter.set_name("COUNTER");
        counter
            .init_durable_object_namespace()
            .set_class_name("Counter");
        let mut cache = bindings.reborrow().get(1);
        cache.set_name("CACHE");
        cache
            .init_durable_object_namespace()
            .set_class_name("Cache");
        let mut remote = bindings.get(2);
        remote.set_name("REMOTE");
        let mut designator = remote.init_durable_object_namespace();
        designator.set_class_name("Remote");
        designator.set_service_name("other");
        let mut policy = designator.init_retry_policy();
        policy.set_max_attempts(3);
        policy.set_timeout_ms(5000);
    });
    let compiled = compile(&message, &actor_configs, false);
    assert_eq!(compiled.errors, Vec::<String>::new());
    assert_eq!(
        rendered(&compiled.globals),
        [
            "(name = \"COUNTER\", durableActorNamespace = (actorChannel = 0, uniqueKey = \
                 \"key-1\"))",
            "(name = \"CACHE\", ephemeralActorNamespace = 1)",
            "(name = \"REMOTE\", durableActorNamespace = (actorChannel = 2, uniqueKey = \
                 \"key-2\", retryPolicy = (maxAttempts = 3, timeoutMs = 5000)))",
        ]
    );
    assert_eq!(
        compiled.actors,
        [
            ActorDesignator {
                service: None,
                class_name: "Counter".to_owned()
            },
            ActorDesignator {
                service: None,
                class_name: "Cache".to_owned()
            },
            ActorDesignator {
                service: Some("other".to_owned()),
                class_name: "Remote".to_owned()
            },
        ]
    );
}

#[test]
fn unknown_durable_object_namespaces_take_no_channel() {
    let mut actor_configs = no_actors();
    actor_configs.insert("other".to_owned(), ActorConfigMap::new());
    let message = worker_message(4, |mut bindings| {
        let mut local = bindings.reborrow().get(0);
        local.set_name("LOCAL");
        local
            .init_durable_object_namespace()
            .set_class_name("Missing");
        let mut no_service = bindings.reborrow().get(1);
        no_service.set_name("NO_SERVICE");
        let mut designator = no_service.init_durable_object_namespace();
        designator.set_class_name("Missing");
        designator.set_service_name("nowhere");
        let mut no_class = bindings.reborrow().get(2);
        no_class.set_name("NO_CLASS");
        let mut designator = no_class.init_durable_object_namespace();
        designator.set_class_name("Missing");
        designator.set_service_name("other");
        let mut class = bindings.get(3);
        class.set_name("CLASS");
        class.init_durable_object_class().set_name("other");
    });
    let compiled = compile(&message, &actor_configs, true);
    assert_eq!(
        compiled.errors,
        [
            "Worker \"main\"'s binding \"LOCAL\" refers to a Durable Object namespace named \
                 \"Missing\", but no such Durable Object namespace is defined by this Worker.",
            "Worker \"main\"'s binding \"NO_SERVICE\" refers to a service \"nowhere\", but no \
                 such service is defined.",
            "Worker \"main\"'s binding \"NO_CLASS\" refers to a Durable Object namespace \
                 named \"Missing\" in service \"other\", but no such Durable Object namespace is \
                 defined by that service.",
        ]
    );
    assert!(compiled.actors.is_empty());
    assert_eq!(
        rendered(&compiled.globals),
        ["(name = \"CLASS\", actorClass = 0)"]
    );
    assert_eq!(
        without_context(&compiled.actor_classes),
        [designator("other")]
    );
}

#[test]
fn wrapped_bindings_share_the_channel_tables() {
    let message = worker_message(3, |mut bindings| {
        let mut wrapped = bindings.reborrow().get(0);
        wrapped.set_name("WRAPPED");
        let mut wrapped = wrapped.init_wrapped();
        wrapped.set_module_name("my-module");
        wrapped.set_entrypoint("make");
        let mut inner = wrapped.init_inner_bindings(2);
        let mut service = inner.reborrow().get(0);
        service.set_name("INNER_SVC");
        service.init_service().set_name("inner-svc");
        let mut text = inner.get(1);
        text.set_name("INNER_TEXT");
        text.set_text("t");

        // One inner binding fails: the wrapped binding has no global, but the channel its
        // first inner binding took stays allocated.
        let mut failing = bindings.reborrow().get(1);
        failing.set_name("FAILING");
        let mut inner = failing.init_wrapped().init_inner_bindings(2);
        let mut service = inner.reborrow().get(0);
        service.set_name("TAKEN");
        service.init_service().set_name("taken-svc");
        let mut unspecified = inner.get(1);
        unspecified.set_name("BROKEN");
        unspecified.set_unspecified(());

        let mut service = bindings.get(2);
        service.set_name("AFTER");
        service.init_service().set_name("after-svc");
    });
    let compiled = compile(&message, &no_actors(), false);
    assert_eq!(
        compiled.errors,
        ["Worker \"main\"'s binding \"BROKEN\" does not specify any binding value."]
    );
    assert_eq!(
        rendered(&compiled.globals),
        [
            "(name = \"WRAPPED\", wrapped = (moduleName = \"my-module\", entrypoint = \
                 \"make\", innerBindings = [(name = \"INNER_SVC\", fetcher = 2), (name = \
                 \"INNER_TEXT\", text = \"t\")]))",
            "(name = \"AFTER\", fetcher = 4)",
        ]
    );
    assert_eq!(
        without_context(&compiled.subrequest),
        [
            designator("inner-svc"),
            designator("taken-svc"),
            designator("after-svc"),
        ]
    );
}

#[test]
fn crypto_key_raw_formats() {
    let message = worker_message(5, |mut bindings| {
        let mut raw = bindings.reborrow().get(0);
        raw.set_name("RAW");
        let mut key = raw.init_crypto_key();
        key.set_raw(&[9, 8]);
        key.reborrow().init_algorithm().set_name("AES-GCM");
        key.set_extractable(true);
        let mut usages = key.init_usages(2);
        usages.set(0, Usage::Encrypt);
        usages.set(1, Usage::DeriveBits);

        let mut hex = bindings.reborrow().get(1);
        hex.set_name("HEX");
        let mut key = hex.init_crypto_key();
        key.set_hex("0aFF");
        key.init_algorithm().set_json("{\"name\":\"HMAC\"}");

        let mut bad_hex = bindings.reborrow().get(2);
        bad_hex.set_name("BAD_HEX");
        let mut key = bad_hex.init_crypto_key();
        key.set_hex("0g1");
        key.init_algorithm().set_name("HMAC");

        let mut base64 = bindings.reborrow().get(3);
        base64.set_name("B64");
        let mut key = base64.init_crypto_key();
        key.set_base64("AQID");
        key.init_algorithm().set_name("a\"b\n");

        let mut bad_base64 = bindings.get(4);
        bad_base64.set_name("BAD_B64");
        let mut key = bad_base64.init_crypto_key();
        key.set_base64("AQ!D");
        key.init_algorithm().set_name("HMAC");
    });
    let compiled = compile(&message, &no_actors(), false);
    assert_eq!(
        compiled.errors,
        [
            "CryptoKey binding \"BAD_HEX\" contained invalid hex.",
            "CryptoKey binding \"BAD_B64\" contained invalid base64.",
        ]
    );
    assert_eq!(
        rendered(&compiled.globals),
        [
            "(name = \"RAW\", cryptoKey = (format = \"raw\", keyData = (bytes = 0x\"0908\"), \
                 algorithm = \"\\\"AES-GCM\\\"\", extractable = true, usages = [encrypt, \
                 deriveBits]))",
            "(name = \"HEX\", cryptoKey = (format = \"raw\", keyData = (bytes = 0x\"0aff\"), \
                 algorithm = \"{\\\"name\\\":\\\"HMAC\\\"}\", extractable = false))",
            "(name = \"B64\", cryptoKey = (format = \"raw\", keyData = (bytes = \
                 0x\"010203\"), algorithm = \"\\\"a\\\\\\\"b\\\\n\\\"\", extractable = false))",
        ]
    );
}

#[test]
fn crypto_key_pem_and_jwk() {
    const PKCS8: &str = "some text before\n\
                             -----BEGIN PRIVATE KEY-----\r\n\
                             AQID\r\n\
                             BAU=\r\n\
                             -----END PRIVATE KEY-----\r\n";
    const SPKI: &str = "-----BEGIN PUBLIC KEY-----\nBgc=\n-----END PUBLIC KEY-----\n";
    let message = worker_message(5, |mut bindings| {
        let mut pkcs8 = bindings.reborrow().get(0);
        pkcs8.set_name("PKCS8");
        let mut key = pkcs8.init_crypto_key();
        key.set_pkcs8(PKCS8);
        key.init_algorithm().set_name("RSA-PSS");

        let mut spki = bindings.reborrow().get(1);
        spki.set_name("SPKI");
        let mut key = spki.init_crypto_key();
        key.set_spki(SPKI);
        key.init_algorithm().set_name("RSA-PSS");

        let mut wrong = bindings.reborrow().get(2);
        wrong.set_name("WRONG");
        let mut key = wrong.init_crypto_key();
        key.set_pkcs8(SPKI);
        key.init_algorithm().set_name("RSA-PSS");

        let mut invalid = bindings.reborrow().get(3);
        invalid.set_name("INVALID");
        let mut key = invalid.init_crypto_key();
        key.set_spki("-----BEGIN PUBLIC KEY-----\nBgc=\n-----END PRIVATE KEY-----\n");
        key.init_algorithm().set_name("RSA-PSS");

        let mut jwk = bindings.get(4);
        jwk.set_name("JWK");
        let mut key = jwk.init_crypto_key();
        key.set_jwk("{\"kty\":\"oct\"}");
        key.init_algorithm().set_name("HMAC");
    });
    let compiled = compile(&message, &no_actors(), false);
    assert_eq!(
        compiled.errors,
        [
            "CryptoKey binding \"WRONG\" contained wrong PEM type, expected PrivateKey but \
                 got PublicKey.",
            "CryptoKey binding \"INVALID\" contained invalid PEM format.",
        ]
    );
    assert_eq!(
        rendered(&compiled.globals),
        [
            "(name = \"PKCS8\", cryptoKey = (format = \"pkcs8\", keyData = (bytes = \
                 0x\"0102030405\"), algorithm = \"\\\"RSA-PSS\\\"\", extractable = false))",
            "(name = \"SPKI\", cryptoKey = (format = \"spki\", keyData = (bytes = 0x\"0607\"), \
                 algorithm = \"\\\"RSA-PSS\\\"\", extractable = false))",
            "(name = \"JWK\", cryptoKey = (format = \"jwk\", keyData = (json = \
                 \"{\\\"kty\\\":\\\"oct\\\"}\"), algorithm = \"\\\"HMAC\\\"\", extractable = \
                 false))",
        ]
    );
}

#[test]
fn experimental_bindings_are_refused_without_the_flag() {
    let build = |mut bindings: struct_list::Builder<'_, binding::Owned>| {
        let mut eval = bindings.reborrow().get(0);
        eval.set_name("EVAL");
        eval.set_unsafe_eval(());
        let mut cache = bindings.reborrow().get(1);
        cache.set_name("CACHE");
        let mut cache = cache.init_memory_cache();
        cache.set_id("shared");
        let mut limits = cache.init_limits();
        limits.set_max_keys(10);
        limits.set_max_value_size(20);
        limits.set_max_total_value_size(30);
        let mut class = bindings.reborrow().get(2);
        class.set_name("CLASS");
        class.init_durable_object_class().set_name("svc");
        let mut loader = bindings.reborrow().get(3);
        loader.set_name("LOADER");
        loader.init_worker_loader().set_id("loader-id");
        let mut anonymous_loader = bindings.reborrow().get(4);
        anonymous_loader.set_name("ANON_LOADER");
        anonymous_loader.init_worker_loader();
        let mut debug = bindings.reborrow().get(5);
        debug.set_name("DEBUG");
        debug.set_workerd_debug_port(());
        let mut engine = bindings.get(6);
        engine.set_name("AE");
        engine.init_analytics_engine().set_name("ae-svc");
    };

    let compiled = compile(&worker_message(7, build), &no_actors(), false);
    assert_eq!(
        compiled.errors,
        [
            "Unsafe eval bindings are an experimental feature which may change or go away in \
                 the future. You must run workerd with `--experimental` to use this feature.",
            "MemoryCache bindings are an experimental feature which may change or go away in \
                 the future. You must run workerd with `--experimental` to use this feature.",
            "Durable Object class bindings are an experimental feature which may change or go \
                 away in the future. You must run workerd with `--experimental` to use this \
                 feature.",
            "Worker loader bindings are an experimental feature which may change or go away \
                 in the future. You must run workerd with `--experimental` to use this feature.",
            "Worker loader bindings are an experimental feature which may change or go away \
                 in the future. You must run workerd with `--experimental` to use this feature.",
            "workerdDebugPort bindings are an experimental feature which may change or go \
                 away in the future. You must run workerd with `--experimental` to use this \
                 feature.",
            "AnalyticsEngine bindings are an experimental feature which may change or go away \
                 in the future. You must run workerd with `--experimental` to use this feature.",
        ]
    );
    assert!(rendered(&compiled.globals).is_empty());
    assert!(compiled.actor_classes.is_empty());
    assert!(compiled.worker_loaders.is_empty());
    assert!(!compiled.has_debug_port);

    let compiled = compile(&worker_message(7, build), &no_actors(), true);
    assert_eq!(compiled.errors, Vec::<String>::new());
    assert_eq!(
        rendered(&compiled.globals),
        [
            "(name = \"EVAL\", unsafeEval = ())",
            "(name = \"CACHE\", memoryCache = (cacheId = \"shared\", maxKeys = 10, \
                 maxValueSize = 20, maxTotalValueSize = 30))",
            "(name = \"CLASS\", actorClass = 0)",
            "(name = \"LOADER\", workerLoader = 0)",
            "(name = \"ANON_LOADER\", workerLoader = 1)",
            "(name = \"DEBUG\", workerdDebugPort = ())",
            "(name = \"AE\", analyticsEngine = (channel = 2, dataset = \"ae-svc\"))",
        ]
    );
    assert_eq!(
        without_context(&compiled.actor_classes),
        [designator("svc")]
    );
    assert_eq!(
        compiled.worker_loaders,
        [
            WorkerLoaderDesignator {
                name: "loader-id".to_owned(),
                id: Some("loader-id".to_owned()),
            },
            WorkerLoaderDesignator {
                name: "ANON_LOADER".to_owned(),
                id: None,
            },
        ]
    );
    assert!(compiled.has_debug_port);
}

#[test]
fn memory_cache_needs_limits() {
    let message = worker_message(1, |bindings| {
        let mut cache = bindings.get(0);
        cache.set_name("CACHE");
        cache.init_memory_cache();
    });
    let compiled = compile(&message, &no_actors(), true);
    assert_eq!(
        compiled.errors,
        [
            "MemoryCache bindings must specify limits. Please update the binding in the worker \
              configuration and try again."
        ]
    );
    assert!(rendered(&compiled.globals).is_empty());
}

#[test]
fn unsupported_bindings() {
    let message = worker_message(3, |mut bindings| {
        let mut unspecified = bindings.reborrow().get(0);
        unspecified.set_name("NONE");
        unspecified.set_unspecified(());
        let mut wasm = bindings.reborrow().get(1);
        wasm.set_name("WASM");
        wasm.set_wasm_module(&[0]);
        let mut obsolete = bindings.get(2);
        obsolete.set_name("OLD");
        obsolete.init_obsolete0().set_name("svc");
    });
    let compiled = compile(&message, &no_actors(), false);
    assert_eq!(
        compiled.errors,
        [
            "Worker \"main\"'s binding \"NONE\" does not specify any binding value.",
            "Worker \"main\"'s binding \"WASM\" is a Wasm binding, but Wasm bindings are not \
                 allowed in modules-based scripts. Use Wasm modules instead.",
            "Worker \"main\"'s binding \"OLD\" uses an obsolete binding type.",
        ]
    );
    assert!(rendered(&compiled.globals).is_empty());
}

#[test]
fn wasm_bindings_of_a_service_worker_script_are_skipped_silently() {
    let mut message = message::Builder::new_default();
    let mut worker = message.init_root::<worker::Builder>();
    worker.set_service_worker_script("addEventListener('fetch', () => {})");
    let mut wasm = worker.init_bindings(1).get(0);
    wasm.set_name("WASM");
    wasm.set_wasm_module(&[0]);
    let compiled = compile(&message, &no_actors(), false);
    assert_eq!(compiled.errors, Vec::<String>::new());
    assert!(rendered(&compiled.globals).is_empty());
}

#[test]
fn loopbacks_are_numbered_after_the_bindings() {
    let mut actor_configs = no_actors();
    let local = actor_configs.get_mut("main").unwrap();
    local.insert("Counter".to_owned(), durable("key-1"));
    local.insert("Cache".to_owned(), EPHEMERAL);
    local.insert("Unexported".to_owned(), durable("key-3"));
    let message = worker_message(3, |mut bindings| {
        let mut service = bindings.reborrow().get(0);
        service.set_name("SVC");
        service.init_service().set_name("svc");
        let mut namespace = bindings.reborrow().get(1);
        namespace.set_name("COUNTER");
        namespace
            .init_durable_object_namespace()
            .set_class_name("Counter");
        let mut class = bindings.get(2);
        class.set_name("CLASS");
        class.init_durable_object_class().set_name("svc");
    });
    let bindings = compile(&message, &actor_configs, true);
    assert_eq!(bindings.errors, Vec::<String>::new());

    let named = [
        "Api".to_owned(),
        "MyWorkflow".to_owned(),
        "Rpc".to_owned(),
        "Unlisted".to_owned(),
    ];
    let workflows = ["MyWorkflow".to_owned(), "Unlisted".to_owned()];
    let classes = ["Counter".to_owned(), "Facet".to_owned(), "Cache".to_owned()];
    let loopbacks = loopback_globals(
        &Exports {
            has_default_entrypoint: true,
            named_entrypoints: &named,
            workflow_classes: &workflows,
            actor_classes: &classes,
        },
        &actor_configs["main"],
        // As `compile_bindings` allocates one for a Workflow the config lists.
        |class| (class == "MyWorkflow").then_some(9),
        len_u32(&bindings.subrequest),
        len_u32(&bindings.actors),
        len_u32(&bindings.actor_classes),
    )
    .unwrap();
    assert_eq!(
        rendered(&loopbacks.globals),
        [
            "(name = \"default\", loopbackServiceStub = 3)",
            "(name = \"Api\", loopbackServiceStub = 4)",
            // A Workflow class takes no loopback channel: the configured one is a wrapped
            // binding over its bindingService's channel, the unlisted one has no entry.
            "(name = \"MyWorkflow\", wrapped = (moduleName = \
                 \"cloudflare-internal:workflows-api\", entrypoint = \"default\", innerBindings = \
                 [(name = \"fetcher\", fetcher = 9)]))",
            "(name = \"Rpc\", loopbackServiceStub = 5)",
            "(name = \"Counter\", loopbackDurableActorNamespace = (actorChannel = 1, \
                 uniqueKey = \"key-1\", classChannel = 1))",
            "(name = \"Facet\", loopbackActorClass = 2)",
            "(name = \"Cache\", loopbackEphemeralActorNamespace = (actorChannel = 2, \
                 classChannel = 3))",
        ]
    );
    assert_eq!(
        loopbacks.subrequest_entrypoints,
        [None, Some("Api".to_owned()), Some("Rpc".to_owned())]
    );
    assert_eq!(loopbacks.actor_classes, classes);
    assert_eq!(
        loopbacks.actor_namespaces,
        ["Counter".to_owned(), "Cache".to_owned()]
    );
}
