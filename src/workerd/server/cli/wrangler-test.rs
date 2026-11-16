// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::collections::HashMap;

use workerd_capnp::service;
use workerd_capnp::service_designator;
use workerd_capnp::worker;

use super::*;

/// A fresh directory in the test's temp directory holding `files`.
fn project(files: &[(&str, &str)]) -> PathBuf {
    use std::sync::atomic::AtomicUsize;
    use std::sync::atomic::Ordering;

    static COUNTER: AtomicUsize = AtomicUsize::new(0);
    let tmp = std::env::var_os("TEST_TMPDIR").map_or_else(std::env::temp_dir, PathBuf::from);
    let dir = tmp.join(format!(
        "workerd-wrangler-test-{}-{}",
        std::process::id(),
        COUNTER.fetch_add(1, Ordering::Relaxed)
    ));
    for (name, contents) in files {
        let path = dir.join(name);
        fs::create_dir_all(path.parent().unwrap()).unwrap();
        fs::write(path, contents).unwrap();
    }
    dir
}

fn load_project(files: &[(&str, &str)]) -> Result<Config, String> {
    let dir = project(files);
    load(
        dir.join("wrangler.jsonc").to_str().unwrap(),
        &Process::new(None, None, Vec::new()),
        &RunOptions::default(),
        true,
    )
}

fn read(config: Config) -> capnp::message::Reader<capnp::serialize::OwnedSegments> {
    let bytes: Vec<u8> = config
        .into_words()
        .iter()
        .flat_map(|w| w.to_ne_bytes())
        .collect();
    capnp::serialize::read_message(bytes.as_slice(), capnp::message::ReaderOptions::new()).unwrap()
}

#[test]
fn jsonc_comments_and_trailing_commas() {
    let src = r#"{
      // a comment
      "url": "https://example.com//x", /* block */
      "quote": "a \" // not a comment",
      "list": [1, 2,],
    }"#;
    let v: Value = serde_json::from_str(&strip_jsonc(src)).unwrap();
    assert_eq!(v["url"], "https://example.com//x");
    assert_eq!(v["quote"], "a \" // not a comment");
    assert_eq!(v["list"], serde_json::json!([1, 2]));
}

#[test]
fn hello() {
    let message = read(
        load_project(&[
            (
                "wrangler.jsonc",
                r#"{ "name": "hello", "main": "index.js", "compatibility_date": "2026-01-01", "no_bundle": true,
                     "vars": { "GREETING": "hi", "N": 3 } }"#,
            ),
            (
                "index.js",
                "export default { fetch() { return new Response('hi') } }",
            ),
            ("lib/util.js", "export const x = 1"),
            ("README.md", "not a module"),
        ])
        .unwrap(),
    );
    let config = message.get_root::<config::Reader>().unwrap();
    let services = config.get_services().unwrap();
    assert_eq!(services.len(), 1);
    let service::Which::Worker(worker) = services.get(0).which().unwrap() else {
        panic!("expected a worker service")
    };
    let worker = worker.unwrap();
    let worker::Which::Modules(modules) = worker.which().unwrap() else {
        panic!("expected a modules worker")
    };
    let names: Vec<_> = modules
        .unwrap()
        .iter()
        .map(|m| m.get_name().unwrap().to_string().unwrap())
        .collect();
    assert_eq!(names, ["index.js", "lib/util.js"]);
    let bindings = worker.get_bindings().unwrap();
    assert_eq!(bindings.get(0).get_name().unwrap(), "GREETING");
    assert_eq!(bindings.get(1).get_name().unwrap(), "N");
    let socket = config.get_sockets().unwrap().get(0);
    assert_eq!(socket.get_address().unwrap(), "localhost:8787");
    assert_eq!(socket.get_service().unwrap().get_name().unwrap(), "hello");
}

#[test]
fn durable_objects() {
    let message = read(
        load_project(&[
            (
                "wrangler.jsonc",
                r#"{
                  "name": "counter",
                  "main": "index.js",
                  "compatibility_date": "2026-01-01",
                  "no_bundle": true,
                  "durable_objects": {
                    "bindings": [{ "name": "COUNTER", "class_name": "Counter" }],
                  },
                  "migrations": [
                    { "tag": "v1", "new_sqlite_classes": ["Old"] },
                    { "tag": "v2", "renamed_classes": [{ "from": "Old", "to": "Counter" }] },
                  ],
                }"#,
            ),
            ("index.js", "export class Counter {}"),
        ])
        .unwrap(),
    );
    let config = message.get_root::<config::Reader>().unwrap();
    let services = config.get_services().unwrap();
    assert_eq!(services.len(), 2);
    let service::Which::Worker(worker) = services.get(0).which().unwrap() else {
        panic!("expected a worker service")
    };
    let worker = worker.unwrap();
    let binding = worker.get_bindings().unwrap().get(0);
    assert_eq!(binding.get_name().unwrap(), "COUNTER");
    let worker::binding::Which::DurableObjectNamespace(target) = binding.which().unwrap() else {
        panic!("expected a durable object binding")
    };
    assert_eq!(target.unwrap().get_class_name().unwrap(), "Counter");
    let ns = worker.get_durable_object_namespaces().unwrap().get(0);
    assert_eq!(ns.get_class_name().unwrap(), "Counter");
    // Renamed, the class keeps the storage it was created with.
    let worker::durable_object_namespace::Which::UniqueKey(key) = ns.which().unwrap() else {
        panic!("expected a unique key")
    };
    assert_eq!(key.unwrap(), "counter-Old");
    assert!(ns.get_enable_sql());
    assert_eq!(services.get(1).get_name().unwrap(), "counter-do-storage");
}

