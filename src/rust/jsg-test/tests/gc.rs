use std::cell::RefCell;
use std::sync::atomic::Ordering;

use jsg::Resource;
use jsg::Type;

use super::CYCLIC_RESOURCE_DROPS;
use super::CyclicResource;
use super::PARENT_RESOURCE_DROPS;
use super::ParentResource;
use super::SIMPLE_RESOURCE_DROPS;
use super::SimpleResource;

#[test]
fn supports_gc_via_ref_drop() {
    SIMPLE_RESOURCE_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|mut lock| {
        let resource = SimpleResource::alloc(
            &mut lock,
            SimpleResource {
                name: "test".to_owned(),
                callback: None,
            },
        );
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 0);
        std::mem::drop(resource);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 0);
        crate::Harness::request_gc(&mut lock);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 1);
    });
}

#[test]
fn supports_gc_via_weak_callback() {
    SIMPLE_RESOURCE_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|mut lock| {
        let resource = SimpleResource::alloc(
            &mut lock,
            SimpleResource {
                name: "test".to_owned(),
                callback: None,
            },
        );
        let _wrapped = SimpleResource::wrap(resource.clone(), &mut lock);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 0);
        std::mem::drop(resource);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 0);
    });

    harness.run_in_context(|mut lock| {
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 0);
        crate::Harness::request_gc(&mut lock);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 1);
    });
}

#[test]
fn resource_with_traced_ref_field() {
    SIMPLE_RESOURCE_DROPS.store(0, Ordering::SeqCst);
    PARENT_RESOURCE_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|mut lock| {
        let child = SimpleResource::alloc(
            &mut lock,
            SimpleResource {
                name: "child".to_owned(),
                callback: None,
            },
        );
        let optional_child = SimpleResource::alloc(
            &mut lock,
            SimpleResource {
                name: "optional_child".to_owned(),
                callback: None,
            },
        );

        let parent = ParentResource::alloc(
            &mut lock,
            ParentResource {
                child: child.clone(),
                optional_child: Some(optional_child.clone()),
            },
        );

        let _wrapped = ParentResource::wrap(parent.clone(), &mut lock);

        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 0);
        assert_eq!(PARENT_RESOURCE_DROPS.load(Ordering::SeqCst), 0);

        std::mem::drop(parent);
    });

    harness.run_in_context(|mut lock| {
        crate::Harness::request_gc(&mut lock);
        assert_eq!(PARENT_RESOURCE_DROPS.load(Ordering::SeqCst), 1);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 2);
    });
}

#[test]
fn child_traced_ref_kept_alive_by_parent() {
    SIMPLE_RESOURCE_DROPS.store(0, Ordering::SeqCst);
    PARENT_RESOURCE_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|mut lock| {
        let child = SimpleResource::alloc(
            &mut lock,
            SimpleResource {
                name: "child".to_owned(),
                callback: None,
            },
        );

        let parent = ParentResource::alloc(
            &mut lock,
            ParentResource {
                child: child.clone(),
                optional_child: None,
            },
        );

        let _parent_wrapped = ParentResource::wrap(parent.clone(), &mut lock);

        std::mem::drop(child);
        crate::Harness::request_gc(&mut lock);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 0);

        std::mem::drop(parent);
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
        let strong = SimpleResource::alloc(
            &mut lock,
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
        crate::Harness::request_gc(&mut lock);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 1);
    });
}

#[test]
fn cppgc_handle_keeps_resource_alive() {
    SIMPLE_RESOURCE_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|mut lock| {
        let strong = SimpleResource::alloc(
            &mut lock,
            SimpleResource {
                name: "test".to_owned(),
                callback: None,
            },
        );
        let _wrapped = SimpleResource::wrap(strong.clone(), &mut lock);
        std::mem::drop(strong);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 0);
    });

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
        let strong = SimpleResource::alloc(
            &mut lock,
            SimpleResource {
                name: "test".to_owned(),
                callback: None,
            },
        );
        let _wrapped = SimpleResource::wrap(strong.clone(), &mut lock);
        let weak = jsg::WeakRef::from(&strong);

        assert!(weak.upgrade().is_some());
        std::mem::drop(strong);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 0);
    });

    harness.run_in_context(|mut lock| {
        crate::Harness::request_gc(&mut lock);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 1);
    });
}

#[test]
fn cppgc_traced_ref_in_gc() {
    SIMPLE_RESOURCE_DROPS.store(0, Ordering::SeqCst);
    PARENT_RESOURCE_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|mut lock| {
        let child = SimpleResource::alloc(
            &mut lock,
            SimpleResource {
                name: "child".to_owned(),
                callback: None,
            },
        );
        let _child_wrapped = SimpleResource::wrap(child.clone(), &mut lock);

        let parent = ParentResource::alloc(
            &mut lock,
            ParentResource {
                child: child.clone(),
                optional_child: None,
            },
        );
        let _parent_wrapped = ParentResource::wrap(parent.clone(), &mut lock);

        std::mem::drop(child);
        std::mem::drop(parent);

        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 0);
        assert_eq!(PARENT_RESOURCE_DROPS.load(Ordering::SeqCst), 0);
    });

    harness.run_in_context(|mut lock| {
        crate::Harness::request_gc(&mut lock);
        assert_eq!(PARENT_RESOURCE_DROPS.load(Ordering::SeqCst), 1);
        crate::Harness::request_gc(&mut lock);
        assert_eq!(SIMPLE_RESOURCE_DROPS.load(Ordering::SeqCst), 1);
    });
}

#[test]
fn circular_references_can_be_collected() {
    CYCLIC_RESOURCE_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|mut lock| {
        let ref_a = CyclicResource::alloc(
            &mut lock,
            CyclicResource {
                name: "A".to_owned(),
                other: RefCell::new(None),
            },
        );
        let ref_b = CyclicResource::alloc(
            &mut lock,
            CyclicResource {
                name: "B".to_owned(),
                other: RefCell::new(None),
            },
        );

        // Create circular reference: A -> B -> A
        *ref_a.other.borrow_mut() = Some(ref_b.clone());
        *ref_b.other.borrow_mut() = Some(ref_a.clone());

        let _wrapped_a = CyclicResource::wrap(ref_a.clone(), &mut lock);
        let _wrapped_b = CyclicResource::wrap(ref_b.clone(), &mut lock);

        std::mem::drop(ref_a);
        std::mem::drop(ref_b);
        assert_eq!(CYCLIC_RESOURCE_DROPS.load(Ordering::SeqCst), 0);
    });

    harness.run_in_context(|mut lock| {
        crate::Harness::request_gc(&mut lock);
        crate::Harness::request_gc(&mut lock);
        assert_eq!(CYCLIC_RESOURCE_DROPS.load(Ordering::SeqCst), 2);
    });
}
