//! Configs synthesized from a project rather than written as a Cap'n Proto schema file: a single
//! source file (`workerd run app.js`) or a Wrangler config (`workerd serve wrangler.jsonc`).
//!
//! A [`Project`] is the small, runtime-independent description of a Worker: its entry module, the
//! files beside it, its compatibility settings and its bindings. [`Project::to_config`] turns it
//! into the encoded `Config` message the C++ driver runs, and [`Project::to_capnp_text`] prints
//! the same config as a schema file the user can keep (`--print-config`), with every module as an
//! `embed` of the original file so the printed config stays small and editable.

use std::collections::BTreeSet;
use std::fmt::Write as _;
use std::fs;
use std::path::Path;
use std::path::PathBuf;

use serde_json::Value;
use workerd_capnp::config;

use crate::config::Config;
use crate::config::words_from_bytes;

/// Directories never scanned for modules.
const SKIPPED_DIRS: &[&str] = &[
    "node_modules",
    ".git",
    ".wrangler",
    ".workerd",
    ".workerd-state",
];

/// The port `workerd run` listens on without `--port`.
pub const DEFAULT_RUN_PORT: u16 = 8080;
/// The port a Wrangler project listens on without `dev.port` or `--port`: Wrangler's own default.
pub const DEFAULT_WRANGLER_PORT: u16 = 8787;

/// A module found in the project directory.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Module {
    /// The module's name in the Worker: its path relative to the project root, with `/`.
    pub name: String,
    /// The file it came from.
    pub path: PathBuf,
    pub kind: ModuleKind,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ModuleKind {
    EsModule,
    /// TypeScript, run through the runtime's type stripping (`typescript_strip_types`).
    TypeScript,
    CommonJs,
    Json,
    Text,
    Data,
    Wasm,
    Python,
}

impl ModuleKind {
    /// The kind a file gets from its extension, or `None` for files that are not modules.
    fn from_extension(ext: &str) -> Option<Self> {
        Some(match ext {
            "js" | "mjs" => Self::EsModule,
            "ts" | "mts" => Self::TypeScript,
            "cjs" => Self::CommonJs,
            "json" => Self::Json,
            "txt" | "html" | "htm" | "css" | "md" | "svg" | "csv" | "xml" => Self::Text,
            "wasm" => Self::Wasm,
            "py" => Self::Python,
            "bin" | "dat" | "png" | "jpg" | "jpeg" | "gif" | "ico" | "woff" | "woff2" => Self::Data,
            _ => return None,
        })
    }

    /// The field of `Worker.Module` this kind is stored in, as named in the schema file.
    const fn capnp_field(self) -> &'static str {
        match self {
            Self::EsModule | Self::TypeScript => "esModule",
            Self::CommonJs => "commonJsModule",
            Self::Json => "json",
            Self::Text => "text",
            Self::Data => "data",
            Self::Wasm => "wasm",
            Self::Python => "pythonModule",
        }
    }

    const fn is_text(self) -> bool {
        !matches!(self, Self::Data | Self::Wasm)
    }
}

/// A `vars` entry: a string becomes a `text` binding, anything else a `json` binding.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Var {
    pub name: String,
    pub value: Value,
}

/// A `durable_objects.bindings` entry.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DurableObjectBinding {
    pub name: String,
    pub class_name: String,
    /// Another Worker's name, when the class lives there (`script_name`).
    pub script_name: Option<String>,
}

/// A `services` entry: a binding to another Worker (or this one) in the same config.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ServiceBinding {
    pub name: String,
    pub service: String,
    pub entrypoint: Option<String>,
}

/// A Worker described by its project files.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Project {
    /// The Worker's name: Wrangler's `name`, or the entry file's stem.
    pub name: String,
    /// The directory module names are relative to.
    pub root: PathBuf,
    /// The entry module and the other files beside it; the entry module is first.
    pub modules: Vec<Module>,
    pub compatibility_date: String,
    pub compatibility_flags: Vec<String>,
    pub vars: Vec<Var>,
    pub durable_objects: Vec<DurableObjectBinding>,
    /// Durable Object classes with SQL-backed storage (`new_sqlite_classes` in `migrations`).
    pub sqlite_classes: BTreeSet<String>,
    /// Durable Object classes declared in migrations without a binding; they still get a
    /// namespace so the Worker's exports are checked.
    pub extra_classes: BTreeSet<String>,
    pub services: Vec<ServiceBinding>,
    pub port: u16,
    /// Where Durable Object storage goes: a directory to persist to, or memory.
    pub persist: Option<PathBuf>,
    /// Config the standalone runtime cannot honour, reported to the user and skipped.
    pub warnings: Vec<String>,
    /// The file this project was read from, when it was a Wrangler config.
    pub config_file: Option<PathBuf>,
}