#[test]
fn a_deployment_holds_only_the_worker() {
    const COUNTER: &str = r#"{
      "name": "counter",
      "main": "index.js",
      "compatibility_date": "2026-01-01",
      "no_bundle": true,
      "durable_objects": { "bindings": [{ "name": "COUNTER", "class_name": "Counter" }] },
      "migrations": [{ "tag": "v1", "new_sqlite_classes": ["Counter"] }],
    }"#;
    let dir = project(&[
        ("wrangler.jsonc", COUNTER),
        ("index.js", "export class Counter {}"),
    ]);
    let path = dir.join("wrangler.jsonc");
    let process = Process::new(None, None, Vec::new());
    for (given, name) in [(None, "counter"), (Some("staging"), "staging")] {
        let (loaded, config) = load_deployment(path.to_str().unwrap(), given, &process).unwrap();
        assert_eq!(loaded, name);
        let message = read(config);
        let config = message.get_root::<config::Reader>().unwrap();
        assert!(!config.has_sockets());
        let services = config.get_services().unwrap();
        assert_eq!(services.len(), 1);
        // Named after the deployment, as its classes' storage keys are, so that the same project
        // deployed under two names composes.
        assert_eq!(services.get(0).get_name().unwrap(), name);
        let service::Which::Worker(worker) = services.get(0).which().unwrap() else {
            panic!("expected a worker service")
        };
        let worker = worker.unwrap();
        let ns = worker.get_durable_object_namespaces().unwrap().get(0);
        let worker::durable_object_namespace::Which::UniqueKey(key) = ns.which().unwrap() else {
            panic!("expected a unique key")
        };
        assert_eq!(key.unwrap(), format!("{name}-Counter"));
        let worker::durable_object_storage::Which::LocalDisk(disk) =
            worker.get_durable_object_storage().which().unwrap()
        else {
            panic!("expected local disk storage")
        };
        assert_eq!(disk.unwrap(), CLUSTER_STORAGE);
    }
    assert!(!dir.join(".wrangler/state").exists());

    let unnamed = project(&[
        (
            "wrangler.jsonc",
            r#"{ "main": "index.js", "compatibility_date": "2026-01-01", "no_bundle": true }"#,
        ),
        ("index.js", "export default {}"),
    ]);
    let path = unnamed.join("wrangler.jsonc");
    let Err(error) = load_deployment(path.to_str().unwrap(), None, &process) else {
        panic!("expected an error")
    };
    assert!(error.contains("--name"), "{error}");
    let (name, _) = load_deployment(path.to_str().unwrap(), Some("app"), &process).unwrap();
    assert_eq!(name, "app");
}

/// A project named `name` with a Counter Durable Object.
fn counter_project(name: &str) -> PathBuf {
    project(&[
        (
            "wrangler.jsonc",
            &format!(
                r#"{{ "name": "{name}", "main": "index.js", "compatibility_date": "2026-01-01",
                     "no_bundle": true,
                     "durable_objects": {{ "bindings": [{{ "name": "C", "class_name": "Counter" }}] }} }}"#
            ),
        ),
        ("index.js", "export class Counter {}"),
    ])
}

/// The options of `serve --cluster-addr`, with `--directory-path` overrides `directories`.
fn joining(directories: &[(&str, &str)]) -> RunOptions {
    let mut options = RunOptions {
        cluster_address: Some("127.0.0.1:0".to_owned()),
        ..RunOptions::default()
    };
    for (name, path) in directories {
        options
            .overrides
            .directories
            .insert((*name).to_owned(), (*path).to_owned());
    }
    options
}

/// The name and path of the disk service that the worker in `config` stores Durable Objects on.
fn storage_of(config: Config) -> (String, String) {
    let message = read(config);
    let config = message.get_root::<config::Reader>().unwrap();
    let services = config.get_services().unwrap();
    let service::Which::Worker(worker) = services.get(0).which().unwrap() else {
        panic!("expected a worker service")
    };
    let worker::durable_object_storage::Which::LocalDisk(disk) = worker
        .unwrap()
        .get_durable_object_storage()
        .which()
        .unwrap()
    else {
        panic!("expected local disk storage")
    };
    let storage = services.get(1);
    assert_eq!(storage.get_name().unwrap(), disk.unwrap());
    let service::Which::Disk(path) = storage.which().unwrap() else {
        panic!("expected a disk service")
    };
    (
        storage.get_name().unwrap().to_string().unwrap(),
        path.unwrap().get_path().unwrap().to_string().unwrap(),
    )
}

#[test]
fn durable_objects_in_a_cluster_are_stored_on_do_storage() {
    let dir = counter_project("counter");
    let path = dir.join("wrangler.jsonc");
    let process = Process::new(None, None, Vec::new());
    let config = load(path.to_str().unwrap(), &process, &joining(&[]), true).unwrap();
    // As the translation makes it: absolute, not canonical, so on Windows not `\\?\`.
    let state = std::path::absolute(&dir)
        .unwrap()
        .join(".wrangler/state/workerd");
    assert_eq!(
        storage_of(config),
        (
            CLUSTER_STORAGE.to_owned(),
            state.to_string_lossy().into_owned()
        )
    );
    assert!(state.is_dir());
}

#[test]
fn a_moved_storage_directory_is_not_created_in_the_project() {
    let process = Process::new(None, None, Vec::new());
    for (options, service) in [
        (joining(&[(CLUSTER_STORAGE, "/elsewhere")]), CLUSTER_STORAGE),
        (
            RunOptions {
                overrides: workerd_server::config::Overrides {
                    directories: HashMap::from([(
                        "counter-do-storage".to_owned(),
                        "/elsewhere".to_owned(),
                    )]),
                    ..Default::default()
                },
                ..RunOptions::default()
            },
            "counter-do-storage",
        ),
    ] {
        let dir = counter_project("counter");
        let path = dir.join("wrangler.jsonc");
        let config = load(path.to_str().unwrap(), &process, &options, true).unwrap();
        // The server applies the override to the service the config names.
        assert_eq!(storage_of(config).0, service);
        assert!(!dir.join(".wrangler").exists());
    }
}

