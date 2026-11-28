// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

#[test]
fn go_text_omits_a_zero_high_half() {
    let id = TraceId {
        high: 0,
        low: 0x1234,
    };
    assert_eq!(id.to_go_text(), "0000000000001234");
    let id = TraceId {
        high: 0xabc,
        low: 0x1234,
    };
    assert_eq!(id.to_go_text(), "0000000000000abc0000000000001234");
    assert_eq!(id.to_string(), "0000000000000abc0000000000001234");
}

#[test]
fn w3c_text_always_has_32_digits() {
    let id = TraceId {
        high: 0,
        low: 0x1234,
    };
    assert_eq!(id.to_w3c_text(), "00000000000000000000000000001234");
}

#[test]
fn go_text_parses_as_jaeger_does() {
    // https://github.com/jaegertracing/jaeger/blob/e46f8737/model/ids.go#L58
    let parse = |s: &str| TraceId::from_go_text(s.as_bytes());
    assert_eq!(parse(""), Some(TraceId::NULL));
    assert_eq!(parse("1"), Some(TraceId { high: 0, low: 1 }));
    assert_eq!(parse("Ff"), Some(TraceId { high: 0, low: 0xff }));
    assert_eq!(
        parse("123456789abcdef0"),
        Some(TraceId {
            high: 0,
            low: 0x1234_5678_9abc_def0
        })
    );
    assert_eq!(
        parse("1123456789abcdef0"),
        Some(TraceId {
            high: 1,
            low: 0x1234_5678_9abc_def0
        })
    );
    assert_eq!(
        parse("ffffffffffffffffffffffffffffffff"),
        Some(TraceId {
            high: u64::MAX,
            low: u64::MAX
        })
    );
    assert_eq!(parse("fffffffffffffffffffffffffffffffff"), None);
    assert_eq!(parse("x"), None);
    assert_eq!(parse("x123456789abcdef0"), None);
    assert_eq!(parse("1x23456789abcdef0"), None);
    assert_eq!(parse("-1"), None);
}

#[test]
fn protobuf_is_big_endian_high_then_low() {
    let id = TraceId {
        high: 0x0102_0304_0506_0708,
        low: 0x090a_0b0c_0d0e_0f10,
    };
    let bytes = id.to_protobuf();
    assert_eq!(
        bytes,
        [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16]
    );
    assert_eq!(TraceId::from_protobuf(&bytes), Some(id));
    assert_eq!(TraceId::from_protobuf(&bytes[..15]), None);
    assert_eq!(TraceId::from_protobuf(&[0; 17]), None);
}

#[test]
fn span_id_text_has_16_digits() {
    assert_eq!(SpanId(0x2a).to_go_text(), "000000000000002a");
    assert_eq!(
        SpanId(0x2a2a_2a2a_2a2a_2a2a).to_string(),
        "2a2a2a2a2a2a2a2a"
    );
    assert_eq!(SpanId::NULL.to_go_text(), "0000000000000000");
}

#[test]
fn sampled_is_bit_zero() {
    assert!(TraceFlags(1).is_sampled());
    assert!(TraceFlags(0xff).is_sampled());
    assert!(!TraceFlags(0xfe).is_sampled());
}
