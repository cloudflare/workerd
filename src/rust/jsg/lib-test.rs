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
