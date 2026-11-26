// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#![expect(
    clippy::float_cmp,
    reason = "the expected values are exactly what the report holds"
)]

use super::*;

const OVERHEAD: Overhead = Overhead {
    sync_ns: 10.0,
    sync_cpu_ns: 8.0,
    async_ns: 1000.0,
    async_cpu_ns: 900.0,
    black_box_ns: 2.0,
};

fn make_case(samples: &[f64], returns_promise: bool) -> CaseResult {
    CaseResult {
        name: b"case".to_vec(),
        status: Status::Ok,
        error: None,
        iterations_per_sample: 100,
        wall_ns: samples.to_vec(),
        cpu_ns: samples.to_vec(),
        returns_promise,
    }
}

/// The group of `result`, encoded and read back as C++ would.
fn round_trip(result: &GroupResult) -> capnp::message::Reader<capnp::serialize::OwnedSegments> {
    let bytes = encode_group(result).unwrap();
    capnp::serialize::read_message(bytes.as_slice(), capnp::message::ReaderOptions::new()).unwrap()
}

/// Fills a report for one case and returns the message.
fn fill(
    case: CaseResult,
    overhead: Option<Overhead>,
) -> capnp::message::Reader<capnp::serialize::OwnedSegments> {
    round_trip(&GroupResult {
        error: None,
        cases: vec![case],
        overhead,
    })
}

fn group(
    message: &capnp::message::Reader<capnp::serialize::OwnedSegments>,
) -> bench_report::group::Reader<'_> {
    message.get_root().unwrap()
}

fn first_case(
    message: &capnp::message::Reader<capnp::serialize::OwnedSegments>,
) -> bench_report::case::Reader<'_> {
    group(message).get_cases().unwrap().get(0)
}

/// Statistics come from a histogram with three significant figures.
fn near(actual: f64, expected: f64) -> bool {
    (actual - expected).abs() <= expected.mul_add(0.001, 0.001)
}

fn has_flag(case: bench_report::case::Reader<'_>, flag: Flag) -> bool {
    case.get_flags().unwrap().iter().any(|f| f.unwrap() == flag)
}

#[test]
fn subtracts_the_sync_overhead_from_cases_that_dont_return_promises() {
    let message = fill(
        make_case(&[100.0, 101.0, 99.0, 100.0, 100.0], false),
        Some(OVERHEAD),
    );
    let case = first_case(&message);

    let wall = case.get_wall_ns().unwrap();
    assert!(near(wall.get_median(), 90.0), "{}", wall.get_median());
    let cpu = case.get_cpu_ns().unwrap();
    assert!(near(cpu.get_median(), 92.0), "{}", cpu.get_median());
    assert_eq!(wall.get_samples().unwrap().get(1), 91.0);
    assert_eq!(case.get_flags().unwrap().len(), 0);
    assert_eq!(case.get_iterations_per_sample(), 100);

    let overhead = group(&message).get_overhead().unwrap();
    assert_eq!(overhead.get_sync_ns(), 10.0);
    assert_eq!(overhead.get_async_ns(), 1000.0);
    assert_eq!(overhead.get_black_box_ns(), 2.0);
}

#[test]
fn subtracts_the_async_overhead_from_cases_that_return_promises() {
    let message = fill(make_case(&[3000.0, 3000.0, 3000.0], true), Some(OVERHEAD));
    let case = first_case(&message);
    let wall = case.get_wall_ns().unwrap();
    assert!(near(wall.get_median(), 2000.0), "{}", wall.get_median());
    let cpu = case.get_cpu_ns().unwrap();
    assert!(near(cpu.get_median(), 2100.0), "{}", cpu.get_median());
    assert!(!has_flag(case, Flag::AtFloor));
}

