// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#![expect(
    clippy::float_cmp,
    reason = "imported floats must round-trip bit for bit"
)]

use super::*;

const MAX_SAFE_INTEGER: i64 = (1 << 53) - 1;

fn from_hex(hex: &str) -> Vec<u8> {
    (0..hex.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(&hex[i..i + 2], 16).unwrap())
        .collect()
}

/// Node.js 26.10 exports version 1, which differs from version 2 only in the version value: the
/// third byte, after the map header and key 0.
fn as_version_2(mut bytes: Vec<u8>) -> Vec<u8> {
    assert_eq!(bytes[1..3], [0x00, 0x01]);
    bytes[2] = 0x02;
    bytes
}

fn new_histogram(options: Options, values: &[i64]) -> Histogram {
    let mut h = Histogram::new(&options).unwrap();
    for &value in values {
        assert!(h.record(value));
    }
    h
}

// Exports of the same histograms by Node.js 26.10.
const EMPTY: &str = "ab00010101021b001fffffffffffff03030400051b7fffffffffffffff0600070008fb3ff00000000000000919b0000a80";
const VALUES: &str = "ab00010101021a000f4240030204060501061a000f423f070008fb3ff0000000000000091907000a8a01010402190111011901e4011903fa01";
const EWMA: &str = "ac00010101021b001fffffffffffff03030403051832061896070008fb3ff00000000000000919b0000a86183201182801183c010ba500fb3fc45d819a94b14c01fb40516f8f0fcf2c8202fb4092caafef090d5403fb3fc12004cb6e5f84041864";

fn empty() -> Histogram {
    new_histogram(
        Options {
            highest: MAX_SAFE_INTEGER,
            ..Options::default()
        },
        &[],
    )
}

fn values() -> Histogram {
    new_histogram(
        Options {
            highest: 1_000_000,
            figures: 2,
            ..Options::default()
        },
        &[1, 5, 5, 300, 4000, 999_999],
    )
}

fn ewma() -> Histogram {
    new_histogram(
        Options {
            highest: MAX_SAFE_INTEGER,
            half_life: 4.0,
            threshold: 100,
            ..Options::default()
        },
        &[50, 150, 90],
    )
}

#[test]
fn export_matches_node() {
    assert_eq!(empty().export(), as_version_2(from_hex(EMPTY)));
    assert_eq!(values().export(), as_version_2(from_hex(VALUES)));
    assert_eq!(ewma().export(), as_version_2(from_hex(EWMA)));
}

#[test]
fn import_node_exports() {
    let h = Histogram::import(&from_hex(VALUES)).unwrap();
    assert_eq!(h.count(), 6);
    assert_eq!(h.figures(), 2);
    assert_eq!(h.highest(), 1_000_000);
    assert_eq!(h.min(), 1);
    assert_eq!(h.percentile(50.0), values().percentile(50.0));
    assert_eq!(h.export(), values().export());

    let h = Histogram::import(&from_hex(EMPTY)).unwrap();
    assert_eq!(h.count(), 0);
    assert_eq!(h.min(), i64::MAX);

    // Node.js reports these EWMA values for the same data.
    let h = Histogram::import(&from_hex(EWMA)).unwrap();
    assert_eq!(h.ewma_mean(), 69.743_106_796_568_13);
    assert_eq!(h.ewma_stddev(), 34.679_558_969_009_87);
    assert_eq!(h.ewma_error_rate(), 0.133_789_634_067_167_04);
    assert_eq!(h.export(), ewma().export());
}

#[test]
fn round_trip() {
    let mut h = values();
    h.record(0);
    let imported = Histogram::import(&h.export()).unwrap();
    assert_eq!(imported.export(), h.export());
    assert_eq!(imported.min(), 0);
    assert_eq!(imported.max(), h.max());
}

/// Builds export data from (key, encoded value) pairs.
fn map(entries: &[(u64, &[u8])]) -> Vec<u8> {
    let mut out = Vec::new();
    write_uint(&mut out, MAJOR_MAP, entries.len() as u64);
    for (key, value) in entries {
        write_uint(&mut out, MAJOR_UINT, *key);
        out.extend_from_slice(value);
    }
    out
}

fn uint(value: u64) -> Vec<u8> {
    let mut out = Vec::new();
    write_uint(&mut out, MAJOR_UINT, value);
    out
}

