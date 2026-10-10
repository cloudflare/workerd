// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

#[test]
fn monotonic_clock_does_not_go_backwards() {
    let clock = MonotonicClock;
    let first = clock.now();
    let second = clock.now();
    assert!(second >= first);
}

#[test]
fn epoch_wall_clock_is_plausible() {
    // 2020-01-01T00:00:00Z. Anything earlier means the conversion is broken.
    assert!(epoch_unix_ms() > 1_577_836_800_000);
}

#[test]
fn epoch_is_fixed() {
    assert_eq!(epoch_unix_ms(), epoch_unix_ms());
}
