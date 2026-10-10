// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

#[test]
fn kind_from_u8_maps_known_values() {
    let expected = [
        Kind::Request,
        Kind::KjToJs,
        Kind::JsToKj,
        Kind::Timer,
        Kind::Microtask,
        Kind::Operation,
        Kind::JsPromise,
        Kind::Other,
    ];
    for (value, kind) in expected.iter().enumerate() {
        assert_eq!(Kind::from_u8(u8::try_from(value).unwrap()), *kind);
    }
}

#[test]
fn kind_from_u8_maps_unknown_values_to_other() {
    assert_eq!(Kind::from_u8(8), Kind::Other);
    assert_eq!(Kind::from_u8(255), Kind::Other);
}

#[test]
fn outcome_from_u8() {
    assert_eq!(Outcome::from_u8(0), Outcome::Ok);
    assert_eq!(Outcome::from_u8(1), Outcome::Error);
    assert_eq!(Outcome::from_u8(2), Outcome::Canceled);
    assert_eq!(Outcome::from_u8(3), Outcome::Error);
}

#[test]
fn tracker_is_send() {
    fn assert_send<T: Send>() {}
    assert_send::<Tracker>();
    assert_send::<IsolateState>();
    assert_send::<NdjsonWriter>();
}

#[test]
fn shared_state_is_sync() {
    fn assert_sync<T: Sync>() {}
    assert_sync::<IsolateState>();
    assert_sync::<NdjsonWriter>();
}
