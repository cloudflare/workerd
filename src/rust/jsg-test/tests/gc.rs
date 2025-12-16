//! GC-related tests for the jsg crate.

use std::sync::atomic::Ordering;

use jsg::Resource;
use jsg::Type;

use super::PARENT_RESOURCE_DROPS;
use super::ParentResource;
use super::SIMPLE_RESOURCE_DROPS;
use super::SimpleResource;

#[test]
fn supports_gc_via_realm_drop() {
    SIMPLE_RESOURCE_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|mut lock| {
        let resource = SimpleResource {
            name: "test".to_owned(),
            callback: None,
        };
        let resource = SimpleResource::alloc(&mut lock, resource);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 0);
        std::mem::drop(resource);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 1);
    });
}

#[test]
fn supports_gc_via_weak_callback() {
    SIMPLE_RESOURCE_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|mut lock| {
        let resource = SimpleResource {
            name: "test".to_owned(),
            callback: None,
        };
        let resource = SimpleResource::alloc(&mut lock, resource);
        let _wrapped = SimpleResource::wrap(resource.clone(), &mut lock);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 0);
        std::mem::drop(resource);
        // There is a JS object that holds a reference to the resource
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 0);
    });

    harness.run_in_context(|mut lock| {
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 0);
        crate::Harness::request_gc(&mut lock);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 1);
    });
}

#[test]
fn resource_with_ref_field_compiles() {
    SIMPLE_RESOURCE_DROPS.store(0, Ordering::SeqCst);
    PARENT_RESOURCE_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|mut lock| {
        let child = SimpleResource {
            name: "child".to_owned(),
            callback: None,
        };
        let child_ref = SimpleResource::alloc(&mut lock, child);

        let optional_child = SimpleResource {
            name: "optional_child".to_owned(),
            callback: None,
        };
        let optional_child_ref = SimpleResource::alloc(&mut lock, optional_child);

        let parent = ParentResource {
            child: child_ref,
            optional_child: Some(optional_child_ref),
        };
        let parent_ref = ParentResource::alloc(&mut lock, parent);
        let _wrapped = ParentResource::wrap(parent_ref.clone(), &mut lock);

        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 0);
        assert_eq!(PARENT_RESOURCE_DROPS.load(Ordering::SeqCst), 0);

        std::mem::drop(parent_ref);
    });

    harness.run_in_context(|mut lock| {
        crate::Harness::request_gc(&mut lock);
        assert_eq!(PARENT_RESOURCE_DROPS.load(Ordering::SeqCst), 1);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 2);
    });
}

#[test]
fn child_ref_kept_alive_by_parent() {
    SIMPLE_RESOURCE_DROPS.store(0, Ordering::SeqCst);
    PARENT_RESOURCE_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|mut lock| {
        let child = SimpleResource {
            name: "child".to_owned(),
            callback: None,
        };
        let child_ref = SimpleResource::alloc(&mut lock, child);
        let child_ref_clone = child_ref.clone();

        let parent = ParentResource {
            child: child_ref,
            optional_child: None,
        };
        let parent_ref = ParentResource::alloc(&mut lock, parent);
        let _parent_wrapped = ParentResource::wrap(parent_ref.clone(), &mut lock);

        std::mem::drop(child_ref_clone);

        // Child not collected because parent still holds a reference
        crate::Harness::request_gc(&mut lock);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 0);

        std::mem::drop(parent_ref);

        // Now both are collected
        crate::Harness::request_gc(&mut lock);
        assert_eq!(PARENT_RESOURCE_DROPS.load(Ordering::SeqCst), 1);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 1);
    });
}

#[test]
fn weak_ref_upgrade() {
    SIMPLE_RESOURCE_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|mut lock| {
        let resource = SimpleResource {
            name: "test".to_owned(),
            callback: None,
        };
        let strong_ref = SimpleResource::alloc(&mut lock, resource);
        let weak_ref = jsg::WeakRef::from(&strong_ref);

        // Weak ref can be upgraded while strong ref exists
        assert_eq!(weak_ref.strong_count(), 1);
        let upgraded = weak_ref.upgrade();
        assert!(upgraded.is_some());
        assert_eq!(weak_ref.strong_count(), 2);

        // Drop the upgraded ref
        std::mem::drop(upgraded);
        assert_eq!(weak_ref.strong_count(), 1);

        // Drop the original strong ref
        std::mem::drop(strong_ref);

        // Resource should be dropped now
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 1);
    });
}

