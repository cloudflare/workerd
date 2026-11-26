// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! What `serve`, `test` and `bench` set up before the server runs.
//!
//! The settings the config gives the process (structured logging, autogates, V8 flags), and the
//! command's options, split into what the process sets up once (`ffi::ProcessOptions`, for
//! `process_start`) and what the worker factory is built with (`ffi::FactoryOptions`, for
//! `factory_new`).

use capnp::message::ReaderOptions;
use capnp::serialize::OwnedSegments;
use workerd_capnp::config;

use crate::Result;
use crate::bridge::ffi;
use crate::config::capnp_error;

/// The options of `serve`, `test` and `bench`: for the process (perfetto, the async trace file),
/// or for the worker factory (async tracing, experimental, Python).
#[expect(
    clippy::struct_excessive_bools,
    reason = "one field per command-line flag"
)]
pub struct ServeOrTestOptions {
    pub perfetto_trace_path: Option<String>,
    pub perfetto_trace_categories: Option<String>,
    pub async_trace_path: Option<String>,
    pub async_trace_stacks: Option<u32>,
    pub async_trace_promises: bool,
    pub experimental: bool,
    pub pyodide_bundle_disk_cache_dir: Option<String>,
    pub python_save_snapshot: bool,
    pub python_save_baseline_snapshot: bool,
    pub python_load_snapshot: Option<String>,
    pub python_snapshot_dir: Option<String>,
}

/// The options of `test`: logging, V8 modes, autogates, the compatibility date every worker
/// gets.
#[expect(
    clippy::struct_excessive_bools,
    reason = "one field per command-line flag"
)]
pub struct TestOptions {
    pub no_verbose: bool,
    pub predictable: bool,
    pub gc_stress: bool,
    pub all_autogates: bool,
    pub compat_date: Option<String>,
}

/// The options of `bench`: logging, autogates, the compatibility dates workers get, whether
/// requests get tail workers. Bench mode is on whenever they are given.
pub struct BenchOptions {
    pub no_verbose: bool,
    pub all_autogates: bool,
    /// Every worker's date; none may specify its own.
    pub compat_date: Option<String>,
    /// The date of workers that don't specify one.
    pub default_compat_date: Option<String>,
    pub trace: bool,
}

/// Whether `config`, an encoded `Config` message, asks for structured logging:
/// `logging.structuredLogging`, or the older top-level `structuredLogging` if `logging` is unset.
pub fn structured_logging(config: &[u64]) -> Result<bool> {
    let message = read(config)?;
    let config = root(&message)?;
    Ok(if config.has_logging() {
        config
            .get_logging()
            .map_err(capnp_error)?
            .get_structured_logging()
    } else {
        config.get_structured_logging()
    })
}

/// The command's options and the config's autogates and V8 flags, as the process and the factory
/// take them.
///
/// `serve_or_test` comes with `test`'s or `bench`'s options, at most one of them given. An
/// autogate or V8 flag that is not UTF-8 is an error.
pub fn command_options(
    config: &[u64],
    serve_or_test: ServeOrTestOptions,
    test: Option<TestOptions>,
    bench: Option<BenchOptions>,
) -> Result<(ffi::ProcessOptions, ffi::FactoryOptions)> {
    let message = read(config)?;
    let config = root(&message)?;
    let autogates = texts(config.get_autogates(), "autogates")?;
    let v8_flags = texts(config.get_v8_flags(), "v8Flags")?;

    let ServeOrTestOptions {
        perfetto_trace_path,
        perfetto_trace_categories,
        async_trace_path,
        async_trace_stacks,
        async_trace_promises,
        experimental,
        pyodide_bundle_disk_cache_dir,
        python_save_snapshot,
        python_save_baseline_snapshot,
        python_load_snapshot,
        python_snapshot_dir,
    } = serve_or_test;
    let mut process = ffi::ProcessOptions {
        perfetto_trace_path: perfetto_trace_path.into(),
        perfetto_trace_categories: perfetto_trace_categories.into(),
        async_trace_path: async_trace_path.into(),
        autogates,
        v8_flags,
        all_autogates: false,
        info_logging: false,
        predictable: false,
        gc_stress: false,
        bench_mode: false,
    };
    let mut factory = ffi::FactoryOptions {
        async_trace_stacks: async_trace_stacks.into(),
        async_trace_promises,
        experimental,
        pyodide_bundle_disk_cache_dir: pyodide_bundle_disk_cache_dir.into(),
        python_save_snapshot,
        python_save_baseline_snapshot,
        python_load_snapshot: python_load_snapshot.into(),
        python_snapshot_dir: python_snapshot_dir.into(),
        compat_date_override: None.into(),
        default_compat_date: None.into(),
        sync_storage: true,
        tail_workers: true,
    };
    if let Some(test) = test {
        // Info logging so that uncaught exceptions are displayed.
        process.info_logging = !test.no_verbose;
        process.predictable = test.predictable;
        process.gc_stress = test.gc_stress;
        process.all_autogates = test.all_autogates;
        factory.compat_date_override = test.compat_date.into();
        factory.sync_storage = false;
    }
    if let Some(bench) = bench {
        process.bench_mode = true;
        process.info_logging = !bench.no_verbose;
        process.all_autogates = bench.all_autogates;
        factory.compat_date_override = bench.compat_date.into();
        factory.default_compat_date = bench.default_compat_date.into();
        factory.tail_workers = bench.trace;
    }
    Ok((process, factory))
}

/// `field`, a list of the config's, as UTF-8 strings.
fn texts(list: capnp::Result<capnp::text_list::Reader<'_>>, field: &str) -> Result<Vec<String>> {
    let list = list.map_err(capnp_error)?;
    list.iter()
        .enumerate()
        .map(|(i, text)| {
            crate::config::text(text)
                .map_err(|error| kj::failed!("config {field}[{i}]: {}", error.description()))
        })
        .collect()
}

/// An encoded message in words, read. Configs can legitimately be very large and are not
/// malicious: the traversal limit is off.
fn read(words: &[u64]) -> Result<capnp::message::Reader<OwnedSegments>> {
    let bytes: Vec<u8> = words.iter().flat_map(|word| word.to_ne_bytes()).collect();
    let options = *ReaderOptions::new().traversal_limit_in_words(None);
    capnp::serialize::read_message(bytes.as_slice(), options).map_err(capnp_error)
}

fn root(message: &capnp::message::Reader<OwnedSegments>) -> Result<config::Reader<'_>> {
    message.get_root().map_err(capnp_error)
}

#[cfg(test)]
#[path = "process-test.rs"]
mod tests;
