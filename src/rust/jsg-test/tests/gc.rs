// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! GC (garbage collection) tests for Rust resources.
//!
//! These tests verify that Rust resources are properly cleaned up when:
//! 1. All Rust `Ref` handles are dropped and no JavaScript wrapper exists (immediate cleanup)
//! 2. All Rust `Ref` handles are dropped and V8 garbage collects the JS wrapper
//!
//! Note: Circular references through `Ref<T>` are NOT collected, matching the behavior
//! of C++ `jsg::Rc<T>` which uses `kj::Own<T>` cross-references.

use std::cell::Cell;
use std::sync::atomic::AtomicUsize;
use std::sync::atomic::Ordering;

use jsg::ToJS;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;

/// Counter to track how many `SimpleResource` instances have been dropped.
static SIMPLE_RESOURCE_DROPS: AtomicUsize = AtomicUsize::new(0);

#[jsg_resource]
struct SimpleResource {
    pub name: String,
}

impl Drop for SimpleResource {
    fn drop(&mut self) {
        SIMPLE_RESOURCE_DROPS.fetch_add(1, Ordering::SeqCst);
    }
}

#[jsg_resource]
#[expect(clippy::unnecessary_wraps)]
impl SimpleResource {
    #[jsg_method]
    fn get_name(&self) -> Result<String, jsg::Error> {
        Ok(self.name.clone())
    }
}

/// Counter to track how many `ParentResource` instances have been dropped.
static PARENT_RESOURCE_DROPS: AtomicUsize = AtomicUsize::new(0);

#[jsg_resource]
struct ParentResource {
    pub child: jsg::Rc<SimpleResource>,
    pub optional_child: Option<jsg::Rc<SimpleResource>>,
}

impl Drop for ParentResource {
    fn drop(&mut self) {
        PARENT_RESOURCE_DROPS.fetch_add(1, Ordering::SeqCst);
    }
}

#[jsg_resource]
impl ParentResource {}

/// Tests that resources are dropped immediately when all Rust Refs are dropped
/// and no JS wrapper exists.
///
/// In the Wrappable model, dropping the last Ref decrements the kj refcount to 0,
/// which immediately destroys the Wrappable (and thus the Rust resource).
/// No GC is needed.
#[test]
fn supports_gc_via_ref_drop() {
    SIMPLE_RESOURCE_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|_lock, _ctx| {
        let resource = jsg::Rc::new(SimpleResource {
            name: "test".to_owned(),
        });
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 0);
        std::mem::drop(resource);
        // In the Wrappable model, no wrapper means immediate cleanup when refcount hits 0
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
}

/// Tests that resources are dropped via V8 GC when JS wrapper is collected.
///
/// When a resource is wrapped for JavaScript:
/// 1. Dropping all Rust `Ref` handles calls `removeStrongRef()` but the `CppgcShim`
///    still holds a `kj::Own` keeping the object alive
/// 2. V8 GC collects the wrapper, `CppgcShim` is destroyed, `kj::Own` is dropped
/// 3. kj refcount reaches 0, Wrappable is destroyed
#[test]
fn supports_gc_via_weak_callback() {
    SIMPLE_RESOURCE_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let resource = jsg::Rc::new(SimpleResource {
            name: "test".to_owned(),
        });
        let _wrapped = resource.clone().to_js(lock);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 0);
        std::mem::drop(resource);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 0);
        Ok(())
    });

    harness.run_in_context(|lock, _ctx| {
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 0);
        crate::Harness::request_gc(lock);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
}

#[test]
fn resource_with_traced_ref_field() {
    SIMPLE_RESOURCE_DROPS.store(0, Ordering::SeqCst);
    PARENT_RESOURCE_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let child = jsg::Rc::new(SimpleResource {
            name: "child".to_owned(),
        });
        let optional_child = jsg::Rc::new(SimpleResource {
            name: "optional_child".to_owned(),
        });

        let parent = jsg::Rc::new(ParentResource {
            child: child.clone(),
            optional_child: Some(optional_child.clone()),
        });

        let _wrapped = parent.clone().to_js(lock);

        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 0);
        assert_eq!(PARENT_RESOURCE_DROPS.load(Ordering::SeqCst), 0);

        std::mem::drop(child);
        std::mem::drop(optional_child);
        std::mem::drop(parent);
        Ok(())
    });

    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(PARENT_RESOURCE_DROPS.load(Ordering::SeqCst), 1);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 2);
        Ok(())
    });
}

#[test]
fn child_traced_ref_kept_alive_by_parent() {
    SIMPLE_RESOURCE_DROPS.store(0, Ordering::SeqCst);
    PARENT_RESOURCE_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let child = jsg::Rc::new(SimpleResource {
            name: "child".to_owned(),
        });

        let parent = jsg::Rc::new(ParentResource {
            child: child.clone(),
            optional_child: None,
        });

        // Wrap the parent so it has a JS object, then let the Local go out of scope
        // by not storing it. The wrapper is now only held weakly by cppgc.
        let _ = parent.clone().to_js(lock);

        // Child not collected because parent still holds a Ref (which holds a kj::Own)
        std::mem::drop(child);
        crate::Harness::request_gc(lock);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 0);

        // Drop the Rust strong ref. The parent still has a wrapper, but with no
        // strong refs and no JS references, the wrapper is eligible for GC.
        std::mem::drop(parent);
        Ok(())
    });

    // GC in a separate context so the Local handle from wrap() is gone
    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(PARENT_RESOURCE_DROPS.load(Ordering::SeqCst), 1);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
}

