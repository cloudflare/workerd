use workerd_capnp::service;
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
    assert_eq!(socket.get_address().unwrap(), "*:8787");
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
                  "migrations": [{ "tag": "v1", "new_sqlite_classes": ["Counter"] }],
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
    let ns = worker.get_durable_object_namespaces().unwrap().get(0);
    assert_eq!(ns.get_class_name().unwrap(), "Counter");
    assert!(ns.get_enable_sql());
    assert_eq!(services.get(1).get_name().unwrap(), STORAGE_SERVICE);
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
fn unsupported_field_is_an_error() {
    let error = load_project(&[
        (
            "wrangler.jsonc",
            r#"{ "main": "index.js", "compatibility_date": "2026-01-01",
                 "kv_namespaces": [{ "binding": "KV", "id": "x" }] }"#,
        ),
        ("index.js", ""),
    ])
    .err()
    .unwrap();
    assert!(error.contains("kv_namespaces"), "{error}");
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

#[test]
fn missing_esbuild_is_explained() {
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
    let dir = project(&[("index.ts", ""), ("node_modules/x/index.js", "")]);
    let esbuild = stub_esbuild(
        &dir,
        r#"echo 'export default {}' > "$out/index.js"; echo wasm > "$out/a.wasm"
echo '{"inputs":{"index.ts":{},"node_modules/x/index.js":{}}}' > "$meta""#,
    );
    let watched = std::cell::RefCell::new(Vec::new());
    let sources = bundle(&esbuild, &dir, "index.ts", &|p| {
        watched.borrow_mut().push(p.to_owned());
        Ok(())
    })
    .unwrap();
    let names: Vec<_> = sources.iter().map(|(n, _, _)| n.as_str()).collect();
    assert_eq!(names, ["index.js", "a.wasm"]);
    assert_eq!(*watched.borrow(), [dir.join("index.ts")]);
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
fn failed_bundle_watches_sources() {
    let dir = project(&[("index.ts", ""), ("README.md", "")]);
    let esbuild = stub_esbuild(&dir, "echo 'syntax error' >&2; exit 1");
    let watched = std::cell::RefCell::new(Vec::new());
    let error = bundle(&esbuild, &dir, "index.ts", &|p| {
        watched.borrow_mut().push(p.to_owned());
        Ok(())
    })
    .err()
    .unwrap();
    assert!(error.contains("syntax error"), "{error}");
    assert_eq!(*watched.borrow(), [dir.join("index.ts")]);
}