/// Command-line overrides applied on top of a project.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Overrides {
    pub name: Option<String>,
    pub port: Option<u16>,
    pub compatibility_date: Option<String>,
    pub compatibility_flags: Vec<String>,
    pub persist: Option<PathBuf>,
}

/// Why a project could not be read.
#[derive(Debug)]
pub struct Error(pub String);

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.0)
    }
}

impl From<std::io::Error> for Error {
    fn from(error: std::io::Error) -> Self {
        Self(error.to_string())
    }
}

/// Whether `path` names a Wrangler config rather than a schema-file config, by extension.
pub fn is_wrangler_config(path: &str) -> bool {
    Path::new(path)
        .extension()
        .and_then(|ext| ext.to_str())
        .is_some_and(|ext| {
            ["json", "jsonc", "toml"]
                .iter()
                .any(|known| ext.eq_ignore_ascii_case(known))
        })
}

/// Whether `path` names a source file `workerd run` can serve directly.
pub fn is_source_file(path: &str) -> bool {
    Path::new(path)
        .extension()
        .and_then(|ext| ext.to_str())
        .is_some_and(|ext| {
            matches!(
                ModuleKind::from_extension(&ext.to_ascii_lowercase()),
                Some(
                    ModuleKind::EsModule
                        | ModuleKind::TypeScript
                        | ModuleKind::CommonJs
                        | ModuleKind::Python
                )
            )
        })
}

impl Project {
    /// A project for one source file: the file is the entry module and the files beside it are
    /// the other modules, so relative imports work.
    pub fn from_source_file(
        file: &str,
        default_compatibility_date: &str,
        overrides: &Overrides,
    ) -> Result<Self, Error> {
        let path = fs::canonicalize(file).map_err(|_| Error(format!("{file}: No such file.")))?;
        if !path.is_file() {
            return Err(Error(format!("{file}: not a file.")));
        }
        let root = path
            .parent()
            .ok_or_else(|| Error(format!("{file}: has no parent directory.")))?
            .to_path_buf();
        let stem = path
            .file_stem()
            .and_then(|stem| stem.to_str())
            .unwrap_or("worker")
            .to_owned();
        let modules = collect_modules(&root, &path)?;
        let mut project = Self {
            name: stem,
            root,
            modules,
            compatibility_date: default_compatibility_date.to_owned(),
            // nodejs_compat is the default from 2026-08-04; earlier dates need it spelled out.
            compatibility_flags: if default_compatibility_date < "2026-08-04" {
                vec!["nodejs_compat".to_owned()]
            } else {
                Vec::new()
            },
            vars: Vec::new(),
            durable_objects: Vec::new(),
            sqlite_classes: BTreeSet::new(),
            extra_classes: BTreeSet::new(),
            services: Vec::new(),
            port: DEFAULT_RUN_PORT,
            persist: None,
            warnings: Vec::new(),
            config_file: None,
        };
        project.apply(overrides);
        Ok(project)
    }