#[test]
fn weak_ref_upgrade() {
    SIMPLE_RESOURCE_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|_lock, _ctx| {
        let strong = jsg::Rc::new(SimpleResource {
            name: "test".to_owned(),
        });
        let weak = strong.downgrade();

        assert!(weak.is_alive());
        let upgraded = weak.upgrade();
        assert!(upgraded.is_some());
        assert!(weak.is_alive());

        std::mem::drop(upgraded);
        assert!(weak.is_alive());

        std::mem::drop(strong);
        // No wrapper, so resource is destroyed immediately when last strong ref drops
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 1);
        assert!(!weak.is_alive());
        assert!(weak.upgrade().is_none());
        Ok(())
    });
}

/// Tests that a wrapped resource stays alive as long as JS wrapper exists.
#[test]
fn wrapped_resource_kept_alive_by_js() {
    SIMPLE_RESOURCE_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let strong = jsg::Rc::new(SimpleResource {
            name: "test".to_owned(),
        });
        let _wrapped = strong.clone().to_js(lock);
        std::mem::drop(strong);
        // CppgcShim still holds kj::Own, keeping it alive
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 0);
        Ok(())
    });

    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
}

/// Tests weak ref behavior with wrapped resources.
#[test]
fn weak_ref_with_wrapped_resource() {
    SIMPLE_RESOURCE_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let strong = jsg::Rc::new(SimpleResource {
            name: "test".to_owned(),
        });
        let _wrapped = strong.clone().to_js(lock);
        let weak = strong.downgrade();

        assert!(weak.upgrade().is_some());
        std::mem::drop(strong);
        // CppgcShim keeps it alive even after dropping the strong ref
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 0);
        Ok(())
    });

    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
}

/// Tests parent-child GC with traced refs.
#[test]
fn traced_ref_in_gc() {
    SIMPLE_RESOURCE_DROPS.store(0, Ordering::SeqCst);
    PARENT_RESOURCE_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let child = jsg::Rc::new(SimpleResource {
            name: "child".to_owned(),
        });
        let _child_wrapped = child.clone().to_js(lock);

        let parent = jsg::Rc::new(ParentResource {
            child: child.clone(),
            optional_child: None,
        });
        let _parent_wrapped = parent.clone().to_js(lock);

        std::mem::drop(child);
        std::mem::drop(parent);

        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 0);
        assert_eq!(PARENT_RESOURCE_DROPS.load(Ordering::SeqCst), 0);
        Ok(())
    });

    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(PARENT_RESOURCE_DROPS.load(Ordering::SeqCst), 1);
        crate::Harness::request_gc(lock);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
}

// =============================================================================
// WeakRef tests
// =============================================================================

/// Tests that `WeakRef::get()` returns the resource data while the resource is alive.
#[test]
fn weak_ref_get_returns_resource_data() {
    let harness = crate::Harness::new();
    harness.run_in_context(|_lock, _ctx| {
        let strong = jsg::Rc::new(SimpleResource {
            name: "hello".to_owned(),
        });
        let weak = strong.downgrade();

        // get() should return a reference to the resource
        let resource = weak.upgrade().expect("weak ref should be alive");
        assert_eq!(resource.name, "hello");
        Ok(())
    });
}

/// Tests that `WeakRef::get()` returns `None` after the resource is dropped.
#[test]
fn weak_ref_get_returns_none_after_drop() {
    SIMPLE_RESOURCE_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|_lock, _ctx| {
        let strong = jsg::Rc::new(SimpleResource {
            name: "ephemeral".to_owned(),
        });
        let weak = strong.downgrade();
        assert!(weak.upgrade().is_some());

        std::mem::drop(strong);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 1);

        // get() must return None without touching freed memory
        assert!(weak.upgrade().is_none());
        Ok(())
    });
}

/// Tests that `WeakRef::default()` creates a dead weak reference.
///
/// A default `WeakRef` has `wrappable: None` and an expired `Weak<R>`.
/// All operations must be safe: `get()` -> `None`, `upgrade()` -> `None`, `is_alive()` -> `false`.
#[test]
fn weak_ref_default_is_dead() {
    let harness = crate::Harness::new();
    harness.run_in_context(|_lock, _ctx| {
        let weak: jsg::Weak<SimpleResource> = jsg::Weak::default();

        assert!(!weak.is_alive());
        assert!(weak.upgrade().is_none());
        assert!(weak.upgrade().is_none());
        Ok(())
    });
}

/// Tests that cloning a `WeakRef` shares the alive marker.
///
/// When the resource dies, ALL clones must see `is_alive() == false` simultaneously.
#[test]
fn weak_ref_clone_shares_alive_marker() {
    SIMPLE_RESOURCE_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|_lock, _ctx| {
        let strong = jsg::Rc::new(SimpleResource {
            name: "shared".to_owned(),
        });

        let weak1 = strong.downgrade();
        let weak2 = weak1.clone();
        let weak3 = weak2.clone();

        assert!(weak1.is_alive());
        assert!(weak2.is_alive());
        assert!(weak3.is_alive());

        std::mem::drop(strong);

        // All clones see death simultaneously
        assert!(!weak1.is_alive());
        assert!(!weak2.is_alive());
        assert!(!weak3.is_alive());
        assert!(weak1.upgrade().is_none());
        assert!(weak2.upgrade().is_none());
        Ok(())
    });
}

