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
//! `r2_buckets`, `triggers` (`crons`), `queues`, `workflows`, `d1_databases`,
//! `secrets_store_secrets`, `ratelimits`, `assets`, `no_bundle` and `$schema`. Any other field is
//! an error rather than silently ignored.
//!
//! Durable Objects are stored on disk in the state directory, `.wrangler/state/workerd/` next to
//! the config unless `--data-dir` names another, one directory per class, named `<name>-<key>` by
//! its storage key (see [`do_classes`]). The disk service is named `<name>-do-storage`, or
//! [`CLUSTER_STORAGE`] when `--cluster-addr` joins the config to a cluster, which defines it even
//! for a project without Durable Objects, in the state directory's `do/`. The bundled Workers'
//! disks and the Cache API's entries are in the state directory too. `workerd compile` translates
//! for a server that joins no cluster.
//! `kv_namespaces` need `--experimental`. Each is a binding to the Worker `<name>-kv`
//! (the bundled module `/workerd/kv.js`), with the namespace's `id` (else its binding name) in
//! its props; `preview_id` and `remote` are ignored. Its objects are in
//! `.wrangler/state/workerd/kv/`, and its large values in `kv/blobs/` there. `r2_buckets`, also
//! `--experimental`, are bindings to the Worker `<name>-r2` (the bundled module `/workerd/r2.js`)
//! in the same way, with the bucket's `bucket_name` (else its binding name) in its props;
//! `preview_bucket_name`, `jurisdiction` and `remote` are ignored. Its objects are in
//! `.wrangler/state/workerd/r2/`, one store object per R2 object. `triggers.crons`,
//! also `--experimental`, are the schedule of the Worker `<name>-cron` (the bundled module
//! `/workerd/cron.js`), whose objects are in `.wrangler/state/workerd/cron/`. `queues`, also
//! `--experimental`, are served by the Worker `<name>-queues` (the bundled module
//! `/workerd/queues.js`): each producer is a `queue` binding to it, and each consumer delivers to
//! this Worker; its messages are in `.wrangler/state/workerd/queues/`. `workflows`, also
//! `--experimental`, run on the Worker `<name>-workflows` (the bundled module
//! `/workerd/workflows.js`), the engine of this Worker's `workflowsEngine`; each binding is the
//! Workflow API over it, and its instances are in `.wrangler/state/workerd/workflows/`.
//! `limits.steps` and `default_retention` apply; `concurrency` and `jurisdiction` are ignored;
//! `schedules`, and a `script_name` other than this Worker, are errors. `d1_databases`, also
//! `--experimental`, are served by the Worker `<name>-d1` (the bundled module `/workerd/d1.js`):
//! each binding is the D1 API over it, with the database's `preview_database_id`, else its
//! `database_id`, else its binding name in its props, as `wrangler dev` names local databases;
//! migrations are not applied. Its databases are in `.wrangler/state/workerd/d1/`.
//! `secrets_store_secrets`, also `--experimental`, are the entrypoint `SecretsStoreSecret` of the
//! Worker `<name>-secrets-store` (the bundled module `/workerd/secrets-store.js`), the store and
//! secret name in its props. A secret's value is the variable [`secret_variable`] names after its
//! `secret_name`, read when the server starts and never stored; a Worker can only `get()` it, as on
//! Cloudflare. `ratelimits`, also `--experimental`, are the entrypoint `RateLimit` of the Worker
//! `<name>-ratelimit` (the bundled module `/workerd/ratelimit.js`), its settings in its props, so
//! they are called over JS RPC; it keeps its counts in memory. `assets`, also `--experimental`, are
//! the files in `directory`, served by the assets service `<name>-assets`, which routes each
//! request to this Worker or to a file as Cloudflare does; `binding` is its `AssetWorker`
//! entrypoint. Without `main` it serves only the files. Under `--experimental` the Worker's
//! `caches` are served by the cache service `<name>-cache`, its entries in
//! `.wrangler/state/workerd/cache/`; without it, `caches` throw "No Cache was configured". The
//! server listens on `localhost:8787`, as `wrangler dev` does, to `<name>-assets` if there are
//! assets, else to the worker; `--socket-addr http=<addr>` overrides the address.
//!
//! For `workerd deploy`, [`load_deployment`] translates the same fields into a cluster deployment
//! (`cluster::compose`), which holds only services: the worker, named after the deployment, with
//! no socket, and its Durable Objects stored on the disk service [`CLUSTER_STORAGE`], which each
//! node's own config defines. A deployment cannot have `kv_namespaces`, `r2_buckets`,
//! `triggers.crons`, `queues`, `workflows`, `d1_databases`, `secrets_store_secrets`, `ratelimits`
//! or `assets` yet.

use std::collections::BTreeMap;
use std::collections::BTreeSet;
use std::fs;
use std::path::Path;
use std::path::PathBuf;

use cluster::compose::workflow_namespace_key;
use serde::Deserialize;
use serde::Serialize;
use serde_json::Value;
use workerd_capnp::config;
use workerd_capnp::service;
use workerd_capnp::service_designator;
use workerd_capnp::worker;
use workerd_server::run::RunOptions;

use crate::config::Config;
use crate::config::words_from_bytes;
use crate::process::Process;

const DEFAULT_ADDRESS: &str = "localhost:8787";

/// The disk service that holds Durable Objects in a cluster. A deployment's are stored on the
/// node's, since a deployment may define no disk. A node that serves a wrangler config and joins a
/// cluster with `--cluster-addr` defines it, so that the node can serve deployments with Durable
/// Objects too. Each class's objects are in a directory of their own on it, named by the class's
/// storage key, which names the worker.
pub const CLUSTER_STORAGE: &str = "do-storage";

