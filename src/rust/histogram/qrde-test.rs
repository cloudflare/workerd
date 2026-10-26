// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;
use crate::Options;

fn histogram_of(values: impl IntoIterator<Item = i64>) -> Histogram {
    let mut h = Histogram::new(&Options::default()).unwrap();
    for value in values {
        assert!(h.record(value));
    }
    h
}

#[track_caller]
fn assert_all_close(actual: &[f64], expected: &[f64]) {
    assert_eq!(actual.len(), expected.len(), "{actual:?} != {expected:?}");
    for (&a, &e) in actual.iter().zip(expected) {
        let tolerance = 1e-12 * e.abs().max(1.0);
        assert!((a - e).abs() <= tolerance, "{actual:?} != {expected:?}");
    }
}

// The expected values in the tests below are from Node.js 26.10.

fn small() -> Histogram {
    histogram_of([1, 2, 2, 3, 10, 10, 10, 50, 4000, 4001, 4003, 9000])
}

#[test]
fn dequantization_modes() {
    let probabilities = Qrde::uniform_probabilities(4).unwrap();
    let cases = [
        (
            Dequantization::None,
            [
                1.0,
                7.901_545_315_850_485,
                464.444_939_154_141_6,
                3_344.996_260_152_889_6,
                9004.0,
            ],
            [
                0.036_223_771_424_906_195,
                0.000_547_593_073_022_431_4,
                0.000_086_788_941_470_176_53,
                0.000_044_177_387_309_299_474,
            ],
        ),
        (
            Dequantization::Hdr,
            [
                1.0,
                7.900_992_940_269_17,
                464.406_433_915_480_93,
                3_345.029_772_886_711_3,
                9004.0,
            ],
            [
                0.036_226_670_881_110_75,
                0.000_547_638_598_711_849_8,
                0.000_086_786_771_674_662_48,
                0.000_044_177_648_930_223_854,
            ],
        ),
        (
            Dequantization::All,
            [
                1.0,
                7.880_707_017_028_898,
                464.436_052_572_011_9,
                3_345.051_412_877_351_6,
                9004.0,
            ],
            [
                0.036_333_475_525_302_986,
                0.000_547_578_738_117_945_2,
                0.000_086_787_012_054_778_62,
                0.000_044_177_817_866_890_21,
            ],
        ),
    ];
    for (dequantization, quantiles, densities) in cases {
        let result = small().qrde(&probabilities, dequantization).unwrap();
        assert_all_close(&result.quantiles, &quantiles);
        assert_all_close(&result.densities, &densities);
        assert_eq!(result.corrections, 0);
        assert_eq!(result.bucket_count, 8);
        assert_eq!(result.count, 12);
    }
}

/// The MINSTD sequence, which the Node.js script that produced the expected values computes
/// exactly in doubles.
fn minstd(seed: i64) -> impl FnMut() -> i64 {
    let mut state = seed;
    move || {
        state = state * 48271 % 2_147_483_647;
        state
    }
}

#[test]
fn many_buckets_use_the_exact_support() {
    let mut next = minstd(12345);
    let h = histogram_of((0..20_000).map(|_| {
        let s = next();
        1 + (s >> (s % 21))
    }));
    let result = h
        .qrde(
            &[0.0, 0.01, 0.25, 0.5, 0.75, 0.99, 0.999, 1.0],
            Dequantization::Hdr,
        )
        .unwrap();
    assert_eq!(result.bucket_count, 13234);
    assert_eq!(result.corrections, 0);
    assert_all_close(
        &result.quantiles,
        &[
            1.0,
            207.509_565_749_821_06,
            20_021.716_189_524_934,
            780_440.176_928_827_5,
            29_248_602.941_716_09,
            1_675_808_889.639_776,
            2_089_650_581.838_535,
            2_146_959_360.0,
        ],
    );
}

#[test]
fn large_counts_use_the_asymptotic_cdf() {
    let h = histogram_of((1..=200).flat_map(|v| std::iter::repeat_n(v * 37, 1000)));
    let result = h
        .qrde(
            &Qrde::uniform_probabilities(5).unwrap(),
            Dequantization::Hdr,
        )
        .unwrap();
    assert_eq!(result.count, 200_000);
    assert_eq!(result.bucket_count, 200);
    assert_all_close(
        &result.quantiles,
        &[
            37.0,
            1_498.483_497_180_411_2,
            2_978.995_877_823_859,
            4_460.003_869_660_284,
            5_940.014_272_653_952_5,
            7402.0,
        ],
    );
}

#[test]
fn single_bucket_spreads_both_ways() {
    let result = histogram_of([7, 7, 7])
        .qrde(&[0.0, 0.5, 1.0], Dequantization::All)
        .unwrap();
    assert_eq!(result.quantiles, [6.5, 7.0, 7.5]);
    assert_eq!(result.densities, [1.0, 1.0]);

    // Without dequantization, every bin has zero width.
    let result = histogram_of([7, 7, 7])
        .qrde(&[0.0, 0.5, 1.0], Dequantization::None)
        .unwrap();
    assert_eq!(result.densities, [f64::INFINITY, f64::INFINITY]);
}

#[test]
fn empty_histogram() {
    let result = histogram_of([])
        .qrde(&[0.0, 1.0], Dequantization::Hdr)
        .unwrap();
    assert!(result.quantiles.is_empty());
    assert!(result.densities.is_empty());
    assert_eq!(result.bucket_count, 0);
}

#[test]
fn invalid_probabilities() {
    let h = small();
    for probabilities in [
        &[0.0][..],
        &[0.0, 0.5],
        &[0.1, 1.0],
        &[0.0, 0.5, 0.5, 1.0],
        &[0.0, 0.7, 0.3, 1.0],
        &[0.0, f64::NAN, 1.0],
        &[-0.0, 1.5],
    ] {
        assert!(
            h.qrde(probabilities, Dequantization::Hdr).is_err(),
            "{probabilities:?}"
        );
    }
    assert!(h.qrde(&vec![0.0; 1002], Dequantization::Hdr).is_err());
    assert!(Qrde::uniform_probabilities(0).is_err());
    assert!(Qrde::uniform_probabilities(1001).is_err());
    assert_eq!(Qrde::uniform_probabilities(2).unwrap(), [0.0, 0.5, 1.0]);
}
