//! GC (garbage collection) tests for Rust resources.
//!
//! These tests verify that Rust resources are properly cleaned up when:
//! 1. All Rust `Ref` handles are dropped and no JavaScript wrapper exists
//! 2. All Rust `Ref` handles are dropped and V8 garbage collects the JS wrapper

use std::cell::RefCell;
use std::sync::atomic::AtomicUsize;
use std::sync::atomic::Ordering;

use jsg::Resource;
use jsg::v8::TracedReference;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;

/// Counter to track how many `SimpleResource` instances have been dropped.
static SIMPLE_RESOURCE_DROPS: AtomicUsize = AtomicUsize::new(0);

#[jsg_resource]
struct SimpleResource {
    pub name: String,
    pub callback: Option<TracedReference<jsg::v8::Object>>,
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
    pub child: jsg::Ref<SimpleResource>,
    pub optional_child: Option<jsg::Ref<SimpleResource>>,
}

impl Drop for ParentResource {
    fn drop(&mut self) {
        PARENT_RESOURCE_DROPS.fetch_add(1, Ordering::SeqCst);
    }
}

#[jsg_resource]
impl ParentResource {}

/// Counter to track how many `CyclicResource` instances have been dropped.
static CYCLIC_RESOURCE_DROPS: AtomicUsize = AtomicUsize::new(0);

#[jsg_resource]
struct CyclicResource {
    #[expect(dead_code)]
    pub name: String,
    pub other: RefCell<Option<jsg::Ref<CyclicResource>>>,
}

impl Drop for CyclicResource {
    fn drop(&mut self) {
        CYCLIC_RESOURCE_DROPS.fetch_add(1, Ordering::SeqCst);
    }
}

#[jsg_resource]
impl CyclicResource {}

/// Tests that resources are dropped when all Rust Refs are dropped and no JS wrapper exists.
///
/// When a resource is allocated but never wrapped for JavaScript, dropping all `Ref` handles
/// should immediately deallocate the resource.
#[test]
fn supports_gc_via_ref_drop() {
    SIMPLE_RESOURCE_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let resource = SimpleResource::alloc(
            lock,
            SimpleResource {
                name: "test".to_owned(),
                callback: None,
            },
        );
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 0);
        std::mem::drop(resource);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 0);
        crate::Harness::request_gc(lock);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
}

/// Tests that resources are dropped via V8 GC weak callback when JS wrapper is collected.
///
/// When a resource is wrapped for JavaScript:
/// 1. Dropping all Rust `Ref` handles makes the V8 Global weak
/// 2. V8 GC can then collect the wrapper and trigger the weak callback
/// 3. The weak callback deallocates the resource
#[test]
fn supports_gc_via_weak_callback() {
    SIMPLE_RESOURCE_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let resource = SimpleResource::alloc(
            lock,
            SimpleResource {
                name: "test".to_owned(),
                callback: None,
            },
        );
        let _wrapped = SimpleResource::wrap(resource.clone(), lock);
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
        let child = SimpleResource::alloc(
            lock,
            SimpleResource {
                name: "child".to_owned(),
                callback: None,
            },
        );
        let optional_child = SimpleResource::alloc(
            lock,
            SimpleResource {
                name: "optional_child".to_owned(),
                callback: None,
            },
        );

        let parent = ParentResource::alloc(
            lock,
            ParentResource {
                child: child.clone(),
                optional_child: Some(optional_child.clone()),
            },
        );

        let _wrapped = ParentResource::wrap(parent.clone(), lock);

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
        let child = SimpleResource::alloc(
            lock,
            SimpleResource {
                name: "child".to_owned(),
                callback: None,
            },
        );

        let parent = ParentResource::alloc(
            lock,
            ParentResource {
                child: child.clone(),
                optional_child: None,
            },
        );

        let _parent_wrapped = ParentResource::wrap(parent.clone(), lock);

        // Child not collected because parent still holds a reference
        std::mem::drop(child);
        crate::Harness::request_gc(lock);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 0);

        std::mem::drop(parent);
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
    harness.run_in_context(|lock, _ctx| {
        let strong = SimpleResource::alloc(
            lock,
            SimpleResource {
                name: "test".to_owned(),
                callback: None,
            },
        );
        let weak = jsg::WeakRef::from(&strong);

        assert!(weak.is_alive());
        let upgraded = weak.upgrade();
        assert!(upgraded.is_some());
        assert!(weak.is_alive());

        std::mem::drop(upgraded);
        assert!(weak.is_alive());

        std::mem::drop(strong);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 0);
        crate::Harness::request_gc(lock);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
}

#[test]
fn cppgc_handle_keeps_resource_alive() {
    SIMPLE_RESOURCE_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let strong = SimpleResource::alloc(
            lock,
            SimpleResource {
                name: "test".to_owned(),
                callback: None,
            },
        );
        let _wrapped = SimpleResource::wrap(strong.clone(), lock);
        std::mem::drop(strong);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 0);
        Ok(())
    });

    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
}

#[test]
fn cppgc_weak_member_cleared_after_gc() {
    SIMPLE_RESOURCE_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let strong = SimpleResource::alloc(
            lock,
            SimpleResource {
                name: "test".to_owned(),
                callback: None,
            },
        );
        let _wrapped = SimpleResource::wrap(strong.clone(), lock);
        let weak = jsg::WeakRef::from(&strong);

        assert!(weak.upgrade().is_some());
        std::mem::drop(strong);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 0);
        Ok(())
    });

    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
}

#[test]
fn cppgc_traced_ref_in_gc() {
    SIMPLE_RESOURCE_DROPS.store(0, Ordering::SeqCst);
    PARENT_RESOURCE_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let child = SimpleResource::alloc(
            lock,
            SimpleResource {
                name: "child".to_owned(),
                callback: None,
            },
        );
        let _child_wrapped = SimpleResource::wrap(child.clone(), lock);

        let parent = ParentResource::alloc(
            lock,
            ParentResource {
                child: child.clone(),
                optional_child: None,
            },
        );
        let _parent_wrapped = ParentResource::wrap(parent.clone(), lock);

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

#[test]
fn circular_references_can_be_collected() {
    CYCLIC_RESOURCE_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let ref_a = CyclicResource::alloc(
            lock,
            CyclicResource {
                name: "A".to_owned(),
                other: RefCell::new(None),
            },
        );
        let ref_b = CyclicResource::alloc(
            lock,
            CyclicResource {
                name: "B".to_owned(),
                other: RefCell::new(None),
            },
        );

        // Create circular reference: A -> B -> A
        *ref_a.other.borrow_mut() = Some(ref_b.clone());
        *ref_b.other.borrow_mut() = Some(ref_a.clone());

        let _wrapped_a = CyclicResource::wrap(ref_a.clone(), lock);
        let _wrapped_b = CyclicResource::wrap(ref_b.clone(), lock);

        std::mem::drop(ref_a);
        std::mem::drop(ref_b);
        assert_eq!(CYCLIC_RESOURCE_DROPS.load(Ordering::SeqCst), 0);
        Ok(())
    });

    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        crate::Harness::request_gc(lock);
        assert_eq!(CYCLIC_RESOURCE_DROPS.load(Ordering::SeqCst), 2);
        Ok(())
    });
}
