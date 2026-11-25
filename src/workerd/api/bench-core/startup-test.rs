// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#![expect(
    clippy::float_cmp,
    reason = "the expected values are exactly what the report holds"
)]

use std::cell::Cell;
use std::future::ready;

use super::*;

/// The words of a startup group's params. A `min_time_ns` of 0 leaves the default sampling
/// budget, which the fake starts below never reach.
fn params(case_filter: &str, samples: u32, quick: bool, min_time_ns: u64) -> Vec<u64> {
    let mut message = capnp::message::TypedBuilder::<bench_params::Owned>::new_default();
    let mut params = message.init_root();
    params.set_case_filter(case_filter);
    params.set_quick(quick);
    let mut defaults = params.init_defaults();
    defaults.set_samples(samples);
    defaults.set_min_time_ns(min_time_ns);
    capnp::serialize::write_message_to_words(message.borrow_inner())
        .chunks_exact(8)
        .map(|chunk| u64::from_ne_bytes(chunk.try_into().unwrap()))
        .collect()
}

/// A start whose phases take 1000, 2000, 3000 and 10000 times `n` ns of wall time, and half that
/// of CPU time, where `n` counts the starts from 1.
fn fake_start(
    starts: &Cell<u32>,
) -> impl FnMut() -> std::future::Ready<Result<StartupTiming, String>> + '_ {
    || {
        starts.set(starts.get() + 1);
        let n = i64::from(starts.get());
        let timing = |ns: i64| Timing {
            wall_ns: ns * n,
            cpu_ns: ns * n / 2,
        };
        ready(Ok(StartupTiming {
            isolate: timing(1000),
            script: timing(2000),
            worker: timing(3000),
            total: timing(10000),
        }))
    }
}

/// Runs a startup group and returns its encoded group, read back.
fn run<F, Fut>(params: &[u64], start: F) -> capnp::message::Reader<capnp::serialize::OwnedSegments>
where
    F: FnMut() -> Fut,
    Fut: Future<Output = Result<StartupTiming, String>>,
{
    let words = futures::executor::block_on(time_startup(params, start)).unwrap();
    let bytes: Vec<u8> = words.iter().flat_map(|word| word.to_ne_bytes()).collect();
    capnp::serialize::read_message(bytes.as_slice(), capnp::message::ReaderOptions::new()).unwrap()
}

fn group(
    message: &capnp::message::Reader<capnp::serialize::OwnedSegments>,
) -> bench_report::group::Reader<'_> {
    message.get_root().unwrap()
}

fn case_names(group: bench_report::group::Reader<'_>) -> Vec<String> {
    group
        .get_cases()
        .unwrap()
        .iter()
        .map(|case| case.get_name().unwrap().to_string().unwrap())
        .collect()
}

/// Statistics come from a histogram with three significant figures.
fn near(actual: f64, expected: f64) -> bool {
    (actual - expected).abs() <= expected.mul_add(0.001, 0.001)
}

#[test]
fn reports_each_phase_of_the_starts_after_a_warmup() {
    let starts = Cell::new(0);
    let message = run(&params("", 3, false, 0), fake_start(&starts));
    let group = group(&message);

    assert_eq!(starts.get(), 4);
    assert!(group.get_startup());
    assert!(!group.has_error());
    assert!(!group.has_overhead());
    let names = [
        "startup:isolate",
        "startup:script",
        "startup:worker",
        "startup:total",
    ];
    assert_eq!(case_names(group), names);
    let scales = [1000.0, 2000.0, 3000.0, 10000.0];
    for (case, scale) in group.get_cases().unwrap().iter().zip(scales) {
        assert_eq!(case.get_status().unwrap(), Status::Ok);
        assert_eq!(case.get_iterations_per_sample(), 1);
        // The first start is the warmup.
        let wall = case.get_wall_ns().unwrap();
        let cpu = case.get_cpu_ns().unwrap();
        let wall_samples: Vec<f64> = wall.get_samples().unwrap().iter().collect();
        let cpu_samples: Vec<f64> = cpu.get_samples().unwrap().iter().collect();
        assert_eq!(wall_samples, [2.0 * scale, 3.0 * scale, 4.0 * scale]);
        assert_eq!(cpu_samples, [scale, 1.5 * scale, 2.0 * scale]);
        assert!(
            near(wall.get_median(), scale * 3.0),
            "{}",
            wall.get_median()
        );
    }
}

#[test]
fn reports_only_the_phases_the_case_filter_selects() {
    let starts = Cell::new(0);
    let message = run(&params("startup:*r*", 2, false, 0), fake_start(&starts));
    assert_eq!(starts.get(), 3);
    assert_eq!(
        case_names(group(&message)),
        ["startup:script", "startup:worker"]
    );

    // Patterns meant for a handler's cases select no phase, so nothing starts.
    for pattern in ["encode*", "w*", "total"] {
        let starts = Cell::new(0);
        let message = run(&params(pattern, 2, false, 0), fake_start(&starts));
        let group = group(&message);
        assert_eq!(starts.get(), 0, "{pattern}");
        assert!(group.get_startup());
        assert!(!group.has_error());
        assert_eq!(group.get_cases().unwrap().len(), 0, "{pattern}");
    }
}

#[test]
fn stops_at_the_sampling_budget_once_it_has_enough_samples() {
    // The timed starts' totals are 20us, 30us, 40us, ...: five add up to 200us and seven to
    // 350us.
    for (min_time_ns, samples) in [(300_000, 7), (1, 5)] {
        let starts = Cell::new(0);
        let message = run(
            &params("startup:total", 50, false, min_time_ns),
            fake_start(&starts),
        );
        assert_eq!(starts.get(), samples + 1, "{min_time_ns}");
        let cases = group(&message).get_cases().unwrap();
        assert_eq!(cases.len(), 1);
        let wall = cases.get(0).get_wall_ns().unwrap();
        assert_eq!(wall.get_samples().unwrap().len(), samples, "{min_time_ns}");
    }
}

#[test]
fn under_quick_skips_the_warmup_and_caps_the_samples() {
    let starts = Cell::new(0);
    let message = run(&params("startup:total", 1000, true, 0), fake_start(&starts));
    assert_eq!(starts.get(), QUICK_SAMPLES);
    let cases = group(&message).get_cases().unwrap();
    assert_eq!(cases.len(), 1);
    let wall = cases.get(0).get_wall_ns().unwrap().get_samples().unwrap();
    assert_eq!(wall.len(), QUICK_SAMPLES);
    assert_eq!(wall.get(0), 10000.0);
}

#[test]
fn fails_the_group_when_a_start_fails() {
    let starts = Cell::new(0);
    let message = run(&params("", 3, false, 0), || {
        // The warmup succeeds; the first timed start fails.
        starts.set(starts.get() + 1);
        ready(if starts.get() == 1 {
            Ok(StartupTiming::default())
        } else {
            Err("the worker failed to start".to_owned())
        })
    });
    let group = group(&message);
    assert_eq!(starts.get(), 2);
    assert!(group.get_startup());
    assert_eq!(
        group.get_error().unwrap().to_str().unwrap(),
        "the worker failed to start"
    );
    assert_eq!(group.get_cases().unwrap().len(), 0);
}
