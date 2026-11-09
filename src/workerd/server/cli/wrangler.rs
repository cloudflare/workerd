// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! A `wrangler.json` / `wrangler.jsonc` config, translated into a workerd `Config`.
//!
//! As wrangler does, `main` is bundled with esbuild (`WORKERD_ESBUILD`, else the project's
//! `node_modules/.bin/esbuild`, else `esbuild` on `PATH`), with the same platform, conditions and
//! externals; every input esbuild read outside `node_modules` is watched. Under `no_bundle: true`,
//! `main` must be an ES module and the modules it imports are found by scanning its directory with
//! wrangler's default module rules. Supported fields: `name`, `main`, `compatibility_date`,
//! `compatibility_flags`, `vars`, `durable_objects`, `exports`, `migrations`, `kv_namespaces`,
//! `no_bundle` and `$schema`. Any other field is an error rather than silently ignored.
//!
//! Durable Objects are stored on disk under `.wrangler/state/workerd/` next to the config, one
//! directory per class, named `<name>-<key>` by its storage key (see [`do_classes`]).
//! `kv_namespaces` need `--experimental`. Each is a binding to the Worker `<name>-kv`
//! (the bundled module `/workerd/kv.js`), with the namespace's `id` (else its binding name) in
//! its props; `preview_id` and `remote` are ignored. Its objects are in
//! `.wrangler/state/workerd/kv/`, and its large values in `kv/blobs/` there. The server listens
//! on `localhost:8787`, as `wrangler dev` does; `--socket-addr http=<addr>` overrides it.
//!
//! For `workerd deploy`, [`load_deployment`] translates the same fields into a cluster deployment
//! (`cluster::compose`), which holds only services: the worker, named after the deployment, with
//! no socket, and its Durable Objects stored on the disk service [`DEPLOYMENT_STORAGE`], which
//! each node's own config defines. A deployment cannot have `kv_namespaces` yet.

use std::collections::BTreeMap;
use std::fs;
use std::path::Path;
use std::path::PathBuf;

use serde::Deserialize;
use serde_json::Value;
use workerd_capnp::config;
use workerd_capnp::service;
use workerd_capnp::worker;

use crate::config::Config;
use crate::config::words_from_bytes;
use crate::process::Process;

const DEFAULT_ADDRESS: &str = "localhost:8787";