#[test]
fn flags_cases_at_the_floor_and_doesnt_go_below_zero() {
    // A median under twice the overhead, with samples below the overhead and noise that would
    // otherwise count as high variance.
    let message = fill(
        make_case(&[5.0, 11.0, 19.0, 15.0, 12.0], false),
        Some(OVERHEAD),
    );
    let case = first_case(&message);
    assert!(has_flag(case, Flag::AtFloor));
    assert!(!has_flag(case, Flag::HighVariance));
    let wall = case.get_wall_ns().unwrap();
    assert_eq!(wall.get_samples().unwrap().get(0), 0.0);
    assert_eq!(wall.get_min(), 0.0);
}

#[test]
fn flags_high_variance() {
    let message = fill(make_case(&[100.0, 300.0, 100.0, 300.0], false), None);
    let case = first_case(&message);
    assert!(has_flag(case, Flag::HighVariance));
    assert!(!has_flag(case, Flag::AtFloor));
    assert!(!group(&message).has_overhead());
}

#[test]
fn fails_cases_with_calls_longer_than_an_hour() {
    // The histograms hold samples of up to an hour, 3.6e12 ns.
    {
        let message = fill(make_case(&[100.0, 3.6e12, 100.0], false), Some(OVERHEAD));
        let case = first_case(&message);
        assert_eq!(case.get_status().unwrap(), Status::Ok);
        let wall = case.get_wall_ns().unwrap();
        assert!(near(wall.get_max(), 3.6e12), "{}", wall.get_max());
    }
    {
        let message = fill(
            make_case(&[100.0, 3.6e12 + 1.0, 100.0], false),
            Some(OVERHEAD),
        );
        let case = first_case(&message);
        assert_eq!(case.get_status().unwrap(), Status::Failed);
        assert_eq!(case.get_error().unwrap().to_str().unwrap(), TOO_LONG);
        assert!(!case.has_wall_ns());
    }
}

#[test]
fn writes_failures_and_skips_without_statistics() {
    let message = round_trip(&GroupResult {
        error: Some(b"handler failed".to_vec()),
        cases: vec![
            CaseResult {
                name: b"broken".to_vec(),
                status: Status::Failed,
                error: Some(b"Error: boom".to_vec()),
                iterations_per_sample: 0,
                wall_ns: Vec::new(),
                cpu_ns: Vec::new(),
                returns_promise: false,
            },
            CaseResult {
                name: b"later".to_vec(),
                status: Status::Skipped,
                error: None,
                iterations_per_sample: 0,
                wall_ns: Vec::new(),
                cpu_ns: Vec::new(),
                returns_promise: false,
            },
        ],
        overhead: None,
    });
    let group = group(&message);
    assert_eq!(
        group.get_error().unwrap().to_str().unwrap(),
        "handler failed"
    );
    let cases = group.get_cases().unwrap();
    assert_eq!(cases.len(), 2);
    assert_eq!(
        cases.get(0).get_error().unwrap().to_str().unwrap(),
        "Error: boom"
    );
    assert!(!cases.get(0).has_wall_ns());
    assert_eq!(cases.get(1).get_status().unwrap(), Status::Skipped);
    assert!(!cases.get(1).has_wall_ns());
}

#[test]
fn keeps_an_empty_error() {
    let mut case = make_case(&[], false);
    case.status = Status::Failed;
    case.error = Some(Vec::new());
    let message = fill(case, None);
    assert!(first_case(&message).has_error());
}

#[test]
fn takes_the_median() {
    assert_eq!(median(&[3.0, 1.0, 2.0]), 2.0);
    assert_eq!(median(&[4.0, 1.0, 3.0, 2.0]), 2.5);
    assert!(median(&[]).is_nan());
}

#[test]
fn words_to_bytes_keeps_memory_order() {
    let words = [0x0102_0304_0506_0708_u64, u64::MAX, 0];
    let expected: Vec<u8> = words.iter().flat_map(|word| word.to_ne_bytes()).collect();
    assert_eq!(words_to_bytes(&words), expected);
    assert!(words_to_bytes(&[]).is_empty());
}
