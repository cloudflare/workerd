// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! GC tracing tests for collection fields in `#[jsg_resource]` structs.
//!
//! Verifies that `jsg::Member<T>` values in standard collections and their `Cell<…>`
//! variants all
//! produce correct GC trace edges so that children are kept alive as long as the parent
//! is reachable, and collected once the parent is collected.
//!
//! Each test follows the same pattern:
//! 1. Wrap the parent for JavaScript (gives it a JS wrapper held by a context global).
//! 2. Drop all Rust `Rc` handles — the parent is now only alive via the JS wrapper.
//! 3. Run a minor + major GC while the global is reachable → children must survive.
//! 4. Move to a fresh context (global is gone) → GC must collect parent and all children.

use std::cell::Cell;
use std::collections::BTreeMap;
use std::collections::BTreeSet;
use std::collections::HashMap;
use std::collections::HashSet;
use std::sync::atomic::AtomicUsize;
use std::sync::atomic::Ordering;

use jsg::ToJS;
use jsg_macros::jsg_method;
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
impl Leaf {
    #[jsg_method]
    fn get_value(&self) -> jsg::Number {
        jsg::Number::from(self.value)
    }
}

// =============================================================================
// Vec<jsg::Member<T>>
// =============================================================================

static VEC_PARENT_DROPS: AtomicUsize = AtomicUsize::new(0);

#[jsg_resource]
struct VecParent {
    pub children: Vec<jsg::Member<Leaf>>,
}

impl Drop for VecParent {
    fn drop(&mut self) {
        VEC_PARENT_DROPS.fetch_add(1, Ordering::SeqCst);
    }
}

#[jsg_resource]
impl VecParent {}

/// `Vec<jsg::Member<T>>` children are kept alive while parent JS wrapper is reachable.
#[test]
fn vec_member_children_kept_alive_through_gc() {
    LEAF_DROPS.store(0, Ordering::SeqCst);
    VEC_PARENT_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let c1 = jsg::Rc::new(Leaf { value: 1 });
        let c2 = jsg::Rc::new(Leaf { value: 2 });
        let c3 = jsg::Rc::new(Leaf { value: 3 });

        let parent = jsg::Rc::new(VecParent {
            children: vec![c1.clone().into(), c2.clone().into(), c3.clone().into()],
        });
        let wrapped = parent.clone().to_js(lock);
        ctx.set_global("parent", wrapped);

        // Drop all Rust refs.
        std::mem::drop(c1);
        std::mem::drop(c2);
        std::mem::drop(c3);
        std::mem::drop(parent);

        // GC while global is reachable — parent and all children must survive.
        crate::Harness::request_gc(lock);
        assert_eq!(VEC_PARENT_DROPS.load(Ordering::SeqCst), 0);
        assert_eq!(LEAF_DROPS.load(Ordering::SeqCst), 0);
        Ok(())
    });

    // Fresh context — global is gone, everything should be collected.
    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(VEC_PARENT_DROPS.load(Ordering::SeqCst), 1);
        assert_eq!(LEAF_DROPS.load(Ordering::SeqCst), 3);
        Ok(())
    });
}

/// An empty `Vec<jsg::Member<T>>` does not crash during GC.
#[test]
fn vec_member_empty_does_not_crash_during_gc() {
    VEC_PARENT_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let parent = jsg::Rc::new(VecParent { children: vec![] });
        let wrapped = parent.clone().to_js(lock);
        ctx.set_global("parent", wrapped);
        std::mem::drop(parent);

        crate::Harness::request_gc(lock);
        assert_eq!(VEC_PARENT_DROPS.load(Ordering::SeqCst), 0);
        Ok(())
    });

    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(VEC_PARENT_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
}

// =============================================================================
// HashMap<K, jsg::Member<T>>
// =============================================================================

static HASHMAP_PARENT_DROPS: AtomicUsize = AtomicUsize::new(0);

#[jsg_resource]
struct HashMapParent {
    pub children: HashMap<String, jsg::Member<Leaf>>,
}

