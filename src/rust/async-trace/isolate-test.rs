// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

fn frame(function: &str, line: u32) -> Frame {
    Frame {
        function: function.to_owned(),
        script: "worker.js".to_owned(),
        script_id: 7,
        line,
        column: 3,
    }
}

#[test]
fn isolates_have_distinct_ids() {
    assert_ne!(IsolateState::new().id(), IsolateState::new().id());
}

#[test]
fn async_ids_are_nonzero_and_increasing() {
    let isolate = IsolateState::new();
    let first = isolate.next_async_id();
    let second = isolate.next_async_id();
    assert_ne!(first, 0);
    assert!(second > first);
}

#[test]
fn empty_stack_has_no_id() {
    assert_eq!(IsolateState::new().intern_stack(Vec::new()), None);
}

#[test]
fn identical_positions_share_an_id() {
    let isolate = IsolateState::new();
    let a = isolate
        .intern_stack(vec![frame("f", 1), frame("g", 2)])
        .unwrap();
    // Same positions; names are not part of the identity.
    let b = isolate
        .intern_stack(vec![frame("renamed", 1), frame("g", 2)])
        .unwrap();
    assert_eq!(a, b);
    assert_eq!(isolate.stack(a).unwrap()[0].function, "f");
}

#[test]
fn different_positions_get_different_ids() {
    let isolate = IsolateState::new();
    let a = isolate.intern_stack(vec![frame("f", 1)]).unwrap();
    let b = isolate.intern_stack(vec![frame("f", 2)]).unwrap();
    let c = isolate
        .intern_stack(vec![frame("f", 1), frame("g", 2)])
        .unwrap();
    assert_ne!(a, b);
    assert_ne!(a, c);
    assert_ne!(b, c);
    assert_ne!(a, 0);
}

#[test]
fn stack_lookup() {
    let isolate = IsolateState::new();
    let id = isolate
        .intern_stack(vec![frame("f", 1), frame("g", 2)])
        .unwrap();
    let frames = isolate.stack(id).unwrap();
    assert_eq!(frames.len(), 2);
    assert_eq!(frames[1], frame("g", 2));
    assert!(isolate.stack(0).is_none());
    assert!(isolate.stack(id + 1).is_none());
}

#[test]
fn new_stacks_beyond_the_count_cap_get_no_id() {
    let mut isolate = IsolateState::new();
    isolate.set_stack_limits(2, usize::MAX);
    let a = isolate.intern_stack(vec![frame("f", 1)]).unwrap();
    let b = isolate.intern_stack(vec![frame("f", 2)]).unwrap();
    assert_eq!(isolate.intern_stack(vec![frame("f", 3)]), None);
    // Stacks already kept still resolve.
    assert_eq!(isolate.intern_stack(vec![frame("f", 1)]), Some(a));
    assert_eq!(isolate.intern_stack(vec![frame("f", 2)]), Some(b));
}

#[test]
fn new_stacks_beyond_the_byte_cap_get_no_id() {
    let one = stack_bytes(&[frame("f", 1)]);
    let mut isolate = IsolateState::new();
    isolate.set_stack_limits(usize::MAX, one * 2);
    let a = isolate.intern_stack(vec![frame("f", 1)]).unwrap();
    // Would take the total past the cap, even though a smaller stack still fits.
    assert_eq!(
        isolate.intern_stack(vec![frame("f", 2), frame("g", 3)]),
        None
    );
    assert!(isolate.intern_stack(vec![frame("f", 4)]).is_some());
    assert_eq!(isolate.intern_stack(vec![frame("f", 5)]), None);
    assert_eq!(isolate.intern_stack(vec![frame("f", 1)]), Some(a));
}
