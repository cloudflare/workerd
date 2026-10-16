// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#![expect(
    clippy::float_cmp,
    reason = "exact cases, such as zero skewness, are compared exactly; others use assert_close()"
)]

use super::*;
use crate::Options;

fn histogram_of(values: &[i64]) -> Histogram {
    let mut h = Histogram::new(&Options::default()).unwrap();
    for &value in values {
        assert!(h.record(value));
    }
    h
}

#[track_caller]
fn assert_close(actual: f64, expected: f64) {
    let tolerance = 1e-12 * expected.abs().max(1.0);
    assert!(
        (actual - expected).abs() <= tolerance,
        "{actual} != {expected}"
    );
}

// The expected values in the tests below are from Node.js 26.10.
fn a() -> Histogram {
    histogram_of(&[1, 2, 3, 4, 5, 6, 7, 8, 9, 10])
}

fn b() -> Histogram {
    histogram_of(&[3, 5, 7, 9, 11, 13, 15, 17, 19, 21, 100])
}

#[test]
fn numerical_helpers() {
    assert_close(normal_cdf(0.0), 0.5);
    assert!((normal_cdf(1.96) - 0.975).abs() < 1e-4);
    // t(0.975, 10) = 2.228138851986...
    assert!((student_t_upper_quantile(0.025, 10.0) - 2.228_138_851_986).abs() < 1e-9);
    assert_close(binomial_cdf(-1, 10, 0.5), 0.0);
    assert_close(binomial_cdf(10, 10, 0.5), 1.0);
    // P(X <= 5) for X ~ Binomial(10, 1/2) is 638/1024.
    assert_close(binomial_cdf(5, 10, 0.5), 638.0 / 1024.0);
}

#[test]
fn shape() {
    assert_close(b().skewness(), 2.615_536_842_180_938);
    assert_close(b().kurtosis(), 5.332_898_195_589_218);
    assert_eq!(histogram_of(&[1, 2]).skewness(), 0.0);
    assert_eq!(histogram_of(&[1, 2, 3]).kurtosis(), 0.0);
    assert_eq!(histogram_of(&[7, 7, 7, 7]).kurtosis(), 0.0);
}

#[test]
fn mean_ci() {
    let ci = a().mean_ci(0.95);
    assert_close(ci.mean, 5.5);
    assert_close(ci.lower, 3.334_149_410_331_883);
    assert_close(ci.upper, 7.665_850_589_668_117);

    let one = histogram_of(&[3]).mean_ci(0.95);
    assert_eq!(one.mean, 3.0);
    assert!(one.lower.is_nan() && one.upper.is_nan());

    let flat = histogram_of(&[3, 3, 3]).mean_ci(0.95);
    assert_eq!((flat.lower, flat.upper), (3.0, 3.0));
}

#[test]
fn percentile_ci() {
    assert_eq!(
        a().percentile_ci(50.0, 0.95),
        PercentileCi {
            value: 5,
            lower: 2,
            upper: 9
        }
    );
    assert_eq!(
        histogram_of(&[4]).percentile_ci(50.0, 0.95),
        PercentileCi {
            value: 4,
            lower: 4,
            upper: 4
        }
    );
}

#[test]
fn welch_test() {
    let w = a().welch_test(&b(), 0.95);
    assert_close(w.t_statistic, -1.759_461_713_556_845_8);
    assert_close(w.degrees_of_freedom, 10.273_367_000_323_427);
    assert_close(w.p_value, 0.108_192_935_213_515_03);
    assert_close(w.ci_lower, -32.796_426_246_981_98);
    assert_close(w.ci_upper, 3.796_426_246_981_983_6);

    let h = a();
    assert_eq!(h.welch_test(&h, 0.95).p_value, 1.0);
    assert_eq!(h.welch_test(&histogram_of(&[1]), 0.95).t_statistic, 0.0);
}

#[test]
fn mann_whitney_test() {
    let mw = a().mann_whitney_test(&b());
    assert_close(mw.u_statistic, 18.0);
    assert_close(mw.z_score, -2.608_851_846_032_169);
    assert_close(mw.p_value, 0.009_084_656_491_886_27);

    let empty = histogram_of(&[]);
    assert_eq!(a().mann_whitney_test(&empty).p_value, 1.0);
}

#[test]
fn effect_sizes() {
    assert_close(a().cohens_d(&b()), -0.732_139_452_880_120_9);
    assert_close(a().cliffs_d(&b()), -0.672_727_272_727_272_7);
    assert_close(a().ks_test(&b()), 0.636_363_636_363_636_4);

    let h = a();
    assert_eq!(h.cohens_d(&h), 0.0);
    assert_eq!(h.cliffs_d(&h), 0.0);
    assert_eq!(h.ks_test(&h), 0.0);
    // An identical but distinct histogram has no difference either.
    assert_eq!(h.ks_test(&a()), 0.0);
    assert_eq!(h.cliffs_d(&a()), 0.0);
}