/// Tests that `WeakRef::upgrade()` with a wrapped resource creates a strong ref
/// that prevents GC collection.
#[test]
fn weak_ref_upgrade_with_wrapped_resource_prevents_gc() {
    SIMPLE_RESOURCE_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let strong = jsg::Rc::new(SimpleResource {
            name: "persistent".to_owned(),
        });
        let _wrapped = strong.clone().to_js(lock);
        let weak = strong.downgrade();

        // Drop the original strong ref, but upgrade from weak creates a new one
        std::mem::drop(strong);
        let upgraded = weak.upgrade().expect("should be alive via wrapper");
        assert_eq!(upgraded.name, "persistent");
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 0);

        // The upgraded Ref keeps the resource alive
        std::mem::drop(upgraded);
        Ok(())
    });

    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
}

/// Tests that `WeakRef::trace()` is a no-op and doesn't prevent GC collection.
///
/// A resource holding a `WeakRef` to another resource should not keep it alive through tracing.
#[test]
fn weak_ref_trace_does_not_prevent_gc() {
    static HOLDER_DROPS: AtomicUsize = AtomicUsize::new(0);

    #[jsg_resource]
    struct WeakRefHolder {
        weak: jsg::Weak<SimpleResource>,
    }

    impl Drop for WeakRefHolder {
        fn drop(&mut self) {
            HOLDER_DROPS.fetch_add(1, Ordering::SeqCst);
        }
    }

    #[jsg_resource]
    impl WeakRefHolder {}

    SIMPLE_RESOURCE_DROPS.store(0, Ordering::SeqCst);
    HOLDER_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let target = jsg::Rc::new(SimpleResource {
            name: "target".to_owned(),
        });
        let _target_wrapped = target.clone().to_js(lock);

        let holder = jsg::Rc::new(WeakRefHolder {
            weak: target.downgrade(),
        });
        let _holder_wrapped = holder.clone().to_js(lock);

        // WeakRef should be upgradable while the target is alive
        assert!(holder.weak.upgrade().is_some());

        // Drop all Rust refs — both are only held by JS wrappers
        std::mem::drop(target);
        std::mem::drop(holder);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 0);
        Ok(())
    });

    // GC should collect the target — the WeakRef in holder doesn't keep it alive
    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        // Both should be collected (holder's WeakRef doesn't prevent target's collection)
        assert_eq!(HOLDER_DROPS.load(Ordering::SeqCst), 1);
        crate::Harness::request_gc(lock);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
}

// =============================================================================
// Ref tests
// =============================================================================

/// Tests that `Ref::clone()` keeps the resource alive after the original is dropped.
#[test]
fn ref_clone_keeps_resource_alive() {
    SIMPLE_RESOURCE_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|_lock, _ctx| {
        let original = jsg::Rc::new(SimpleResource {
            name: "cloned".to_owned(),
        });
        let clone1 = original.clone();
        let clone2 = original.clone();

        std::mem::drop(original);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 0);
        assert_eq!(clone1.name, "cloned");

        std::mem::drop(clone1);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 0);
        assert_eq!(clone2.name, "cloned");

        std::mem::drop(clone2);
        // Only now all refs are gone — resource is destroyed
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
}

/// Tests that dropping multiple Refs only triggers destruction on the last one.
#[test]
fn multiple_ref_drops_only_last_triggers_destruction() {
    SIMPLE_RESOURCE_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|_lock, _ctx| {
        let original = jsg::Rc::new(SimpleResource {
            name: "multi".to_owned(),
        });

        // Create several clones
        let clones: Vec<_> = (0..5).map(|_| original.clone()).collect();
        std::mem::drop(original);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 0);

        // Drop one by one
        for (i, c) in clones.into_iter().enumerate() {
            std::mem::drop(c);
            if i < 4 {
                assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 0);
            }
        }
        // Last clone dropped — resource is destroyed
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
}

/// Tests that `Ref::deref()` returns the correct resource data.
#[test]
fn ref_deref_returns_correct_resource_data() {
    let harness = crate::Harness::new();
    harness.run_in_context(|_lock, _ctx| {
        let r = jsg::Rc::new(SimpleResource {
            name: "deref-test".to_owned(),
        });

        // Deref should return the resource with correct data
        assert_eq!(r.name, "deref-test");

        // Clone should also deref to the same data
        #[expect(clippy::redundant_clone)]
        let clone = r.clone();
        assert_eq!(clone.name, "deref-test");
        Ok(())
    });
}

// =============================================================================
// unwrap / FromJS tests
// =============================================================================