impl Drop for HashMapParent {
    fn drop(&mut self) {
        HASHMAP_PARENT_DROPS.fetch_add(1, Ordering::SeqCst);
    }
}

#[jsg_resource]
impl HashMapParent {}

/// `HashMap<K, jsg::Member<T>>` values are kept alive while parent JS wrapper is reachable.
#[test]
fn hashmap_member_values_kept_alive_through_gc() {
    LEAF_DROPS.store(0, Ordering::SeqCst);
    HASHMAP_PARENT_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let a = jsg::Rc::new(Leaf { value: 10 });
        let b = jsg::Rc::new(Leaf { value: 20 });

        let mut map = HashMap::new();
        map.insert("a".to_owned(), a.clone().into());
        map.insert("b".to_owned(), b.clone().into());

        let parent = jsg::Rc::new(HashMapParent { children: map });
        let wrapped = parent.clone().to_js(lock);
        ctx.set_global("parent", wrapped);

        std::mem::drop(a);
        std::mem::drop(b);
        std::mem::drop(parent);

        crate::Harness::request_gc(lock);
        assert_eq!(HASHMAP_PARENT_DROPS.load(Ordering::SeqCst), 0);
        assert_eq!(LEAF_DROPS.load(Ordering::SeqCst), 0);
        Ok(())
    });

    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(HASHMAP_PARENT_DROPS.load(Ordering::SeqCst), 1);
        assert_eq!(LEAF_DROPS.load(Ordering::SeqCst), 2);
        Ok(())
    });
}

/// An empty `HashMap<K, jsg::Member<T>>` does not crash during GC.
#[test]
fn hashmap_member_empty_does_not_crash_during_gc() {
    HASHMAP_PARENT_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let parent = jsg::Rc::new(HashMapParent {
            children: HashMap::new(),
        });
        let wrapped = parent.clone().to_js(lock);
        ctx.set_global("parent", wrapped);
        std::mem::drop(parent);

        crate::Harness::request_gc(lock);
        assert_eq!(HASHMAP_PARENT_DROPS.load(Ordering::SeqCst), 0);
        Ok(())
    });

    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(HASHMAP_PARENT_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
}

// =============================================================================
// BTreeMap<K, jsg::Member<T>>
// =============================================================================

static BTREEMAP_PARENT_DROPS: AtomicUsize = AtomicUsize::new(0);

#[jsg_resource]
struct BTreeMapParent {
    pub children: BTreeMap<String, jsg::Member<Leaf>>,
}

impl Drop for BTreeMapParent {
    fn drop(&mut self) {
        BTREEMAP_PARENT_DROPS.fetch_add(1, Ordering::SeqCst);
    }
}

#[jsg_resource]
impl BTreeMapParent {}

/// `BTreeMap<K, jsg::Member<T>>` values are kept alive while parent JS wrapper is reachable.
#[test]
fn btreemap_member_values_kept_alive_through_gc() {
    LEAF_DROPS.store(0, Ordering::SeqCst);
    BTREEMAP_PARENT_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let x = jsg::Rc::new(Leaf { value: 100 });
        let y = jsg::Rc::new(Leaf { value: 200 });

        let mut map = BTreeMap::new();
        map.insert("x".to_owned(), x.clone().into());
        map.insert("y".to_owned(), y.clone().into());

        let parent = jsg::Rc::new(BTreeMapParent { children: map });
        let wrapped = parent.clone().to_js(lock);
        ctx.set_global("parent", wrapped);

        std::mem::drop(x);
        std::mem::drop(y);
        std::mem::drop(parent);

        crate::Harness::request_gc(lock);
        assert_eq!(BTREEMAP_PARENT_DROPS.load(Ordering::SeqCst), 0);
        assert_eq!(LEAF_DROPS.load(Ordering::SeqCst), 0);
        Ok(())
    });

    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(BTREEMAP_PARENT_DROPS.load(Ordering::SeqCst), 1);
        assert_eq!(LEAF_DROPS.load(Ordering::SeqCst), 2);
        Ok(())
    });
}