/// The disk service a deployment's Durable Objects are stored on: a node's, since a deployment
/// may define no disk. Each class's objects are in a directory of their own on it, named by the
/// class's storage key, which names the deployment.
pub const DEPLOYMENT_STORAGE: &str = "do-storage";

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Wrangler {
    #[serde(rename = "$schema")]
    _schema: Option<String>,
    name: Option<String>,
    main: String,
    compatibility_date: String,
    #[serde(default)]
    compatibility_flags: Vec<String>,
    #[serde(default)]
    vars: BTreeMap<String, Value>,
    durable_objects: Option<DurableObjects>,
    #[serde(default)]
    exports: BTreeMap<String, Export>,
    #[serde(default)]
    migrations: Vec<Migration>,
    #[serde(default)]
    kv_namespaces: Vec<KvNamespace>,
    #[serde(default)]
    no_bundle: bool,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct KvNamespace {
    binding: String,
    id: Option<String>,
    #[serde(rename = "preview_id")]
    _preview_id: Option<String>,
    #[serde(rename = "remote")]
    _remote: Option<bool>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct DurableObjects {
    bindings: Vec<DurableObjectBinding>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct DurableObjectBinding {
    name: String,
    class_name: String,
}

/// An `exports` entry: a Durable Object class and its lifecycle state.
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Export {
    #[serde(rename = "type")]
    _type: ExportType,
    #[serde(default)]
    state: State,
    storage: Option<Storage>,
    renamed_to: Option<String>,
}

#[derive(Deserialize)]
#[serde(rename_all = "kebab-case")]
enum ExportType {
    DurableObject,
}

#[derive(Deserialize, Default, PartialEq)]
#[serde(rename_all = "kebab-case")]
enum State {
    #[default]
    Created,
    Deleted,
    Renamed,
}

#[derive(Deserialize, PartialEq)]
#[serde(rename_all = "kebab-case")]
enum Storage {
    Sqlite,
    LegacyKv,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Migration {
    #[serde(rename = "tag")]
    _tag: Option<String>,
    #[serde(default)]
    new_classes: Vec<String>,
    #[serde(default)]
    new_sqlite_classes: Vec<String>,
    #[serde(default)]
    renamed_classes: Vec<Rename>,
    #[serde(default)]
    deleted_classes: Vec<String>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Rename {
    from: String,
    to: String,
}

enum Kind {
    EsModule,
    CommonJs,
    Wasm,
    Text,
    Data,
}

/// wrangler's default module rules, by extension.
fn kind_of(path: &Path) -> Option<Kind> {
    match path.extension()?.to_str()? {
        "js" | "mjs" => Some(Kind::EsModule),
        "cjs" => Some(Kind::CommonJs),
        "wasm" => Some(Kind::Wasm),
        "txt" | "html" | "sql" => Some(Kind::Text),
        "bin" => Some(Kind::Data),
        _ => None,
    }
}

pub fn is_wrangler_config(path: &str) -> bool {
    let ext = Path::new(path).extension().unwrap_or_default();
    ext.eq_ignore_ascii_case("json") || ext.eq_ignore_ascii_case("jsonc")
}

/// The wrangler config of the project in `dir`, in the order wrangler looks for one: its
/// `wrangler.json`, else its `wrangler.jsonc`; a `wrangler.toml` is not supported.
pub fn project_config(dir: &Path) -> Result<PathBuf, String> {
    for candidate in ["wrangler.json", "wrangler.jsonc"] {
        let path = dir.join(candidate);
        if path.is_file() {
            return Ok(path);
        }
    }
    if dir.join("wrangler.toml").exists() {
        return Err(format!(
            "{}: wrangler.toml is not supported. Convert it to wrangler.jsonc.",
            dir.display()
        ));
    }
    Err(format!(
        "{} does not contain wrangler.json or wrangler.jsonc.",
        dir.display()
    ))
}

/// What a wrangler config is translated into.
enum Target<'a> {
    /// A config to serve: the worker named after the config's `name`, else `main`, a socket, and
    /// a disk service for its Durable Objects under `.wrangler/state/workerd`.
    Serve { experimental: bool },
    /// A cluster deployment, services only: the worker named `name`, else after the config's
    /// `name`, and its Durable Objects stored on the node's [`DEPLOYMENT_STORAGE`].
    Deployment { name: Option<&'a str> },
}

/// The config `workerd serve` runs for the wrangler config at `path`.
pub fn load(path: &str, process: &Process, experimental: bool) -> Result<Config, String> {
    translate(path, process, &Target::Serve { experimental }).map(|(_, config)| config)
}

/// The deployment `workerd deploy` publishes for the wrangler config at `path`, and its name:
/// `name`, else the config's `name`.
pub fn load_deployment(
    path: &str,
    name: Option<&str>,
    process: &Process,
) -> Result<(String, Config), String> {
    translate(path, process, &Target::Deployment { name })
}

fn translate(path: &str, process: &Process, target: &Target) -> Result<(String, Config), String> {
    let watch = |p: &Path| {
        process
            .watch_file(p.as_os_str().as_encoded_bytes())
            .map_err(|e| e.description().to_owned())
    };
    // Not `canonicalize`, whose `\\?\` paths on Windows kj cannot parse.
    let file = std::path::absolute(path).map_err(|e| e.to_string())?;
    watch(&file)?;
    let source = fs::read_to_string(&file).map_err(|e| e.to_string())?;
    let wrangler: Wrangler =
        serde_json::from_str(&strip_jsonc(&source)).map_err(|e| e.to_string())?;
    check_kv(&wrangler, target)?;
    let root = file.parent().unwrap_or(&file);

    let name = match target {
        Target::Serve { .. } => wrangler.name.clone().unwrap_or_else(|| "main".to_owned()),
        Target::Deployment { name } => name
            .map(str::to_owned)
            .or_else(|| wrangler.name.clone())
            .ok_or(
                "This deployment needs a name. Set \"name\" in the wrangler config or pass --name.",
            )?,
    };
    let classes = do_classes(&wrangler)?;

    let sources = if wrangler.no_bundle {
        unbundled(root, &wrangler.main, &watch)?
    } else {
        bundle(&esbuild(root), root, &wrangler.main, &watch)?
    };

    let serving = matches!(target, Target::Serve { .. });
    let storage = root.join(".wrangler/state/workerd");
    if serving && !classes.is_empty() {
        fs::create_dir_all(&storage).map_err(|e| format!("{}: {e}", storage.display()))?;
    }
    let kv = !wrangler.kv_namespaces.is_empty();

    let mut message = capnp::message::Builder::new_default();
    let mut conf = message.init_root::<config::Builder>();
    let do_storage = u32::from(serving && !classes.is_empty());
    let mut services = conf
        .reborrow()
        .init_services(1 + do_storage + 3 * u32::from(kv));
    // Named after the worker, so they cannot collide with it.
    let kv_service = format!("{name}-kv");
    if kv {
        kv_services(
            &mut services,
            1 + do_storage,
            &kv_service,
            &storage.join("kv"),
        )?;
    }

    let mut service = services.reborrow().get(0);
    service.set_name(&name);
    let mut worker = service.init_worker();
    worker.set_compatibility_date(&wrangler.compatibility_date);
    worker
        .set_compatibility_flags(&wrangler.compatibility_flags[..])
        .map_err(|e| e.to_string())?;

    set_modules(worker.reborrow(), &sources)?;
    set_bindings(worker.reborrow(), &wrangler, &kv_service);

    if !classes.is_empty() {
        let mut ns = worker
            .reborrow()
            .init_durable_object_namespaces(classes.len() as u32);
        for (i, (class, key, sql)) in classes.iter().enumerate() {
            let mut n = ns.reborrow().get(i as u32);
            n.set_class_name(class);
            n.set_unique_key(format!("{name}-{key}"));
            n.set_enable_sql(*sql);
        }
        if serving {
            // Named after the worker, so it cannot collide with it.
            let storage_service = format!("{name}-do-storage");
            worker
                .init_durable_object_storage()
                .set_local_disk(&storage_service);
            let mut disk_service = services.get(1);
            disk_service.set_name(&storage_service);
            let mut disk = disk_service.init_disk();
            disk.set_path(storage.to_string_lossy());
            disk.set_writable(true);
        } else {
            worker
                .init_durable_object_storage()
                .set_local_disk(DEPLOYMENT_STORAGE);
        }
    }

    if serving {
        let mut socket = conf.init_sockets(1).get(0);
        socket.set_name("http");
        socket.set_address(DEFAULT_ADDRESS);
        socket.reborrow().init_http();
        socket.init_service().set_name(&name);
    }

    let config = Config::from_words(&words_from_bytes(
        &capnp::serialize::write_message_to_words(&message),
    ))
    .map_err(|e| e.to_string())?;
    Ok((name, config))
}

fn set_modules(worker: worker::Builder<'_>, sources: &[Source]) -> Result<(), String> {
    let mut ms = worker.init_modules(sources.len() as u32);
    for (i, (module_name, kind, bytes)) in sources.iter().enumerate() {
        let mut m = ms.reborrow().get(i as u32);
        m.set_name(module_name);
        let text = || std::str::from_utf8(bytes).map_err(|_| format!("{module_name}: not UTF-8"));
        match kind {
            Kind::EsModule => m.set_es_module(text()?),
            Kind::CommonJs => m.set_common_js_module(text()?),
            Kind::Text => m.set_text(text()?),
            Kind::Wasm => m.set_wasm(bytes),
            Kind::Data => m.set_data(bytes),
        }
    }
    Ok(())
}

/// `kv_namespaces` need `--experimental`, and a deployment cannot have them yet.
fn check_kv(wrangler: &Wrangler, target: &Target) -> Result<(), String> {
    match target {
        _ if wrangler.kv_namespaces.is_empty() => Ok(()),
        Target::Serve { experimental: true } => Ok(()),
        Target::Serve { .. } => Err("kv_namespaces is an experimental feature which may change \
                                     or go away in the future. You must run workerd with \
                                     `--experimental` to use it."
            .to_owned()),
        Target::Deployment { .. } => Err("A deployment cannot have kv_namespaces yet.".to_owned()),
    }
}

/// The KV Worker `name` at `services[at]`, then its disks under `dir`.
fn kv_services(
    services: &mut capnp::struct_list::Builder<'_, service::Owned>,
    at: u32,
    name: &str,
    dir: &Path,
) -> Result<(), String> {
    let blobs = dir.join("blobs");
    fs::create_dir_all(&blobs).map_err(|e| format!("{}: {e}", blobs.display()))?;
    for (i, what, path) in [(at + 1, "storage", dir), (at + 2, "blobs", &blobs)] {
        let mut disk_service = services.reborrow().get(i);
        disk_service.set_name(format!("{name}-{what}"));
        let mut disk = disk_service.init_disk();
        disk.set_path(path.to_string_lossy());
        disk.set_writable(true);
    }
    kv_worker(services.reborrow().get(at), name);
    Ok(())
}

/// The KV Worker `name`, over the disks `<name>-storage` and `<name>-blobs`.
fn kv_worker(mut service: service::Builder<'_>, name: &str) {
    service.set_name(name);
    let mut w = service.init_worker();
    let mut module = w.reborrow().init_modules(1).get(0);
    module.set_name("kv.js");
    module.set_bundled("/workerd/kv.js");
    w.set_compatibility_date("2026-01-01");
    let mut class = w.reborrow().init_durable_object_namespaces(1).get(0);
    class.set_class_name("KVNamespace");
    class.set_unique_key("KVNamespace");
    class.set_enable_sql(true);
    w.reborrow()
        .init_durable_object_storage()
        .set_local_disk(format!("{name}-storage"));
    let mut bindings = w.init_bindings(2);
    let mut shard = bindings.reborrow().get(0);
    shard.set_name("SHARD");
    shard
        .init_durable_object_namespace()
        .set_class_name("KVNamespace");
    let mut blobs = bindings.get(1);
    blobs.set_name("BLOBS");
    blobs.init_service().set_name(format!("{name}-blobs"));
}

fn set_bindings(worker: worker::Builder<'_>, wrangler: &Wrangler, kv_service: &str) {
    let kv = &wrangler.kv_namespaces;
    let do_bindings = wrangler
        .durable_objects
        .as_ref()
        .map_or(&[][..], |d| &d.bindings);
    let mut bs = worker.init_bindings((wrangler.vars.len() + do_bindings.len() + kv.len()) as u32);
    for (i, (k, v)) in wrangler.vars.iter().enumerate() {
        let mut b = bs.reborrow().get(i as u32);
        b.set_name(k);
        match v {
            Value::String(s) => b.set_text(s),
            other => b.set_json(other.to_string()),
        }
    }
    for (i, d) in do_bindings.iter().enumerate() {
        let mut b = bs.reborrow().get((wrangler.vars.len() + i) as u32);
        b.set_name(&d.name);
        b.init_durable_object_namespace()
            .set_class_name(&d.class_name);
    }
    for (i, k) in kv.iter().enumerate() {
        let mut b = bs
            .reborrow()
            .get((wrangler.vars.len() + do_bindings.len() + i) as u32);
        b.set_name(&k.binding);
        let id = k.id.as_ref().unwrap_or(&k.binding);
        let mut designator = b.init_kv_namespace();
        designator.set_name(kv_service);
        let props = serde_json::json!({ "namespace": id });
        designator.init_props().set_json(props.to_string());
    }
}

type Source = (String, Kind, Vec<u8>);
type Watch<'a> = &'a dyn Fn(&Path) -> Result<(), String>;

/// `main` and the modules beside it, as files (`no_bundle`).
fn unbundled(root: &Path, main: &str, watch: Watch) -> Result<Vec<Source>, String> {
    let main_path = root.join(main);
    if !matches!(kind_of(&main_path), Some(Kind::EsModule)) {
        return Err(format!(
            "main: {main}: must be a .js or .mjs ES module under \"no_bundle\""
        ));
    }
    let base = main_path.parent().unwrap_or(root);
    let others = files(base).into_iter().filter(|p| *p != main_path);
    std::iter::once(main_path.clone())
        .chain(others)
        .filter_map(|m| Some((kind_of(&m)?, m)))
        .map(|(kind, m)| {
            watch(&m)?;
            let name = m.strip_prefix(base).unwrap_or(&m).to_string_lossy();
            let bytes = fs::read(&m).map_err(|e| format!("{}: {e}", m.display()))?;
            Ok((name.replace('\\', "/"), kind, bytes))
        })
        .collect()
}

/// The esbuild to run: `WORKERD_ESBUILD`, else the nearest `node_modules/.bin/esbuild` at or
/// above the project, else `esbuild` on `PATH`.
fn esbuild(root: &Path) -> PathBuf {
    if let Some(path) = std::env::var_os("WORKERD_ESBUILD") {
        return path.into();
    }
    root.ancestors()
        .map(|dir| dir.join("node_modules/.bin/esbuild"))
        .find(|path| path.is_file())
        .unwrap_or_else(|| "esbuild".into())
}

/// Node's builtin modules, which workerd provides under `nodejs_compat`. esbuild leaves them,
/// their subpaths and their `node:` spellings external, as wrangler does.
#[rustfmt::skip]
const NODE_BUILTINS: &[&str] = &[
    "assert", "async_hooks", "buffer", "child_process", "cluster", "console", "constants",
    "crypto", "dgram", "diagnostics_channel", "dns", "domain", "events", "fs", "http", "http2",
    "https", "inspector", "module", "net", "os", "path", "perf_hooks", "process", "punycode",
    "querystring", "readline", "repl", "stream", "string_decoder", "sys", "timers", "tls",
    "trace_events", "tty", "url", "util", "v8", "vm", "wasi", "worker_threads", "zlib",
];

/// esbuild leaves a CJS dependency's `require()` of an external builtin behind its
/// `__require` helper, which throws in a module Worker. wrangler rewrites those calls with a
/// plugin the esbuild CLI cannot run, so the bundle gets a `require` that resolves builtins.
const REQUIRE_BRIDGE: &str = r#"var require = (id) => {
  const builtin = process.getBuiltinModule(String(id));
  if (builtin === undefined) {
    const error = new Error(`Cannot find module '${id}'`);
    error.code = "MODULE_NOT_FOUND";
    throw error;
  }
  return builtin;
};"#;

/// Bundles `main` with esbuild into an ES module plus the files its `copy` loader emits
/// (`.wasm`), watching every input esbuild read outside `node_modules`.
fn bundle(esbuild: &Path, root: &Path, main: &str, watch: Watch) -> Result<Vec<Source>, String> {
    use std::sync::atomic::AtomicUsize;
    use std::sync::atomic::Ordering;

    static COUNTER: AtomicUsize = AtomicUsize::new(0);
    let out = root.join(format!(
        ".wrangler/tmp/bundle-{}-{}",
        std::process::id(),
        COUNTER.fetch_add(1, Ordering::Relaxed)
    ));
    let _ = fs::remove_dir_all(&out);
    fs::create_dir_all(&out).map_err(|e| format!("{}: {e}", out.display()))?;
    let result = run_esbuild(esbuild, root, main, &out, watch);
    let _ = fs::remove_dir_all(&out);
    result
}

fn run_esbuild(
    esbuild: &Path,
    root: &Path,
    main: &str,
    out: &Path,
    watch: Watch,
) -> Result<Vec<Source>, String> {
    // Watched up front: if `main` is missing, creating it reloads.
    watch(&root.join(main))?;
    let metafile = out.join("meta.json");
    let output = std::process::Command::new(esbuild)
        .current_dir(root)
        .arg(main)
        .args([
            "--bundle",
            "--format=esm",
            "--platform=browser",
            "--target=es2024",
            "--conditions=workerd,worker,browser",
            "--entry-names=index",
            "--log-level=warning",
            "--external:node:*",
            "--external:cloudflare:*",
            "--external:workerd:*",
            "--loader:.wasm=copy",
            "--loader:.txt=text",
            "--loader:.html=text",
            "--loader:.sql=text",
            "--loader:.bin=binary",
        ])
        .args(
            NODE_BUILTINS
                .iter()
                .flat_map(|m| [format!("--external:{m}"), format!("--external:{m}/*")]),
        )
        .arg(format!("--banner:js={REQUIRE_BRIDGE}"))
        .arg(format!("--outdir={}", out.display()))
        .arg(format!("--metafile={}", metafile.display()))
        .output()
        .map_err(|e| match e.kind() {
            std::io::ErrorKind::NotFound => format!(
                "esbuild not found ({}). wrangler configs are bundled with esbuild: install it \
                 (`npm i -D esbuild`), set WORKERD_ESBUILD to its path, or set \"no_bundle\": true.",
                esbuild.display()
            ),
            _ => format!("{}: {e}", esbuild.display()),
        })?;
    if !output.status.success() {
        // No metafile names the inputs, so watch the project's sources: fixing the error reloads.
        const SOURCES: &[&str] = &["js", "mjs", "cjs", "jsx", "ts", "mts", "cts", "tsx", "json"];
        for p in files(root) {
            if p.extension()
                .and_then(|e| e.to_str())
                .is_some_and(|e| SOURCES.contains(&e))
            {
                watch(&p)?;
            }
        }
        return Err(format!(
            "esbuild failed:\n{}",
            String::from_utf8_lossy(&output.stderr).trim()
        ));
    }
    eprint!("{}", String::from_utf8_lossy(&output.stderr));

    let meta: Value = serde_json::from_slice(&fs::read(&metafile).map_err(|e| e.to_string())?)
        .map_err(|e| e.to_string())?;
    for input in meta["inputs"]
        .as_object()
        .into_iter()
        .flat_map(|m| m.keys())
    {
        let path = root.join(input);
        if !input.contains("node_modules/") && path.is_file() {
            watch(&path)?;
        }
    }

    let read = |p: &Path| fs::read(p).map_err(|e| format!("{}: {e}", p.display()));
    let mut sources = vec![(
        "index.js".to_owned(),
        Kind::EsModule,
        read(&out.join("index.js"))?,
    )];
    let mut emitted: Vec<PathBuf> = fs::read_dir(out)
        .map_err(|e| e.to_string())?
        .filter_map(Result::ok)
        .map(|e| e.path())
        .filter(|p| matches!(kind_of(p), Some(Kind::Wasm)))
        .collect();
    emitted.sort();
    for p in emitted {
        let name = p
            .file_name()
            .unwrap_or_default()
            .to_string_lossy()
            .into_owned();
        sources.push((name, Kind::Wasm, read(&p)?));
    }
    Ok(sources)
}

/// Every file under `dir`, outside `node_modules` and dot-directories, in a stable order.
fn files(dir: &Path) -> Vec<PathBuf> {
    let mut entries: Vec<_> = fs::read_dir(dir)
        .into_iter()
        .flatten()
        .filter_map(Result::ok)
        .filter(|e| {
            let name = e.file_name();
            let name = name.to_string_lossy();
            !name.starts_with('.') && name != "node_modules"
        })
        // `file_type` does not follow symlinks, so a symlink cycle cannot recurse.
        .map(|e| (e.path(), e.file_type().is_ok_and(|t| t.is_dir())))
        .collect();
    entries.sort();
    entries
        .into_iter()
        .flat_map(|(p, dir)| if dir { files(&p) } else { vec![p] })
        .collect()
}

/// The Durable Object classes, each with its storage key and whether it is SQLite-backed, from
/// the live `exports` entries or `migrations` applied in order, plus any bound class neither
/// mentions (not SQLite-backed, as in wrangler). A class keeps its key across renames, and a key is
/// never reused: a class created under a name an earlier class was keyed by (since deleted or
/// renamed) gets `<name>~<n>` and starts empty, as in production. `exports` holds no history, so
/// there a class is keyed by its name, or by its old name while its `renamed` entry remains, and a
/// `deleted` entry leaves the class's objects on disk.
fn do_classes(w: &Wrangler) -> Result<Vec<(String, String, bool)>, String> {
    let mut classes: Vec<(String, String, bool)> = Vec::new();
    let mut used: Vec<String> = Vec::new();
    let mut key = |c: &str| {
        let k = std::iter::once(c.to_owned())
            .chain((1..=used.len()).map(|n| format!("{c}~{n}")))
            .find(|k| !used.contains(k))
            .unwrap_or_default();
        used.push(k.clone());
        k
    };
    if !w.exports.is_empty() && !w.migrations.is_empty() {
        return Err("exports and migrations cannot both declare Durable Objects".to_owned());
    }
    let live = |c: &str| w.exports.get(c).is_some_and(|e| e.state == State::Created);
    let renames = |c: &String| {
        w.exports
            .values()
            .filter(|e| e.renamed_to.as_ref() == Some(c))
            .count()
    };
    for (c, e) in &w.exports {
        match (&e.state, &e.storage, &e.renamed_to) {
            (State::Created, Some(s), None) => {
                // A renamed class keeps the old class's key, and so its objects.
                let from = w
                    .exports
                    .iter()
                    .find(|(_, e)| e.renamed_to.as_ref() == Some(c));
                let k = key(from.map_or(c, |(f, _)| f));
                classes.push((c.clone(), k, *s == Storage::Sqlite));
            }
            (State::Deleted, None, None) => {}
            (State::Renamed, None, Some(to)) if renames(to) > 1 => {
                return Err(format!("exports: more than one class is renamed to {to}"));
            }
            (State::Renamed, None, Some(to)) if live(to) => {}
            (State::Renamed, None, Some(to)) => {
                return Err(format!(
                    "exports: {c} is renamed to {to}, which is not live"
                ));
            }
            _ => return Err(format!("exports: {c} has the wrong fields for its state")),
        }
    }
    for m in &w.migrations {
        // In wrangler's order: deletes, then renames, then new classes.
        let find = |classes: &[(String, String, bool)], c: &str, what: &str| {
            let i = classes.iter().position(|(n, _, _)| n == c);
            i.ok_or_else(|| format!("migrations: {what} class {c} does not exist"))
        };
        for c in &m.deleted_classes {
            classes.remove(find(&classes, c, "deleted")?);
        }
        for r in &m.renamed_classes {
            let i = find(&classes, &r.from, "renamed")?;
            if find(&classes, &r.to, "").is_ok() {
                return Err(format!(
                    "migrations: renamed-to class {} already exists",
                    r.to
                ));
            }
            classes[i].0.clone_from(&r.to);
        }
        let new = m.new_classes.iter().map(|c| (c, false));
        for (c, sql) in new.chain(m.new_sqlite_classes.iter().map(|c| (c, true))) {
            if find(&classes, c, "new").is_ok() {
                return Err(format!("migrations: new class {c} already exists"));
            }
            classes.push((c.clone(), key(c), sql));
        }
    }
    for b in w.durable_objects.iter().flat_map(|d| &d.bindings) {
        if !classes.iter().any(|(c, _, _)| *c == b.class_name) {
            classes.push((b.class_name.clone(), key(&b.class_name), false));
        }
    }
    Ok(classes)
}

/// Removes `//` and `/* */` comments and trailing commas outside strings, so `serde_json` can
/// parse JSONC. A comment becomes a space; a comma is held until the next token shows whether it
/// trails.
fn strip_jsonc(src: &str) -> String {
    let mut out = String::with_capacity(src.len());
    let mut comma = false;
    let mut chars = src.chars().peekable();
    while let Some(c) = chars.next() {
        match c {
            '/' if chars.peek() == Some(&'/') => {
                while chars.next_if(|&c| c != '\n').is_some() {}
                out.push(' ');
            }
            '/' if chars.peek() == Some(&'*') => {
                chars.next();
                let mut prev = '\0';
                for c in chars.by_ref() {
                    if prev == '*' && c == '/' {
                        break;
                    }
                    prev = c;
                }
                out.push(' ');
            }
            c if c.is_whitespace() => out.push(c),
            ',' => {
                if comma {
                    out.push(',');
                }
                comma = true;
            }
            _ => {
                if comma && !matches!(c, '}' | ']') {
                    out.push(',');
                }
                comma = false;
                out.push(c);
                if c == '"' {
                    while let Some(c) = chars.next() {
                        out.push(c);
                        match c {
                            '\\' => out.extend(chars.next()),
                            '"' => break,
                            _ => {}
                        }
                    }
                }
            }
        }
    }
    if comma {
        out.push(',');
    }
    out
}

#[cfg(test)]
#[path = "wrangler-test.rs"]
mod tests;