/// Tests that `FromJS for Ref<R>` creates a strong reference from a JS wrapper.
///
/// The returned `Ref<R>` must keep the resource alive even after dropping the original.
#[test]
fn from_js_creates_strong_reference_from_js_wrapper() {
    SIMPLE_RESOURCE_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Rc::new(SimpleResource {
            name: "unwrap-me".to_owned(),
        });
        let wrapped = resource.clone().to_js(lock);
        ctx.set_global("obj", wrapped);

        // Get the JS value back and use FromJS to create a new strong Ref
        let js_val = ctx.eval_raw("obj").unwrap();
        let new_ref: jsg::Rc<SimpleResource> =
            <jsg::Rc<SimpleResource> as jsg::FromJS>::from_js(lock, js_val)
                .expect("FromJS should succeed for a wrapped resource");
        assert_eq!(new_ref.name, "unwrap-me");

        // Drop the original Ref — the unwrapped ref should keep it alive
        std::mem::drop(resource);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 0);

        // The unwrapped ref is still valid
        assert_eq!(new_ref.name, "unwrap-me");

        // Dropping the unwrapped ref (with wrapper still alive) should not destroy
        std::mem::drop(new_ref);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 0);
        Ok(())
    });

    // GC the JS wrapper — now the resource can be collected
    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
}

/// Tests that `FromJS` returns `Err` for a plain JS object.
///
/// A plain `{}` object has no internal fields and no wrappable tag.
/// `FromJS::from_js` must return `Err` without crashing.
#[test]
fn from_js_returns_err_for_plain_js_object() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let plain_obj = ctx.eval_raw("({})").unwrap();
        let result = <jsg::Rc<SimpleResource> as jsg::FromJS>::from_js(lock, plain_obj);
        assert!(
            result.is_err(),
            "FromJS should return Err for a plain JS object"
        );
        Ok(())
    });
}

// =============================================================================
// Wrap/template caching tests
// =============================================================================

/// Tests that wrapping multiple instances of the same type uses the same template.
///
/// Verifies that `Realm::get_constructor()` caches the template on first use
/// and returns the same one on subsequent calls.
#[test]
fn wrap_multiple_instances_of_same_type() {
    SIMPLE_RESOURCE_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r1 = jsg::Rc::new(SimpleResource {
            name: "first".to_owned(),
        });
        let r2 = jsg::Rc::new(SimpleResource {
            name: "second".to_owned(),
        });

        let w1 = r1.to_js(lock);
        let w2 = r2.to_js(lock);
        ctx.set_global("r1", w1);
        ctx.set_global("r2", w2);

        // Both should be instances of the same constructor (same prototype chain)
        let same_proto: bool = ctx
            .eval(
                lock,
                "Object.getPrototypeOf(r1) === Object.getPrototypeOf(r2)",
            )
            .unwrap();
        assert!(same_proto);

        // Both should work independently
        let n1: String = ctx.eval(lock, "r1.getName()").unwrap();
        let n2: String = ctx.eval(lock, "r2.getName()").unwrap();
        assert_eq!(n1, "first");
        assert_eq!(n2, "second");
        Ok(())
    });
}

/// Tests that wrapping the same resource twice returns the same JS object.
///
/// `Wrappable::tryGetHandle()` should detect an existing wrapper and return it.
#[test]
fn wrap_same_resource_twice_returns_same_object() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Rc::new(SimpleResource {
            name: "singleton".to_owned(),
        });

        let w1 = resource.clone().to_js(lock);
        ctx.set_global("w1", w1);
        let w2 = resource.to_js(lock);
        ctx.set_global("w2", w2);

        // Both wraps should return the exact same JS object
        let same: bool = ctx.eval(lock, "w1 === w2").unwrap();
        assert!(same);
        Ok(())
    });
}

// =============================================================================
// Resource data integrity tests
// =============================================================================

/// Tests that resource data survives a GC cycle when held by a JS global.
#[test]
fn resource_data_survives_gc_via_js_global() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Rc::new(SimpleResource {
            name: "survivor".to_owned(),
        });
        let wrapped = resource.to_js(lock);
        ctx.set_global("obj", wrapped);

        // Force GC — the global reference should keep it alive
        crate::Harness::request_gc(lock);

        // Data should still be correct
        let name: String = ctx.eval(lock, "obj.getName()").unwrap();
        assert_eq!(name, "survivor");
        Ok(())
    });
}

/// Tests parent-child relationships where parent is only held by JS.
///
/// When the parent is wrapped and held by a JS global, its `Ref<SimpleResource>` child
/// should be traced during GC and kept alive even without any Rust strong refs.
#[test]
fn parent_ref_keeps_child_alive_through_gc() {
    SIMPLE_RESOURCE_DROPS.store(0, Ordering::SeqCst);
    PARENT_RESOURCE_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let child = jsg::Rc::new(SimpleResource {
            name: "child".to_owned(),
        });
        let parent = jsg::Rc::new(ParentResource {
            child: child.clone(),
            optional_child: None,
        });
        let wrapped = parent.clone().to_js(lock);
        ctx.set_global("parent", wrapped);

        // Drop all Rust refs
        std::mem::drop(child);
        std::mem::drop(parent);

        // GC should NOT collect the parent (held by global) or child (held by parent's Ref)
        crate::Harness::request_gc(lock);
        assert_eq!(PARENT_RESOURCE_DROPS.load(Ordering::SeqCst), 0);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 0);
        Ok(())
    });

    // New context — global is gone
    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(PARENT_RESOURCE_DROPS.load(Ordering::SeqCst), 1);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
}

