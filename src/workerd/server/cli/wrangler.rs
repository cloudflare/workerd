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
//! `compatibility_flags`, `vars`, `durable_objects`, `migrations`, `no_bundle` and `$schema`. Any
//! other field is an error rather than silently ignored.
//!
//! Durable Objects are stored on disk under `.wrangler/state/workerd/` next to the config, one
//! directory per class, named `<name>-<key>` by its storage key (see [`do_classes`]). The server
//! listens on `localhost:8787`, as `wrangler dev` does; `--socket-addr http=<addr>` overrides it.

use std::collections::BTreeMap;
use std::fs;
use std::path::Path;
use std::path::PathBuf;

use serde::Deserialize;
use serde_json::Value;
use workerd_capnp::config;

use crate::config::Config;
use crate::config::words_from_bytes;
use crate::process::Process;

const DEFAULT_ADDRESS: &str = "localhost:8787";

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
    migrations: Vec<Migration>,
    #[serde(default)]
    no_bundle: bool,
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

pub fn load(path: &str, process: &Process) -> Result<Config, String> {
    let watch = |p: &Path| {
        process
            .watch_file(p.as_os_str().as_encoded_bytes())
            .map_err(|e| e.description().to_owned())
    };
    let file = fs::canonicalize(path).map_err(|e| e.to_string())?;
    watch(&file)?;
    let source = fs::read_to_string(&file).map_err(|e| e.to_string())?;
    let wrangler: Wrangler =
        serde_json::from_str(&strip_jsonc(&source)).map_err(|e| e.to_string())?;
    let root = file.parent().unwrap_or(&file);

    let sources = if wrangler.no_bundle {
        unbundled(root, &wrangler.main, &watch)?
    } else {
        bundle(&esbuild(root), root, &wrangler.main, &watch)?
    };

    let classes = do_classes(&wrangler)?;
    let name = wrangler.name.clone().unwrap_or_else(|| "main".to_owned());
    let storage = root.join(".wrangler/state/workerd");
    if !classes.is_empty() {
        fs::create_dir_all(&storage).map_err(|e| format!("{}: {e}", storage.display()))?;
    }

    let mut message = capnp::message::Builder::new_default();
    let mut conf = message.init_root::<config::Builder>();
    let mut services = conf
        .reborrow()
        .init_services(if classes.is_empty() { 1 } else { 2 });

    let mut service = services.reborrow().get(0);
    service.set_name(&name);
    let mut worker = service.init_worker();
    worker.set_compatibility_date(&wrangler.compatibility_date);
    worker
        .set_compatibility_flags(&wrangler.compatibility_flags[..])
        .map_err(|e| e.to_string())?;

    let mut ms = worker.reborrow().init_modules(sources.len() as u32);
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

    let do_bindings = wrangler
        .durable_objects
        .as_ref()
        .map_or(&[][..], |d| &d.bindings);
    let mut bs = worker
        .reborrow()
        .init_bindings((wrangler.vars.len() + do_bindings.len()) as u32);
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
    }

    let mut socket = conf.init_sockets(1).get(0);
    socket.set_name("http");
    socket.set_address(DEFAULT_ADDRESS);
    socket.reborrow().init_http();
    socket.init_service().set_name(&name);

    Config::from_words(&words_from_bytes(
        &capnp::serialize::write_message_to_words(&message),
    ))
    .map_err(|e| e.to_string())
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
/// `migrations` applied in order, plus any bound class the migrations never mention (not
/// SQLite-backed, as in wrangler). A class keeps its key across renames, and a key is never
/// reused: a class created under a name an earlier class was keyed by (since deleted or renamed)
/// gets `<name>~<n>` and starts empty, as in production.
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