#[test]
fn a_project_in_a_cluster_cannot_be_named_after_its_storage() {
    let path = counter_project(CLUSTER_STORAGE).join("wrangler.jsonc");
    let process = Process::new(None, None, Vec::new());
    let Err(error) = load(path.to_str().unwrap(), &process, &joining(&[]), true) else {
        panic!("expected an error")
    };
    assert_eq!(
        error,
        "name: \"do-storage\" is the name of the disk service that holds the Durable Objects in a \
         cluster. Choose another name."
    );
    // Outside a cluster, the disk is named after the worker.
    let config = load(
        path.to_str().unwrap(),
        &process,
        &RunOptions::default(),
        true,
    )
    .unwrap();
    assert_eq!(storage_of(config).0, "do-storage-do-storage");
}

/// A project named `name` without Durable Objects, with `files` besides its config and module.
fn plain_project(name: &str, files: &[(&str, &str)]) -> PathBuf {
    let config = format!(
        r#"{{ "name": "{name}", "main": "index.js", "compatibility_date": "2026-01-01",
             "no_bundle": true }}"#
    );
    let mut all = vec![
        ("wrangler.jsonc", config.as_str()),
        ("index.js", "export default {}"),
    ];
    all.extend_from_slice(files);
    project(&all)
}

/// The names and paths of the disk services in `config`.
fn disks_of(config: Config) -> Vec<(String, String)> {
    let message = read(config);
    let config = message.get_root::<config::Reader>().unwrap();
    let services = config.get_services().unwrap();
    services
        .iter()
        .filter_map(|service| match service.which().unwrap() {
            service::Which::Disk(disk) => Some((
                service.get_name().unwrap().to_string().unwrap(),
                disk.unwrap().get_path().unwrap().to_string().unwrap(),
            )),
            _ => None,
        })
        .collect()
}

#[test]
fn a_project_without_durable_objects_in_a_cluster_defines_do_storage() {
    let process = Process::new(None, None, Vec::new());
    let dir = plain_project("hello", &[]);
    let path = dir.join("wrangler.jsonc");
    let path = path.to_str().unwrap();
    // Outside a cluster, there is no disk.
    let config = load(path, &process, &RunOptions::default(), true).unwrap();
    assert_eq!(disks_of(config), []);
    assert!(!dir.join(".wrangler").exists());
    // In one, the disk holds deployments' Durable Objects, wherever --directory-path puts it.
    let config = load(
        path,
        &process,
        &joining(&[(CLUSTER_STORAGE, "/data")]),
        true,
    )
    .unwrap();
    assert_eq!(disks_of(config).len(), 1);
    assert!(!dir.join(".wrangler").exists());
    let config = load(path, &process, &joining(&[]), true).unwrap();
    // As the translation makes it: absolute, not canonical, so on Windows not `\\?\`.
    let state = std::path::absolute(&dir)
        .unwrap()
        .join(".wrangler/state/workerd");
    assert_eq!(
        disks_of(config),
        [(
            CLUSTER_STORAGE.to_owned(),
            state.to_string_lossy().into_owned()
        )]
    );
    assert!(state.is_dir());
    // So the worker cannot take its name.
    let path = plain_project(CLUSTER_STORAGE, &[]).join("wrangler.jsonc");
    let Err(error) = load(path.to_str().unwrap(), &process, &joining(&[]), true) else {
        panic!("expected an error")
    };
    assert!(
        error.starts_with("name: \"do-storage\" is the name"),
        "{error}"
    );
}

#[test]
fn a_storage_directory_that_cannot_be_created_fails_only_a_project_with_durable_objects() {
    let process = Process::new(None, None, Vec::new());
    // A file where the directory would be.
    let blocked = [(".wrangler", "")];
    let path = plain_project("hello", &blocked).join("wrangler.jsonc");
    let config = load(path.to_str().unwrap(), &process, &joining(&[]), true).unwrap();
    assert_eq!(disks_of(config), []);

    let dir = counter_project("counter");
    fs::write(dir.join(".wrangler"), "").unwrap();
    let path = dir.join("wrangler.jsonc");
    let Err(error) = load(path.to_str().unwrap(), &process, &joining(&[]), true) else {
        panic!("expected an error")
    };
    assert!(
        error.ends_with(
            ". Pass --directory-path do-storage=<dir> to store Durable Objects elsewhere."
        ),
        "{error}"
    );
}

#[test]
fn moved_kv_cron_queues_and_workflows_directories_are_not_created_in_the_project() {
    let dir = project(&[
        (
            "wrangler.jsonc",
            r#"{ "name": "app", "main": "index.js", "compatibility_date": "2026-01-01",
                 "no_bundle": true, "kv_namespaces": [{ "binding": "KV" }],
                 "triggers": { "crons": ["0 * * * *"] },
                 "queues": { "consumers": [{ "queue": "jobs" }] },
                 "workflows": [{ "binding": "R", "name": "reports", "class_name": "Report" }] }"#,
        ),
        ("index.js", "export default {}"),
    ]);
    let path = dir.join("wrangler.jsonc");
    let mut options = RunOptions::default();
    for service in [
        "app-kv-storage",
        "app-kv-blobs",
        "app-cron-storage",
        "app-queues-storage",
        "app-workflows-storage",
    ] {
        options
            .overrides
            .directories
            .insert(service.to_owned(), format!("/data/{service}"));
    }
    let process = Process::new(None, None, Vec::new());
    load(path.to_str().unwrap(), &process, &options, true).unwrap();
    assert!(!dir.join(".wrangler").exists());
}