    /// A project from a Wrangler config (`wrangler.json` / `wrangler.jsonc`).
    #[expect(
        clippy::too_many_lines,
        reason = "one straight read of each Wrangler section, in the order the docs list them"
    )]
    pub fn from_wrangler_config(
        file: &str,
        default_compatibility_date: &str,
        overrides: &Overrides,
    ) -> Result<Self, Error> {
        let config_path =
            fs::canonicalize(file).map_err(|_| Error(format!("{file}: No such file.")))?;
        if file.to_ascii_lowercase().ends_with(".toml") {
            return Err(Error(format!(
                "{file}: wrangler.toml is not supported yet; convert it to wrangler.jsonc \
                 (https://developers.cloudflare.com/workers/wrangler/configuration/)."
            )));
        }
        let text = fs::read_to_string(&config_path)?;
        let json: Value = serde_json::from_str(&strip_jsonc(&text))
            .map_err(|error| Error(format!("{file}: {error}")))?;
        let Value::Object(top) = &json else {
            return Err(Error(format!(
                "{file}: expected a JSON object at the top level."
            )));
        };
        let root = config_path
            .parent()
            .ok_or_else(|| Error(format!("{file}: has no parent directory.")))?
            .to_path_buf();

        let main = top
            .get("main")
            .and_then(Value::as_str)
            .ok_or_else(|| Error(format!("{file}: \"main\" is required.")))?;
        let main_path = root.join(main);
        if !main_path.is_file() {
            return Err(Error(format!(
                "{file}: \"main\" points to {main}, which does not exist. If the project needs \
                 bundling, run `wrangler deploy --dry-run --outdir dist` and point \"main\" at the \
                 bundle."
            )));
        }
        let main_path = fs::canonicalize(&main_path)?;

        let name = top.get("name").and_then(Value::as_str).map_or_else(
            || {
                main_path
                    .file_stem()
                    .and_then(|stem| stem.to_str())
                    .unwrap_or("worker")
                    .to_owned()
            },
            str::to_owned,
        );

        let mut warnings = Vec::new();

        let compatibility_date = top
            .get("compatibility_date")
            .and_then(Value::as_str)
            .map_or_else(
                || {
                    warnings.push(format!(
                        "\"compatibility_date\" is not set; using {default_compatibility_date}."
                    ));
                    default_compatibility_date.to_owned()
                },
                str::to_owned,
            );
        let compatibility_flags: Vec<String> = top
            .get("compatibility_flags")
            .and_then(Value::as_array)
            .map(|flags| {
                flags
                    .iter()
                    .filter_map(Value::as_str)
                    .map(str::to_owned)
                    .collect()
            })
            .unwrap_or_default();

        let vars: Vec<Var> = top
            .get("vars")
            .and_then(Value::as_object)
            .map(|vars| {
                vars.iter()
                    .map(|(name, value)| Var {
                        name: name.clone(),
                        value: value.clone(),
                    })
                    .collect()
            })
            .unwrap_or_default();

        let durable_objects: Vec<DurableObjectBinding> = top
            .get("durable_objects")
            .and_then(|d| d.get("bindings"))
            .and_then(Value::as_array)
            .map(|bindings| {
                bindings
                    .iter()
                    .filter_map(|b| {
                        Some(DurableObjectBinding {
                            name: b.get("name")?.as_str()?.to_owned(),
                            class_name: b.get("class_name")?.as_str()?.to_owned(),
                            script_name: b
                                .get("script_name")
                                .and_then(Value::as_str)
                                .map(str::to_owned),
                        })
                    })
                    .collect()
            })
            .unwrap_or_default();

        let mut sqlite_classes = BTreeSet::new();
        let mut extra_classes = BTreeSet::new();
        if let Some(migrations) = top.get("migrations").and_then(Value::as_array) {
            for migration in migrations {
                for class in string_list(migration.get("new_sqlite_classes")) {
                    sqlite_classes.insert(class.clone());
                    extra_classes.insert(class);
                }
                for class in string_list(migration.get("new_classes")) {
                    extra_classes.insert(class);
                }
                for class in string_list(migration.get("deleted_classes")) {
                    extra_classes.remove(&class);
                    sqlite_classes.remove(&class);
                }
            }
        }
        for binding in &durable_objects {
            if binding.script_name.is_none() {
                extra_classes.remove(&binding.class_name);
            }
        }

        let services: Vec<ServiceBinding> = top
            .get("services")
            .and_then(Value::as_array)
            .map(|services| {
                services
                    .iter()
                    .filter_map(|s| {
                        Some(ServiceBinding {
                            name: s.get("binding")?.as_str()?.to_owned(),
                            service: s.get("service")?.as_str()?.to_owned(),
                            entrypoint: s
                                .get("entrypoint")
                                .and_then(Value::as_str)
                                .map(str::to_owned),
                        })
                    })
                    .collect()
            })
            .unwrap_or_default();

        // Everything the standalone runtime has no local implementation of.
        for (key, what) in [
            ("kv_namespaces", "KV namespaces"),
            ("r2_buckets", "R2 buckets"),
            ("d1_databases", "D1 databases"),
            ("queues", "Queues"),
            ("assets", "static assets"),
            ("ai", "Workers AI"),
            ("images", "Images"),
            ("browser", "Browser Rendering"),
            ("vectorize", "Vectorize"),
            ("hyperdrive", "Hyperdrive"),
            ("workflows", "Workflows"),
            ("analytics_engine_datasets", "Analytics Engine"),
            ("send_email", "Email"),
            ("mtls_certificates", "mTLS certificates"),
            ("dispatch_namespaces", "dispatch namespaces"),
            ("pipelines", "Pipelines"),
            ("secrets_store_secrets", "Secrets Store"),
            ("triggers", "cron triggers"),
            ("tail_consumers", "tail consumers"),
        ] {
            if let Some(value) = top.get(key) {
                let names = binding_names(value);
                let suffix = if names.is_empty() {
                    String::new()
                } else {
                    format!(" ({})", names.join(", "))
                };
                warnings.push(format!(
                    "\"{key}\": {what} are not available in standalone workerd yet; skipped{suffix}."
                ));
            }
        }

        let port = top
            .get("dev")
            .and_then(|dev| dev.get("port"))
            .and_then(Value::as_u64)
            .and_then(|port| u16::try_from(port).ok())
            .unwrap_or(DEFAULT_WRANGLER_PORT);

        let modules = collect_modules(&root, &main_path)?;
        let mut project = Self {
            name,
            root,
            modules,
            compatibility_date,
            compatibility_flags,
            vars,
            durable_objects,
            sqlite_classes,
            extra_classes,
            services,
            port,
            persist: None,
            warnings,
            config_file: Some(config_path),
        };
        project.apply(overrides);
        Ok(project)
    }

    fn apply(&mut self, overrides: &Overrides) {
        if let Some(name) = &overrides.name {
            self.name.clone_from(name);
        }
        if let Some(port) = overrides.port {
            self.port = port;
        }
        if let Some(date) = &overrides.compatibility_date {
            self.compatibility_date.clone_from(date);
        }
        for flag in &overrides.compatibility_flags {
            if !self.compatibility_flags.contains(flag) {
                self.compatibility_flags.push(flag.clone());
            }
        }
        if let Some(dir) = &overrides.persist {
            self.persist = Some(dir.clone());
        } else if !self.own_classes().is_empty() {
            // The runtime's in-memory Durable Object storage has no SQLite, so a project with
            // Durable Objects is persisted beside its code, as Wrangler does under .wrangler.
            self.persist = Some(self.root.join(".workerd-state"));
        }
        if self.uses_typescript() {
            let flag = "typescript_strip_types".to_owned();
            if !self.compatibility_flags.contains(&flag) {
                self.compatibility_flags.push(flag);
            }
        }
        if self.uses_python() {
            let flag = "python_workers".to_owned();
            if !self.compatibility_flags.contains(&flag) {
                self.compatibility_flags.push(flag);
            }
        }
    }

    /// Whether any module needs type stripping, which is an experimental feature.
    pub fn uses_typescript(&self) -> bool {
        self.modules
            .iter()
            .any(|m| m.kind == ModuleKind::TypeScript)
    }

    pub fn uses_python(&self) -> bool {
        self.modules
            .first()
            .is_some_and(|m| m.kind == ModuleKind::Python)
    }

    /// Whether the config needs `--experimental` to be accepted by the runtime.
    pub fn needs_experimental(&self) -> bool {
        self.uses_typescript()
    }

    /// The files the config depends on, for `--watch`.
    pub fn watched_files(&self) -> impl Iterator<Item = &Path> {
        self.config_file
            .iter()
            .map(PathBuf::as_path)
            .chain(self.modules.iter().map(|m| m.path.as_path()))
    }

    /// The name of the disk service that backs Durable Object storage under `--persist`.
    fn storage_service_name(&self) -> String {
        format!("{}-storage", self.name)
    }

    /// Every Durable Object class this Worker implements, with its namespace.
    fn own_classes(&self) -> Vec<(String, bool)> {
        let mut classes: Vec<String> = self
            .durable_objects
            .iter()
            .filter(|b| b.script_name.is_none())
            .map(|b| b.class_name.clone())
            .collect();
        for class in &self.extra_classes {
            if !classes.contains(class) {
                classes.push(class.clone());
            }
        }
        classes
            .into_iter()
            .map(|class| {
                let sql = self.sqlite_classes.contains(&class);
                (class, sql)
            })
            .collect()
    }

    fn unique_key(&self, class: &str) -> String {
        format!("{}-{class}", self.name)
    }

    /// The config as an encoded message for the C++ driver, with every module's contents read
    /// from disk.
    #[expect(
        clippy::too_many_lines,
        reason = "fills one capnp message field by field; the builders borrow the message, so splitting it would only pass builders around"
    )]
    pub fn to_config(&self) -> Result<Config, Error> {
        let mut message = capnp::message::Builder::new_default();
        let mut root = message.init_root::<config::Builder>();

        let persist_dir = match &self.persist {
            Some(dir) => {
                fs::create_dir_all(dir)?;
                Some(fs::canonicalize(dir)?)
            }
            None => None,
        };

        let service_count = 1 + usize::from(persist_dir.is_some());
        let mut services = root.reborrow().init_services(list_len(service_count)?);
        {
            let mut service = services.reborrow().get(0);
            service.set_name(self.name.as_str());
            let mut worker = service.init_worker();
            worker.set_compatibility_date(self.compatibility_date.as_str());
            {
                let mut flags = worker
                    .reborrow()
                    .init_compatibility_flags(list_len(self.compatibility_flags.len())?);
                for (i, flag) in self.compatibility_flags.iter().enumerate() {
                    flags.set(list_index(i)?, flag.as_str());
                }
            }
            {
                let mut modules = worker
                    .reborrow()
                    .init_modules(list_len(self.modules.len())?);
                for (i, module) in self.modules.iter().enumerate() {
                    let mut m = modules.reborrow().get(list_index(i)?);
                    m.set_name(module.name.as_str());
                    let bytes = fs::read(&module.path)
                        .map_err(|error| Error(format!("{}: {error}", module.path.display())))?;
                    if module.kind.is_text() {
                        let text = String::from_utf8(bytes).map_err(|_| {
                            Error(format!("{}: not valid UTF-8", module.path.display()))
                        })?;
                        match module.kind {
                            ModuleKind::EsModule | ModuleKind::TypeScript => {
                                m.set_es_module(text.as_str());
                            }
                            ModuleKind::CommonJs => m.set_common_js_module(text.as_str()),
                            ModuleKind::Json => m.set_json(text.as_str()),
                            ModuleKind::Text => m.set_text(text.as_str()),
                            ModuleKind::Python => m.set_python_module(text.as_str()),
                            ModuleKind::Data | ModuleKind::Wasm => unreachable!(),
                        }
                    } else {
                        match module.kind {
                            ModuleKind::Wasm => m.set_wasm(bytes.as_slice()),
                            _ => m.set_data(bytes.as_slice()),
                        }
                    }
                }
            }
            {
                let binding_count =
                    self.vars.len() + self.durable_objects.len() + self.services.len();
                let mut bindings = worker.reborrow().init_bindings(list_len(binding_count)?);
                let mut i = 0;
                for var in &self.vars {
                    let mut b = bindings.reborrow().get(list_index(i)?);
                    b.set_name(var.name.as_str());
                    match &var.value {
                        Value::String(s) => b.set_text(s.as_str()),
                        other => b.set_json(other.to_string().as_str()),
                    }
                    i += 1;
                }
                for d in &self.durable_objects {
                    let mut b = bindings.reborrow().get(list_index(i)?);
                    b.set_name(d.name.as_str());
                    let mut ns = b.init_durable_object_namespace();
                    ns.set_class_name(d.class_name.as_str());
                    if let Some(script) = &d.script_name {
                        ns.set_service_name(script.as_str());
                    }
                    i += 1;
                }
                for s in &self.services {
                    let mut b = bindings.reborrow().get(list_index(i)?);
                    b.set_name(s.name.as_str());
                    let mut designator = b.init_service();
                    designator.set_name(s.service.as_str());
                    if let Some(entrypoint) = &s.entrypoint {
                        designator.set_entrypoint(entrypoint.as_str());
                    }
                    i += 1;
                }
            }
            let classes = self.own_classes();
            if !classes.is_empty() {
                let mut namespaces = worker
                    .reborrow()
                    .init_durable_object_namespaces(list_len(classes.len())?);
                for (i, (class, sql)) in classes.iter().enumerate() {
                    let mut ns = namespaces.reborrow().get(list_index(i)?);
                    ns.set_class_name(class.as_str());
                    ns.set_unique_key(self.unique_key(class).as_str());
                    ns.set_enable_sql(*sql);
                }
                let mut storage = worker.reborrow().init_durable_object_storage();
                if persist_dir.is_some() {
                    storage.set_local_disk(self.storage_service_name().as_str());
                } else {
                    storage.set_in_memory(());
                }
            }
        }
        if let Some(dir) = &persist_dir {
            let mut service = services.reborrow().get(1);
            service.set_name(self.storage_service_name().as_str());
            let mut disk = service.init_disk();
            disk.set_path(dir.to_string_lossy().as_ref());
            disk.set_writable(true);
        }

        let mut sockets = root.reborrow().init_sockets(1);
        let mut socket = sockets.reborrow().get(0);
        socket.set_name("http");
        socket.set_address(format!("*:{}", self.port).as_str());
        socket.reborrow().init_http();
        socket.init_service().set_name(self.name.as_str());

        Config::from_words(&words_from_bytes(
            &capnp::serialize::write_message_to_words(&message),
        ))
        .map_err(|error| Error(format!("synthesized config is malformed: {error}")))
    }

    /// The config as a schema file, with modules as `embed`s of the project files relative to
    /// the project root. Written to the root, it runs as `workerd serve <file>`.
    #[expect(
        clippy::too_many_lines,
        reason = "prints one schema file top to bottom; the order of the output is the structure"
    )]
    pub fn to_capnp_text(&self) -> String {
        let mut out = String::new();
        let worker_const = const_name(&self.name);
        let _ = writeln!(
            out,
            "# Generated by `workerd` from {}.",
            self.origin_description()
        );
        let _ = writeln!(
            out,
            "# Save this next to your code and run it with `workerd serve <file>`.\n"
        );
        let _ = writeln!(out, "using Workerd = import \"/workerd/workerd.capnp\";\n");
        let _ = writeln!(out, "const config :Workerd.Config = (");
        let _ = writeln!(out, "  services = [");
        let _ = writeln!(
            out,
            "    (name = {}, worker = .{worker_const}),",
            quote(&self.name)
        );
        if let Some(dir) = &self.persist {
            let _ = writeln!(
                out,
                "    (name = {}, disk = (path = {}, writable = true)),",
                quote(&self.storage_service_name()),
                quote(&self.display_path(dir))
            );
        }
        let _ = writeln!(out, "  ],");
        let _ = writeln!(
            out,
            "  sockets = [ (name = \"http\", address = \"*:{}\", http = (), service = {}) ],",
            self.port,
            quote(&self.name)
        );
        let _ = writeln!(out, ");\n");

        let _ = writeln!(out, "const {worker_const} :Workerd.Worker = (");
        let _ = writeln!(
            out,
            "  compatibilityDate = {},",
            quote(&self.compatibility_date)
        );
        if !self.compatibility_flags.is_empty() {
            let flags: Vec<String> = self.compatibility_flags.iter().map(|f| quote(f)).collect();
            let _ = writeln!(out, "  compatibilityFlags = [{}],", flags.join(", "));
        }
        let _ = writeln!(out, "  modules = [");
        for module in &self.modules {
            let _ = writeln!(
                out,
                "    (name = {}, {} = embed {}),",
                quote(&module.name),
                module.kind.capnp_field(),
                quote(&module.name)
            );
        }
        let _ = writeln!(out, "  ],");

        let has_bindings =
            !self.vars.is_empty() || !self.durable_objects.is_empty() || !self.services.is_empty();
        if has_bindings {
            let _ = writeln!(out, "  bindings = [");
            for var in &self.vars {
                match &var.value {
                    Value::String(s) => {
                        let _ = writeln!(
                            out,
                            "    (name = {}, text = {}),",
                            quote(&var.name),
                            quote(s)
                        );
                    }
                    other => {
                        let _ = writeln!(
                            out,
                            "    (name = {}, json = {}),",
                            quote(&var.name),
                            quote(&other.to_string())
                        );
                    }
                }
            }
            for d in &self.durable_objects {
                match &d.script_name {
                    Some(script) => {
                        let _ = writeln!(
                            out,
                            "    (name = {}, durableObjectNamespace = (className = {}, serviceName = {})),",
                            quote(&d.name),
                            quote(&d.class_name),
                            quote(script)
                        );
                    }
                    None => {
                        let _ = writeln!(
                            out,
                            "    (name = {}, durableObjectNamespace = {}),",
                            quote(&d.name),
                            quote(&d.class_name)
                        );
                    }
                }
            }
            for s in &self.services {
                match &s.entrypoint {
                    Some(entrypoint) => {
                        let _ = writeln!(
                            out,
                            "    (name = {}, service = (name = {}, entrypoint = {})),",
                            quote(&s.name),
                            quote(&s.service),
                            quote(entrypoint)
                        );
                    }
                    None => {
                        let _ = writeln!(
                            out,
                            "    (name = {}, service = {}),",
                            quote(&s.name),
                            quote(&s.service)
                        );
                    }
                }
            }
            let _ = writeln!(out, "  ],");
        }

        let classes = self.own_classes();
        if !classes.is_empty() {
            let _ = writeln!(out, "  durableObjectNamespaces = [");
            for (class, sql) in &classes {
                let sql = if *sql { ", enableSql = true" } else { "" };
                let _ = writeln!(
                    out,
                    "    (className = {}, uniqueKey = {}{sql}),",
                    quote(class),
                    quote(&self.unique_key(class))
                );
            }
            let _ = writeln!(out, "  ],");
            if self.persist.is_some() {
                let _ = writeln!(
                    out,
                    "  durableObjectStorage = (localDisk = {}),",
                    quote(&self.storage_service_name())
                );
            } else {
                let _ = writeln!(out, "  durableObjectStorage = (inMemory = void),");
            }
        }
        let _ = writeln!(out, ");");
        out
    }

    /// `path` relative to the project root when it is inside it, for the printed config.
    fn display_path(&self, path: &Path) -> String {
        path.strip_prefix(&self.root).map_or_else(
            |_| path.to_string_lossy().into_owned(),
            |relative| relative.to_string_lossy().into_owned(),
        )
    }

    fn origin_description(&self) -> String {
        match &self.config_file {
            Some(file) => file.file_name().map_or_else(
                || file.display().to_string(),
                |n| n.to_string_lossy().into_owned(),
            ),
            None => self
                .modules
                .first()
                .map_or_else(|| "a project".to_owned(), |m| m.name.clone()),
        }
    }
}