/// An empty `BTreeMap<K, jsg::Member<T>>` does not crash during GC.
#[test]
fn btreemap_member_empty_does_not_crash_during_gc() {
    BTREEMAP_PARENT_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let parent = jsg::Rc::new(BTreeMapParent {
            children: BTreeMap::new(),
        });
        let wrapped = parent.clone().to_js(lock);
        ctx.set_global("parent", wrapped);
        std::mem::drop(parent);

        crate::Harness::request_gc(lock);
        assert_eq!(BTREEMAP_PARENT_DROPS.load(Ordering::SeqCst), 0);
        Ok(())
    });

    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(BTREEMAP_PARENT_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
}

// =============================================================================
// HashSet<jsg::Member<T>>
// =============================================================================

static HASHSET_PARENT_DROPS: AtomicUsize = AtomicUsize::new(0);

// jsg::Member<T> implements Hash + Eq by pointer identity.
#[jsg_resource]
struct HashSetParent {
    pub children: HashSet<jsg::Member<Leaf>>,
}

impl Drop for HashSetParent {
    fn drop(&mut self) {
        HASHSET_PARENT_DROPS.fetch_add(1, Ordering::SeqCst);
    }
}

#[jsg_resource]
impl HashSetParent {}

/// `HashSet<jsg::Member<T>>` elements are kept alive while parent JS wrapper is reachable.
// jsg::Member uses pointer-identity Hash+Eq, so interior mutability (Cell fields) doesn't
// affect correctness. Suppress the mutable_key_type lint that fires on HashSet::new().
#[expect(
    clippy::mutable_key_type,
    reason = "jsg::Member hashes by pointer address, not interior state"
)]
#[test]
fn hashset_member_elements_kept_alive_through_gc() {
    LEAF_DROPS.store(0, Ordering::SeqCst);
    HASHSET_PARENT_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let p = jsg::Rc::new(Leaf { value: 1 });
        let q = jsg::Rc::new(Leaf { value: 2 });

        let mut set = HashSet::new();
        set.insert(p.clone().into());
        set.insert(q.clone().into());

        let parent = jsg::Rc::new(HashSetParent { children: set });
        let wrapped = parent.clone().to_js(lock);
        ctx.set_global("parent", wrapped);

        std::mem::drop(p);
        std::mem::drop(q);
        std::mem::drop(parent);

        crate::Harness::request_gc(lock);
        assert_eq!(HASHSET_PARENT_DROPS.load(Ordering::SeqCst), 0);
        assert_eq!(LEAF_DROPS.load(Ordering::SeqCst), 0);
        Ok(())
    });

    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(HASHSET_PARENT_DROPS.load(Ordering::SeqCst), 1);
        assert_eq!(LEAF_DROPS.load(Ordering::SeqCst), 2);
        Ok(())
    });
}

/// An empty `HashSet<jsg::Member<T>>` does not crash during GC.
#[test]
fn hashset_member_empty_does_not_crash_during_gc() {
    HASHSET_PARENT_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let parent = jsg::Rc::new(HashSetParent {
            children: HashSet::new(),
        });
        let wrapped = parent.clone().to_js(lock);
        ctx.set_global("parent", wrapped);
        std::mem::drop(parent);

        crate::Harness::request_gc(lock);
        assert_eq!(HASHSET_PARENT_DROPS.load(Ordering::SeqCst), 0);
        Ok(())
    });

    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(HASHSET_PARENT_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
}

// =============================================================================
// BTreeSet<jsg::Member<T>>
// =============================================================================

static BTREESET_PARENT_DROPS: AtomicUsize = AtomicUsize::new(0);

#[jsg_resource]
struct BTreeSetParent {
    pub children: BTreeSet<jsg::Member<Leaf>>,
}

impl Drop for BTreeSetParent {
    fn drop(&mut self) {
        BTREESET_PARENT_DROPS.fetch_add(1, Ordering::SeqCst);
    }
}

