// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use bench_capnp::bench_report;

use super::*;

/// The group that `report` encodes, read back as C++ reads it.
fn decode(report: &BenchGroupReport) -> capnp::message::Reader<capnp::serialize::OwnedSegments> {
    let words = report.encode().unwrap();
    let bytes: Vec<u8> = words.iter().flat_map(|word| word.to_ne_bytes()).collect();
    capnp::serialize::read_message(bytes.as_slice(), capnp::message::ReaderOptions::new()).unwrap()
}

fn cases(
    message: &capnp::message::Reader<capnp::serialize::OwnedSegments>,
) -> capnp::struct_list::Reader<'_, bench_report::case::Owned> {
    message
        .get_root::<bench_report::group::Reader<'_>>()
        .unwrap()
        .get_cases()
        .unwrap()
}

/// Adds a failed case named `name`, without samples.
fn add_failed(report: &mut BenchGroupReport, name: &[u8]) {
    report
        .add_case(name, Status::Failed as u16, 0, &[], &[], false)
        .unwrap();
}

#[test]
fn an_empty_case_error_is_set() {
    let mut report = new_bench_group_report();
    add_failed(&mut report, b"case");
    report.set_case_error(b"");

    let message = decode(&report);
    let case = cases(&message).get(0);
    assert!(case.has_error());
    assert_eq!(case.get_error().unwrap().as_bytes(), b"");
}

#[test]
fn a_case_has_no_error_unless_one_is_set() {
    let mut report = new_bench_group_report();
    add_failed(&mut report, b"case");

    let message = decode(&report);
    assert!(!cases(&message).get(0).has_error());
}

#[test]
fn a_case_error_goes_to_the_case_added_last() {
    let mut report = new_bench_group_report();
    add_failed(&mut report, b"first");
    add_failed(&mut report, b"second");
    report.set_case_error(b"boom");

    let message = decode(&report);
    let cases = cases(&message);
    assert!(!cases.get(0).has_error());
    assert_eq!(cases.get(1).get_error().unwrap().as_bytes(), b"boom");
}

#[test]
fn a_case_error_before_any_case_is_ignored() {
    let mut report = new_bench_group_report();
    report.set_case_error(b"boom");
    add_failed(&mut report, b"case");

    let message = decode(&report);
    assert!(!cases(&message).get(0).has_error());
}

#[test]
fn strings_that_arent_utf8_are_written_unchanged() {
    // A lone surrogate as WTF-8, and a truncated sequence.
    let name = b"case \xed\xa0\x80";
    let error = b"error \xe2\x82";
    let mut report = new_bench_group_report();
    report.set_error(error);
    add_failed(&mut report, name);
    report.set_case_error(error);

    let message = decode(&report);
    let group = message
        .get_root::<bench_report::group::Reader<'_>>()
        .unwrap();
    assert_eq!(group.get_error().unwrap().as_bytes(), error);
    let case = group.get_cases().unwrap().get(0);
    assert_eq!(case.get_name().unwrap().as_bytes(), name);
    assert_eq!(case.get_error().unwrap().as_bytes(), error);
}

#[test]
fn an_unknown_status_is_an_error() {
    let mut report = new_bench_group_report();
    assert!(report.add_case(b"case", 3, 0, &[], &[], false).is_err());
}
