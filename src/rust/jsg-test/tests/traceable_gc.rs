// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! GC tracing tests for manual `jsg::Traced` implementations on helper types.
//!
//! `#[jsg_resource]` now traces every field via `Traced::trace`, so nested
//! non-resource helper structs/enums just need to implement `Traced`.

use std::sync::atomic::AtomicUsize;
use std::sync::atomic::Ordering;

use jsg::ToJS;
use jsg_macros::jsg_resource;

// =============================================================================
// Shared leaf resource
// =============================================================================

static LEAF_DROPS: AtomicUsize = AtomicUsize::new(0);

#[jsg_resource]
struct Leaf {
    pub value: u32,
}

impl Drop for Leaf {
    fn drop(&mut self) {
        LEAF_DROPS.fetch_add(1, Ordering::SeqCst);
    }
}

#[jsg_resource]
impl Leaf {}

// =============================================================================
// Enum helper implementing Traced
// =============================================================================

static ENUM_PARENT_DROPS: AtomicUsize = AtomicUsize::new(0);

enum StreamState {
    Closed,
    Errored { reason: jsg::Rc<Leaf> },
    Readable(jsg::Rc<Leaf>),
}

impl jsg::Traced for StreamState {
    fn trace(&self, visitor: &mut jsg::GcVisitor) {
        match self {
            Self::Closed => {}
            Self::Errored { reason } => jsg::Traced::trace(reason, visitor),
            Self::Readable(inner) => jsg::Traced::trace(inner, visitor),
        }
    }
}

#[jsg_resource]
struct StreamController {
    pub state: StreamState,
}

impl Drop for StreamController {
    fn drop(&mut self) {
        ENUM_PARENT_DROPS.fetch_add(1, Ordering::SeqCst);
    }
}

#[jsg_resource]
impl StreamController {}

#[test]
fn enum_unit_variant_does_not_crash_during_gc() {
    ENUM_PARENT_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let parent = jsg::Rc::new(StreamController {
            state: StreamState::Closed,
        });
        let wrapped = parent.clone().to_js(lock);
        ctx.set_global("parent", wrapped);
        std::mem::drop(parent);

        crate::Harness::request_gc(lock);
        assert_eq!(ENUM_PARENT_DROPS.load(Ordering::SeqCst), 0);
        Ok(())
    });

    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(ENUM_PARENT_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
}

#[test]
fn enum_variant_child_kept_alive_through_gc() {
    LEAF_DROPS.store(0, Ordering::SeqCst);
    ENUM_PARENT_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let child = jsg::Rc::new(Leaf { value: 10 });

        let parent = jsg::Rc::new(StreamController {
            state: StreamState::Readable(child.clone()),
        });
        let wrapped = parent.clone().to_js(lock);
        ctx.set_global("parent", wrapped);

        std::mem::drop(child);
        std::mem::drop(parent);

        crate::Harness::request_gc(lock);
        assert_eq!(ENUM_PARENT_DROPS.load(Ordering::SeqCst), 0);
        assert_eq!(LEAF_DROPS.load(Ordering::SeqCst), 0);
        Ok(())
    });

    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(ENUM_PARENT_DROPS.load(Ordering::SeqCst), 1);
        assert_eq!(LEAF_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
}

#[test]
fn enum_named_variant_child_kept_alive_through_gc() {
    LEAF_DROPS.store(0, Ordering::SeqCst);
    ENUM_PARENT_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let child = jsg::Rc::new(Leaf { value: 11 });

        let parent = jsg::Rc::new(StreamController {
            state: StreamState::Errored {
                reason: child.clone(),
            },
        });
        let wrapped = parent.clone().to_js(lock);
        ctx.set_global("parent", wrapped);

        std::mem::drop(child);
        std::mem::drop(parent);

        crate::Harness::request_gc(lock);
        assert_eq!(ENUM_PARENT_DROPS.load(Ordering::SeqCst), 0);
        assert_eq!(LEAF_DROPS.load(Ordering::SeqCst), 0);
        Ok(())
    });

    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(ENUM_PARENT_DROPS.load(Ordering::SeqCst), 1);
        assert_eq!(LEAF_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
}
