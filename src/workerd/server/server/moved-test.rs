// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use cxx::KjException;

use super::Reach;
use super::Seen;
use super::classify;
use crate::bridge::ffi;
use crate::cluster::moved;
use crate::cluster::not_delivered;

fn delivered() -> u64 {
    ffi::request_delivered_detail_id()
}

fn has(error: &KjException, id: u64) -> bool {
    error.detail(id).is_some()
}

#[test]
fn moved_is_seen_at_either_reach_and_left_unmarked() {
    for reach in [Reach::Local, Reach::Peer] {
        let mut error = KjException::from(moved("the Durable Object moved"));
        assert_eq!(classify(&mut error, reach), Some(Seen::Moved));
        assert!(!has(&error, delivered()));
    }
}

#[test]
fn moved_from_a_peer_marked_delivered_is_seen_and_left_unchanged() {
    let mut error = KjException::from(moved("the Durable Object moved"));
    error.set_detail(delivered(), &[]);
    let details = error.details();
    assert_eq!(classify(&mut error, Reach::Peer), Some(Seen::Moved));
    assert_eq!(error.details(), details);
}

#[test]
fn local_failure_is_left_alone() {
    let mut error = KjException::from(kj::disconnected!("gone"));
    assert_eq!(classify(&mut error, Reach::Local), None);
    assert!(error.details().is_none());
}

#[test]
fn peer_disconnect_is_lost_and_marked_delivered() {
    let mut error = KjException::from(kj::disconnected!("gone"));
    assert_eq!(classify(&mut error, Reach::Peer), Some(Seen::Lost));
    assert!(has(&error, delivered()));
    assert_eq!(error.what(), "gone");
}

#[test]
fn peer_failure_is_marked_delivered_without_telling_the_hook() {
    let mut error = KjException::from(kj::failed!("thrown"));
    assert_eq!(classify(&mut error, Reach::Peer), None);
    assert!(has(&error, delivered()));
}

#[test]
fn peer_failure_marked_by_either_side_is_left_alone() {
    let mut before = KjException::from(not_delivered(kj::disconnected!("unsent")));
    assert_eq!(classify(&mut before, Reach::Peer), None);
    assert!(!has(&before, delivered()));

    let mut after = KjException::from(kj::disconnected!("ran"));
    after.set_detail(delivered(), &[]);
    assert_eq!(classify(&mut after, Reach::Peer), None);
    assert_eq!(after.details().map(|d| d.len()), Some(1));
}