#[test]
fn a_project_in_a_cluster_cannot_have_kv_namespaces_or_queues_but_can_have_crons() {
    let process = Process::new(None, None, Vec::new());
    for (field, json) in [
        ("kv_namespaces", r#""kv_namespaces": [{ "binding": "KV" }]"#),
        (
            "queues",
            r#""queues": { "producers": [{ "binding": "Q", "queue": "jobs" }] }"#,
        ),
    ] {
        let dir = project(&[
            (
                "wrangler.jsonc",
                format!(
                    r#"{{ "name": "app", "main": "index.js", "compatibility_date": "2026-01-01",
                         "no_bundle": true, {json} }}"#
                )
                .as_str(),
            ),
            ("index.js", "export default {}"),
        ]);
        let path = dir.join("wrangler.jsonc");
        let Err(error) = load(path.to_str().unwrap(), &process, &joining(&[]), true) else {
            panic!("expected an error")
        };
        assert_eq!(
            error,
            format!("A config that joins a cluster with --cluster-addr cannot have {field} yet.")
        );
    }
    let cron = project(&[
        (
            "wrangler.jsonc",
            r#"{ "name": "app", "main": "index.js", "compatibility_date": "2026-01-01",
                 "no_bundle": true, "triggers": { "crons": ["0 * * * *"] } }"#,
        ),
        ("index.js", "export default {}"),
    ]);
    let path = cron.join("wrangler.jsonc");
    let options = joining(&[
        (CLUSTER_STORAGE, "/data"),
        ("app-cron-storage", "/data/cron"),
    ]);
    let config = load(path.to_str().unwrap(), &process, &options, true).unwrap();
    let names: Vec<String> = disks_of(config).into_iter().map(|(name, _)| name).collect();
    assert_eq!(names, [CLUSTER_STORAGE, "app-cron-storage"]);
    assert!(!cron.join(".wrangler").exists());
}

#[test]
fn a_bundle_directory_that_cannot_be_created_suggests_no_bundle() {
    let error = load_project(&[
        (
            "wrangler.jsonc",
            r#"{ "main": "index.js", "compatibility_date": "2026-01-01" }"#,
        ),
        ("index.js", "export default {}"),
        (".wrangler", ""),
    ])
    .err()
    .unwrap();
    assert!(
        error.ends_with(
            ". esbuild bundles `main` there. Set \"no_bundle\": true in the wrangler config to \
             serve the modules as they are."
        ),
        "{error}"
    );
}

#[test]
fn migrations_rename_and_delete() {
    let w: Wrangler = serde_json::from_str(
        r#"{ "main": "index.js", "compatibility_date": "2026-01-01",
             "durable_objects": { "bindings": [{ "name": "B", "class_name": "Bound" }] },
             "migrations": [
               { "tag": "v1", "new_classes": ["A"], "new_sqlite_classes": ["S", "Gone"] },
               { "tag": "v2", "renamed_classes": [{ "from": "A", "to": "A2" }],
                 "deleted_classes": ["Gone"] }
             ] }"#,
    )
    .unwrap();
    assert_eq!(
        do_classes(&w).unwrap(),
        [
            ("A2".to_owned(), "A".to_owned(), false),
            ("S".to_owned(), "S".to_owned(), true),
            ("Bound".to_owned(), "Bound".to_owned(), false)
        ]
    );
}

#[test]
fn migrations_follow_wranglers_order_and_checks() {
    let classes = |migrations: &str| {
        let w: Wrangler = serde_json::from_str(&format!(
            r#"{{ "main": "index.js", "compatibility_date": "2026-01-01",
                  "migrations": {migrations} }}"#
        ))
        .unwrap();
        do_classes(&w)
    };
    // Deletes apply before new classes, so a class can be re-created in one migration.
    let recreated = r#"[{ "tag": "v1", "new_classes": ["A"] },
                        { "tag": "v2", "deleted_classes": ["A"], "new_sqlite_classes": ["A"] }]"#;
    assert_eq!(
        classes(recreated).unwrap(),
        [("A".to_owned(), "A~1".to_owned(), true)]
    );
    // A key is never reused, so a re-created or renamed-away name starts empty.
    let reused = r#"[{ "tag": "v1", "new_classes": ["A"] },
                     { "tag": "v2", "renamed_classes": [{ "from": "A", "to": "B" }] },
                     { "tag": "v3", "new_classes": ["A"] }]"#;
    assert_eq!(
        classes(reused).unwrap(),
        [
            ("B".to_owned(), "A".to_owned(), false),
            ("A".to_owned(), "A~1".to_owned(), false)
        ]
    );
    let error = classes(
        r#"[{ "tag": "v1", "new_classes": ["A"] },
            { "tag": "v2", "deleted_classes": ["A"],
              "renamed_classes": [{ "from": "A", "to": "B" }] }]"#,
    )
    .unwrap_err();
    assert!(error.contains("renamed class A does not exist"), "{error}");
    let error = classes(r#"[{ "tag": "v1", "deleted_classes": ["A"] }]"#).unwrap_err();
    assert!(error.contains("deleted class A does not exist"), "{error}");
    let error = classes(r#"[{ "tag": "v1", "new_classes": ["A"], "new_sqlite_classes": ["A"] }]"#)
        .unwrap_err();
    assert!(error.contains("new class A already exists"), "{error}");
    let error = classes(
        r#"[{ "tag": "v1", "new_classes": ["A", "B"] },
            { "tag": "v2", "renamed_classes": [{ "from": "A", "to": "B" }] }]"#,
    )
    .unwrap_err();
    assert!(
        error.contains("renamed-to class B already exists"),
        "{error}"
    );
}