fn counts(pairs: &[u64]) -> Vec<u8> {
    let mut out = Vec::new();
    write_uint(&mut out, MAJOR_ARRAY, pairs.len() as u64);
    for &value in pairs {
        write_uint(&mut out, MAJOR_UINT, value);
    }
    out
}

// The counts array length of the default layout (1, i64::MAX, 3).
const DEFAULT_COUNTS_LEN: u64 = 54 * 1024;

#[test]
fn absent_fields_are_derived() {
    let data = map(&[
        (KEY_COUNTS_LEN, &uint(DEFAULT_COUNTS_LEN)),
        (KEY_COUNTS, &counts(&[3, 2, 4, 1])),
    ]);
    let h = Histogram::import(&data).unwrap();
    assert_eq!(h.count(), 3);
    assert_eq!(h.min(), 3);
    assert_eq!(h.max(), 7);
    assert_eq!(h.highest(), i64::MAX);
}

#[test]
fn unknown_keys_depend_on_version() {
    let len = uint(DEFAULT_COUNTS_LEN);
    let unknown: &[u8] = &[0x82, 0x01, 0x61, b'x'];
    let v1 = map(&[(KEY_COUNTS_LEN, &len), (40, unknown)]);
    assert_eq!(
        Histogram::import(&v1).unwrap_err(),
        Error::InvalidExportData
    );
    let v2 = map(&[
        (KEY_COUNTS_LEN, &len),
        (40, unknown),
        (KEY_VERSION, &uint(2)),
    ]);
    assert!(Histogram::import(&v2).is_ok());
    let v3 = map(&[(KEY_COUNTS_LEN, &len), (KEY_VERSION, &uint(3))]);
    assert!(Histogram::import(&v3).is_err());
}

#[test]
fn invalid_data_is_rejected() {
    let len = uint(DEFAULT_COUNTS_LEN);
    let cases: Vec<(&str, Vec<u8>)> = vec![
        ("empty input", vec![]),
        ("not a map", vec![0x80]),
        ("truncated", from_hex(VALUES)[..20].to_vec()),
        ("indefinite map", vec![0xbf, 0xff]),
        (
            "duplicate key",
            map(&[(KEY_COUNTS_LEN, &len), (KEY_COUNTS_LEN, &len)]),
        ),
        ("counts_len mismatch", map(&[(KEY_COUNTS_LEN, &uint(100))])),
        (
            "nonzero offset",
            map(&[(KEY_COUNTS_LEN, &len), (KEY_NORM_OFFSET, &uint(1))]),
        ),
        (
            "index out of range",
            map(&[
                (KEY_COUNTS_LEN, &len),
                (KEY_COUNTS, &counts(&[DEFAULT_COUNTS_LEN, 1])),
            ]),
        ),
        (
            "repeated index",
            map(&[(KEY_COUNTS_LEN, &len), (KEY_COUNTS, &counts(&[3, 1, 0, 1]))]),
        ),
        (
            "odd counts array",
            map(&[(KEY_COUNTS_LEN, &len), (KEY_COUNTS, &counts(&[3]))]),
        ),
        (
            "oversized counts array",
            map(&[
                (KEY_COUNTS_LEN, &len),
                (KEY_COUNTS, &[0x9b, 0x10, 0, 0, 0, 0, 0, 0, 0]),
            ]),
        ),
        (
            "total count mismatch",
            map(&[
                (KEY_COUNTS_LEN, &len),
                (KEY_TOTAL_COUNT, &uint(2)),
                (KEY_COUNTS, &counts(&[3, 1])),
            ]),
        ),
        (
            "count overflow",
            map(&[
                (KEY_COUNTS_LEN, &len),
                (KEY_COUNTS, &counts(&[1, i64::MAX as u64, 1, 1])),
            ]),
        ),
        (
            "count above i64::MAX",
            map(&[
                (KEY_COUNTS_LEN, &len),
                (KEY_COUNTS, &counts(&[1, u64::MAX])),
            ]),
        ),
        ("invalid layout", map(&[(KEY_FIGURES, &uint(6))])),
        (
            "deeply nested unknown value",
            map(&[(KEY_VERSION, &uint(2)), (40, &[0x81; 40])]),
        ),
    ];
    for (name, data) in cases {
        assert_eq!(
            Histogram::import(&data).unwrap_err(),
            Error::InvalidExportData,
            "{name}"
        );
    }
}
