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

/// A resource type whose every instance is interchangeable, as `#[jsg_resource(snapshot_stateless)]`
/// declares.
struct Stateless;

impl crate::Traced for Stateless {
    fn trace(&self, _visitor: &mut crate::GcVisitor) {}
}

impl GarbageCollected for Stateless {
    fn memory_name(&self) -> &'static std::ffi::CStr {
        c"Stateless"
    }

    fn snapshot_recreate(&self) -> Option<crate::SnapshotRecreated> {
        Some(crate::snapshot_recreate_leak(Self))
    }
}

#[test]
fn snapshot_recreate_leaks_a_fresh_instance_of_the_type() {
    let recreated = Stateless
        .snapshot_recreate()
        .expect("a stateless type recreates");
    let ptr = recreated.0;
    assert!(!ptr.is_cleared());
    assert_eq!(ptr.type_id(), std::any::TypeId::of::<Stateless>());
    // SAFETY: `ptr` holds the leaked instance until `drop_rc` below releases it.
    let gc = unsafe { ptr.as_gc_ref() };
    assert_eq!(gc.memory_name(), c"Stateless");
    // SAFETY: releases the one `Rc` the leak created; nothing else refers to the instance.
    unsafe { ptr.drop_rc() };
}
