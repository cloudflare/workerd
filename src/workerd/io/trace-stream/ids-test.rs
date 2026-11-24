// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

#[test]
fn trace_id_go_drops_a_zero_high_half() {
    assert_eq!(trace_id_go(0, 0x2a), "000000000000002a");
    assert_eq!(trace_id_go(0, 0), "0000000000000000");
    assert_eq!(
        trace_id_go(0x0123_4567_89ab_cdef, 0xfedc_ba98_7654_3210),
        "0123456789abcdeffedcba9876543210"
    );
    assert_eq!(trace_id_go(1, 0), "00000000000000010000000000000000");
}

#[test]
fn trace_id_w3c_always_has_32_digits() {
    assert_eq!(trace_id_w3c(0, 0x2a), "0000000000000000000000000000002a");
    assert_eq!(
        trace_id_w3c(u64::MAX, u64::MAX),
        "ffffffffffffffffffffffffffffffff"
    );
}

#[test]
fn span_id_go_has_16_digits() {
    assert_eq!(span_id_go(0), "0000000000000000");
    assert_eq!(span_id_go(0xab), "00000000000000ab");
    assert_eq!(span_id_go(u64::MAX), "ffffffffffffffff");
}

#[test]
fn uuid_groups_the_digits() {
    assert_eq!(
        uuid(0x0123_4567_89ab_cdef, 0x0fed_cba9_8765_4321)
            .as_ref()
            .map(IdText::as_str),
        Some("01234567-89ab-cdef-0fed-cba987654321")
    );
    assert_eq!(
        uuid(0, 1).as_ref().map(IdText::as_str),
        Some("00000000-0000-0000-0000-000000000001")
    );
    assert_eq!(
        uuid(1, 0).as_ref().map(IdText::as_str),
        Some("00000000-0000-0001-0000-000000000000")
    );
}

#[test]
fn uuid_of_zero_is_none() {
    assert!(uuid(0, 0).is_none());
}
