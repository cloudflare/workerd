// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Parity with Node.js: every query on every dataset and layout in testdata/node-golden.json,
//! which testdata/generate-node-golden.mjs produces with Node.js.
//!
//! Results computed with the same operations as Node.js must be identical. Results that go through
//! `lgamma()` or `erfc()`, where libm can differ from the platform's C library by an ulp, are
//! compared with a relative tolerance of 1e-12.

use serde_json::Value;

use super::*;

const GOLDEN: &str = include_str!("testdata/node-golden.json");

fn golden() -> Value {
    serde_json::from_str(GOLDEN).unwrap()
}

/// A float in the golden data, which stores them as strings in JavaScript's format.
fn float(value: &Value) -> f64 {
    match value.as_str().unwrap() {
        "NaN" => f64::NAN,
        "Infinity" => f64::INFINITY,
        "-Infinity" => f64::NEG_INFINITY,
        s => s.parse().unwrap(),
    }
}

/// An integer in the golden data, as a JSON number or a decimal string.
fn int(value: &Value) -> i64 {
    match value {
        Value::String(s) => s.parse().unwrap(),
        _ => value
            .as_i64()
            .unwrap_or_else(|| value.as_f64().unwrap() as i64),
    }
}

fn floats(value: &Value) -> Vec<f64> {
    value.as_array().unwrap().iter().map(float).collect()
}

#[track_caller]
fn assert_same(actual: f64, expected: f64, what: &str) {
    assert!(
        actual.to_bits() == expected.to_bits() || (actual.is_nan() && expected.is_nan()),
        "{what}: {actual} != {expected}"
    );
}

#[track_caller]
fn assert_close(actual: f64, expected: f64, what: &str) {
    if actual.is_nan() || expected.is_nan() || expected.is_infinite() {
        assert_same(actual, expected, what);
        return;
    }
    let tolerance = 1e-12 * expected.abs().max(1.0);
    assert!(
        (actual - expected).abs() <= tolerance,
        "{what}: {actual} != {expected}"
    );
}

#[track_caller]
fn assert_all_close(actual: &[f64], expected: &[f64], what: &str) {
    assert_eq!(
        actual.len(),
        expected.len(),
        "{what}: {actual:?} != {expected:?}"
    );
    for (&a, &e) in actual.iter().zip(expected) {
        assert_close(a, e, what);
    }
}

/// The entries of a JavaScript `Map` built from the pairs: in order of first insertion, with the
/// last value for each key.
fn as_map<K: PartialEq + Copy, V: Copy>(pairs: &[(K, V)]) -> Vec<(K, V)> {
    let mut map: Vec<(K, V)> = Vec::new();
    for &(key, value) in pairs {
        match map.iter_mut().find(|(k, _)| *k == key) {
            Some(entry) => entry.1 = value,
            None => map.push((key, value)),
        }
    }
    map
}

fn int_entries(value: &Value) -> Vec<(i64, i64)> {
    value
        .as_array()
        .unwrap()
        .iter()
        .map(|e| (int(&e[0]), int(&e[1])))
        .collect()
}

fn options(layout: &Value) -> Options {
    Options {
        lowest: int(&layout["lowest"]),
        highest: int(&layout["highest"]),
        figures: i32::try_from(int(&layout["figures"])).unwrap(),
        ..Options::default()
    }
}

fn build(options: &Options, values: &Value) -> Histogram {
    let mut h = Histogram::new(options).unwrap();
    for value in values.as_array().unwrap() {
        h.record(int(value));
    }
    h
}

fn from_hex(hex: &str) -> Vec<u8> {
    (0..hex.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(&hex[i..i + 2], 16).unwrap())
        .collect()
}