#[test]
fn exports() {
    let classes = |exports: &str| {
        let w: Wrangler = serde_json::from_str(&format!(
            r#"{{ "main": "index.js", "compatibility_date": "2026-01-01",
                  "durable_objects": {{ "bindings": [{{ "name": "B", "class_name": "Bound" }}] }},
                  "exports": {exports} }}"#
        ))
        .unwrap();
        do_classes(&w)
    };
    let ok = r#"{ "Counter": { "type": "durable-object", "storage": "sqlite" },
                  "Kv": { "type": "durable-object", "storage": "legacy-kv" },
                  "Gone": { "type": "durable-object", "state": "deleted" },
                  "Old": { "type": "durable-object", "state": "renamed",
                           "renamed_to": "Counter" } }"#;
    assert_eq!(
        classes(ok).unwrap(),
        [
            ("Counter".to_owned(), "Old".to_owned(), true),
            ("Kv".to_owned(), "Kv".to_owned(), false),
            ("Bound".to_owned(), "Bound".to_owned(), false)
        ]
    );
    let error = classes(r#"{ "A": { "type": "durable-object" } }"#).unwrap_err();
    assert!(error.contains("A has the wrong fields"), "{error}");
    let error = classes(
        r#"{ "A": { "type": "durable-object", "state": "deleted", "storage": "sqlite" } }"#,
    )
    .unwrap_err();
    assert!(error.contains("A has the wrong fields"), "{error}");
    let error =
        classes(r#"{ "A": { "type": "durable-object", "state": "renamed", "renamed_to": "B" } }"#)
            .unwrap_err();
    assert!(error.contains("which is not live"), "{error}");
    let error = classes(
        r#"{ "A": { "type": "durable-object", "state": "renamed", "renamed_to": "C" },
             "B": { "type": "durable-object", "state": "renamed", "renamed_to": "C" },
             "C": { "type": "durable-object", "storage": "sqlite" } }"#,
    )
    .unwrap_err();
    assert!(
        error.contains("more than one class is renamed to C"),
        "{error}"
    );
    let both: Result<Wrangler, _> = serde_json::from_str(
        r#"{ "main": "index.js", "compatibility_date": "2026-01-01",
             "exports": { "A": { "type": "durable-object", "storage": "sqlite" } },
             "migrations": [{ "tag": "v1", "new_sqlite_classes": ["A"] }] }"#,
    );
    let error = do_classes(&both.unwrap()).unwrap_err();
    assert!(error.contains("cannot both"), "{error}");
}

#[test]
fn sample_parses() {
    let src = include_str!("../../../../samples/wrangler/wrangler.jsonc");
    let w: Wrangler = serde_json::from_str(&strip_jsonc(src)).unwrap();
    assert_eq!(
        do_classes(&w).unwrap(),
        [("Counter".to_owned(), "Counter".to_owned(), true)]
    );
}

/// The kubernetes sample's projects translate for the flags that `kubernetes.yaml` passes, which
/// name each project's disk service.
#[test]
fn kubernetes_samples_translate() {
    let process = Process::new(None, None, Vec::new());
    let app = project(&[
        (
            "wrangler.json",
            include_str!("../../../../samples/kubernetes/app/wrangler.json"),
        ),
        (
            "src/index.js",
            include_str!("../../../../samples/kubernetes/app/src/index.js"),
        ),
    ]);
    let bucket = project(&[
        (
            "wrangler.json",
            include_str!("../../../../samples/kubernetes/bucket/wrangler.json"),
        ),
        (
            "fake-s3.js",
            include_str!("../../../../samples/kubernetes/bucket/fake-s3.js"),
        ),
    ]);
    let standalone = RunOptions {
        overrides: workerd_server::config::Overrides {
            directories: HashMap::from([("fake-s3-do-storage".to_owned(), "/data".to_owned())]),
            ..Default::default()
        },
        ..RunOptions::default()
    };
    for (dir, options, worker, disk) in [
        (
            app,
            joining(&[(CLUSTER_STORAGE, "/data")]),
            "counter",
            CLUSTER_STORAGE,
        ),
        (bucket, standalone, "fake-s3", "fake-s3-do-storage"),
    ] {
        let path = dir.join("wrangler.json");
        let config = load(path.to_str().unwrap(), &process, &options, true).unwrap();
        let message = read(config);
        let names: Vec<String> = message
            .get_root::<config::Reader>()
            .unwrap()
            .get_services()
            .unwrap()
            .iter()
            .map(|service| service.get_name().unwrap().to_string().unwrap())
            .collect();
        assert_eq!(names, [worker, disk]);
        // Bundling would write to the project, which is read-only in the image.
        assert!(!dir.join(".wrangler").exists());
    }
}

#[test]
fn a_missing_config_names_the_one_next_to_it() {
    let dir = project(&[("wrangler.jsonc", "{}")]);
    let missing = dir.join("wrangler.json");
    let missing = missing.to_str().unwrap();
    assert_eq!(
        missing_config(missing),
        format!(
            "{missing}: No such file. Did you mean {}?",
            dir.join("wrangler.jsonc").display()
        )
    );
    let dir = project(&[("wrangler.toml", "")]);
    let missing = dir.join("wrangler.json");
    let missing = missing.to_str().unwrap();
    assert_eq!(
        missing_config(missing),
        format!(
            "{missing}: No such file. There is a {}, but wrangler.toml is not supported. Convert \
             it to wrangler.jsonc.",
            dir.join("wrangler.toml").display()
        )
    );
    let dir = project(&[("index.js", "")]);
    let missing = dir.join("wrangler.json");
    let missing = missing.to_str().unwrap();
    assert_eq!(missing_config(missing), format!("{missing}: No such file."));
}

#[test]
fn unsupported_field_is_an_error() {
    let error = load_project(&[
        (
            "wrangler.jsonc",
            r#"{ "main": "index.js", "compatibility_date": "2026-01-01",
                 "kv_namespaces": [{ "binding": "KV", "bogus": "y" }] }"#,
        ),
        ("index.js", ""),
    ])
    .err()
    .unwrap();
    assert!(error.contains("bogus"), "{error}");
}

#[test]
fn kv_namespaces_require_experimental() {
    let dir = project(&[
        (
            "wrangler.jsonc",
            r#"{ "main": "index.js", "compatibility_date": "2026-01-01",
                 "kv_namespaces": [{ "binding": "KV", "id": "x" }] }"#,
        ),
        ("index.js", ""),
    ]);
    let process = Process::new(None, None, Vec::new());
    let path = dir.join("wrangler.jsonc");
    let error = load(
        path.to_str().unwrap(),
        &process,
        &RunOptions::default(),
        false,
    )
    .err()
    .unwrap();
    assert!(error.contains("kv_namespaces"), "{error}");
    assert!(error.contains("--experimental"), "{error}");
    let error = load_deployment(path.to_str().unwrap(), Some("app"), &process)
        .err()
        .unwrap();
    assert!(error.contains("kv_namespaces"), "{error}");
}

