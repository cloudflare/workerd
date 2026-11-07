// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

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
        assert_eq!(disk.unwrap(), DEPLOYMENT_STORAGE);
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
    let error = load(path.to_str().unwrap(), &process, false).err().unwrap();
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