/// The modules under `root`: `main` first, then every other module file, sorted by name.
fn collect_modules(root: &Path, main: &Path) -> Result<Vec<Module>, Error> {
    let main_name = module_name(root, main)?;
    let main_kind = kind_of(main).ok_or_else(|| {
        Error(format!(
            "{}: not a file type workerd can run (expected .js, .mjs, .cjs, .ts or .py).",
            main.display()
        ))
    })?;
    let mut others = Vec::new();
    walk(root, root, &mut others)?;
    others.retain(|m| m.path != main);
    others.sort_by(|a, b| a.name.cmp(&b.name));
    let mut modules = Vec::with_capacity(others.len() + 1);
    modules.push(Module {
        name: main_name,
        path: main.to_path_buf(),
        kind: main_kind,
    });
    modules.extend(others);
    Ok(modules)
}

fn walk(root: &Path, dir: &Path, out: &mut Vec<Module>) -> Result<(), Error> {
    let entries =
        fs::read_dir(dir).map_err(|error| Error(format!("{}: {error}", dir.display())))?;
    for entry in entries {
        let entry = entry.map_err(|error| Error(format!("{}: {error}", dir.display())))?;
        let path = entry.path();
        let file_type = entry
            .file_type()
            .map_err(|error| Error(format!("{}: {error}", path.display())))?;
        let skipped = path
            .file_name()
            .and_then(|n| n.to_str())
            .is_some_and(|n| SKIPPED_DIRS.contains(&n));
        if file_type.is_dir() {
            if !skipped {
                walk(root, &path, out)?;
            }
        } else if file_type.is_file()
            && let Some(kind) = kind_of(&path)
        {
            out.push(Module {
                name: module_name(root, &path)?,
                path,
                kind,
            });
        }
    }
    Ok(())
}