// ============================================================================
// cppgc module tests
// ============================================================================

#[test]
fn cppgc_handle_keeps_resource_alive() {
    SIMPLE_RESOURCE_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|mut lock| {
        let resource = SimpleResource {
            name: "test".to_owned(),
            callback: None,
        };
        let strong_ref = SimpleResource::alloc(&mut lock, resource);

        // Wrap the resource to allocate it on the cppgc heap
        let _wrapped = SimpleResource::wrap(strong_ref.clone(), &mut lock);

        // Drop the Rust ref - resource should still be alive due to cppgc handle
        std::mem::drop(strong_ref);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 0);
    });

    // After context exits and GC runs, resource should be dropped
    harness.run_in_context(|mut lock| {
        crate::Harness::request_gc(&mut lock);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 1);
    });
}

#[test]
fn cppgc_weak_member_cleared_after_gc() {
    SIMPLE_RESOURCE_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|mut lock| {
        let resource = SimpleResource {
            name: "test".to_owned(),
            callback: None,
        };
        let strong_ref = SimpleResource::alloc(&mut lock, resource);

        // Wrap to create cppgc allocation, then create a weak ref
        let _wrapped = SimpleResource::wrap(strong_ref.clone(), &mut lock);
        let weak_ref = jsg::WeakRef::from(&strong_ref);

        // Weak ref should be upgradeable while strong ref exists
        assert!(weak_ref.upgrade().is_some());

        // Drop strong ref and wrapped object goes out of scope
        std::mem::drop(strong_ref);

        // Resource not dropped yet - JS wrapper holds it
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 0);
    });

    // After GC, weak member should be cleared
    harness.run_in_context(|mut lock| {
        crate::Harness::request_gc(&mut lock);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 1);
    });
}

#[test]
fn cppgc_weak_handle_default_behavior() {
    // WeakHandle doesn't have a Default impl, but we can verify it
    // doesn't prevent GC when the strong handle is released
    SIMPLE_RESOURCE_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|mut lock| {
        let resource = SimpleResource {
            name: "test".to_owned(),
            callback: None,
        };
        let strong_ref = SimpleResource::alloc(&mut lock, resource);

        // Wrap the resource
        let _wrapped = SimpleResource::wrap(strong_ref.clone(), &mut lock);

        // Drop Rust ref - cppgc handle still holds it
        std::mem::drop(strong_ref);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 0);
    });

    // GC should collect it
    harness.run_in_context(|mut lock| {
        crate::Harness::request_gc(&mut lock);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 1);
    });
}

/// Test that traced weak references are properly managed during GC
#[test]
fn cppgc_weak_member_traced_in_gc() {
    SIMPLE_RESOURCE_DROPS.store(0, Ordering::SeqCst);
    PARENT_RESOURCE_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|mut lock| {
        let child = SimpleResource {
            name: "child".to_owned(),
            callback: None,
        };
        let child_ref = SimpleResource::alloc(&mut lock, child);

        // Wrap child to create cppgc allocation
        let _child_wrapped = SimpleResource::wrap(child_ref.clone(), &mut lock);

        let parent = ParentResource {
            child: child_ref.clone(),
            optional_child: None,
        };
        let parent_ref = ParentResource::alloc(&mut lock, parent);

        // Wrap parent
        let _parent_wrapped = ParentResource::wrap(parent_ref.clone(), &mut lock);

        // Drop Rust refs
        std::mem::drop(child_ref);
        std::mem::drop(parent_ref);

        // Nothing dropped yet - JS wrappers keep them alive
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 0);
        assert_eq!(PARENT_RESOURCE_DROPS.load(Ordering::SeqCst), 0);
    });

    // GC may take multiple cycles to collect both resources
    // Parent holds a Ref to child, so child won't be collected until parent is
    harness.run_in_context(|mut lock| {
        // First GC - should collect parent (no strong refs)
        crate::Harness::request_gc(&mut lock);
        // Parent dropped, which releases the child Ref
        assert_eq!(PARENT_RESOURCE_DROPS.load(Ordering::SeqCst), 1);

        // Second GC - should collect child now that parent released the Ref
        crate::Harness::request_gc(&mut lock);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 1);
    });
}
