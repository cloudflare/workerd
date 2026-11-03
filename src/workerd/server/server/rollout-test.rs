// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use futures::executor::block_on;

use super::*;

#[test]
fn sigterm_during_a_pending_roll_drains_without_a_restart() {
    let stop = Stop::default();
    assert_eq!(block_on(drain_signal(async {}, stop.clone())), None);
    // The watch finds its turn while the server drains.
    stop.request(Reason::Restart);
    assert_eq!(stop.reason(), Some(Reason::Drain));
}

#[test]
fn a_restart_drains_for_a_bounded_time() {
    let stop = Stop::default();
    let drain = drain_signal(std::future::pending(), stop.clone());
    stop.request(Reason::Restart);
    assert_eq!(block_on(drain), Some(RESTART_DRAIN));
    assert_eq!(stop.reason(), Some(Reason::Restart));
}

#[test]
fn a_restart_and_sigterm_at_once_stay_a_restart() {
    let stop = Stop::default();
    stop.request(Reason::Restart);
    assert_eq!(
        block_on(drain_signal(async {}, stop.clone())),
        Some(RESTART_DRAIN)
    );
    assert_eq!(stop.reason(), Some(Reason::Restart));
}
