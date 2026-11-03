// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

fn from_description(description: &str) -> Error {
    Error::from_kj_description(description)
}

#[test]
fn plain_jsg_prefix() {
    let err = from_description("jsg.TypeError: boom");
    assert_eq!(err.name, ExceptionType::TypeError);
    assert_eq!(err.message, "boom");
    assert!(!err.is_internal, "jsg. errors must not be redacted");
}

#[test]
fn jsg_require_wraps_with_expected_prefix() {
    // What JSG_REQUIRE(cond, TypeError, "boom") actually produces via KJ_REQUIRE.
    let err = from_description("expected someCondition; jsg.TypeError: boom");
    assert_eq!(err.name, ExceptionType::TypeError);
    assert_eq!(err.message, "boom");
    assert!(!err.is_internal);
}

#[test]
fn nested_expected_prefixes() {
    let err = from_description("expected a; expected b; jsg.RangeError: boom");
    assert_eq!(err.name, ExceptionType::RangeError);
    assert_eq!(err.message, "boom");
    assert!(!err.is_internal);
}

#[test]
fn broken_prefix() {
    let err = from_description("broken.inputGateBroken; jsg.Error: boom");
    assert_eq!(err.name, ExceptionType::Error);
    assert_eq!(err.message, "boom");
    assert!(!err.is_internal);
}

#[test]
fn remote_prefix() {
    let err = from_description("remote.jsg.DOMException(AbortError): boom");
    assert_eq!(err.name, ExceptionType::DOMAbortError);
    assert_eq!(err.message, "boom");
    assert!(!err.is_internal);
}

#[test]
fn remote_exception_prefix() {
    let err = from_description("remote exception: jsg.TypeError: boom");
    assert_eq!(err.name, ExceptionType::TypeError);
    assert_eq!(err.message, "boom");
    assert!(!err.is_internal);
}

#[test]
fn dom_exception() {
    let err = from_description("jsg.DOMException(AbortError): boom");
    assert_eq!(err.name, ExceptionType::DOMAbortError);
    assert_eq!(err.message, "boom");
    assert!(!err.is_internal);
}

#[test]
fn dom_exception_with_expected_prefix() {
    let err = from_description("expected someCondition; jsg.DOMException(AbortError): boom");
    assert_eq!(err.name, ExceptionType::DOMAbortError);
    assert_eq!(err.message, "boom");
    assert!(!err.is_internal);
}

#[test]
fn jsg_internal_prefix_preserves_type_but_is_redacted() {
    let err = from_description("jsg-internal.TypeError: boom");
    assert_eq!(err.name, ExceptionType::TypeError);
    assert_eq!(err.message, "boom");
    assert!(
        err.is_internal,
        "jsg-internal. errors must be redacted by Lock::throw_exception()"
    );
}

#[test]
fn jsg_internal_dom_exception_is_redacted() {
    let err = from_description("jsg-internal.DOMException(OperationError): boom");
    assert_eq!(err.name, ExceptionType::DOMOperationError);
    assert_eq!(err.message, "boom");
    assert!(err.is_internal);
}

#[test]
fn unrecognized_description_is_redacted() {
    let err = from_description("some unrelated kj exception");
    assert_eq!(err.message, "some unrelated kj exception");
    assert!(
        err.is_internal,
        "untunneled descriptions must be redacted, matching C++ makeDefaultError()"
    );
}

#[test]
fn expected_prefix_without_tunneling_tag_is_redacted() {
    let err = from_description("expected someCondition; some message");
    assert_eq!(err.message, "expected someCondition; some message");
    assert!(err.is_internal);
}

#[test]
fn dom_syntax_error_is_distinct_from_js_syntax_error() {
    let dom = from_description("jsg.DOMException(SyntaxError): boom");
    assert_eq!(dom.name, ExceptionType::DOMSyntaxError);
    let js = from_description("jsg.SyntaxError: boom");
    assert_eq!(js.name, ExceptionType::SyntaxError);
}

#[test]
fn dom_exception_without_dom_type_keeps_its_name() {
    let err = from_description("jsg.DOMException(IndexSizeError): boom");
    assert_eq!(err.name, ExceptionType::Error);
    assert_eq!(err.dom_exception_name(), Some("IndexSizeError"));
    assert_eq!(err.message, "boom");
    assert!(!err.is_internal);
}

/// Every `ExceptionType` variant, found through the variant name the cxx shared
/// enum's `Debug` prints (`ExceptionType(<repr>)` for values that are not one).
/// This depends on that `Debug` format; it lets the test below check
/// `ExceptionType::DOM_EXCEPTION_NAMES` against the enum without listing the
/// variants a second time.
fn all_exception_types() -> Vec<(ExceptionType, String)> {
    (0..=u8::MAX)
        .map(|repr| ExceptionType { repr })
        .map(|ty| (ty, format!("{ty:?}")))
        .filter(|(_, variant)| !variant.starts_with("ExceptionType("))
        .collect()
}