#[test]
fn kv_namespaces_bind_the_kv_worker() {
    let dir = project(&[
        (
            "wrangler.jsonc",
            r#"{ "name": "app", "main": "index.js", "compatibility_date": "2026-01-01",
                 "no_bundle": true,
                 "kv_namespaces": [{ "binding": "A", "id": "a", "preview_id": "p" },
                                   { "binding": "B", "remote": true }] }"#,
        ),
        ("index.js", ""),
    ]);
    let config = load(
        dir.join("wrangler.jsonc").to_str().unwrap(),
        &Process::new(None, None, Vec::new()),
        &RunOptions::default(),
        true,
    )
    .unwrap();
    let message = read(config);
    let conf = message.get_root::<config::Reader>().unwrap();
    let services = conf.get_services().unwrap();
    let names: Vec<_> = services
        .iter()
        .map(|s| s.get_name().unwrap().to_str().unwrap().to_owned())
        .collect();
    assert_eq!(names, ["app", "app-kv", "app-kv-storage", "app-kv-blobs"]);
    let service::Which::Disk(disk) = services.get(3).which().unwrap() else {
        panic!("not a disk");
    };
    let disk = disk.unwrap();
    assert!(disk.get_writable());
    let blobs = dir.join(".wrangler/state/workerd/kv/blobs");
    assert!(blobs.is_dir());
    assert_eq!(
        fs::canonicalize(disk.get_path().unwrap().to_str().unwrap()).unwrap(),
        fs::canonicalize(blobs).unwrap()
    );
    let service::Which::Worker(w) = services.get(0).which().unwrap() else {
        panic!("not a worker");
    };
    let bound: Vec<_> = w
        .unwrap()
        .get_bindings()
        .unwrap()
        .iter()
        .map(|b| {
            let worker::binding::Which::KvNamespace(d) = b.which().unwrap() else {
                panic!("not a kvNamespace");
            };
            let d = d.unwrap();
            let service_designator::props::Which::Json(props) = d.get_props().which().unwrap()
            else {
                panic!("no JSON props");
            };
            let text = |t: capnp::text::Reader<'_>| t.to_str().unwrap().to_owned();
            (
                text(b.get_name().unwrap()),
                text(d.get_name().unwrap()),
                text(props.unwrap()),
            )
        })
        .collect();
    let kv = |b: &str, id: &str| {
        (
            b.into(),
            "app-kv".into(),
            format!(r#"{{"namespace":"{id}"}}"#),
        )
    };
    assert_eq!(bound, [kv("A", "a"), kv("B", "B")]);
}

#[test]
fn triggers_crons_require_experimental() {
    let dir = project(&[
        (
            "wrangler.jsonc",
            r#"{ "main": "index.js", "compatibility_date": "2026-01-01",
                 "triggers": { "crons": ["* * * * *"] } }"#,
        ),
        ("index.js", ""),
    ]);
    let process = Process::new(None, None, Vec::new());
    let path = dir.join("wrangler.jsonc");
    let error = load(
        path.to_str().unwrap(),
        &process,
        &RunOptions::default(),
        false,
    )
    .err()
    .unwrap();
    assert!(error.contains("triggers.crons"), "{error}");
    assert!(error.contains("--experimental"), "{error}");
    let error = load_deployment(path.to_str().unwrap(), Some("app"), &process)
        .err()
        .unwrap();
    assert!(error.contains("triggers.crons"), "{error}");
}

#[test]
fn triggers_crons_refuse_a_worker_named_like_their_bindings() {
    let dir = project(&[
        (
            "wrangler.jsonc",
            r#"{ "name": "CRONS", "main": "index.js", "compatibility_date": "2026-01-01",
                 "no_bundle": true, "triggers": { "crons": ["* * * * *"] } }"#,
        ),
        ("index.js", ""),
    ]);
    let process = Process::new(None, None, Vec::new());
    let path = dir.join("wrangler.jsonc");
    let error = load(
        path.to_str().unwrap(),
        &process,
        &RunOptions::default(),
        true,
    )
    .err()
    .unwrap();
    assert!(error.contains("named CRONS"), "{error}");
}

#[test]
fn triggers_crons_run_on_the_cron_worker() {
    let dir = project(&[
        (
            "wrangler.jsonc",
            r#"{ "name": "app", "main": "index.js", "compatibility_date": "2026-01-01",
                 "no_bundle": true, "triggers": { "crons": ["0 * * * *", "0 12 * * MON"] } }"#,
        ),
        ("index.js", ""),
    ]);
    let config = load(
        dir.join("wrangler.jsonc").to_str().unwrap(),
        &Process::new(None, None, Vec::new()),
        &RunOptions::default(),
        true,
    )
    .unwrap();
    let message = read(config);
    let services = message
        .get_root::<config::Reader>()
        .unwrap()
        .get_services()
        .unwrap();
    let names: Vec<_> = services
        .iter()
        .map(|s| s.get_name().unwrap().to_str().unwrap().to_owned())
        .collect();
    assert_eq!(names, ["app", "app-cron", "app-cron-storage"]);
    assert!(dir.join(".wrangler/state/workerd/cron").is_dir());
    let service::Which::Worker(w) = services.get(1).which().unwrap() else {
        panic!("not a worker");
    };
    let bindings = w.unwrap().get_bindings().unwrap();
    let worker::binding::Which::Json(crons) = bindings.get(1).which().unwrap() else {
        panic!("CRONS is not JSON");
    };
    assert_eq!(
        crons.unwrap().to_str().unwrap(),
        r#"{"app":["0 * * * *","0 12 * * MON"]}"#
    );
    assert_eq!(bindings.get(2).get_name().unwrap().to_str().unwrap(), "app");
}

