// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! GC tracing tests for manual `jsg::Traced` implementations on helper types.
//!
//! `#[jsg_resource]` now traces every field via `Traced::trace`, so nested
//! non-resource helper structs/enums just need to implement `Traced`.

use std::cell::RefCell;
use std::sync::atomic::AtomicUsize;
use std::sync::atomic::Ordering;

use jsg::ToJS;
use jsg::Traced;
use jsg_macros::jsg_resource;
use jsg_macros::jsg_struct;

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
        let wrapped = parent.clone().to_js(lock)?;
        ctx.set_global(lock, "parent", wrapped);
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
        let wrapped = parent.clone().to_js(lock)?;
        ctx.set_global(lock, "parent", wrapped);

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
        let wrapped = parent.clone().to_js(lock)?;
        ctx.set_global(lock, "parent", wrapped);

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

#[jsg_struct]
struct PlainStruct {
    pub label: String,
}

struct PlainData;
jsg::jsg_traced!(PlainData);

/// `jsg::Rc` and resources can hold GC references; `jsg::Weak` and `jsg_traced!` types cannot,
/// and a `#[jsg_struct]` type can when a field can.
#[test]
fn holds_gc_references_of_jsg_types() {
    assert!(<jsg::Rc<Leaf>>::holds_gc_references());
    assert!(Leaf::holds_gc_references());
    assert!(!<jsg::Weak<Leaf>>::holds_gc_references());
    assert!(!PlainStruct::holds_gc_references());
    assert!(Link::holds_gc_references());
    assert!(!PlainData::holds_gc_references());
}

// =============================================================================
// #[jsg_struct] fields
// =============================================================================

static NODE_DROPS: AtomicUsize = AtomicUsize::new(0);
static BACK_DROPS: AtomicUsize = AtomicUsize::new(0);

/// A struct whose field is a GC reference.
#[jsg_struct]
struct Link {
    pub target: jsg::Rc<Back>,
}

#[jsg_resource]
struct Node {
    pub link: RefCell<Option<Link>>,
}

impl Drop for Node {
    fn drop(&mut self) {
        NODE_DROPS.fetch_add(1, Ordering::SeqCst);
    }
}

#[jsg_resource]
impl Node {}

/// Holds a `Node`'s wrapper, which closes a cycle through the `Node`'s `Link`.
#[jsg_resource]
struct Back {
    node: std::cell::Cell<Option<jsg::v8::TracedGlobal<jsg::v8::Value>>>,
}

impl Drop for Back {
    fn drop(&mut self) {
        BACK_DROPS.fetch_add(1, Ordering::SeqCst);
    }
}

#[jsg_resource]
impl Back {}

/// A `#[jsg_struct]` traces its fields, so a cycle through one is collected: `Node` holds a `Back`
/// through a `Link`, and `Back` holds the `Node`'s wrapper. Were the `Link` not traced, the
/// `jsg::Rc<Back>` in it would stay strong and the cycle would never be collected.
#[test]
fn cycle_through_struct_field_is_collected() {
    NODE_DROPS.store(0, Ordering::SeqCst);
    BACK_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let back = jsg::Rc::new(Back {
            node: std::cell::Cell::new(None),
        });
        let node = jsg::Rc::new(Node {
            link: RefCell::new(Some(Link {
                target: back.clone(),
            })),
        });
        let node_wrapper = node.clone().to_js(lock)?;
        back.node.set(Some(node_wrapper.to_traced_global(lock)));
        // `Back` needs a wrapper for the `jsg::Rc<Back>` in the `Link` to trace it.
        let _ = back.clone().to_js(lock)?;
        std::mem::drop(node);
        std::mem::drop(back);
        assert_eq!(NODE_DROPS.load(Ordering::SeqCst), 0);
        assert_eq!(BACK_DROPS.load(Ordering::SeqCst), 0);
        Ok(())
    });

    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(NODE_DROPS.load(Ordering::SeqCst), 1);
        assert_eq!(BACK_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
}
