// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use bench_report::case::Flag;
use bench_report::case::Status;

use super::*;

#[test]
fn durations_have_three_significant_figures_and_a_fitting_unit() {
    assert_eq!(format_duration(0.0), "0.00 ns");
    assert_eq!(format_duration(1.234), "1.23 ns");
    assert_eq!(format_duration(12.34), "12.3 ns");
    assert_eq!(format_duration(123.4), "123 ns");
    assert_eq!(format_duration(1234.0), "1.23 us");
    assert_eq!(format_duration(1.5e6), "1.50 ms");
    assert_eq!(format_duration(2.5e9), "2.50 s");
    assert_eq!(format_duration(7200e9), "7200 s");
}

#[test]
fn text_has_the_environment_then_a_table_for_each_group() {
    let mut message = TypedBuilder::<bench_report::Owned>::new_default();
    let mut report = message.init_root();
    let mut environment = report.reborrow().init_environment();
    environment.set_workerd_version("2026-10-08");
    environment.set_build_mode("debug");
    environment.set_v8_version("14.0.0");
    environment.set_compat_date("2026-01-01");
    environment
        .init_warnings(1)
        .set(0, "This is a debug build.");

    let mut groups = report.init_groups(2);
    let mut group = groups.reborrow().get(0);
    group.set_name("main:encoding");
    let mut cases = group.reborrow().init_cases(4);

    let mut case = cases.reborrow().get(0);
    case.set_name("encode small");
    case.set_iterations_per_sample(1024);
    let mut wall = case.reborrow().init_wall_ns();
    wall.reborrow().init_samples(3);
    wall.set_median(12.34);
    wall.set_median_low(12.2);
    wall.set_median_high(12.5);
    wall.set_mean(12.4);
    case.init_cpu_ns().set_median(12.3);

    let mut case = cases.reborrow().get(1);
    case.set_name("noisy");
    case.set_iterations_per_sample(1);
    case.reborrow().init_wall_ns().set_median(2e6);
    case.reborrow().init_cpu_ns();
    let mut flags = case.init_flags(2);
    flags.set(0, Flag::HighVariance);
    flags.set(1, Flag::AtFloor);

    let mut case = cases.reborrow().get(2);
    case.set_name("broken");
    case.set_status(Status::Failed);
    case.set_error("Error: boom\n    at stack");

    let mut case = cases.get(3);
    case.set_name("later");
    case.set_status(Status::Skipped);

    let mut overhead = group.init_overhead();
    overhead.set_sync_ns(5.5);
    overhead.set_async_ns(250.0);
    overhead.set_black_box_ns(2.0);

    let mut group = groups.get(1);
    group.set_name("main:empty");
    group.set_error("bench() did not register any cases.");

    let text = format_text(message.get_root_as_reader().unwrap()).unwrap();
    assert_eq!(
        text,
        "Results compare runs on this machine and build; they don't predict production latency.
workerd 2026-10-08, debug build, V8 14.0.0, compat date 2026-01-01
WARNING: This is a debug build.

main:encoding
  overhead per call, subtracted: 5.50 ns, 250 ns async; blackBox() 2.00 ns
  case          median   95% CI             mean     cpu median  samples
  encode small  12.3 ns  12.2 ns - 12.5 ns  12.4 ns  12.3 ns     3 x 1024
  noisy         2.00 ms  0.00 ns - 0.00 ns  0.00 ns  0.00 ns     0 x 1     \
(high variance: try more samples or a longer minTime; at measurement floor: pass results through \
b.blackBox())
  broken        FAILED: Error: boom
  later         skipped

main:empty
  FAILED: bench() did not register any cases.
"
    );
}

#[test]
fn json_is_capnp_json_with_null_pointers_left_out() {
    let mut message = TypedBuilder::<bench_report::Owned>::new_default();
    let mut report = message.init_root();
    let mut environment = report.reborrow().init_environment();
    environment.set_workerd_version("2026-10-08");
    environment.set_build_mode("release");
    environment.set_v8_version("14.0.0");
    environment.set_all_autogates(true);
    environment.init_warnings(1).set(0, "a \"warning\"");
    let mut group = report.init_groups(1).get(0);
    group.set_name("main");
    let mut case = group.init_cases(1).get(0);
    case.set_name("broken");
    case.set_status(Status::Failed);
    case.set_error("Error: boom\n");
    case.set_iterations_per_sample(u64::MAX);

    let json = format_json(message.get_root_as_reader().unwrap()).unwrap();
    assert_eq!(
        json,
        r#"{
  "environment": {
    "workerdVersion": "2026-10-08",
    "buildMode": "release",
    "v8Version": "14.0.0",
    "allAutogates": true,
    "warnings": [
      "a \"warning\""
    ]
  },
  "groups": [
    {
      "name": "main",
      "cases": [
        {
          "name": "broken",
          "status": "failed",
          "error": "Error: boom\n",
          "iterationsPerSample": "18446744073709551615"
        }
      ]
    }
  ]
}
"#
    );
}

#[test]
fn json_writes_floats_that_are_not_numbers_as_strings() {
    let mut message = TypedBuilder::<bench_report::Owned>::new_default();
    let mut case = message
        .init_root()
        .init_groups(1)
        .get(0)
        .init_cases(1)
        .get(0);
    case.reborrow().init_flags(1).set(0, Flag::HighVariance);
    let mut wall = case.init_wall_ns();
    wall.reborrow().init_samples(1).set(0, 2.5);
    wall.set_median(1.5);
    wall.set_mean(f64::NAN);
    wall.set_max(f64::INFINITY);
    wall.set_min(f64::NEG_INFINITY);

    let json = format_json(message.get_root_as_reader().unwrap()).unwrap();
    let value: serde_json::Value = serde_json::from_str(&json).unwrap();
    let case = &value["groups"][0]["cases"][0];
    assert_eq!(case["status"], "ok");
    assert_eq!(case["flags"], serde_json::json!(["highVariance"]));
    let wall = &case["wallNs"];
    assert_eq!(wall["samples"], serde_json::json!([2.5]));
    assert_eq!(wall["median"], 1.5);
    assert_eq!(wall["mean"], "NaN");
    assert_eq!(wall["max"], "Infinity");
    assert_eq!(wall["min"], "-Infinity");
    assert_eq!(wall["stddev"], 0.0);
}