#[test]
fn dom_prefixed_variants_are_exactly_the_dom_exception_types() {
    let types = all_exception_types();
    assert_eq!(types.len(), 18);
    for (ty, variant) in types {
        assert_eq!(
            ty.dom_exception_name(),
            variant.strip_prefix("DOM"),
            "{variant}"
        );
    }
}

#[test]
fn every_type_round_trips_through_a_tunneled_description() {
    for (ty, variant) in all_exception_types() {
        let err = from_description(&format!("jsg.{ty}: boom"));
        assert_eq!(err.name, ty, "{variant}");
        assert_eq!(err.message, "boom", "{variant}");
    }
}

#[test]
fn dom_exception_names_round_trip() {
    for (ty, name) in ExceptionType::DOM_EXCEPTION_NAMES {
        assert_eq!(ty.dom_exception_name(), Some(name));
        assert_eq!(ExceptionType::from_dom_exception_name(name), Some(ty));
        assert_eq!(ty.to_string(), format!("DOMException({name})"));
    }
    assert_eq!(ExceptionType::SyntaxError.dom_exception_name(), None);
    assert_eq!(ExceptionType::from_dom_exception_name("TypeError"), None);
}

#[test]
fn dom_names_are_not_js_error_names() {
    assert_eq!(ExceptionType::from("AbortError"), ExceptionType::Error);
    assert_eq!(
        ExceptionType::from("SyntaxError"),
        ExceptionType::SyntaxError
    );
}

// =============================================================================
// error_message_for_v8 / utf8_prefix
// =============================================================================

#[test]
fn utf8_prefix_keeps_bytes_that_fit() {
    assert_eq!(utf8_prefix(b"abc", 3), b"abc");
    assert_eq!(utf8_prefix(b"abc", 4), b"abc");
    assert_eq!(utf8_prefix(b"", 0), b"");
}

#[test]
fn utf8_prefix_cuts_ascii_at_the_limit() {
    assert_eq!(utf8_prefix(b"abcdef", 4), b"abcd");
    assert_eq!(utf8_prefix(b"abcdef", 1), b"a");
    assert_eq!(utf8_prefix(b"abcdef", 0), b"");
}

#[test]
fn utf8_prefix_does_not_cut_inside_a_sequence() {
    // "aé" is 61 C3 A9: a limit of 2 falls inside the two-byte sequence.
    let bytes = "aé".as_bytes();
    assert_eq!(utf8_prefix(bytes, 2), b"a");
    assert_eq!(utf8_prefix(bytes, 1), b"a");
    assert_eq!(utf8_prefix(bytes, 3), bytes);

    // A four-byte sequence followed by an ASCII byte.
    let bytes = "😀z".as_bytes();
    for limit in 1..4 {
        assert_eq!(utf8_prefix(bytes, limit), b"", "limit {limit}");
    }
    assert_eq!(utf8_prefix(bytes, 4), "😀".as_bytes());
    assert_eq!(utf8_prefix(bytes, 5), bytes);

    // The cut lands between two multi-byte characters.
    let bytes = "éé".as_bytes();
    assert_eq!(utf8_prefix(bytes, 2), "é".as_bytes());
    assert_eq!(utf8_prefix(bytes, 3), "é".as_bytes());
}

#[test]
fn utf8_prefix_of_invalid_bytes_moves_back_at_most_three() {
    // Continuation bytes with no lead byte are not valid UTF-8. The cut moves back over at most
    // three of them, and never runs off the start.
    assert_eq!(utf8_prefix(&[0x80, 0x80, 0x80], 2), b"");
    assert_eq!(utf8_prefix(&[0x61, 0x80, 0x80, 0x80], 2), b"");
    assert_eq!(utf8_prefix(&[0x80; 8], 6), &[0x80; 3]);
    assert_eq!(utf8_prefix(&[0x80; 8], 3), b"");
}

#[test]
fn error_message_for_v8_keeps_a_short_message() {
    assert_eq!(error_message_for_v8(b"boom"), b"boom");
    assert_eq!(error_message_for_v8(b""), b"");
}

#[test]
fn error_message_for_v8_cuts_to_v8s_limit() {
    let max = usize::try_from(v8::String::MAX_LENGTH).unwrap();
    // Zeroed, so that the allocation costs no physical memory until it is read, and the cut
    // reads one byte of it.
    let message = vec![0u8; max + 1];
    assert_eq!(error_message_for_v8(&message).len(), max);
    assert_eq!(error_message_for_v8(&message[..max]).len(), max);
}