/// Tests that dropping all strong refs correctly invalidates ALL weak refs.
///
/// Creates multiple `WeakRef`s from different `Ref`s, verifies they all see death at the same time.
#[test]
fn instance_drop_invalidates_all_weak_refs() {
    SIMPLE_RESOURCE_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|_lock, _ctx| {
        let original = jsg::Rc::new(SimpleResource {
            name: "shared-target".to_owned(),
        });
        let clone1 = original.clone();
        let clone2 = original.clone();

        // Create weak refs from different strong refs
        let weak_from_original = original.downgrade();
        let weak_from_clone1 = clone1.downgrade();
        let weak_from_clone2 = clone2.downgrade();

        // All alive
        assert!(weak_from_original.is_alive());
        assert!(weak_from_clone1.is_alive());
        assert!(weak_from_clone2.is_alive());

        // Drop all strong refs — resource is destroyed
        std::mem::drop(original);
        std::mem::drop(clone1);
        std::mem::drop(clone2);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 1);

        // ALL weak refs see death
        assert!(!weak_from_original.is_alive());
        assert!(!weak_from_clone1.is_alive());
        assert!(!weak_from_clone2.is_alive());
        assert!(weak_from_original.upgrade().is_none());
        assert!(weak_from_clone1.upgrade().is_none());
        assert!(weak_from_clone2.upgrade().is_none());
        Ok(())
    });
}

// =============================================================================
// Nullable<Ref<T>> tracing tests
// =============================================================================

/// Counter to track how many `NullableParent` instances have been dropped.
static NULLABLE_PARENT_DROPS: AtomicUsize = AtomicUsize::new(0);

#[jsg_resource]
struct NullableParent {
    pub child: jsg::Nullable<jsg::Rc<SimpleResource>>,
}

impl Drop for NullableParent {
    fn drop(&mut self) {
        NULLABLE_PARENT_DROPS.fetch_add(1, Ordering::SeqCst);
    }
}

#[jsg_resource]
impl NullableParent {}

/// Tests that `Nullable<Ref<T>>` with `Nullable::Some` keeps the child alive through GC tracing.
#[test]
fn nullable_ref_some_keeps_child_alive_through_gc() {
    SIMPLE_RESOURCE_DROPS.store(0, Ordering::SeqCst);
    NULLABLE_PARENT_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let child = jsg::Rc::new(SimpleResource {
            name: "nullable_child".to_owned(),
        });
        let parent = jsg::Rc::new(NullableParent {
            child: jsg::Nullable::Some(child.clone()),
        });
        let wrapped = parent.clone().to_js(lock);
        ctx.set_global("parent", wrapped);

        // Drop all Rust refs
        std::mem::drop(child);
        std::mem::drop(parent);

        // GC should NOT collect the parent (held by global) or child (traced via Nullable::Some)
        crate::Harness::request_gc(lock);
        assert_eq!(NULLABLE_PARENT_DROPS.load(Ordering::SeqCst), 0);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 0);
        Ok(())
    });

    // New context — global is gone
    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(NULLABLE_PARENT_DROPS.load(Ordering::SeqCst), 1);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
}

/// Tests that `Nullable<Ref<T>>` with `Nullable::Null` doesn't cause issues during GC.
#[test]
fn nullable_ref_null_does_not_crash_during_gc() {
    NULLABLE_PARENT_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let parent = jsg::Rc::new(NullableParent {
            child: jsg::Nullable::Null,
        });
        let wrapped = parent.clone().to_js(lock);
        ctx.set_global("parent", wrapped);

        std::mem::drop(parent);

        // GC with Nullable::Null should not crash
        crate::Harness::request_gc(lock);
        assert_eq!(NULLABLE_PARENT_DROPS.load(Ordering::SeqCst), 0);
        Ok(())
    });

    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(NULLABLE_PARENT_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
}

/// Tests that `Nullable<Ref<T>>` with `Nullable::Undefined` doesn't cause issues during GC.
#[test]
fn nullable_ref_undefined_does_not_crash_during_gc() {
    NULLABLE_PARENT_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let parent = jsg::Rc::new(NullableParent {
            child: jsg::Nullable::Undefined,
        });
        let wrapped = parent.clone().to_js(lock);
        ctx.set_global("parent", wrapped);

        std::mem::drop(parent);

        // GC with Nullable::Undefined should not crash
        crate::Harness::request_gc(lock);
        assert_eq!(NULLABLE_PARENT_DROPS.load(Ordering::SeqCst), 0);
        Ok(())
    });

    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(NULLABLE_PARENT_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
}

// ---------------------------------------------------------------------------
// Rc<NativeState> shared ownership — native object outlives Ref but is
// dropped when V8 GC collects the JS wrapper.
// ---------------------------------------------------------------------------

static NATIVE_STATE_DROPS: AtomicUsize = AtomicUsize::new(0);

struct NativeState {
    value: u64,
}

impl Drop for NativeState {
    fn drop(&mut self) {
        NATIVE_STATE_DROPS.fetch_add(1, Ordering::SeqCst);
    }
}

#[jsg_resource]
struct NativeOwnerResource {
    state: std::rc::Rc<NativeState>,
}

