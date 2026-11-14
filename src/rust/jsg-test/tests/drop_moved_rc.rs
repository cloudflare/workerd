// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! A `jsg::Rc` that a resource's `Drop` moves out of a field the garbage collector has visited.

use std::sync::Mutex;
use std::sync::atomic::AtomicUsize;
use std::sync::atomic::Ordering;

use jsg::GarbageCollected;
use jsg::ToJS;
use jsg_macros::jsg_resource;

static LEAF_DROPS: AtomicUsize = AtomicUsize::new(0);

#[jsg_resource]
struct DropLeaf {}

impl Drop for DropLeaf {
    fn drop(&mut self) {
        LEAF_DROPS.fetch_add(1, Ordering::SeqCst);
    }
}

#[jsg_resource]
impl DropLeaf {}

static STASH: Mutex<Vec<jsg::Rc<DropLeaf>>> = Mutex::new(Vec::new());

#[jsg_resource]
struct StashesOnDrop {
    child: Option<jsg::Rc<DropLeaf>>,
}

impl Drop for StashesOnDrop {
    fn drop(&mut self) {
        if let Some(child) = self.child.take() {
            STASH.lock().unwrap().push(child);
        }
    }
}

#[jsg_resource]
impl StashesOnDrop {}

#[jsg_resource]
struct KeepsOnDrop {
    child: Option<jsg::Rc<DropLeaf>>,
}

impl Drop for KeepsOnDrop {
    fn drop(&mut self) {}
}

#[jsg_resource]
impl KeepsOnDrop {}

#[jsg_resource]
struct PlainHolder {
    child: Option<jsg::Rc<DropLeaf>>,
}

#[jsg_resource]
impl PlainHolder {}

#[jsg_resource]
struct HolderOwner {
    holder: Option<jsg::Rc<StashesOnDrop>>,
}

#[jsg_resource]
impl HolderOwner {}

/// The holder's wrapper is unreachable at the first GC, which sweeps it. Its child, which the
/// holder's first strong reference visited, keeps the holder as its parent, and the `Rc` its
/// `Drop` moves out must work in a new owner, and keep its resource alive until that owner lets
/// go of it.
#[test]
fn rc_moved_out_in_drop_can_be_reowned() {
    let harness = crate::Harness::new();
    let drops_before = LEAF_DROPS.load(Ordering::SeqCst);
    harness.run_in_context(|lock, _ctx| {
        let leaf = jsg::Rc::new(lock, DropLeaf {});
        let holder = jsg::Rc::new(lock, StashesOnDrop { child: Some(leaf) });
        let _ = holder.to_js(lock)?;
        Ok(())
    });
    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        Ok(())
    });
    harness.run_in_context(|lock, _ctx| {
        let stashed = STASH.lock().unwrap().pop().expect("stashed by Drop");
        let next = jsg::Rc::new(
            lock,
            PlainHolder {
                child: Some(stashed),
            },
        );
        let _ = next.to_js(lock)?;
        Ok(())
    });
    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        Ok(())
    });
    assert_eq!(LEAF_DROPS.load(Ordering::SeqCst), drops_before + 1);
}

/// An owner reachable through its wrapper traces a holder that has no wrapper, which makes the
/// holder's child weak with the holder as its parent. Once the owner is swept, the weak `Rc` that
/// the holder's `Drop` moves out must work in a new owner.
#[test]
fn weak_rc_moved_out_in_drop_can_be_reowned() {
    let harness = crate::Harness::new();
    let drops_before = LEAF_DROPS.load(Ordering::SeqCst);
    harness.run_in_context(|lock, ctx| {
        let leaf = jsg::Rc::new(lock, DropLeaf {});
        let holder = jsg::Rc::new(lock, StashesOnDrop { child: Some(leaf) });
        let owner = jsg::Rc::new(
            lock,
            HolderOwner {
                holder: Some(holder),
            },
        );
        ctx.set_global(lock, "owner", owner);
        Ok(())
    });
    harness.run_in_context(|lock, ctx| {
        crate::Harness::request_gc(lock);
        let _: bool = ctx.eval(lock, "delete globalThis.owner").unwrap();
        Ok(())
    });
    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        Ok(())
    });
    harness.run_in_context(|lock, _ctx| {
        let stashed = STASH.lock().unwrap().pop().expect("stashed by Drop");
        let next = jsg::Rc::new(
            lock,
            PlainHolder {
                child: Some(stashed),
            },
        );
        let _ = next.to_js(lock)?;
        Ok(())
    });
    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        Ok(())
    });
    assert_eq!(LEAF_DROPS.load(Ordering::SeqCst), drops_before + 1);
}