#[jsg_resource]
impl BTreeSetParent {}

/// `BTreeSet<jsg::Member<T>>` elements are kept alive while parent JS wrapper is reachable.
// Same pointer-identity rationale as the HashSet test above.
#[expect(
    clippy::mutable_key_type,
    reason = "jsg::Member orders by pointer address, not interior state"
)]
#[test]
fn btreeset_member_elements_kept_alive_through_gc() {
    LEAF_DROPS.store(0, Ordering::SeqCst);
    BTREESET_PARENT_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(Leaf { value: 7 });
        let s = jsg::Rc::new(Leaf { value: 8 });

        let mut set = BTreeSet::new();
        set.insert(r.clone().into());
        set.insert(s.clone().into());

        let parent = jsg::Rc::new(BTreeSetParent { children: set });
        let wrapped = parent.clone().to_js(lock);
        ctx.set_global("parent", wrapped);

        std::mem::drop(r);
        std::mem::drop(s);
        std::mem::drop(parent);

        crate::Harness::request_gc(lock);
        assert_eq!(BTREESET_PARENT_DROPS.load(Ordering::SeqCst), 0);
        assert_eq!(LEAF_DROPS.load(Ordering::SeqCst), 0);
        Ok(())
    });

    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(BTREESET_PARENT_DROPS.load(Ordering::SeqCst), 1);
        assert_eq!(LEAF_DROPS.load(Ordering::SeqCst), 2);
        Ok(())
    });
}

/// An empty `BTreeSet<jsg::Member<T>>` does not crash during GC.
#[test]
fn btreeset_member_empty_does_not_crash_during_gc() {
    BTREESET_PARENT_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let parent = jsg::Rc::new(BTreeSetParent {
            children: BTreeSet::new(),
        });
        let wrapped = parent.clone().to_js(lock);
        ctx.set_global("parent", wrapped);
        std::mem::drop(parent);

        crate::Harness::request_gc(lock);
        assert_eq!(BTREESET_PARENT_DROPS.load(Ordering::SeqCst), 0);
        Ok(())
    });

    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(BTREESET_PARENT_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
}

// =============================================================================
// Cell<Vec<jsg::Member<T>>>
// =============================================================================

static CELL_VEC_PARENT_DROPS: AtomicUsize = AtomicUsize::new(0);

#[jsg_resource]
struct CellVecParent {
    pub children: Cell<Vec<jsg::Member<Leaf>>>,
}

impl Drop for CellVecParent {
    fn drop(&mut self) {
        CELL_VEC_PARENT_DROPS.fetch_add(1, Ordering::SeqCst);
    }
}

#[jsg_resource]
impl CellVecParent {}

/// `Cell<Vec<jsg::Member<T>>>` children are kept alive while parent JS wrapper is reachable.
#[test]
fn cell_vec_member_children_kept_alive_through_gc() {
    LEAF_DROPS.store(0, Ordering::SeqCst);
    CELL_VEC_PARENT_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let c1 = jsg::Rc::new(Leaf { value: 11 });
        let c2 = jsg::Rc::new(Leaf { value: 22 });

        let parent = jsg::Rc::new(CellVecParent {
            children: Cell::new(vec![c1.clone().into(), c2.clone().into()]),
        });
        let wrapped = parent.clone().to_js(lock);
        ctx.set_global("parent", wrapped);

        std::mem::drop(c1);
        std::mem::drop(c2);
        std::mem::drop(parent);

        crate::Harness::request_gc(lock);
        assert_eq!(CELL_VEC_PARENT_DROPS.load(Ordering::SeqCst), 0);
        assert_eq!(LEAF_DROPS.load(Ordering::SeqCst), 0);
        Ok(())
    });

    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(CELL_VEC_PARENT_DROPS.load(Ordering::SeqCst), 1);
        assert_eq!(LEAF_DROPS.load(Ordering::SeqCst), 2);
        Ok(())
    });
}