#[test]
fn queues_run_on_the_queues_worker() {
    let dir = project(&[
        (
            "wrangler.jsonc",
            r#"{ "name": "app", "main": "index.js", "compatibility_date": "2026-01-01",
                 "no_bundle": true, "queues": {
                   "producers": [{ "binding": "JOBS", "queue": "jobs" }],
                   "consumers": [{ "queue": "jobs", "max_batch_size": 4,
                                   "dead_letter_queue": "dead" }] } }"#,
        ),
        ("index.js", ""),
    ]);
    let process = Process::new(None, None, Vec::new());
    let options = RunOptions::default();
    let path = dir.join("wrangler.jsonc");
    let error = load(path.to_str().unwrap(), &process, &options, false)
        .err()
        .unwrap();
    assert!(error.contains("queues"), "{error}");
    let message = read(load(path.to_str().unwrap(), &process, &options, true).unwrap());
    let services = message
        .get_root::<config::Reader>()
        .unwrap()
        .get_services()
        .unwrap();
    let names: Vec<_> = services
        .iter()
        .map(|s| s.get_name().unwrap().to_str().unwrap().to_owned())
        .collect();
    assert_eq!(names, ["app", "app-queues", "app-queues-storage"]);
    let bindings_of = |i| {
        let service::Which::Worker(w) = services.get(i).which().unwrap() else {
            panic!("not a worker");
        };
        w.unwrap().get_bindings().unwrap()
    };
    let worker::binding::Which::Queue(queue) = bindings_of(0).get(0).which().unwrap() else {
        panic!("JOBS is not a queue");
    };
    let queue = queue.unwrap();
    assert_eq!(queue.get_name().unwrap().to_str().unwrap(), "app-queues");
    let worker::binding::Which::Json(consumers) = bindings_of(1).get(1).which().unwrap() else {
        panic!("CONSUMERS is not JSON");
    };
    assert_eq!(
        consumers.unwrap().to_str().unwrap(),
        r#"{"jobs":{"consumer":"worker","deadLetterQueue":"dead","maxBatchSize":4}}"#
    );
    let worker = bindings_of(1).get(2);
    assert_eq!(worker.get_name().unwrap().to_str().unwrap(), "worker");
}

#[test]
fn a_queue_with_two_consumers_is_an_error() {
    let error = load_project(&[
        (
            "wrangler.jsonc",
            r#"{ "main": "index.js", "compatibility_date": "2026-01-01", "no_bundle": true,
                 "queues": { "consumers": [{ "queue": "jobs" }, { "queue": "jobs" }] } }"#,
        ),
        ("index.js", ""),
    ])
    .err()
    .unwrap();
    assert!(
        error.contains(r#"queue "jobs" has more than one consumer"#),
        "{error}"
    );
}

#[test]
fn workflows_run_on_the_workflows_worker() {
    let dir = project(&[
        (
            "wrangler.jsonc",
            r#"{ "name": "app", "main": "index.js", "compatibility_date": "2026-01-01",
                 "no_bundle": true, "workflows": [
                   { "binding": "REPORTS", "name": "reports", "class_name": "Report" }] }"#,
        ),
        ("index.js", ""),
    ]);
    let process = Process::new(None, None, Vec::new());
    let options = RunOptions::default();
    let path = dir.join("wrangler.jsonc");
    let error = load(path.to_str().unwrap(), &process, &options, false)
        .err()
        .unwrap();
    assert!(error.contains("workflows"), "{error}");
    let message = read(load(path.to_str().unwrap(), &process, &options, true).unwrap());
    let services = message
        .get_root::<config::Reader>()
        .unwrap()
        .get_services()
        .unwrap();
    let names: Vec<_> = services
        .iter()
        .map(|s| s.get_name().unwrap().to_str().unwrap().to_owned())
        .collect();
    assert_eq!(names, ["app", "app-workflows", "app-workflows-storage"]);
    let worker = |i| {
        let service::Which::Worker(w) = services.get(i).which().unwrap() else {
            panic!("not a worker");
        };
        w.unwrap()
    };
    let app = worker(0);
    let worker::binding::Which::Wrapped(wrapped) =
        app.get_bindings().unwrap().get(0).which().unwrap()
    else {
        panic!("REPORTS is not wrapped");
    };
    let wrapped = wrapped.unwrap();
    let module = wrapped.get_module_name().unwrap().to_str().unwrap();
    assert_eq!(module, "cloudflare-internal:workflows-api");
    let engine = app.get_workflows_engine().unwrap();
    let class = engine.get_actor_class().unwrap();
    assert_eq!(class.get_name().unwrap().to_str().unwrap(), "app-workflows");
    assert_eq!(class.get_entrypoint().unwrap().to_str().unwrap(), "Engine");
    let flow = engine.get_workflows().unwrap().get(0);
    assert_eq!(flow.get_class_name().unwrap().to_str().unwrap(), "Report");
    let binding = flow.get_binding_service().unwrap();
    assert_eq!(
        binding.get_entrypoint().unwrap().to_str().unwrap(),
        "WorkflowBinding"
    );
    let instances = worker(1).get_bindings().unwrap().get(0);
    assert_eq!(
        instances.get_name().unwrap().to_str().unwrap(),
        "workflow:reports"
    );
    let worker::binding::Which::DurableObjectNamespace(ns) = instances.which().unwrap() else {
        panic!("reports is not a namespace");
    };
    let ns = ns.unwrap();
    let class = ns.get_class_name().unwrap().to_str().unwrap();
    assert_eq!(class, "miniflare-workflows-reports");
    assert_eq!(ns.get_service_name().unwrap().to_str().unwrap(), "app");
}