#[expect(
    clippy::too_many_lines,
    reason = "one check per query in the golden data"
)]
fn check_histogram(h: &Histogram, expected: &Value, what: &str) {
    assert_eq!(
        h.count(),
        expected["count"].as_u64().unwrap(),
        "{what} count"
    );
    assert_eq!(h.min(), int(&expected["min"]), "{what} min");
    assert_eq!(h.max(), int(&expected["max"]), "{what} max");
    assert_same(h.mean(), float(&expected["mean"]), &format!("{what} mean"));
    assert_same(
        h.stddev(),
        float(&expected["stddev"]),
        &format!("{what} stddev"),
    );

    let percentiles = [0.1, 1.0, 12.0, 25.0, 50.0, 75.0, 90.0, 99.0, 99.9, 100.0];
    for (&p, value) in percentiles
        .iter()
        .zip(expected["percentile"].as_array().unwrap())
    {
        assert_eq!(
            h.percentile(p).unwrap(),
            int(value),
            "{what} percentile({p})"
        );
    }
    let at: Vec<(f64, i64)> = percentiles
        .iter()
        .copied()
        .zip(h.percentiles_at(&percentiles))
        .collect();
    let expected_at: Vec<(f64, i64)> = expected["percentilesAt"]
        .as_array()
        .unwrap()
        .iter()
        .map(|e| (float(&e[0]), int(&e[1])))
        .collect();
    assert_eq!(at, expected_at, "{what} percentilesAt");

    let expected_percentiles: Vec<(f64, i64)> = expected["percentiles"]
        .as_array()
        .unwrap()
        .iter()
        .map(|e| (float(&e[0]), int(&e[1])))
        .collect();
    assert_eq!(
        as_map(&h.percentiles()),
        expected_percentiles,
        "{what} percentiles"
    );

    for probe in expected["cdf"].as_array().unwrap() {
        let value = int(&probe[0]);
        assert_same(
            h.cdf(value),
            float(&probe[1]),
            &format!("{what} cdf({value})"),
        );
    }
    for probe in expected["countAt"].as_array().unwrap() {
        let value = int(&probe[0]);
        assert_eq!(h.count_at(value), int(&probe[1]), "{what} countAt({value})");
    }

    let step = int(&expected["linearStep"]);
    assert_eq!(
        as_map(&h.linear_buckets(step)),
        int_entries(&expected["linearBuckets"]),
        "{what} linearBuckets({step})"
    );
    assert_eq!(
        as_map(&h.log_buckets(1, 2.0)),
        int_entries(&expected["logBuckets"]),
        "{what} logBuckets(1, 2)"
    );
    assert_eq!(
        as_map(&h.log_buckets(10, 10.0)),
        int_entries(&expected["logBuckets10"]),
        "{what} logBuckets(10, 10)"
    );

    assert_close(
        h.skewness(),
        float(&expected["skewness"]),
        &format!("{what} skewness"),
    );
    assert_close(
        h.kurtosis(),
        float(&expected["kurtosis"]),
        &format!("{what} kurtosis"),
    );
    for (key, confidence) in [("meanCI95", 0.95), ("meanCI99", 0.99)] {
        let ci = h.mean_ci(confidence);
        assert_all_close(
            &[ci.mean, ci.lower, ci.upper],
            &floats(&expected[key]),
            &format!("{what} {key}"),
        );
    }
    for case in expected["percentileCI"].as_array().unwrap() {
        let (p, confidence) = (float(&case[0]), float(&case[1]));
        assert_eq!(
            h.percentile_ci(p, confidence),
            PercentileCi {
                value: int(&case[2]),
                lower: int(&case[3]),
                upper: int(&case[4]),
            },
            "{what} percentileCI({p}, {confidence})"
        );
    }

    // Node.js 26.10 exports version 1, which differs only in the version value.
    let mut export = from_hex(expected["export"].as_str().unwrap());
    assert_eq!(export[1..3], [0x00, 0x01], "{what} export version");
    export[2] = 0x02;
    assert_eq!(h.export(), export, "{what} export");

    if let Some(cases) = expected.get("qrde") {
        for case in cases.as_array().unwrap() {
            let dequantization = match case["dequantize"].as_str().unwrap() {
                "none" => Dequantization::None,
                "hdr" => Dequantization::Hdr,
                "all" => Dequantization::All,
                mode => panic!("unexpected mode {mode}"),
            };
            let result = h
                .qrde(&floats(&case["probabilities"]), dequantization)
                .unwrap();
            let what = format!("{what} qrde({dequantization:?})");
            assert_all_close(&result.quantiles, &floats(&case["quantiles"]), &what);
            assert_all_close(&result.densities, &floats(&case["densities"]), &what);
            assert_eq!(
                result.corrections as u64,
                case["corrections"].as_u64().unwrap(),
                "{what}"
            );
            assert_eq!(
                result.bucket_count as u64,
                case["bucketCount"].as_u64().unwrap(),
                "{what}"
            );
        }
    }
}

#[test]
fn matches_node() {
    let golden = golden();
    let datasets = &golden["datasets"];
    for layout in golden["layouts"].as_array().unwrap() {
        let options = options(&layout["options"]);
        for (name, expected) in layout["histograms"].as_object().unwrap() {
            let h = build(&options, &datasets[name]);
            let what = format!("{options:?} {name}");
            assert_eq!(
                h.exceeds(),
                expected["exceeds"].as_u64().unwrap(),
                "{what} exceeds"
            );
            check_histogram(&h, expected, &what);
        }

        for comparison in layout["comparisons"].as_array().unwrap() {
            let a_name = comparison["a"].as_str().unwrap();
            let b_name = comparison["b"].as_str().unwrap();
            let a = build(&options, &datasets[a_name]);
            let b = build(&options, &datasets[b_name]);
            let what = format!("{options:?} {a_name} vs {b_name}");

            let welch = a.welch_test(&b, 0.95);
            assert_all_close(
                &[
                    welch.t_statistic,
                    welch.degrees_of_freedom,
                    welch.p_value,
                    welch.ci_lower,
                    welch.ci_upper,
                ],
                &floats(&comparison["welch"]),
                &format!("{what} welch"),
            );
            let mw = a.mann_whitney_test(&b);
            assert_all_close(
                &[mw.u_statistic, mw.z_score, mw.p_value],
                &floats(&comparison["mannWhitney"]),
                &format!("{what} mannWhitney"),
            );
            assert_close(a.cohens_d(&b), float(&comparison["cohensD"]), &what);
            assert_close(a.cliffs_d(&b), float(&comparison["cliffsD"]), &what);
            assert_close(a.ks_test(&b), float(&comparison["ksTest"]), &what);
        }
    }
}

#[test]
fn imports_node_exports() {
    let golden = golden();
    for layout in golden["layouts"].as_array().unwrap() {
        for (name, expected) in layout["histograms"].as_object().unwrap() {
            let data = from_hex(expected["export"].as_str().unwrap());
            // The export format does not include the exceeds count.
            let h = Histogram::import(&data).unwrap();
            check_histogram(&h, expected, &format!("imported {name}"));
        }
    }
}