/// An empty `Cell<Vec<jsg::Member<T>>>` does not crash during GC.
#[test]
fn cell_vec_member_empty_does_not_crash_during_gc() {
    CELL_VEC_PARENT_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let parent = jsg::Rc::new(CellVecParent {
            children: Cell::new(vec![]),
        });
        let wrapped = parent.clone().to_js(lock);
        ctx.set_global("parent", wrapped);
        std::mem::drop(parent);

        crate::Harness::request_gc(lock);
        assert_eq!(CELL_VEC_PARENT_DROPS.load(Ordering::SeqCst), 0);
        Ok(())
    });

    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(CELL_VEC_PARENT_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
}

// =============================================================================
// Cell<HashMap<K, jsg::Member<T>>>
// =============================================================================

static CELL_MAP_PARENT_DROPS: AtomicUsize = AtomicUsize::new(0);

#[jsg_resource]
struct CellHashMapParent {
    pub children: Cell<HashMap<String, jsg::Member<Leaf>>>,
}

impl Drop for CellHashMapParent {
    fn drop(&mut self) {
        CELL_MAP_PARENT_DROPS.fetch_add(1, Ordering::SeqCst);
    }
}

#[jsg_resource]
impl CellHashMapParent {}

/// `Cell<HashMap<K, jsg::Member<T>>>` values are kept alive while parent JS wrapper is reachable.
#[test]
fn cell_hashmap_member_values_kept_alive_through_gc() {
    LEAF_DROPS.store(0, Ordering::SeqCst);
    CELL_MAP_PARENT_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let m = jsg::Rc::new(Leaf { value: 99 });

        let mut map = HashMap::new();
        map.insert("m".to_owned(), m.clone().into());

        let parent = jsg::Rc::new(CellHashMapParent {
            children: Cell::new(map),
        });
        let wrapped = parent.clone().to_js(lock);
        ctx.set_global("parent", wrapped);

        std::mem::drop(m);
        std::mem::drop(parent);

        crate::Harness::request_gc(lock);
        assert_eq!(CELL_MAP_PARENT_DROPS.load(Ordering::SeqCst), 0);
        assert_eq!(LEAF_DROPS.load(Ordering::SeqCst), 0);
        Ok(())
    });

    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(CELL_MAP_PARENT_DROPS.load(Ordering::SeqCst), 1);
        assert_eq!(LEAF_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
}

// =============================================================================
// Mixed field resource — Vec + HashMap + direct Member
// =============================================================================

static MIXED_PARENT_DROPS: AtomicUsize = AtomicUsize::new(0);

#[jsg_resource]
struct MixedParent {
    pub singles: Vec<jsg::Member<Leaf>>,
    pub named: HashMap<String, jsg::Member<Leaf>>,
    pub direct: jsg::Member<Leaf>,
}

impl Drop for MixedParent {
    fn drop(&mut self) {
        MIXED_PARENT_DROPS.fetch_add(1, Ordering::SeqCst);
    }
}

#[jsg_resource]
impl MixedParent {}

/// A resource with `Vec`, `HashMap`, and direct `Member` fields.
#[test]
fn mixed_collection_fields_all_traced() {
    LEAF_DROPS.store(0, Ordering::SeqCst);
    MIXED_PARENT_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let vec_child = jsg::Rc::new(Leaf { value: 1 });
        let map_child = jsg::Rc::new(Leaf { value: 2 });
        let direct_child = jsg::Rc::new(Leaf { value: 3 });

        let mut named = HashMap::new();
        named.insert("key".to_owned(), map_child.clone().into());

        let parent = jsg::Rc::new(MixedParent {
            singles: vec![vec_child.clone().into()],
            named,
            direct: direct_child.clone().into(),
        });
        let wrapped = parent.clone().to_js(lock);
        ctx.set_global("parent", wrapped);

        std::mem::drop(vec_child);
        std::mem::drop(map_child);
        std::mem::drop(direct_child);
        std::mem::drop(parent);

        // All 3 children must survive (1 from vec, 1 from map, 1 direct).
        crate::Harness::request_gc(lock);
        assert_eq!(MIXED_PARENT_DROPS.load(Ordering::SeqCst), 0);
        assert_eq!(LEAF_DROPS.load(Ordering::SeqCst), 0);
        Ok(())
    });

    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(MIXED_PARENT_DROPS.load(Ordering::SeqCst), 1);
        assert_eq!(LEAF_DROPS.load(Ordering::SeqCst), 3);
        Ok(())
    });
}

