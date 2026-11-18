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
    let err = from_description("remote.jsg.AbortError: boom");
    assert_eq!(err.name, ExceptionType::AbortError);
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
    assert_eq!(err.name, ExceptionType::AbortError);
    assert_eq!(err.message, "boom");
    assert!(!err.is_internal);
}

#[test]
fn dom_exception_with_expected_prefix() {
    let err = from_description("expected someCondition; jsg.DOMException(AbortError): boom");
    assert_eq!(err.name, ExceptionType::AbortError);
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
    assert_eq!(err.name, ExceptionType::OperationError);
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
fn do_not_log_marker_is_kept() {
    // The KJ exception C++ jsg::Deserializer throws for an unsupported header.
    let err = from_description("worker_do_not_log; message = Unable to deserialize cloned data");
    assert!(err.is_internal);
    assert!(err.do_not_log);
    assert_eq!(
        err.internal_error_log(),
        v8::ffi::InternalErrorLog::DoNotLog
    );

    let err = from_description("remote exception: worker_do_not_log; script exceeded time limit");
    assert!(err.do_not_log);
    let err = from_description("broken.outputGateBroken; worker_do_not_log; failed");
    assert!(err.do_not_log);
}

#[test]
fn do_not_log_marker_anywhere_is_kept_for_a_tunneled_error() {
    let err = from_description("jsg.TypeError: worker_do_not_log boom");
    assert_eq!(err.name, ExceptionType::TypeError);
    assert!(err.do_not_log);
    let err = from_description("expected x; worker_do_not_log; jsg.TypeError: boom");
    assert_eq!(err.name, ExceptionType::TypeError);
    assert!(err.do_not_log);
}

#[test]
fn do_not_log_marker_is_looked_for_where_cpp_looks() {
    // As C++ makeDefaultError() looks for it: for an untunneled "expected ..." description, only
    // in what follows its last "; ".
    let err = from_description("expected x; worker_do_not_log; failed");
    assert!(err.is_internal);
    assert!(!err.do_not_log);
    let err = from_description("expected x; failed: worker_do_not_log");
    assert!(err.do_not_log);
}

#[test]
fn errors_without_the_marker_are_logged() {
    for description in [
        "some unrelated kj exception",
        "expected someCondition; some message",
        "jsg-internal.TypeError: boom",
    ] {
        let err = from_description(description);
        assert!(!err.do_not_log, "{description}");
        assert_eq!(
            err.internal_error_log(),
            v8::ffi::InternalErrorLog::WithId,
            "{description}"
        );
    }
    assert!(!Error::new_internal_error("worker_do_not_log").do_not_log);
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

#[test]
fn dom_exception_name_outside_exception_type_is_kept() {
    let err = from_description("expected q; jsg.DOMException(QuotaExceededError): full");
    assert_eq!(
        err.name,
        ErrorName::DomException("QuotaExceededError".into())
    );
    assert!(err.name.is_dom_exception());
    assert_eq!(err.name.class(), None);
    assert_eq!(err.message, "full");
}

#[test]
fn dom_syntax_error_is_not_the_javascript_class() {
    let err = from_description("jsg.DOMException(SyntaxError): bad");
    assert!(err.name.is_dom_exception());
    assert_ne!(err.name, ExceptionType::SyntaxError);
    assert_eq!(
        from_description("jsg.SyntaxError: bad").name,
        ExceptionType::SyntaxError
    );
}

#[test]
fn error_names_compare_by_what_they_throw() {
    let dom = ErrorName::DomException("DataError".into());
    assert_eq!(ErrorName::from(ExceptionType::DataError), dom);
    assert_eq!(ErrorName::Class(ExceptionType::DataError), dom);
    assert_eq!(dom, ExceptionType::DataError);
    assert_eq!(ExceptionType::DataError, dom);
    assert_ne!(
        ErrorName::Class(ExceptionType::SyntaxError),
        ErrorName::DomException("SyntaxError".into())
    );
    assert_eq!(
        ErrorName::from(ExceptionType::TypeError).class(),
        Some(ExceptionType::TypeError)
    );
    assert_eq!(ErrorName::Class(ExceptionType::AbortError).class(), None);
}