#[test]
fn workflow_settings_reach_the_engine() {
    let project = |extra: &str| {
        load_project(&[
            (
                "wrangler.jsonc",
                &format!(
                    r#"{{ "name": "app", "main": "index.js", "compatibility_date": "2026-01-01",
                         "no_bundle": true, "workflows": [{{ "binding": "R", "name": "reports",
                         "class_name": "Report", {extra} }}] }}"#
                ),
            ),
            ("index.js", ""),
        ])
    };
    let config = project(
        r#""script_name": "app", "limits": { "steps": 50 }, "concurrency": { "limit": 2 },
           "default_retention": { "success_retention": "1 day" }, "jurisdiction": "eu""#,
    );
    let message = read(config.unwrap());
    let services = message
        .get_root::<config::Reader>()
        .unwrap()
        .get_services()
        .unwrap();
    let service::Which::Worker(w) = services.get(1).which().unwrap() else {
        panic!("not a worker");
    };
    let binding = w.unwrap().get_bindings().unwrap().get(1);
    assert_eq!(binding.get_name().unwrap().to_str().unwrap(), "WORKFLOWS");
    let worker::binding::Which::Json(json) = binding.which().unwrap() else {
        panic!("WORKFLOWS is not JSON");
    };
    let json: serde_json::Value = serde_json::from_str(json.unwrap().to_str().unwrap()).unwrap();
    let want = serde_json::json!({
        "reports": { "steps": 50, "retention": { "success": "1 day", "error": null } }
    });
    assert_eq!(json, want);
    for (extra, field) in [
        (r#""schedules": "0 * * * *""#, "schedules"),
        (r#""script_name": "other""#, "script_name"),
    ] {
        let error = project(extra).err().unwrap();
        assert!(
            error.contains(&format!("{field} is not supported")),
            "{error}"
        );
    }
    for name in ["-r", "a.b", &"r".repeat(65)] {
        let wrangler = format!(
            r#"{{ "main": "index.js", "compatibility_date": "2026-01-01", "no_bundle": true,
                 "workflows": [{{ "binding": "R", "name": "{name}", "class_name": "R" }}] }}"#
        );
        let error = load_project(&[("wrangler.jsonc", &wrangler), ("index.js", "")]);
        assert!(error.err().unwrap().contains("Workflow names must be"));
    }
}

#[test]
fn unbundled_typescript_main_is_an_error() {
    let error = load_project(&[
        (
            "wrangler.jsonc",
            r#"{ "main": "src/index.ts", "compatibility_date": "2026-01-01", "no_bundle": true }"#,
        ),
        ("src/index.ts", ""),
    ])
    .err()
    .unwrap();
    assert!(error.contains("no_bundle"), "{error}");
}

/// Held by every test that spawns a process. Writing the stub esbuild while another thread
/// forks would leave the fork holding it open for writing, and running it would fail (ETXTBSY).
static SPAWN: std::sync::Mutex<()> = std::sync::Mutex::new(());

#[test]
fn missing_esbuild_is_explained() {
    let _spawn = SPAWN
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner);
    let dir = project(&[("index.ts", "export default {}")]);
    let error = bundle(Path::new("/nonexistent/esbuild"), &dir, "index.ts", &|_| {
        Ok(())
    })
    .err()
    .unwrap();
    assert!(error.contains("esbuild not found"), "{error}");
    assert!(error.contains("no_bundle"), "{error}");
}

/// A stand-in for esbuild: a shell script running `body` with `$out` and `$meta` set from
/// `--outdir=` and `--metafile=`, and its arguments in `args.txt`.
#[cfg(unix)]
fn stub_esbuild(dir: &Path, body: &str) -> PathBuf {
    use std::os::unix::fs::PermissionsExt;

    let path = dir.join("esbuild.sh");
    let script = format!(
        "#!/bin/sh\nprintf '%s\\n' \"$@\" > {args}\nfor a; do case $a in \
         --outdir=*) out=${{a#*=}};; --metafile=*) meta=${{a#*=}};; esac; done\n{body}\n",
        args = dir.join("args.txt").display()
    );
    fs::write(&path, script).unwrap();
    fs::set_permissions(&path, fs::Permissions::from_mode(0o755)).unwrap();
    path
}

#[cfg(unix)]
#[test]
fn bundle_reads_esbuild_output_and_watches_inputs() {
    let _spawn = SPAWN
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner);
    let dir = project(&[
        ("index.ts", ""),
        ("lib.ts", ""),
        ("node_modules/x/index.js", ""),
    ]);
    let esbuild = stub_esbuild(
        &dir,
        r#"echo 'export default {}' > "$out/index.js"; echo wasm > "$out/a.wasm"
echo '{"inputs":{"index.ts":{},"lib.ts":{},"node_modules/x/index.js":{}}}' > "$meta""#,
    );
    let watched = std::cell::RefCell::new(Vec::new());
    let sources = bundle(&esbuild, &dir, "index.ts", &|p| {
        watched.borrow_mut().push(p.to_owned());
        Ok(())
    })
    .unwrap();
    let names: Vec<_> = sources.iter().map(|(n, _, _)| n.as_str()).collect();
    assert_eq!(names, ["index.js", "a.wasm"]);
    assert!(watched.borrow().contains(&dir.join("index.ts")));
    // Only the metafile names `lib.ts`.
    assert!(watched.borrow().contains(&dir.join("lib.ts")));
    assert!(
        !watched
            .borrow()
            .iter()
            .any(|p| p.starts_with(dir.join("node_modules")))
    );
    let args = fs::read_to_string(dir.join("args.txt")).unwrap();
    for arg in [
        "index.ts",
        "--bundle",
        "--external:node:*",
        "--external:fs/*",
    ] {
        assert!(args.lines().any(|l| l == arg), "{arg} not in {args}");
    }
    assert!(args.contains("--banner:js=var require"), "{args}");
    assert!(!dir.join(".wrangler/tmp").read_dir().unwrap().any(|_| true));
}

#[cfg(unix)]
#[test]
fn failed_bundle_watches_sources_and_missing_main() {
    let _spawn = SPAWN
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner);
    let dir = project(&[("lib.ts", ""), ("README.md", "")]);
    let esbuild = stub_esbuild(&dir, "echo 'syntax error' >&2; exit 1");
    let watched = std::cell::RefCell::new(Vec::new());
    let error = bundle(&esbuild, &dir, "index.ts", &|p| {
        watched.borrow_mut().push(p.to_owned());
        Ok(())
    })
    .err()
    .unwrap();
    assert!(error.contains("syntax error"), "{error}");
    watched.borrow_mut().sort();
    assert_eq!(
        *watched.borrow(),
        [dir.join("index.ts"), dir.join("lib.ts")]
    );
}