fn kind_of(path: &Path) -> Option<ModuleKind> {
    let ext = path.extension()?.to_str()?.to_ascii_lowercase();
    ModuleKind::from_extension(&ext)
}

/// `path` relative to `root`, with `/` separators, as the module's name in the Worker.
fn module_name(root: &Path, path: &Path) -> Result<String, Error> {
    let relative = path.strip_prefix(root).map_err(|_| {
        Error(format!(
            "{}: not inside the project root {}",
            path.display(),
            root.display()
        ))
    })?;
    Ok(relative
        .components()
        .map(|c| c.as_os_str().to_string_lossy().into_owned())
        .collect::<Vec<_>>()
        .join("/"))
}

fn string_list(value: Option<&Value>) -> Vec<String> {
    value
        .and_then(Value::as_array)
        .map(|items| {
            items
                .iter()
                .filter_map(Value::as_str)
                .map(str::to_owned)
                .collect()
        })
        .unwrap_or_default()
}

/// The binding names declared in a Wrangler section, for a warning about it.
fn binding_names(value: &Value) -> Vec<String> {
    match value {
        Value::Array(items) => items
            .iter()
            .filter_map(|item| item.get("binding").and_then(Value::as_str))
            .map(str::to_owned)
            .collect(),
        Value::Object(object) => object
            .get("binding")
            .and_then(Value::as_str)
            .map(str::to_owned)
            .into_iter()
            .collect(),
        _ => Vec::new(),
    }
}

