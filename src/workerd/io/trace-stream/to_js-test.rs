// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#![expect(
    clippy::float_cmp,
    reason = "a duration is a whole number of milliseconds"
)]

use super::*;

#[test]
fn http_methods_end_at_ban() {
    assert_eq!(HTTP_METHODS.len(), 28);
    assert_eq!(HTTP_METHODS[22], "MSEARCH");
    assert_eq!(HTTP_METHODS[27], "BAN");
}

#[test]
fn durations_are_whole_milliseconds_of_a_kj_duration() {
    assert_eq!(duration_millis(0), 0.0);
    assert_eq!(duration_millis(345), 345.0);
    // kj::Duration holds signed nanoseconds, which wrap past i64::MAX.
    let past = u64::try_from(i64::MAX / 1_000_000).unwrap() + 1;
    let wrapped = past.wrapping_mul(1_000_000).cast_signed() / 1_000_000;
    #[expect(clippy::cast_precision_loss)]
    let expected = wrapped as f64;
    assert_eq!(duration_millis(past), expected);
    assert!(duration_millis(past) < 0.0);
}

#[test]
fn enumerants_the_schema_does_not_know_are_numbers() {
    let name = |value: u8| match value {
        0 => "zero",
        _ => "other",
    };
    assert_eq!(enumerant(Ok(0), name), "zero");
    assert_eq!(enumerant::<u8>(Err(capnp::NotInSchema(42)), name), "42");
}