// =============================================================================
// Vec<jsg::Member<T>> — child kept alive after the outer Rust root is dropped
// =============================================================================

/// Verifies that a child held only by a `Vec` inside a JS-wrapped parent is NOT
/// collected while the parent is reachable, even after its own Rust `Rc` is gone.
#[test]
fn vec_member_child_kept_alive_after_rust_root_dropped() {
    LEAF_DROPS.store(0, Ordering::SeqCst);
    VEC_PARENT_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let child = jsg::Rc::new(Leaf { value: 42 });

        let parent = jsg::Rc::new(VecParent {
            children: vec![child.clone().into()],
        });

        // Wrap parent so it gets a JS object.
        let _ = parent.clone().to_js(lock);

        // Drop the child Rust ref — vec inside parent is the only holder.
        std::mem::drop(child);

        // Force a GC: parent wrapper is unreachable (no global), but the local
        // handle keeps it alive in this scope.
        crate::Harness::request_gc(lock);
        // Child must NOT have been collected yet — the parent's vec still holds it.
        assert_eq!(LEAF_DROPS.load(Ordering::SeqCst), 0);

        std::mem::drop(parent);
        Ok(())
    });

    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(VEC_PARENT_DROPS.load(Ordering::SeqCst), 1);
        assert_eq!(LEAF_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
}

// =============================================================================
// HashMap with integer key — u32 key, jsg::Member<T> value
// =============================================================================

static INT_KEY_MAP_PARENT_DROPS: AtomicUsize = AtomicUsize::new(0);

#[jsg_resource]
struct IntKeyMapParent {
    pub children: HashMap<u32, jsg::Member<Leaf>>,
}

impl Drop for IntKeyMapParent {
    fn drop(&mut self) {
        INT_KEY_MAP_PARENT_DROPS.fetch_add(1, Ordering::SeqCst);
    }
}

#[jsg_resource]
impl IntKeyMapParent {}

/// `HashMap<u32, jsg::Member<T>>` — integer key does not affect tracing.
#[test]
fn hashmap_integer_key_member_values_traced() {
    LEAF_DROPS.store(0, Ordering::SeqCst);
    INT_KEY_MAP_PARENT_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let v = jsg::Rc::new(Leaf { value: 55 });

        let mut map = HashMap::new();
        map.insert(0u32, v.clone().into());
        map.insert(1u32, v.clone().into()); // same child twice

        let parent = jsg::Rc::new(IntKeyMapParent { children: map });
        let wrapped = parent.clone().to_js(lock);
        ctx.set_global("parent", wrapped);

        std::mem::drop(v);
        std::mem::drop(parent);

        crate::Harness::request_gc(lock);
        assert_eq!(INT_KEY_MAP_PARENT_DROPS.load(Ordering::SeqCst), 0);
        assert_eq!(LEAF_DROPS.load(Ordering::SeqCst), 0);
        Ok(())
    });

    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(INT_KEY_MAP_PARENT_DROPS.load(Ordering::SeqCst), 1);
        // One unique Leaf despite two map entries (both were clones of the same Rc).
        assert_eq!(LEAF_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
}

// =============================================================================
// Nested struct tracing via `Traced`
//
// Mirrors the C++ pattern:
//   struct PrivateData { void visitForGc(jsg::GcVisitor&) { ... } };
//   class Foo : public jsg::Object { PrivateData privateData_; ... };
// =============================================================================

static TRACE_DELEGATION_PARENT_DROPS: AtomicUsize = AtomicUsize::new(0);