fn list_len(len: usize) -> Result<u32, Error> {
    u32::try_from(len).map_err(|_| Error("too many items for a config list".to_owned()))
}

fn list_index(index: usize) -> Result<u32, Error> {
    list_len(index)
}

/// A Cap'n Proto constant name for a Worker: a lowerCamelCase identifier.
fn const_name(name: &str) -> String {
    let mut out = String::new();
    let mut upper_next = false;
    for c in name.chars() {
        if c.is_ascii_alphanumeric() {
            if upper_next {
                out.extend(c.to_uppercase());
                upper_next = false;
            } else {
                out.push(c);
            }
        } else {
            upper_next = !out.is_empty();
        }
    }
    if out.is_empty() {
        out.push_str("worker");
    } else if out.starts_with(|c: char| c.is_ascii_digit()) {
        out.insert(0, '_');
    }
    out.push_str("Worker");
    out
}

/// `s` as a Cap'n Proto text literal.
fn quote(s: &str) -> String {
    let mut out = String::with_capacity(s.len() + 2);
    out.push('"');
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => {
                let _ = write!(out, "\\x{:02x}", c as u32);
            }
            c => out.push(c),
        }
    }
    out.push('"');
    out
}

/// JSON with comments and trailing commas (what Wrangler accepts) reduced to strict JSON. Comments
/// are replaced by spaces so error positions stay meaningful.
pub fn strip_jsonc(text: &str) -> String {
    let bytes = text.as_bytes();
    let mut out: Vec<u8> = Vec::with_capacity(bytes.len());
    let mut i = 0;
    let mut in_string = false;
    while i < bytes.len() {
        let b = bytes[i];
        if in_string {
            out.push(b);
            if b == b'\\' && i + 1 < bytes.len() {
                out.push(bytes[i + 1]);
                i += 2;
                continue;
            }
            if b == b'"' {
                in_string = false;
            }
            i += 1;
            continue;
        }
        match b {
            b'"' => {
                in_string = true;
                out.push(b);
                i += 1;
            }
            b'/' if bytes.get(i + 1) == Some(&b'/') => {
                while i < bytes.len() && bytes[i] != b'\n' {
                    i += 1;
                }
            }
            b'/' if bytes.get(i + 1) == Some(&b'*') => {
                i += 2;
                while i < bytes.len() && !(bytes[i] == b'*' && bytes.get(i + 1) == Some(&b'/')) {
                    if bytes[i] == b'\n' {
                        out.push(b'\n');
                    }
                    i += 1;
                }
                i += 2;
            }
            b',' => {
                // A trailing comma: skip it when the next token closes an object or array.
                let mut j = i + 1;
                while j < bytes.len() {
                    match bytes[j] {
                        b' ' | b'\t' | b'\r' | b'\n' => j += 1,
                        b'/' if bytes.get(j + 1) == Some(&b'/') => {
                            while j < bytes.len() && bytes[j] != b'\n' {
                                j += 1;
                            }
                        }
                        b'/' if bytes.get(j + 1) == Some(&b'*') => {
                            j += 2;
                            while j < bytes.len()
                                && !(bytes[j] == b'*' && bytes.get(j + 1) == Some(&b'/'))
                            {
                                j += 1;
                            }
                            j += 2;
                        }
                        _ => break,
                    }
                }
                if !matches!(bytes.get(j), Some(b'}' | b']')) {
                    out.push(b',');
                }
                i += 1;
            }
            _ => {
                out.push(b);
                i += 1;
            }
        }
    }
    String::from_utf8(out).unwrap_or_default()
}

#[cfg(test)]
#[path = "project-test.rs"]
mod tests;
