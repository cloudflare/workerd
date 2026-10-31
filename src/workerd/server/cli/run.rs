// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! `workerd run`: one Worker, from a JavaScript or TypeScript file or from source (`--eval`, or
//! stdin as `-`), served or tested (`--test`) with no config file.
//!
//! The code is bundled as a wrangler config bundles its `main` ([`wrangler::bundle`]): a file
//! from its directory, source as TypeScript from the working directory, which its relative
//! imports resolve against.
//!
//! The rest of the config comes from a template (`--config`), a `Config` with exactly one Worker
//! that has no modules, which the code goes into. Without one, the template is a lone Worker named
//! `main`. The command line's compatibility date replaces the template's, its compatibility flags
//! and autogates add to the template's, and, to be served, a template without sockets gets one on
//! `localhost:<port>`.

use std::fs;
use std::path::Path;

use workerd_capnp::config;
use workerd_capnp::service;
use workerd_capnp::worker;

use crate::config::Config;
use crate::process::Process;
use crate::wrangler;
use crate::wrangler::Source;

/// The extensions of the entry modules `workerd run` accepts: the JavaScript and TypeScript
/// files esbuild bundles.
pub const EXTENSIONS: &[&str] = &["js", "mjs", "cjs", "jsx", "ts", "mts", "cts", "tsx"];

/// How many consecutive ports, from [`wrangler::DEFAULT_PORT`], the socket `workerd run` adds
/// may try when no port is given: the first one free is used.
pub const PORT_ATTEMPTS: u16 = 10;

/// The name of the socket `workerd run` adds to a template without sockets.
pub const SOCKET: &str = "http";

/// The prefix of the names in a config's `autogates`.
const AUTOGATE_PREFIX: &str = "workerd-autogate-";

/// Whether `workerd run` accepts `path` as an entry module, by its extension.
pub fn is_runnable(path: &str) -> bool {
    let ext = Path::new(path).extension().unwrap_or_default();
    EXTENSIONS.iter().any(|e| ext.eq_ignore_ascii_case(e))
}