/// A child that a resource's `Drop` leaves in its field is released with the resource, once.
#[test]
fn rc_kept_in_drop_is_released_once() {
    let harness = crate::Harness::new();
    let drops_before = LEAF_DROPS.load(Ordering::SeqCst);
    harness.run_in_context(|lock, _ctx| {
        let leaf = jsg::Rc::new(lock, DropLeaf {});
        let holder = jsg::Rc::new(lock, KeepsOnDrop { child: Some(leaf) });
        let _ = holder.to_js(lock)?;
        Ok(())
    });
    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        Ok(())
    });
    assert_eq!(LEAF_DROPS.load(Ordering::SeqCst), drops_before + 1);
}

static NESTED_STASH: Mutex<Vec<jsg::Rc<DropLeaf>>> = Mutex::new(Vec::new());

/// A field type whose own `Drop` moves its `Rc` out, which its `Traced` impl says.
struct StashesInner {
    child: Option<jsg::Rc<DropLeaf>>,
}

impl jsg::Traced for StashesInner {
    fn trace(&self, visitor: &mut jsg::v8::GcVisitor) {
        jsg::Traced::trace(&self.child, visitor);
    }

    fn needs_drop_detach() -> bool {
        true
    }
}

impl Drop for StashesInner {
    fn drop(&mut self) {
        if let Some(child) = self.child.take() {
            NESTED_STASH.lock().unwrap().push(child);
        }
    }
}

#[jsg_resource]
struct InnerHolder {
    inner: StashesInner,
}

#[jsg_resource]
impl InnerHolder {}

#[jsg_resource]
struct OptionHolder {
    inner: Option<StashesInner>,
}

#[jsg_resource]
impl OptionHolder {}

/// Drops a resource whose wrapper is unreachable at a GC, made by `make` around a leaf, then
/// gives the `Rc` that a field's `Drop` moved out a new owner, and checks that the leaf is
/// released once that owner is.
fn reown_rc_moved_out_by_a_field<R: jsg::Resource>(make: impl FnOnce(jsg::Rc<DropLeaf>) -> R) {
    let harness = crate::Harness::new();
    let drops_before = LEAF_DROPS.load(Ordering::SeqCst);
    harness.run_in_context(|lock, _ctx| {
        let leaf = jsg::Rc::new(lock, DropLeaf {});
        let holder = jsg::Rc::new(lock, make(leaf));
        let _ = holder.to_js(lock)?;
        Ok(())
    });
    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        Ok(())
    });
    harness.run_in_context(|lock, _ctx| {
        let stashed = NESTED_STASH.lock().unwrap().pop().expect("stashed by Drop");
        let next = jsg::Rc::new(
            lock,
            PlainHolder {
                child: Some(stashed),
            },
        );
        let _ = next.to_js(lock)?;
        Ok(())
    });
    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        Ok(())
    });
    assert_eq!(LEAF_DROPS.load(Ordering::SeqCst), drops_before + 1);
}

/// The `Drop` of a field's type, not of the resource, moves the `Rc` out.
#[test]
fn rc_moved_out_in_a_field_drop_can_be_reowned() {
    reown_rc_moved_out_by_a_field(|leaf| InnerHolder {
        inner: StashesInner { child: Some(leaf) },
    });
}

/// The `Drop` of a type inside an `Option` field moves the `Rc` out.
#[test]
fn rc_moved_out_in_a_nested_field_drop_can_be_reowned() {
    reown_rc_moved_out_by_a_field(|leaf| OptionHolder {
        inner: Some(StashesInner { child: Some(leaf) }),
    });
}

#[jsg_resource]
struct ManyHolder {
    children: Vec<jsg::Rc<DropLeaf>>,
    direct: Option<jsg::Rc<DropLeaf>>,
}

#[jsg_resource]
impl ManyHolder {}

/// A resource detaches its references before it is dropped when it implements `Drop`, or when a
/// field's type says it needs it through any wrappers, and not for the `Drop` impls of
/// `jsg::Rc` and the std collections.
#[test]
fn detach_before_drop_follows_the_field_types() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let leaf = jsg::Rc::new(lock, DropLeaf {});
        assert!(leaf.get(lock).detach_before_drop());
        assert!(
            InnerHolder {
                inner: StashesInner { child: None },
            }
            .detach_before_drop()
        );
        assert!(OptionHolder { inner: None }.detach_before_drop());
        let many = ManyHolder {
            children: vec![leaf.clone(lock)],
            direct: None,
        };
        assert!(!many.detach_before_drop());
        let plain = PlainHolder { child: Some(leaf) };
        assert!(!plain.detach_before_drop());
        Ok(())
    });
}
