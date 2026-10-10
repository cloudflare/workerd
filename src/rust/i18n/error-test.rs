// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use jsg::ExceptionType;

use super::*;

#[test]
fn allocation_failure_is_a_range_error() {
    let error = jsg::Error::from(TranscodeError::AllocationFailed);
    assert_eq!(error.name, ExceptionType::RangeError);
    assert_eq!(error.message, "Failed to allocate memory for Uint8Array");
}

#[test]
fn other_failures_are_plain_errors() {
    let error = jsg::Error::from(TranscodeError::OddUtf16leInput);
    assert_eq!(error.name, ExceptionType::Error);
}