#[jsg_resource]
impl NativeOwnerResource {
    #[jsg_method]
    fn get_value(&self) -> jsg::Number {
        #[expect(clippy::cast_precision_loss)]
        jsg::Number::new(self.state.value as f64)
    }
}

/// Regression test for the `strong` flag bug in `wrappable_remove_strong_ref`.
///
/// When a parent resource (with a JS wrapper) holds a `Ref<Child>`, GC tracing
/// transitions the child's ref from strong→weak via `visitRef`, which calls
/// `removeStrongRef()` once.
///
/// **Bug**: `wrappable_remove_strong_ref` always passed `strong=true` to
/// `maybeDeferDestruction`, so when the parent was later collected and the
/// child `Ref` dropped, `~RefToDelete` called `removeStrongRef()` a second
/// time — underflowing the strong refcount.
///
/// This test keeps a direct Rust ref to the child alive so we can observe
/// the strong refcount after the parent's traced ref is dropped by GC.
/// With the bug: `strong_refcount()` returns 0 instead of 1.
#[test]
#[cfg(debug_assertions)]
fn traced_ref_drop_respects_strong_flag() {
    let harness = crate::Harness::new();
    let mut child_holder: Option<jsg::Rc<SimpleResource>> = None;

    harness.run_in_context(|lock, _ctx| {
        let child = jsg::Rc::new(SimpleResource {
            name: "traced_child".to_owned(),
        });

        // strongRefcount = 1 (our ref).
        assert_eq!(child.strong_refcount(), 1);

        let parent = jsg::Rc::new(ParentResource {
            child: child.clone(),
            optional_child: None,
        });

        // strongRefcount = 2 (our ref + parent's ref).
        assert_eq!(child.strong_refcount(), 2);

        // Wrap the parent — child is reachable only through the parent's Ref.
        let _wrapped = parent.clone().to_js(lock);

        // GC traces parent wrapper → visitRef on child → parent's ref
        // transitions strong→weak, removeStrongRef() called once.
        // strongRefcount = 1 (only our direct ref remains strong).
        crate::Harness::request_gc(lock);
        assert_eq!(child.strong_refcount(), 1);

        // Drop parent Rust ref. JS wrapper keeps parent alive.
        std::mem::drop(parent);

        // Move child out so we can observe it after GC collects the parent.
        child_holder = Some(child);
        Ok(())
    });

    let child = child_holder.expect("child not set");

    // Second GC: parent's JS wrapper is unreachable → parent collected →
    // parent's child Ref drops → wrappable_remove_strong_ref called.
    //
    // With the bug: parent's ref has strong=false but remove_strong_ref
    // always passes true → removeStrongRef() called → strongRefcount
    // goes from 1 to 0, but our ref is still strong → should be 1!
    //
    // With the fix: strong=false is threaded through → no removeStrongRef
    // call → strongRefcount stays at 1 (correct).
    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(
            child.strong_refcount(),
            1,
            "strong refcount underflowed — \
            wrappable_remove_strong_ref passed strong=true for a traced (weak) ref"
        );
        Ok(())
    });
}