/// A plain Rust struct (not a jsg resource) that holds traceable children.
/// It manually implements `Traced`.
struct PrivateData {
    child: jsg::Member<Leaf>,
}

impl jsg::Traced for PrivateData {
    fn trace(&self, visitor: &mut jsg::GcVisitor) {
        jsg::Traced::trace(&self.child, visitor);
    }
}

/// Resource with a plain nested field — auto-generated tracing delegates to
/// `Traced::trace` for each field, including this one.
#[jsg_resource]
struct TraceDelegationParent {
    pub data: PrivateData,
}

impl Drop for TraceDelegationParent {
    fn drop(&mut self) {
        TRACE_DELEGATION_PARENT_DROPS.fetch_add(1, Ordering::SeqCst);
    }
}

#[jsg_resource]
impl TraceDelegationParent {}

/// Children held inside a nested `Traced` struct are kept alive while the
/// parent JS wrapper is reachable.
#[test]
fn trace_delegation_child_kept_alive_through_gc() {
    LEAF_DROPS.store(0, Ordering::SeqCst);
    TRACE_DELEGATION_PARENT_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let child = jsg::Rc::new(Leaf { value: 77 });

        let parent = jsg::Rc::new(TraceDelegationParent {
            data: PrivateData {
                child: child.clone().into(),
            },
        });
        let wrapped = parent.clone().to_js(lock);
        ctx.set_global("parent", wrapped);

        std::mem::drop(child);
        std::mem::drop(parent);

        // GC while global reachable — child inside PrivateData must survive.
        crate::Harness::request_gc(lock);
        assert_eq!(TRACE_DELEGATION_PARENT_DROPS.load(Ordering::SeqCst), 0);
        assert_eq!(LEAF_DROPS.load(Ordering::SeqCst), 0);
        Ok(())
    });

    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(TRACE_DELEGATION_PARENT_DROPS.load(Ordering::SeqCst), 1);
        assert_eq!(LEAF_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
}

// =============================================================================
// custom_trace — user-written Traced impl
// =============================================================================

static CUSTOM_TRACE_PARENT_DROPS: AtomicUsize = AtomicUsize::new(0);

/// Resource that opts out of macro-generated tracing via `custom_trace`.
/// The user provides their own `Traced` impl with custom logic.
#[jsg_resource(custom_trace)]
struct CustomTraceParent {
    pub child: jsg::Member<Leaf>,
}

impl Drop for CustomTraceParent {
    fn drop(&mut self) {
        CUSTOM_TRACE_PARENT_DROPS.fetch_add(1, Ordering::SeqCst);
    }
}

// User-supplied Traced impl — not generated by the macro because of custom_trace.
impl jsg::Traced for CustomTraceParent {
    fn trace(&self, visitor: &mut jsg::GcVisitor) {
        jsg::Traced::trace(&self.child, visitor);
    }
}

#[jsg_resource]
impl CustomTraceParent {}

/// With `custom_trace`, the user-supplied `Traced` impl is used and
/// children are traced correctly.
#[test]
fn custom_trace_child_kept_alive_through_gc() {
    LEAF_DROPS.store(0, Ordering::SeqCst);
    CUSTOM_TRACE_PARENT_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let child = jsg::Rc::new(Leaf { value: 88 });

        let parent = jsg::Rc::new(CustomTraceParent {
            child: child.clone().into(),
        });
        let wrapped = parent.clone().to_js(lock);
        ctx.set_global("parent", wrapped);

        std::mem::drop(child);
        std::mem::drop(parent);

        crate::Harness::request_gc(lock);
        assert_eq!(CUSTOM_TRACE_PARENT_DROPS.load(Ordering::SeqCst), 0);
        assert_eq!(LEAF_DROPS.load(Ordering::SeqCst), 0);
        Ok(())
    });

    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(CUSTOM_TRACE_PARENT_DROPS.load(Ordering::SeqCst), 1);
        assert_eq!(LEAF_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
}
