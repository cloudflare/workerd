// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

#[test]
fn clocks_advance_with_work() {
    let start = now();
    let mut x = 0u64;
    while (now() - start).wall_ns < 20_000_000 {
        x = std::hint::black_box(x.wrapping_add(1));
    }
    let elapsed = now() - start;
    assert!(elapsed.wall_ns >= 20_000_000);
    // A busy loop on this thread uses CPU time, though the scheduler may run others too.
    assert!(elapsed.cpu_ns > 0);
}

#[test]
fn monotonic_never_goes_back() {
    let mut last = now().wall_ns;
    for _ in 0..10_000 {
        let next = now().wall_ns;
        assert!(next >= last);
        last = next;
    }
}
