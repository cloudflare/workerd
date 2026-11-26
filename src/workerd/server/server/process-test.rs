// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

/// The encoded `Config` that `build` makes.
fn words(build: impl FnOnce(config::Builder<'_>)) -> Vec<u64> {
    let mut message = capnp::message::Builder::new_default();
    build(message.init_root::<config::Builder<'_>>());
    capnp::serialize::write_message_to_words(&message)
        .chunks_exact(8)
        .map(|chunk| u64::from_ne_bytes(chunk.try_into().unwrap()))
        .collect()
}

fn serve_or_test() -> ServeOrTestOptions {
    ServeOrTestOptions {
        perfetto_trace_path: Some("trace.pftrace".to_owned()),
        perfetto_trace_categories: None,
        async_trace_path: Some("async.ndjson".to_owned()),
        async_trace_stacks: Some(8),
        async_trace_promises: true,
        experimental: true,
        pyodide_bundle_disk_cache_dir: None,
        python_save_snapshot: false,
        python_save_baseline_snapshot: true,
        python_load_snapshot: None,
        python_snapshot_dir: Some("snapshots".to_owned()),
    }
}

fn some(maybe: KjMaybeString) -> Option<String> {
    maybe.into()
}

type KjMaybeString = kj_rs::KjMaybe<String>;

#[test]
fn structured_logging_is_the_logging_field_when_set() {
    let config = words(|mut config| {
        config.set_structured_logging(true);
        config.init_logging().set_structured_logging(false);
    });
    assert!(!structured_logging(&config).unwrap());

    let config = words(|config| {
        config.init_logging().set_structured_logging(true);
    });
    assert!(structured_logging(&config).unwrap());
}

#[test]
fn structured_logging_is_the_top_level_field_without_logging() {
    let config = words(|mut config| config.set_structured_logging(true));
    assert!(structured_logging(&config).unwrap());
    assert!(!structured_logging(&words(|_| {})).unwrap());
}

#[test]
fn serve_sets_up_no_mode_and_keeps_the_factory_defaults() {
    let config = words(|mut config| {
        let mut gates = config.reborrow().init_autogates(1);
        gates.set(0, "workerd-autogate-example");
        config.init_v8_flags(1).set(0, "--no-opt");
    });
    let (process, factory) = command_options(&config, serve_or_test(), None, None).unwrap();

    assert_eq!(process.autogates, ["workerd-autogate-example"]);
    assert_eq!(process.v8_flags, ["--no-opt"]);
    assert_eq!(
        some(process.perfetto_trace_path).as_deref(),
        Some("trace.pftrace")
    );
    assert_eq!(
        some(process.async_trace_path).as_deref(),
        Some("async.ndjson")
    );
    assert!(!process.all_autogates);
    assert!(!process.info_logging);
    assert!(!process.predictable);
    assert!(!process.gc_stress);
    assert!(!process.bench_mode);

    assert_eq!(Option::<u32>::from(factory.async_trace_stacks), Some(8));
    assert!(factory.async_trace_promises);
    assert!(factory.experimental);
    assert!(factory.python_save_baseline_snapshot);
    assert_eq!(
        some(factory.python_snapshot_dir).as_deref(),
        Some("snapshots")
    );
    assert_eq!(some(factory.compat_date_override), None);
    assert_eq!(some(factory.default_compat_date), None);
    assert!(factory.sync_storage);
    assert!(factory.tail_workers);
}

#[test]
fn test_sets_its_modes_and_storage_is_not_synchronous() {
    let test = TestOptions {
        no_verbose: false,
        predictable: true,
        gc_stress: true,
        all_autogates: true,
        compat_date: Some("2026-01-01".to_owned()),
    };
    let (process, factory) =
        command_options(&words(|_| {}), serve_or_test(), Some(test), None).unwrap();

    assert!(process.info_logging);
    assert!(process.predictable);
    assert!(process.gc_stress);
    assert!(process.all_autogates);
    assert!(!process.bench_mode);
    assert_eq!(
        some(factory.compat_date_override).as_deref(),
        Some("2026-01-01")
    );
    assert!(!factory.sync_storage);
    assert!(factory.tail_workers);
}

#[test]
fn bench_sets_bench_mode_and_its_dates() {
    let bench = BenchOptions {
        no_verbose: true,
        all_autogates: false,
        compat_date: None,
        default_compat_date: Some("2026-02-02".to_owned()),
        trace: false,
    };
    let (process, factory) =
        command_options(&words(|_| {}), serve_or_test(), None, Some(bench)).unwrap();

    assert!(process.bench_mode);
    assert!(!process.info_logging);
    assert!(!process.predictable);
    assert!(!process.all_autogates);
    assert_eq!(some(factory.compat_date_override), None);
    assert_eq!(
        some(factory.default_compat_date).as_deref(),
        Some("2026-02-02")
    );
    assert!(factory.sync_storage);
    assert!(!factory.tail_workers);
}

#[test]
fn an_autogate_or_v8_flag_that_is_not_utf8_is_an_error() {
    let not_utf8 = capnp::text::Reader::from(&b"\xff"[..]);

    let config = words(|config| config.init_autogates(1).set(0, not_utf8));
    let error = command_options(&config, serve_or_test(), None, None)
        .err()
        .expect("a non-UTF-8 autogate is an error");
    assert!(
        error.description().contains("config autogates[0]: "),
        "{}",
        error.description()
    );

    let config = words(|config| config.init_v8_flags(1).set(0, not_utf8));
    let error = command_options(&config, serve_or_test(), None, None)
        .err()
        .expect("a non-UTF-8 V8 flag is an error");
    assert!(
        error.description().contains("config v8Flags[0]: "),
        "{}",
        error.description()
    );
}
