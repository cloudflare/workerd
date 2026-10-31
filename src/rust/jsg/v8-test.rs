// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

#[test]
fn v8_length_accepts_up_to_max_length() {
    assert_eq!(String::v8_length(0), Some(0));
    assert_eq!(String::v8_length(1), Some(1));
    let max = usize::try_from(String::MAX_LENGTH).unwrap();
    assert_eq!(String::v8_length(max), Some(String::MAX_LENGTH));
}

#[test]
fn v8_length_refuses_longer_input() {
    let max = usize::try_from(String::MAX_LENGTH).unwrap();
    assert_eq!(String::v8_length(max + 1), None);
    // Longer than V8's `int` length parameter, which must never see it.
    assert_eq!(
        String::v8_length(usize::try_from(i32::MAX).unwrap() + 1),
        None
    );
    assert_eq!(String::v8_length(usize::MAX), None);
}
