// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::cell::Cell;

use jsg::ToJS;
use jsg_macros::jsg_resource;

thread_local! {
    // Tests run on threads of their own, and a resource is dropped on the thread that holds its
    // isolate's lock.
    static PANIC_DROPS: Cell<usize> = const { Cell::new(0) };
}

#[jsg_resource]
struct PanicsOnDrop {}

impl Drop for PanicsOnDrop {
    fn drop(&mut self) {
        PANIC_DROPS.set(PANIC_DROPS.get() + 1);
        panic!("resource Drop panicked");
    }
}

#[jsg_resource]
impl PanicsOnDrop {}

/// A panic in a resource's `Drop` while the garbage collector sweeps it is logged and marks the
/// isolate to be aborted, and the process keeps running.
#[test]
fn panic_in_drop_during_gc_sweep() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let resource = jsg::Rc::new(lock, PanicsOnDrop {});
        let _wrapper = resource.to_js(lock)?;
        Ok(())
    });
    assert_eq!(PANIC_DROPS.get(), 0);
    harness.run_in_context(|lock, _ctx| {
        assert!(!crate::Harness::resource_drop_panicked(lock));
        crate::Harness::request_gc(lock);
        assert!(crate::Harness::resource_drop_panicked(lock));
        Ok(())
    });
    assert_eq!(PANIC_DROPS.get(), 1);
}

/// A resource that was never wrapped is dropped with its last `Rc`, and marks its isolate.
#[test]
fn panic_in_drop_of_unwrapped_resource() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        drop(jsg::Rc::new(lock, PanicsOnDrop {}));
        assert_eq!(PANIC_DROPS.get(), 1);
        assert!(crate::Harness::resource_drop_panicked(lock));
        Ok(())
    });
}

/// An unwrapped resource of isolate A dropped while isolate B is entered marks A, not B.
#[test]
fn panic_in_drop_marks_the_resource_isolate_not_the_entered_one() {
    let a = crate::Harness::new();
    let b = crate::Harness::new();
    a.run_in_context(|lock_a, _ctx| {
        let mut resource = Some(jsg::Rc::new(lock_a, PanicsOnDrop {}));
        // SAFETY: no handle crosses between the isolates.
        unsafe {
            jsg::allow_scopes_of_another_isolate(|| {
                b.run_in_context(|lock_b, _ctx| {
                    drop(resource.take());
                    assert_eq!(PANIC_DROPS.get(), 1);
                    assert!(!crate::Harness::resource_drop_panicked(lock_b));
                    Ok(())
                });
            });
        }
        assert!(crate::Harness::resource_drop_panicked(lock_a));
        Ok(())
    });
}

/// An unwrapped resource dropped after its isolate is disposed marks no isolate, including one
/// entered at the time.
#[test]
fn panic_in_drop_after_dispose_marks_no_isolate() {
    // A harness keeps its isolate entered while it lives, so the isolates are disposed in the
    // reverse order of their creation.
    let b = crate::Harness::new();
    let a = crate::Harness::new();
    let mut resource = None;
    a.run_in_context(|lock, _ctx| {
        resource = Some(jsg::Rc::new(lock, PanicsOnDrop {}));
        Ok(())
    });
    drop(a);
    b.run_in_context(|lock, _ctx| {
        drop(resource.take());
        assert_eq!(PANIC_DROPS.get(), 1);
        assert!(!crate::Harness::resource_drop_panicked(lock));
        Ok(())
    });
}