/// The directory of [`CLUSTER_STORAGE`] in the state directory. It is apart from the bundled
/// services' directories, such as `cache/` and `d1/`, which a deployment's storage key could
/// otherwise name.
const CLUSTER_STORAGE_DIR: &str = "do";

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Wrangler {
    #[serde(rename = "$schema")]
    _schema: Option<String>,
    name: Option<String>,
    main: Option<String>,
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
    r2_buckets: Vec<R2Bucket>,
    #[serde(default)]
    triggers: Triggers,
    #[serde(default)]
    queues: Queues,
    #[serde(default)]
    workflows: Vec<Workflow>,
    #[serde(default)]
    d1_databases: Vec<D1Database>,
    #[serde(default)]
    secrets_store_secrets: Vec<SecretsStoreSecret>,
    #[serde(default)]
    ratelimits: Vec<Ratelimit>,
    #[serde(default)]
    no_bundle: bool,
    assets: Option<Assets>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct SecretsStoreSecret {
    binding: String,
    store_id: String,
    secret_name: String,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Ratelimit {
    name: String,
    namespace_id: String,
    simple: SimpleLimit,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct SimpleLimit {
    limit: u64,
    period: u64,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Assets {
    directory: String,
    binding: Option<String>,
    html_handling: Option<HtmlHandling>,
    not_found_handling: Option<NotFoundHandling>,
    base_path: Option<String>,
    run_worker_first: Option<RunWorkerFirst>,
}

#[derive(Deserialize, Serialize)]
#[serde(rename_all = "kebab-case")]
enum HtmlHandling {
    AutoTrailingSlash,
    ForceTrailingSlash,
    DropTrailingSlash,
    None,
}

#[derive(Deserialize, Serialize)]
#[serde(rename_all = "kebab-case")]
enum NotFoundHandling {
    SinglePageApplication,
    #[serde(rename = "404-page")]
    Page404,
    None,
}

#[derive(Deserialize, Serialize)]
#[serde(untagged)]
enum RunWorkerFirst {
    All(bool),
    Rules(Vec<String>),
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct D1Database {
    binding: String,
    database_id: Option<String>,
    preview_database_id: Option<String>,
    #[serde(rename = "database_name")]
    _database_name: Option<String>,
    #[serde(rename = "migrations_table")]
    _migrations_table: Option<String>,
    #[serde(rename = "migrations_dir")]
    _migrations_dir: Option<String>,
    #[serde(rename = "migrations_pattern")]
    _migrations_pattern: Option<String>,
    #[serde(rename = "database_internal_env")]
    _database_internal_env: Option<String>,
    #[serde(rename = "remote")]
    _remote: Option<bool>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Workflow {
    binding: String,
    name: String,
    class_name: String,
    script_name: Option<String>,
    limits: Option<WorkflowLimits>,
    #[serde(rename = "concurrency")]
    _concurrency: Option<serde_json::Value>,
    schedules: Option<serde_json::Value>,
    default_retention: Option<WorkflowRetention>,
    #[serde(rename = "jurisdiction")]
    _jurisdiction: Option<String>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct WorkflowLimits {
    steps: Option<u32>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct WorkflowRetention {
    success_retention: Option<serde_json::Value>,
    error_retention: Option<serde_json::Value>,
}

#[derive(Default, Deserialize)]
#[serde(deny_unknown_fields)]
struct Queues {
    #[serde(default)]
    producers: Vec<Producer>,
    #[serde(default)]
    consumers: Vec<Consumer>,
}

impl Queues {
    fn is_empty(&self) -> bool {
        self.producers.is_empty() && self.consumers.is_empty()
    }
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Producer {
    binding: String,
    queue: String,
    #[serde(rename = "remote")]
    _remote: Option<bool>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Consumer {
    queue: String,
    max_batch_size: Option<u32>,
    max_batch_timeout: Option<u32>,
    max_retries: Option<u32>,
    dead_letter_queue: Option<String>,
    retry_delay: Option<u32>,
    max_concurrency: Option<u32>,
}

#[derive(Default, Deserialize)]
#[serde(deny_unknown_fields)]
struct Triggers {
    #[serde(default)]
    crons: Vec<String>,
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
struct R2Bucket {
    binding: String,
    bucket_name: Option<String>,
    #[serde(rename = "preview_bucket_name")]
    _preview_bucket_name: Option<String>,
    #[serde(rename = "jurisdiction")]
    _jurisdiction: Option<String>,
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
    /// A config to serve with `options`: the worker named after the config's `name`, else `main`,
    /// a socket, and a disk service for Durable Objects (see [`served_storage`]). Its state is
    /// under `data_dir` (`--data-dir`), else under `.wrangler/state/workerd` in the project.
    Serve {
        options: &'a RunOptions,
        data_dir: Option<&'a Path>,
        experimental: bool,
    },
    /// A cluster deployment, services only: the worker named `name`, else after the config's
    /// `name`, and its Durable Objects stored on the node's [`CLUSTER_STORAGE`].
    Deployment { name: Option<&'a str> },
}

/// The disk service that holds the Durable Objects of the worker `name`, with `classes`, in a
/// config served with `options`, and the directory of that service if the config defines it.
///
/// Joined to a cluster with `--cluster-addr`, the config defines [`CLUSTER_STORAGE`] even without
/// Durable Objects of its own, for deployments' ones. Otherwise the config defines
/// `<name>-do-storage` if it has Durable Objects. The directory, created here, is
/// [`CLUSTER_STORAGE_DIR`] in `storage`, the state directory, or for `<name>-do-storage` the
/// state directory itself, whose classes' directories are named `<name>-<key>`.
fn served_storage(
    storage: &Path,
    name: &str,
    classes: &[(String, String, bool)],
    options: &RunOptions,
) -> Result<(String, Option<PathBuf>), String> {
    let joining = options.cluster_address.is_some();
    let service = if joining {
        CLUSTER_STORAGE.to_owned()
    } else {
        // Named after the worker, so it cannot collide with it.
        format!("{name}-do-storage")
    };
    if name == service {
        return Err(format!(
            "name: \"{name}\" is the name of the disk service that holds the Durable Objects in a \
             cluster. Choose another name."
        ));
    }
    if !joining && classes.is_empty() {
        return Ok((service, None));
    }
    let dir = if joining {
        storage.join(CLUSTER_STORAGE_DIR)
    } else {
        storage.to_owned()
    };
    create_state_dir(&dir)?;
    Ok((service, Some(dir)))
}

/// The config `workerd serve` runs with `options` for the wrangler config at `path`, its state
/// under `data_dir` if given.
pub fn load(
    path: &str,
    process: &Process,
    options: &RunOptions,
    data_dir: Option<&Path>,
    experimental: bool,
) -> Result<Config, String> {
    let target = Target::Serve {
        options,
        data_dir,
        experimental,
    };
    translate(path, process, &target).map(|(_, config)| config)
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

#[expect(clippy::too_many_lines, reason = "a service per wrangler field")]
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
    check_experimental(&wrangler, target)?;
    check_bindings(&wrangler)?;
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

    let sources = match &wrangler.main {
        None if wrangler.assets.is_some() => {
            assets_only(&wrangler)?;
            vec![]
        }
        None => {
            return Err("Missing entry-point to Worker script or to assets directory".to_owned());
        }
        Some(main) if wrangler.no_bundle => unbundled(root, main, &watch)?,
        Some(main) => bundle(&esbuild(root), root, main, &watch)?,
    };
    let workers = u32::from(wrangler.main.is_some());

    // Durable Objects and the bundled Workers keep their state here.
    let storage = match target {
        Target::Serve {
            data_dir: Some(dir),
            ..
        } => std::path::absolute(dir).map_err(|e| format!("--data-dir: {e}"))?,
        _ => root.join(".wrangler/state/workerd"),
    };
    let (storage_service, disk) = match target {
        Target::Serve { options, .. } => served_storage(&storage, &name, &classes, options)?,
        Target::Deployment { .. } => (CLUSTER_STORAGE.to_owned(), None),
    };
    let mut message = capnp::message::Builder::new_default();
    let mut conf = message.init_root::<config::Builder>();
    let do_storage = u32::from(disk.is_some());
    let cache = u32::from(
        workers == 1
            && matches!(
                target,
                Target::Serve {
                    experimental: true,
                    ..
                }
            ),
    );
    let mut services = conf.reborrow().init_services(
        workers
            + do_storage
            + cache
            + 3 * u32::from(!wrangler.kv_namespaces.is_empty())
            + 3 * u32::from(!wrangler.r2_buckets.is_empty())
            + u32::from(!wrangler.secrets_store_secrets.is_empty())
            + 2 * u32::from(!wrangler.triggers.crons.is_empty())
            + 2 * u32::from(!wrangler.queues.is_empty())
            + 2 * u32::from(!wrangler.workflows.is_empty())
            + 2 * u32::from(!wrangler.d1_databases.is_empty())
            + u32::from(!wrangler.ratelimits.is_empty())
            + u32::from(wrangler.assets.is_some()),
    );
    let at = workers + do_storage;
    bundled_services(&mut services, at, &name, root, &storage, &wrangler, &watch)?;

    if workers == 1 {
        let mut service = services.reborrow().get(0);
        service.set_name(&name);
        let mut worker = service.init_worker();
        worker.set_compatibility_date(&wrangler.compatibility_date);
        worker
            .set_compatibility_flags(&wrangler.compatibility_flags[..])
            .map_err(|e| e.to_string())?;
        set_modules(worker.reborrow(), &sources)?;
        set_bindings(worker.reborrow(), &wrangler, &name);
        if !wrangler.workflows.is_empty() {
            set_workflows_engine(worker.reborrow(), &name, &wrangler.workflows);
        }
        set_durable_objects(worker, &classes, &name, &storage_service);
    }
    if cache == 1 {
        cache_service(&mut services, &name, &storage)?;
    }
    if let Some(path) = disk {
        disk_service(services.get(workers), &storage_service, &path);
    }

    if matches!(target, Target::Serve { .. }) {
        let mut socket = conf.init_sockets(1).get(0);
        socket.set_name("http");
        socket.set_address(DEFAULT_ADDRESS);
        socket.reborrow().init_http();
        let target = match &wrangler.assets {
            Some(_) => format!("{name}-assets"),
            None => name.clone(),
        };
        socket.init_service().set_name(target);
    }

    let config = Config::from_words(&words_from_bytes(
        &capnp::serialize::write_message_to_words(&message),
    ))
    .map_err(|e| e.to_string())?;
    Ok((name, config))
}

/// Assets without `main` have no Worker for Worker-only fields to configure.
fn assets_only(wrangler: &Wrangler) -> Result<(), String> {
    let worker_only = [
        ("vars", !wrangler.vars.is_empty()),
        ("durable_objects", wrangler.durable_objects.is_some()),
        ("exports", !wrangler.exports.is_empty()),
        ("migrations", !wrangler.migrations.is_empty()),
        ("kv_namespaces", !wrangler.kv_namespaces.is_empty()),
        ("r2_buckets", !wrangler.r2_buckets.is_empty()),
        ("triggers.crons", !wrangler.triggers.crons.is_empty()),
        ("queues", !wrangler.queues.is_empty()),
        ("workflows", !wrangler.workflows.is_empty()),
        ("d1_databases", !wrangler.d1_databases.is_empty()),
        (
            "secrets_store_secrets",
            !wrangler.secrets_store_secrets.is_empty(),
        ),
        ("ratelimits", !wrangler.ratelimits.is_empty()),
        (
            "assets.binding",
            wrangler
                .assets
                .as_ref()
                .is_some_and(|a| a.binding.is_some()),
        ),
    ];
    match worker_only.iter().find(|(_, used)| *used) {
        Some((field, _)) => Err(format!("{field} needs a Worker, but there is no main")),
        None => Ok(()),
    }
}

/// The Durable Object namespaces of `classes`, stored on the disk service `storage`.
fn set_durable_objects(
    mut worker: worker::Builder<'_>,
    classes: &[(String, String, bool)],
    name: &str,
    storage: &str,
) {
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
        worker.init_durable_object_storage().set_local_disk(storage);
    }
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

/// `kv_namespaces`, `r2_buckets`, `triggers.crons`, `queues`, `workflows`, `d1_databases`,
/// `secrets_store_secrets`, `ratelimits` and `assets` need `--experimental`, and a deployment
/// cannot have them yet. Nor can a config joined to a cluster have `kv_namespaces`, whose large
/// values would stay on the disk of the instance that wrote them, or `queues`, whose Worker this
/// translation does not set up for a cluster.
fn check_experimental(wrangler: &Wrangler, target: &Target) -> Result<(), String> {
    if let Target::Serve { options, .. } = target
        && options.cluster_address.is_some()
    {
        for (used, field) in [
            (!wrangler.kv_namespaces.is_empty(), "kv_namespaces"),
            (!wrangler.queues.is_empty(), "queues"),
        ] {
            if used {
                return Err(format!(
                    "A config that joins a cluster with --cluster-addr cannot have {field} yet."
                ));
            }
        }
    }
    for (used, field) in [
        (!wrangler.kv_namespaces.is_empty(), "kv_namespaces"),
        (!wrangler.r2_buckets.is_empty(), "r2_buckets"),
        (!wrangler.triggers.crons.is_empty(), "triggers.crons"),
        (!wrangler.queues.is_empty(), "queues"),
        (!wrangler.workflows.is_empty(), "workflows"),
        (!wrangler.d1_databases.is_empty(), "d1_databases"),
        (
            !wrangler.secrets_store_secrets.is_empty(),
            "secrets_store_secrets",
        ),
        (!wrangler.ratelimits.is_empty(), "ratelimits"),
        (wrangler.assets.is_some(), "assets"),
    ] {
        match target {
            _ if !used => {}
            Target::Serve {
                experimental: true, ..
            } => {}
            Target::Serve { .. } => {
                return Err(format!(
                    "{field} is an experimental feature which may change or go away in the \
                     future. You must run workerd with `--experimental` to use it."
                ));
            }
            Target::Deployment { .. } => {
                return Err(format!("A deployment cannot have {field} yet."));
            }
        }
    }
    Ok(())
}

/// The environment variable holding the secret `secret_name`: `WORKERD_SECRET_` and the name
/// upper-cased, other characters than letters and digits as `_`. The prefix keeps every other
/// variable, the node's own credentials included, out of a Worker's reach.
fn secret_variable(secret_name: &str) -> String {
    let name: String = (secret_name.chars())
        .map(|c| match c {
            'a'..='z' | 'A'..='Z' | '0'..='9' => c.to_ascii_uppercase(),
            _ => '_',
        })
        .collect();
    format!("WORKERD_SECRET_{name}")
}

/// A rate limit's period is 10 or 60 seconds, as on Cloudflare. Two secrets of different names
/// cannot read the same variable, and a variable that is not set is a warning.
fn check_bindings(wrangler: &Wrangler) -> Result<(), String> {
    let periods = [10, 60];
    if let Some(r) = (wrangler.ratelimits.iter()).find(|r| !periods.contains(&r.simple.period)) {
        return Err(format!(
            "ratelimits: {}: simple.period must be 10 or 60",
            r.name
        ));
    }
    let mut names = BTreeMap::new();
    for s in &wrangler.secrets_store_secrets {
        let variable = secret_variable(&s.secret_name);
        if std::env::var_os(&variable).is_none() {
            eprintln!(
                "secrets_store_secrets: {variable} is not set; \"{}\" is not found",
                s.secret_name
            );
        }
        match names.insert(variable.clone(), &s.secret_name) {
            Some(other) if *other != s.secret_name => {
                return Err(format!(
                    "secrets_store_secrets: \"{other}\" and \"{}\" would both read {variable}",
                    s.secret_name
                ));
            }
            _ => {}
        }
    }
    Ok(())
}

/// The KV, R2, Secrets Store, cron, Queues, Workflows, D1, rate-limiting and assets services
/// `wrangler` asks for, with their disks under `storage`, from `services[at]`. Each is named after
/// the worker, `<name>-<kind>`, so it cannot collide with it.
fn bundled_services(
    services: &mut capnp::struct_list::Builder<'_, service::Owned>,
    mut at: u32,
    name: &str,
    root: &Path,
    storage: &Path,
    wrangler: &Wrangler,
    watch: Watch,
) -> Result<(), String> {
    if !wrangler.kv_namespaces.is_empty() {
        let dir = storage.join("kv");
        kv_services(services, at, &format!("{name}-kv"), &dir)?;
        at += 3;
    }
    if !wrangler.r2_buckets.is_empty() {
        r2_services(services, at, &format!("{name}-r2"), storage)?;
        at += 3;
    }
    let secrets = &wrangler.secrets_store_secrets;
    if !secrets.is_empty() {
        secrets_service(services.reborrow().get(at), name, secrets);
        at += 1;
    }
    let crons = &wrangler.triggers.crons;
    if !crons.is_empty() {
        cron_services(services, at, name, &storage.join("cron"), crons)?;
        at += 2;
    }
    if !wrangler.queues.is_empty() {
        let dir = storage.join("queues");
        queues_services(services, at, name, &dir, &wrangler.queues)?;
        at += 2;
    }
    if !wrangler.workflows.is_empty() {
        let dir = storage.join("workflows");
        workflows_services(services, at, name, &dir, wrangler)?;
        at += 2;
    }
    if !wrangler.d1_databases.is_empty() {
        let dir = storage.join("d1");
        d1_services(services, at, &format!("{name}-d1"), &dir)?;
        at += 2;
    }
    if !wrangler.ratelimits.is_empty() {
        let service = services.reborrow().get(at);
        stateless_worker(service, &format!("{name}-ratelimit"), "ratelimit.js", 0);
        at += 1;
    }
    if let Some(assets) = &wrangler.assets {
        assets_services(services, at, name, root, assets, wrangler, watch)?;
    }
    Ok(())
}

/// The assets service `<target>-assets` at `services[at]`, which the socket serves: the files
/// of `assets.directory`, and `target` if there is a `main`. Its config holds the text of the
/// directory's `_headers`, `_redirects` and `.assetsignore`, which are watched.
fn assets_services(
    services: &mut capnp::struct_list::Builder<'_, service::Owned>,
    at: u32,
    target: &str,
    root: &Path,
    assets: &Assets,
    wrangler: &Wrangler,
    watch: Watch,
) -> Result<(), String> {
    let dir = root.join(&assets.directory);
    if !dir.is_dir() {
        return Err(format!(
            "assets.directory: {}: not a directory",
            dir.display()
        ));
    }
    let text = |file| {
        let path = dir.join(file);
        watch(&path)?;
        match fs::read_to_string(&path) {
            Ok(text) => Ok(Some(text)),
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(None),
            Err(e) => Err(format!("{}: {e}", path.display())),
        }
    };
    let config = serde_json::json!({
        "html_handling": assets.html_handling,
        "not_found_handling": assets.not_found_handling,
        "base_path": assets.base_path,
        "run_worker_first": assets.run_worker_first,
        "headers": text("_headers")?,
        "redirects": text("_redirects")?,
        "ignore": text(".assetsignore")?,
    });
    let mut service = services.reborrow().get(at);
    service.set_name(format!("{target}-assets"));
    let mut service = service.init_assets();
    service.set_path(dir.to_string_lossy());
    service.set_config(config.to_string());
    if wrangler.main.is_some() {
        service.init_user_worker().set_name(target);
    } else {
        service.set_compatibility_date(&wrangler.compatibility_date);
        service
            .set_compatibility_flags(&wrangler.compatibility_flags[..])
            .map_err(|e| e.to_string())?;
    }
    Ok(())
}

/// The D1 Worker `name` at `services[at]`, then its disk `dir`.
fn d1_services(
    services: &mut capnp::struct_list::Builder<'_, service::Owned>,
    at: u32,
    name: &str,
    dir: &Path,
) -> Result<(), String> {
    let disk = format!("{name}-storage");
    create_state_dir(dir)?;
    disk_service(services.reborrow().get(at + 1), &disk, dir);
    let service = services.reborrow().get(at);
    let class = Some(("DATABASE", "D1Database"));
    bundled_worker(service, name, "d1.js", &[], class, 1);
    Ok(())
}

/// Creates `dir`, a directory of the state directory.
fn create_state_dir(dir: &Path) -> Result<(), String> {
    fs::create_dir_all(dir).map_err(|e| {
        format!(
            "{}: {e}. Pass --data-dir <dir> to keep state elsewhere.",
            dir.display()
        )
    })
}

/// The KV Worker `name` at `services[at]`, then its disks under `dir`.
fn kv_services(
    services: &mut capnp::struct_list::Builder<'_, service::Owned>,
    at: u32,
    name: &str,
    dir: &Path,
) -> Result<(), String> {
    let blobs = dir.join("blobs");
    for (i, what, path) in [(at + 1, "storage", dir), (at + 2, "blobs", &blobs)] {
        let service = format!("{name}-{what}");
        create_state_dir(path)?;
        disk_service(services.reborrow().get(i), &service, path);
    }
    let service = services.reborrow().get(at);
    let bindings = bundled_worker(
        service,
        name,
        "kv.js",
        &[],
        Some(("SHARD", "KVNamespace")),
        2,
    );
    let mut blobs = bindings.get(1);
    blobs.set_name("BLOBS");
    blobs.init_service().set_name(format!("{name}-blobs"));
    Ok(())
}

/// The Worker `name` of the bundled module `/workerd/<module>`, without Durable Objects or a
/// disk, and `n` bindings, returned.
fn stateless_worker<'a>(
    mut service: service::Builder<'a>,
    name: &str,
    module: &str,
    n: u32,
) -> capnp::struct_list::Builder<'a, worker::binding::Owned> {
    service.set_name(name);
    let mut w = service.init_worker();
    let mut m = w.reborrow().init_modules(1).get(0);
    m.set_name(module);
    m.set_bundled(format!("/workerd/{module}"));
    w.set_compatibility_date("2026-01-01");
    w.init_bindings(n)
}

/// The Secrets Store Worker `<name>-secrets-store`, each secret's variable bound as
/// `env:<secret_name>`.
fn secrets_service(service: service::Builder<'_>, name: &str, secrets: &[SecretsStoreSecret]) {
    let names: BTreeSet<_> = secrets.iter().map(|s| &s.secret_name).collect();
    let name = format!("{name}-secrets-store");
    let n = names.len() as u32;
    let mut bindings = stateless_worker(service, &name, "secrets-store.js", n);
    for (i, secret) in (0u32..).zip(names) {
        let mut b = bindings.reborrow().get(i);
        b.set_name(format!("env:{secret}"));
        b.set_from_environment(secret_variable(secret));
    }
}

/// The R2 Worker `name` at `services[at]`, then its disk, `storage`, and its `BLOBS`, a bucket
/// service storing the objects in `storage/r2/`, or in a cluster in the cluster's bucket.
fn r2_services(
    services: &mut capnp::struct_list::Builder<'_, service::Owned>,
    at: u32,
    name: &str,
    storage: &Path,
) -> Result<(), String> {
    let disk = format!("{name}-storage");
    create_state_dir(storage)?;
    disk_service(services.reborrow().get(at + 1), &disk, storage);
    let mut service = services.reborrow().get(at + 2);
    service.set_name(format!("{name}-blobs"));
    let mut bucket = service.init_bucket();
    bucket.set_path("r2");
    bucket.set_local_disk(disk);
    let service = services.reborrow().get(at);
    let mut blobs = bundled_worker(service, name, "r2.js", &[], None, 1).get(0);
    blobs.set_name("BLOBS");
    blobs.init_service().set_name(format!("{name}-blobs"));
    Ok(())
}

/// A writable disk service `name` at `path`.
fn disk_service(mut service: service::Builder<'_>, name: &str, path: &Path) {
    service.set_name(name);
    let mut disk = service.init_disk();
    disk.set_path(path.to_string_lossy());
    disk.set_writable(true);
}

/// The cache service `<name>-cache`, last in `services`, its entries under `storage`, as the Cache
/// API backend of the Worker, `services[0]`.
fn cache_service(
    services: &mut capnp::struct_list::Builder<'_, service::Owned>,
    name: &str,
    storage: &Path,
) -> Result<(), String> {
    let dir = storage.join("cache");
    let cache = format!("{name}-cache");
    create_state_dir(&dir)?;
    let last = services.len() - 1;
    let mut service = services.reborrow().get(last);
    service.set_name(&cache);
    service.init_cache().set_path(dir.to_string_lossy());
    let Ok(service::Which::Worker(Ok(worker))) = services.reborrow().get(0).which() else {
        unreachable!("services[0] is the Worker");
    };
    worker.init_cache_api_outbound().set_name(&cache);
    Ok(())
}

/// The Worker `name` of the bundled module `/workerd/<module>`, with `flags`, its Durable Objects
/// on the disk `<name>-storage`, and `n` bindings, returned. With `class`, the SQLite-backed
/// Durable Object class `class` is the first binding, `binding`.
fn bundled_worker<'a>(
    mut service: service::Builder<'a>,
    name: &str,
    module: &str,
    flags: &[&str],
    class: Option<(&str, &str)>,
    n: u32,
) -> capnp::struct_list::Builder<'a, worker::binding::Owned> {
    service.set_name(name);
    let mut w = service.init_worker();
    let mut m = w.reborrow().init_modules(1).get(0);
    m.set_name(module);
    m.set_bundled(format!("/workerd/{module}"));
    w.set_compatibility_date("2026-01-01");
    let mut list = w.reborrow().init_compatibility_flags(flags.len() as u32);
    for (i, flag) in (0u32..).zip(flags) {
        list.set(i, flag);
    }
    if let Some((_, class)) = class {
        let mut ns = w.reborrow().init_durable_object_namespaces(1).get(0);
        ns.set_class_name(class);
        ns.set_unique_key(class);
        ns.set_enable_sql(true);
    }
    w.reborrow()
        .init_durable_object_storage()
        .set_local_disk(format!("{name}-storage"));
    let mut bindings = w.init_bindings(n);
    if let Some((binding, class)) = class {
        let mut b = bindings.reborrow().get(0);
        b.set_name(binding);
        b.init_durable_object_namespace().set_class_name(class);
    }
    bindings
}

/// The cron Worker `<target>-cron` at `services[at]`, running `crons` on the Worker `target`,
/// then its disk `dir`.
fn cron_services(
    services: &mut capnp::struct_list::Builder<'_, service::Owned>,
    at: u32,
    target: &str,
    dir: &Path,
    crons: &[String],
) -> Result<(), String> {
    // The target's binding sits beside these two.
    if matches!(target, "SCHEDULE" | "CRONS") {
        return Err(format!(
            "A Worker named {target} cannot have triggers.crons."
        ));
    }
    let name = format!("{target}-cron");
    create_state_dir(dir)?;
    disk_service(
        services.reborrow().get(at + 1),
        &format!("{name}-storage"),
        dir,
    );
    let flags = ["service_binding_extra_handlers"];
    let service = services.reborrow().get(at);
    let class = Some(("SCHEDULE", "Schedule"));
    let mut bindings = bundled_worker(service, &name, "cron.js", &flags, class, 3);
    let mut list = bindings.reborrow().get(1);
    list.set_name("CRONS");
    list.set_json(serde_json::json!({ target: crons }).to_string());
    let mut binding = bindings.get(2);
    binding.set_name(target);
    binding.init_service().set_name(target);
    Ok(())
}

/// The bindings of the worker `name`: `vars`, Durable Objects, those to the bundled Workers
/// [`bundled_services`] names after it, Secrets Store secrets, and rate limits.
fn set_bindings(worker: worker::Builder<'_>, wrangler: &Wrangler, name: &str) {
    let kv = &wrangler.kv_namespaces;
    let r2 = &wrangler.r2_buckets;
    let producers = &wrangler.queues.producers;
    let workflows = &wrangler.workflows;
    let d1 = &wrangler.d1_databases;
    let secrets = &wrangler.secrets_store_secrets;
    let limits = &wrangler.ratelimits;
    let do_bindings = wrangler
        .durable_objects
        .as_ref()
        .map_or(&[][..], |d| &d.bindings);
    let n = wrangler.vars.len() + do_bindings.len() + kv.len() + r2.len();
    let assets = wrangler.assets.as_ref().and_then(|a| a.binding.as_ref());
    let m = n + producers.len() + workflows.len();
    let at = m + d1.len();
    let limits_at = at + secrets.len();
    let mut bs =
        worker.init_bindings((limits_at + limits.len() + usize::from(assets.is_some())) as u32);
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
        designator.set_name(format!("{name}-kv"));
        let props = serde_json::json!({ "namespace": id });
        designator.init_props().set_json(props.to_string());
    }
    for (i, r) in r2.iter().enumerate() {
        let at = wrangler.vars.len() + do_bindings.len() + kv.len() + i;
        let mut b = bs.reborrow().get(at as u32);
        b.set_name(&r.binding);
        let bucket = r.bucket_name.as_ref().unwrap_or(&r.binding);
        let mut designator = b.init_r2_bucket();
        designator.set_name(format!("{name}-r2"));
        let props = serde_json::json!({ "bucket": bucket });
        designator.init_props().set_json(props.to_string());
    }
    for (i, p) in producers.iter().enumerate() {
        let mut b = bs.reborrow().get((n + i) as u32);
        b.set_name(&p.binding);
        let mut designator = b.init_queue();
        designator.set_name(format!("{name}-queues"));
        let props = serde_json::json!({ "queue": p.queue });
        designator.init_props().set_json(props.to_string());
    }
    for (i, w) in workflows.iter().enumerate() {
        let mut b = bs.reborrow().get((n + producers.len() + i) as u32);
        b.set_name(&w.binding);
        let mut wrapped = b.init_wrapped();
        wrapped.set_module_name("cloudflare-internal:workflows-api");
        let mut fetcher = wrapped.init_inner_bindings(1).get(0);
        fetcher.set_name("fetcher");
        workflow_binding(fetcher.init_service(), name, &w.name);
    }
    for (i, d) in d1.iter().enumerate() {
        let mut b = bs.reborrow().get((m + i) as u32);
        b.set_name(&d.binding);
        let mut wrapped = b.init_wrapped();
        wrapped.set_module_name("cloudflare-internal:d1-api");
        let mut fetcher = wrapped.init_inner_bindings(1).get(0);
        fetcher.set_name("fetcher");
        let mut designator = fetcher.init_service();
        designator.set_name(format!("{name}-d1"));
        let id = (d.preview_database_id.as_ref())
            .or(d.database_id.as_ref())
            .unwrap_or(&d.binding);
        let props = serde_json::json!({ "database": id });
        designator.init_props().set_json(props.to_string());
    }
    for (i, secret) in secrets.iter().enumerate() {
        set_secret(bs.reborrow().get((at + i) as u32), secret, name);
    }
    for (i, r) in limits.iter().enumerate() {
        set_ratelimit(bs.reborrow().get((limits_at + i) as u32), r, name);
    }
    if let Some(binding) = assets {
        let mut b = bs.get((limits_at + limits.len()) as u32);
        b.set_name(binding);
        let mut designator = b.init_service();
        designator.set_name(format!("{name}-assets"));
        designator.set_entrypoint("AssetWorker");
    }
}

/// The Secrets Store secret `secret` of the worker `name`: the entrypoint `SecretsStoreSecret` of
/// `<name>-secrets-store`, the store and the secret's name in its props.
fn set_secret(mut b: worker::binding::Builder<'_>, secret: &SecretsStoreSecret, name: &str) {
    b.set_name(&secret.binding);
    let mut designator = b.init_service();
    designator.set_name(format!("{name}-secrets-store"));
    designator.set_entrypoint("SecretsStoreSecret");
    let props = serde_json::json!({
        "store_id": secret.store_id,
        "secret_name": secret.secret_name,
    });
    designator.init_props().set_json(props.to_string());
}

/// The rate limit `r` of the worker `name`: the entrypoint `RateLimit` of
/// `<name>-ratelimit`, its settings in its props.
fn set_ratelimit(mut b: worker::binding::Builder<'_>, r: &Ratelimit, name: &str) {
    b.set_name(&r.name);
    let mut designator = b.init_service();
    designator.set_name(format!("{name}-ratelimit"));
    designator.set_entrypoint("RateLimit");
    let props = serde_json::json!({
        "namespace": r.namespace_id,
        "limit": r.simple.limit,
        "period": r.simple.period,
    });
    designator.init_props().set_json(props.to_string());
}

/// The Workflow API of the Workflow `workflow` of the worker `name`: the entrypoint
/// `WorkflowBinding` of `<name>-workflows`, the Workflow in its props.
fn workflow_binding(mut designator: service_designator::Builder<'_>, name: &str, workflow: &str) {
    designator.set_name(format!("{name}-workflows"));
    designator.set_entrypoint("WorkflowBinding");
    let props = serde_json::json!({ "workflow": workflow });
    designator.init_props().set_json(props.to_string());
}

/// The worker `name`'s `workflowsEngine`: the class `Engine` of `<name>-workflows` runs each of
/// `workflows`.
fn set_workflows_engine(worker: worker::Builder<'_>, name: &str, workflows: &[Workflow]) {
    let mut engine = worker.init_workflows_engine();
    let mut class = engine.reborrow().init_actor_class();
    class.set_name(format!("{name}-workflows"));
    class.set_entrypoint("Engine");
    let mut list = engine.init_workflows(workflows.len() as u32);
    for (i, w) in (0u32..).zip(workflows) {
        let mut workflow = list.reborrow().get(i);
        workflow.set_class_name(&w.class_name);
        workflow.set_name(&w.name);
        workflow_binding(workflow.init_binding_service(), name, &w.name);
    }
}

const PRESERVE_NON_RETRYABLE: &str = "workflows_preserve_non_retryable_error_message";

/// The Workflows Worker `<target>-workflows` at `services[at]`, then its disk under `dir`, which
/// holds the instances: a namespace binding to each Workflow's instances, named after it.
fn workflows_services(
    services: &mut capnp::struct_list::Builder<'_, service::Owned>,
    at: u32,
    target: &str,
    dir: &Path,
    wrangler: &Wrangler,
) -> Result<(), String> {
    let workflows = &wrangler.workflows;
    let name = format!("{target}-workflows");
    create_state_dir(dir)?;
    disk_service(
        services.reborrow().get(at + 1),
        &format!("{name}-storage"),
        dir,
    );
    let mut config = serde_json::Map::new();
    for w in workflows {
        let unsupported = |field| {
            Err(format!(
                "workflows: \"{}\": {field} is not supported",
                w.name
            ))
        };
        let ok = |c: char| c.is_ascii_alphanumeric() || c == '_';
        let mut chars = w.name.chars();
        if w.name.len() > 64 || !chars.next().is_some_and(ok) || !chars.all(|c| ok(c) || c == '-') {
            return Err(format!(
                "workflows: \"{}\": Workflow names must be 1-64 characters \
                 long, start with a letter, number, or underscore, and may \
                 only contain letters, numbers, underscores, or hyphens.",
                w.name
            ));
        }
        if w.script_name.as_ref().is_some_and(|s| s != target) {
            return unsupported("script_name");
        }
        if w.schedules.is_some() {
            return unsupported("schedules");
        }
        let mut settings = serde_json::json!({});
        if let Some(steps) = w.limits.as_ref().and_then(|l| l.steps) {
            settings["steps"] = steps.into();
        }
        if let Some(r) = &w.default_retention {
            settings["retention"] =
                serde_json::json!({ "success": r.success_retention, "error": r.error_retention });
        }
        config.insert(w.name.clone(), settings);
    }
    let service = services.reborrow().get(at);
    let n = workflows.len() as u32;
    // The engine reports a step's NonRetryableError as the Worker's flags ask.
    let flag = |f: &str| wrangler.compatibility_flags.iter().any(|g| g == f);
    let preserve = flag(PRESERVE_NON_RETRYABLE)
        || (wrangler.compatibility_date.as_str() >= "2026-05-14"
            && !flag("workflows_replace_non_retryable_error_message"));
    let flags: &[&str] = if preserve {
        &[PRESERVE_NON_RETRYABLE]
    } else {
        &[]
    };
    let mut bindings = bundled_worker(service, &name, "workflows.js", flags, None, n + 1);
    for (i, w) in (0u32..).zip(workflows) {
        let mut b = bindings.reborrow().get(i);
        b.set_name(format!("workflow:{}", w.name));
        let mut ns = b.init_durable_object_namespace();
        ns.set_class_name(workflow_namespace_key(&w.name));
        ns.set_service_name(target);
    }
    let mut b = bindings.get(n);
    b.set_name("WORKFLOWS");
    b.set_json(serde_json::Value::Object(config).to_string());
    Ok(())
}

/// The Queues Worker `<target>-queues` at `services[at]`, then its disk under `dir`. Its
/// consumers deliver to `target`, bound as `worker`.
fn queues_services(
    services: &mut capnp::struct_list::Builder<'_, service::Owned>,
    at: u32,
    target: &str,
    dir: &Path,
    queues: &Queues,
) -> Result<(), String> {
    let name = format!("{target}-queues");
    create_state_dir(dir)?;
    disk_service(
        services.reborrow().get(at + 1),
        &format!("{name}-storage"),
        dir,
    );
    let mut consumers = serde_json::Map::new();
    for c in &queues.consumers {
        let mut settings = serde_json::json!({ "consumer": "worker" });
        for (key, v) in [
            ("maxBatchSize", c.max_batch_size),
            ("maxBatchTimeout", c.max_batch_timeout),
            ("maxRetries", c.max_retries),
            ("retryDelay", c.retry_delay),
            ("maxConcurrency", c.max_concurrency),
        ] {
            if let Some(v) = v {
                settings[key] = v.into();
            }
        }
        if let Some(dlq) = &c.dead_letter_queue {
            settings["deadLetterQueue"] = dlq.as_str().into();
        }
        if consumers.insert(c.queue.clone(), settings).is_some() {
            return Err(format!(
                "queues: queue \"{}\" has more than one consumer",
                c.queue
            ));
        }
    }
    let consumed = !consumers.is_empty();
    let flags = ["service_binding_extra_handlers"];
    let service = services.reborrow().get(at);
    let class = Some(("SHARD", "QueueShard"));
    let n = 2 + u32::from(consumed);
    let mut bindings = bundled_worker(service, &name, "queues.js", &flags, class, n);
    let mut list = bindings.reborrow().get(1);
    list.set_name("CONSUMERS");
    list.set_json(serde_json::Value::Object(consumers).to_string());
    if consumed {
        let mut binding = bindings.get(2);
        binding.set_name("worker");
        binding.init_service().set_name(target);
    }
    Ok(())
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
    fs::create_dir_all(&out).map_err(|e| {
        format!(
            "{}: {e}. esbuild bundles `main` there. Set \"no_bundle\": true in the wrangler config \
             to serve the modules as they are.",
            out.display()
        )
    })?;
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