/// A Rust resource holds an `Rc<NativeState>`. After wrapping for JS and
/// dropping the Rust `Ref`, the native state is kept alive by the `CppgcShim`.
/// Minor (young-generation) GC must collect the wrapper and drop the native
/// state — proving the `ResetRoot` / `detachLater` path works for Rust resources.
#[test]
fn rc_native_object_dropped_on_minor_gc() {
    NATIVE_STATE_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    let state = std::rc::Rc::new(NativeState { value: 99 });
    let state_clone = state.clone();

    harness.run_in_context(|lock, _ctx| {
        let resource = jsg::Rc::new(NativeOwnerResource { state: state_clone });

        // Wrap for JS so CppgcShim takes ownership of the Wrappable.
        let _wrapped = resource.clone().to_js(lock);

        // Drop the Rust Ref. CppgcShim still keeps the resource alive.
        std::mem::drop(resource);
        assert_eq!(NATIVE_STATE_DROPS.load(Ordering::SeqCst), 0);
        Ok(())
    });

    // Drop the external Rc clone. The resource's Rc inside the Wrappable
    // is the last one, but the Wrappable is alive (held by CppgcShim).
    std::mem::drop(state);
    assert_eq!(NATIVE_STATE_DROPS.load(Ordering::SeqCst), 0);

    // Minor GC collects the young-generation wrapper via the ResetRoot path,
    // which detaches the CppgcShim → drops kj::Own<Wrappable> → drops the
    // resource → drops Rc<NativeState> → NativeState is dropped.
    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_minor_gc(lock);
        assert_eq!(NATIVE_STATE_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
}

// =============================================================================
// Global<Value> back-reference cycle — collected via visit_global tracing
// =============================================================================
//
// `jsg::v8::Global<T>` fields on Rust resources participate in GC tracing via
// `GcVisitor::visit_global`, which implements the same strong↔traced dual-mode
// switching as `jsg::Data` / `jsg::V8Ref<T>` in C++.
//
// When the parent Wrappable has strong Rust refs the handle stays strong.
// Once all Rust refs are dropped and only the JS wrapper keeps it alive,
// `visit_global` downgrades the handle to a `v8::TracedReference` that cppgc
// can follow — allowing the GC to detect and collect the cycle.

static CYCLIC_RESOURCE_DROPS: AtomicUsize = AtomicUsize::new(0);

/// A resource that stores a `Global<Value>` via `Cell` so the back-reference
/// to the resource's own JS wrapper can be installed after wrapping.
///
/// `Cell<Option<…>>` provides interior mutability through `&self`, matching
/// the access pattern of `GarbageCollected::trace(&self)`.
#[jsg_resource]
struct CyclicResource {
    /// The stored "callback". Uses `Cell` so it can be set after `to_js()`
    /// returns the wrapper `Local`, without requiring `&mut self`.
    callback: std::cell::Cell<Option<jsg::v8::Global<jsg::v8::Value>>>,
}

impl Drop for CyclicResource {
    fn drop(&mut self) {
        CYCLIC_RESOURCE_DROPS.fetch_add(1, Ordering::SeqCst);
    }
}

#[jsg_resource]
impl CyclicResource {}

/// Verifies that a `Global<Value>` back-reference cycle is collected by GC.
///
/// The cycle:
///   CyclicResource.callback (Global<Value>) → JS wrapper
///   JS wrapper → `CppgcShim` → `Wrappable` → `CyclicResource` (same object)
///
/// `visit_global` downgrades the `Global` to a `v8::TracedReference` once all
/// strong Rust refs are dropped, making the cycle visible to cppgc so it can
/// be collected on the next full GC.
#[test]
fn global_value_back_ref_is_collected_by_gc() {
    CYCLIC_RESOURCE_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let resource = jsg::Rc::new(CyclicResource {
            callback: std::cell::Cell::new(None),
        });

        // Wrap to produce the JS object, then immediately promote to a
        // Global so we can store it back into the resource's own field.
        // `to_js` consumes the Rc clone but the original `resource` still
        // holds a strong ref, so the resource stays alive.
        let wrapper_local = resource.clone().to_js(lock);
        let wrapper_global = wrapper_local.to_global(lock);

        // Install the back-reference: resource now holds a Global pointing
        // to its own JS wrapper, closing the cycle.
        resource.callback.set(Some(wrapper_global));

        // Drop the only Rust Rc. The cycle is now closed but the Global
        // will be downgraded to a TracedReference by visit_global, making
        // it visible to cppgc.
        std::mem::drop(resource);
        assert_eq!(CYCLIC_RESOURCE_DROPS.load(Ordering::SeqCst), 0);
        Ok(())
    });

    // Full GC can now detect and collect the cycle because visit_global
    // downgrades the Global to a TracedReference during tracing.
    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(
            CYCLIC_RESOURCE_DROPS.load(Ordering::SeqCst),
            1,
            "full GC should collect the cyclic resource via visit_global tracing"
        );
        Ok(())
    });
}

// =============================================================================
// Cell<T> tracing tests
// =============================================================================

static CELL_CHILD_DROPS: AtomicUsize = AtomicUsize::new(0);

#[jsg_resource]
struct CellChild;

impl Drop for CellChild {
    fn drop(&mut self) {
        CELL_CHILD_DROPS.fetch_add(1, Ordering::SeqCst);
    }
}

#[jsg_resource]
impl CellChild {}

static CELL_PARENT_DROPS: AtomicUsize = AtomicUsize::new(0);

/// Resource with a `Cell<jsg::Rc<T>>` field.
#[jsg_resource]
struct CellParent {
    pub child: Cell<jsg::Rc<CellChild>>,
}

impl Drop for CellParent {
    fn drop(&mut self) {
        CELL_PARENT_DROPS.fetch_add(1, Ordering::SeqCst);
    }
}

#[jsg_resource]
impl CellParent {}

/// Resource with a `Cell<Option<jsg::Rc<T>>>` field.
#[jsg_resource]
struct CellOptionParent {
    pub child: Cell<Option<jsg::Rc<CellChild>>>,
}

impl Drop for CellOptionParent {
    fn drop(&mut self) {
        CELL_PARENT_DROPS.fetch_add(1, Ordering::SeqCst);
    }
}

#[jsg_resource]
impl CellOptionParent {}

/// Resource with a `Cell<jsg::Nullable<jsg::Rc<T>>>` field.
#[jsg_resource]
struct CellNullableParent {
    pub child: Cell<jsg::Nullable<jsg::Rc<CellChild>>>,
}

impl Drop for CellNullableParent {
    fn drop(&mut self) {
        CELL_PARENT_DROPS.fetch_add(1, Ordering::SeqCst);
    }
}

#[jsg_resource]
impl CellNullableParent {}

/// Resource using the fully-qualified `std::cell::Cell<jsg::Rc<T>>` syntax.
#[jsg_resource]
struct StdCellParent {
    pub child: std::cell::Cell<jsg::Rc<CellChild>>,
}

impl Drop for StdCellParent {
    fn drop(&mut self) {
        CELL_PARENT_DROPS.fetch_add(1, Ordering::SeqCst);
    }
}

#[jsg_resource]
impl StdCellParent {}

