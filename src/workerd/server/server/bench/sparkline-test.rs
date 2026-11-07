// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

fn linear(samples: &[f64]) -> String {
    render(samples, Binning::Linear, Charset::Unicode).unwrap()
}

fn qrde(samples: &[f64]) -> String {
    render(samples, Binning::Qrde, Charset::Unicode).unwrap()
}

/// `count` samples evenly spaced from `low` to `high`.
fn spaced(count: usize, low: f64, high: f64) -> Vec<f64> {
    (0..count)
        .map(|i| low + (high - low) * i as f64 / (count - 1) as f64)
        .collect()
}

#[test]
fn linear_counts_samples_in_equal_width_columns() {
    // 0 and 16 bound the axis, so each column is 1 ns wide; 16 falls in the last column.
    let samples = [0.0, 0.5, 0.9, 1.0, 8.0, 8.0, 8.0, 8.0, 16.0];
    assert_eq!(linear(&samples), "▆▂      █      ▂");
}

#[test]
fn every_sparkline_spans_the_full_width() {
    let samples = spaced(50, 100.0, 200.0);
    for binning in [Binning::Linear, Binning::Qrde] {
        let line = render(&samples, binning, Charset::Unicode).unwrap();
        assert_eq!(line.chars().count(), WIDTH, "{binning:?}: {line:?}");
    }
}

#[test]
fn a_uniform_distribution_is_flat() {
    let tall = |c: char| matches!(c, '▆' | '▇' | '█');
    let samples = spaced(64, 100.0, 200.0);
    // Columns hold 3 or 4 of the samples.
    let line = linear(&samples);
    assert!(line.chars().all(tall), "{line}");
    // The density estimate is smooth, but not exactly flat at the edges.
    let line = qrde(&samples);
    let middle: String = line.chars().skip(2).take(WIDTH - 4).collect();
    assert!(middle.chars().all(tall), "{line}");
}

#[test]
fn a_bimodal_distribution_has_two_peaks() {
    let mut samples = vec![10.0; 20];
    samples.extend([20.0; 20]);
    samples.extend([10.5, 19.5]);
    for binning in [Binning::Linear, Binning::Qrde] {
        let line = render(&samples, binning, Charset::Unicode).unwrap();
        let chars: Vec<char> = line.chars().collect();
        assert_eq!(chars[0], '█', "{binning:?}: {line}");
        assert_eq!(chars[WIDTH - 1], '█', "{binning:?}: {line}");
        let middle = &chars[WIDTH / 2 - 2..WIDTH / 2 + 2];
        assert!(
            middle.iter().all(|&c| c == ' ' || c == '▁'),
            "{binning:?}: {line}"
        );
    }
}

#[test]
fn an_outlier_stretches_the_axis_and_stays_visible() {
    let mut samples = vec![10.0; 49];
    samples.push(100.0);
    assert_eq!(linear(&samples), "█              ▁");
    let line = qrde(&samples);
    assert!(line.starts_with('█'), "{line}");
    assert!(!line.ends_with(' '), "{line}");
}

#[test]
fn equal_samples_are_one_full_column() {
    assert_eq!(linear(&[5.0, 5.0, 5.0]), "█");
    assert_eq!(qrde(&[5.0]), "█");
}

#[test]
fn quick_runs_with_few_samples_still_draw() {
    let samples = [10.0, 11.0, 12.0, 13.0, 30.0];
    // Columns are 1.25 ns wide: 10 and 11 share the first.
    assert_eq!(linear(&samples), "█▄▄            ▄");
    assert_eq!(qrde(&samples).chars().count(), WIDTH);
}

#[test]
fn samples_that_are_not_finite_are_left_out() {
    assert_eq!(linear(&[]), "");
    assert_eq!(linear(&[f64::NAN, f64::INFINITY]), "");
    assert_eq!(linear(&[f64::NAN, 0.0, 16.0]), "█              █");
}

#[test]
fn zero_samples_are_estimated() {
    // Samples are clamped at zero once the overhead is subtracted.
    let mut samples = vec![0.0; 10];
    samples.extend(spaced(10, 1.0, 2.0));
    assert_eq!(qrde(&samples).chars().count(), WIDTH);
}

#[test]
fn ascii_uses_the_same_levels() {
    let samples = [0.0, 0.5, 0.9, 1.0, 8.0, 8.0, 8.0, 8.0, 16.0];
    let line = render(&samples, Binning::Linear, Charset::Ascii).unwrap();
    assert_eq!(line, "*:      @      :");
    assert!(line.is_ascii());
}

#[test]
fn utf8_locales_get_unicode_and_others_ascii() {
    assert_eq!(Charset::for_locale(None), Charset::Unicode);
    assert_eq!(Charset::for_locale(Some("en_US.UTF-8")), Charset::Unicode);
    assert_eq!(Charset::for_locale(Some("C.utf8")), Charset::Unicode);
    assert_eq!(Charset::for_locale(Some("C")), Charset::Ascii);
    assert_eq!(Charset::for_locale(Some("POSIX")), Charset::Ascii);
    assert_eq!(
        Charset::for_locale(Some("en_US.ISO-8859-1")),
        Charset::Ascii
    );
}

#[test]
fn metrics_select_wall_cpu_or_both() {
    assert!(Metrics::Wall.wall() && !Metrics::Wall.cpu());
    assert!(!Metrics::Cpu.wall() && Metrics::Cpu.cpu());
    assert!(Metrics::Both.wall() && Metrics::Both.cpu());
}