/// The Worker's code.
pub enum Entry {
    /// A file.
    File(String),
    /// Source, JavaScript or TypeScript; `name` (`--eval`, `-`) stands for it in errors.
    Source { name: &'static str, source: String },
}

/// The template, compiled, and the path it was compiled from, for errors.
pub struct Template {
    pub path: String,
    pub config: Config,
}

/// What `workerd run` takes from the command line besides the code and the template.
pub struct Options {
    /// Whether a template without sockets gets one: the config is to be served, not tested.
    pub listen: bool,
    /// The port of the socket a template without sockets gets; [`wrangler::DEFAULT_PORT`] if
    /// none. An error with a template that has sockets.
    pub port: Option<u16>,
    /// Replaces the template's compatibility date.
    pub compatibility_date: Option<String>,
    /// The compatibility date of a Worker with none, from either.
    pub default_compatibility_date: String,
    pub compatibility_flags: Vec<String>,
    /// Autogate names, with or without [`AUTOGATE_PREFIX`].
    pub autogates: Vec<String>,
}

/// The config `workerd run` builds.
pub struct Run {
    pub config: Config,
    /// The Worker the code went into.
    pub worker: String,
    /// Whether the config's socket is [`SOCKET`] on the default port, which may move to the next
    /// free one ([`PORT_ATTEMPTS`]).
    pub default_port: bool,
}

/// The config of `workerd run`. Every file it depends on is registered with `process`'s watcher.
pub fn config(
    entry: &Entry,
    template: Option<&Template>,
    options: &Options,
    process: &Process,
) -> Result<Run, String> {
    let sources = match entry {
        Entry::File(file) => bundle(file, process).map_err(|e| format!("{file}: {e}"))?,
        Entry::Source { name, source } => {
            let root = std::env::current_dir().map_err(|e| e.to_string())?;
            wrangler::bundle(
                &wrangler::esbuild(&root),
                &root,
                wrangler::Input::Source { name, source },
                &wrangler::watcher(process),
            )
            .map_err(|e| format!("{name}: {e}"))?
        }
    };
    if let Some(template) = template {
        let message = template.config.message().map_err(|e| e.to_string())?;
        return fill(
            message.get_root().map_err(|e| e.to_string())?,
            &sources,
            options,
        )
        .map_err(|e| format!("{}: {e}", template.path));
    }
    let mut message = capnp::message::Builder::new_default();
    let mut service = message
        .init_root::<config::Builder>()
        .init_services(1)
        .get(0);
    service.set_name("main");
    service.init_worker();
    let template = message.get_root_as_reader().map_err(|e| e.to_string())?;
    fill(template, &sources, options)
}

/// `file`, bundled with esbuild from the directory it is in.
fn bundle(file: &str, process: &Process) -> Result<Vec<Source>, String> {
    let path = fs::canonicalize(file).map_err(|e| e.to_string())?;
    let (Some(root), Some(main)) = (path.parent(), path.file_name()) else {
        return Err("not a file.".to_owned());
    };
    wrangler::bundle(
        &wrangler::esbuild(root),
        root,
        wrangler::Input::File(&main.to_string_lossy()),
        &wrangler::watcher(process),
    )
}

/// `template` with `sources` as the code of its one Worker without modules, and `options`.
fn fill(template: config::Reader, sources: &[Source], options: &Options) -> Result<Run, String> {
    let e = |e: capnp::Error| e.to_string();
    let services = template.get_services().map_err(e)?;
    let mut empty = Vec::new();
    for (i, s) in services.iter().enumerate() {
        if let Ok(service::Which::Worker(Ok(w))) = s.which()
            && let Ok(worker::Which::Modules(Ok(modules))) = w.which()
            && modules.is_empty()
        {
            let name = s
                .get_name()
                .map_err(e)?
                .to_string()
                .map_err(|e| e.to_string())?;
            empty.push((i as u32, name));
        }
    }
    let (index, name) = match <[_; 1]>::try_from(empty) {
        Ok([only]) => only,
        Err(empty) if empty.is_empty() => {
            return Err("Expected a Worker with no modules, for the code to run.".to_owned());
        }
        Err(empty) => {
            let names: Vec<_> = empty.into_iter().map(|(_, n)| n).collect();
            return Err(format!(
                "Expected one Worker with no modules, for the code to run; found {}.",
                names.join(", ")
            ));
        }
    };
    let has_sockets = !template.get_sockets().map_err(e)?.is_empty();
    if options.listen
        && has_sockets
        && let Some(port) = options.port
    {
        return Err(format!(
            "--port={port}: The template has its own sockets; use --socket-addr to move them."
        ));
    }

    let mut message = capnp::message::Builder::new_default();
    message.set_root(template).map_err(e)?;
    let mut conf = message.get_root::<config::Builder>().map_err(e)?;

    let autogates = merged(
        conf.reborrow().get_autogates().map_err(e)?.into_reader(),
        options.autogates.iter().map(|name| {
            if name.starts_with(AUTOGATE_PREFIX) {
                name.clone()
            } else {
                format!("{AUTOGATE_PREFIX}{name}")
            }
        }),
    )?;
    conf.set_autogates(&autogates[..]).map_err(e)?;

    let service = conf.reborrow().get_services().map_err(e)?.get(index);
    let service::WhichBuilder::Worker(worker) = service.which().map_err(|e| e.to_string())? else {
        unreachable!("service {index} is a Worker");
    };
    let mut worker = worker.map_err(e)?;
    if let Some(date) = &options.compatibility_date {
        worker.set_compatibility_date(date);
    } else if !worker.has_compatibility_date() {
        worker.set_compatibility_date(&options.default_compatibility_date);
    }
    let flags = merged(
        worker
            .reborrow()
            .get_compatibility_flags()
            .map_err(e)?
            .into_reader(),
        options.compatibility_flags.iter().cloned(),
    )?;
    worker.set_compatibility_flags(&flags[..]).map_err(e)?;
    wrangler::set_modules(worker, sources)?;

    let added = options.listen && !has_sockets;
    if added {
        let mut socket = conf.init_sockets(1).get(0);
        socket.set_name(SOCKET);
        socket.set_address(format!(
            "localhost:{}",
            options.port.unwrap_or(wrangler::DEFAULT_PORT)
        ));
        socket.reborrow().init_http();
        socket.init_service().set_name(&name);
    }

    Ok(Run {
        config: Config::from_builder(&message).map_err(e)?,
        worker: name,
        default_port: added && options.port.is_none(),
    })
}

/// `list` followed by those of `more` it does not have yet.
fn merged(
    list: capnp::text_list::Reader,
    more: impl Iterator<Item = String>,
) -> Result<Vec<String>, String> {
    let mut merged: Vec<String> = list
        .iter()
        .map(|t| Ok(t?.to_string()?))
        .collect::<capnp::Result<_>>()
        .map_err(|e| e.to_string())?;
    for item in more {
        if !merged.contains(&item) {
            merged.push(item);
        }
    }
    Ok(merged)
}

#[cfg(test)]
#[path = "run-test.rs"]
mod tests;