/// `Cell<jsg::Rc<T>>` keeps the child alive through GC.
#[test]
fn cell_ref_keeps_child_alive_through_gc() {
    CELL_CHILD_DROPS.store(0, Ordering::SeqCst);
    CELL_PARENT_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let child = jsg::Rc::new(CellChild);
        let parent = jsg::Rc::new(CellParent {
            child: Cell::new(child.clone()),
        });
        let wrapped = parent.clone().to_js(lock);
        ctx.set_global("parent", wrapped);

        // Drop all Rust refs.
        std::mem::drop(child);
        std::mem::drop(parent);

        // Parent is JS-global-held; child is kept alive via Cell<Rc<T>> tracing.
        crate::Harness::request_gc(lock);
        assert_eq!(CELL_PARENT_DROPS.load(Ordering::SeqCst), 0);
        assert_eq!(CELL_CHILD_DROPS.load(Ordering::SeqCst), 0);
        Ok(())
    });

    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(CELL_PARENT_DROPS.load(Ordering::SeqCst), 1);
        assert_eq!(CELL_CHILD_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
}

/// `Cell<Option<jsg::Rc<T>>>` with `Some` keeps the child alive through GC.
#[test]
fn cell_option_ref_some_keeps_child_alive_through_gc() {
    CELL_CHILD_DROPS.store(0, Ordering::SeqCst);
    CELL_PARENT_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let child = jsg::Rc::new(CellChild);
        let parent = jsg::Rc::new(CellOptionParent {
            child: Cell::new(Some(child.clone())),
        });
        let wrapped = parent.clone().to_js(lock);
        ctx.set_global("parent", wrapped);

        std::mem::drop(child);
        std::mem::drop(parent);

        crate::Harness::request_gc(lock);
        assert_eq!(CELL_PARENT_DROPS.load(Ordering::SeqCst), 0);
        assert_eq!(CELL_CHILD_DROPS.load(Ordering::SeqCst), 0);
        Ok(())
    });

    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(CELL_PARENT_DROPS.load(Ordering::SeqCst), 1);
        assert_eq!(CELL_CHILD_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
}

/// `Cell<Option<jsg::Rc<T>>>` with `None` does not crash during GC.
#[test]
fn cell_option_ref_none_does_not_crash_during_gc() {
    CELL_PARENT_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let parent = jsg::Rc::new(CellOptionParent {
            child: Cell::new(None),
        });
        let wrapped = parent.clone().to_js(lock);
        ctx.set_global("parent", wrapped);

        std::mem::drop(parent);

        crate::Harness::request_gc(lock);
        assert_eq!(CELL_PARENT_DROPS.load(Ordering::SeqCst), 0);
        Ok(())
    });

    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(CELL_PARENT_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
}

/// `Cell<jsg::Nullable<jsg::Rc<T>>>` with `Nullable::Some` keeps child alive.
#[test]
fn cell_nullable_ref_some_keeps_child_alive_through_gc() {
    CELL_CHILD_DROPS.store(0, Ordering::SeqCst);
    CELL_PARENT_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let child = jsg::Rc::new(CellChild);
        let parent = jsg::Rc::new(CellNullableParent {
            child: Cell::new(jsg::Nullable::Some(child.clone())),
        });
        let wrapped = parent.clone().to_js(lock);
        ctx.set_global("parent", wrapped);

        std::mem::drop(child);
        std::mem::drop(parent);

        crate::Harness::request_gc(lock);
        assert_eq!(CELL_PARENT_DROPS.load(Ordering::SeqCst), 0);
        assert_eq!(CELL_CHILD_DROPS.load(Ordering::SeqCst), 0);
        Ok(())
    });

    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(CELL_PARENT_DROPS.load(Ordering::SeqCst), 1);
        assert_eq!(CELL_CHILD_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
}

/// `Cell<jsg::Nullable<jsg::Rc<T>>>` with `Nullable::Null` does not crash during GC.
#[test]
fn cell_nullable_ref_null_does_not_crash_during_gc() {
    CELL_PARENT_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let parent = jsg::Rc::new(CellNullableParent {
            child: Cell::new(jsg::Nullable::Null),
        });
        let wrapped = parent.clone().to_js(lock);
        ctx.set_global("parent", wrapped);

        std::mem::drop(parent);

        crate::Harness::request_gc(lock);
        assert_eq!(CELL_PARENT_DROPS.load(Ordering::SeqCst), 0);
        Ok(())
    });

    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(CELL_PARENT_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
}

/// `std::cell::Cell<jsg::Rc<T>>` (fully-qualified path) keeps child alive through GC.
#[test]
fn std_cell_ref_keeps_child_alive_through_gc() {
    CELL_CHILD_DROPS.store(0, Ordering::SeqCst);
    CELL_PARENT_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let child = jsg::Rc::new(CellChild);
        let parent = jsg::Rc::new(StdCellParent {
            child: std::cell::Cell::new(child.clone()),
        });
        let wrapped = parent.clone().to_js(lock);
        ctx.set_global("parent", wrapped);

        std::mem::drop(child);
        std::mem::drop(parent);

        crate::Harness::request_gc(lock);
        assert_eq!(CELL_PARENT_DROPS.load(Ordering::SeqCst), 0);
        assert_eq!(CELL_CHILD_DROPS.load(Ordering::SeqCst), 0);
        Ok(())
    });

    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(CELL_PARENT_DROPS.load(Ordering::SeqCst), 1);
        assert_eq!(CELL_CHILD_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
}
