// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::time::Duration;
use std::time::SystemTime;

use super::time_from_ns;
use super::time_to_ns;

#[test]
fn converts_times_to_and_from_nanoseconds_around_the_epoch() {
    for ns in [0, 1, -1, 3_600_000_000_001, -3_599_999_999_999] {
        assert_eq!(time_to_ns(time_from_ns(ns)), ns);
    }
    assert_eq!(
        time_from_ns(-1_500_000_000),
        SystemTime::UNIX_EPOCH - Duration::from_millis(1_500)
    );
    assert_eq!(
        time_from_ns(2_000_000_001),
        SystemTime::UNIX_EPOCH + Duration::new(2, 1)
    );
}

#[test]
fn saturates_times_beyond_the_range_of_nanoseconds() {
    let far = Duration::from_secs(u64::from(u32::MAX) * 1_000);
    assert_eq!(time_to_ns(SystemTime::UNIX_EPOCH + far), i64::MAX);
    assert_eq!(time_to_ns(SystemTime::UNIX_EPOCH - far), i64::MIN);
}
