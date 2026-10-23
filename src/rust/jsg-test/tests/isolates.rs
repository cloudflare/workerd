// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Tests that keep handles of one isolate out of another.

#[cfg(not(debug_assertions))]
use std::cell::Cell;
use std::panic::AssertUnwindSafe;

use jsg::FromJS;
use jsg::ToJS;
use jsg::v8;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;

fn panic_message(payload: &(dyn std::any::Any + Send)) -> &str {
    payload
        .downcast_ref::<&str>()
        .copied()
        .or_else(|| payload.downcast_ref::<String>().map(String::as_str))
        .unwrap_or("<non-string panic payload>")
}

#[test]
fn lock_derefs_to_its_isolate() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let ptr = lock.isolate().as_ffi();
        let shared: &jsg::Isolate = lock;
        assert_eq!(shared.as_ffi(), ptr);
        let isolate: &mut jsg::Isolate = lock;
        assert_eq!(isolate.as_ffi(), ptr);
        // SAFETY: the lock's isolate is locked by this thread, and neither `lock` nor `isolate`
        // is used while `raw` lives.
        let raw = unsafe { jsg::Isolate::from_raw(ptr) };
        assert_eq!(raw.as_ffi(), ptr);
        Ok(())
    });
}

#[test]
fn scope_of_another_isolate_panics() {
    let a = crate::Harness::new();
    let b = crate::Harness::new();
    a.run_in_context(|_lock_a, _ctx| {
        let result = std::panic::catch_unwind(AssertUnwindSafe(|| {
            b.run_in_context(|_lock_b, _ctx| Ok(()));
        }));
        let payload = result.expect_err("entering B while A's scope is open must panic");
        assert!(
            panic_message(&*payload).contains("a scope of another isolate is open"),
            "unexpected panic: {}",
            panic_message(&*payload)
        );
        Ok(())
    });
    // Both isolates are usable once the other's scopes are closed.
    b.run_in_context(|lock, _ctx| {
        let s = "b".to_js(lock);
        assert_eq!(String::from_js(lock, s)?, "b");
        Ok(())
    });
}

#[test]
fn scope_of_another_isolate_is_allowed_when_set_aside() {
    let a = crate::Harness::new();
    let b = crate::Harness::new();
    a.run_in_context(|lock_a, _ctx| {
        // SAFETY: no handle crosses between the isolates.
        unsafe {
            jsg::allow_scopes_of_another_isolate(|| {
                b.run_in_context(|lock_b, _ctx| {
                    let s = "b".to_js(lock_b);
                    assert_eq!(String::from_js(lock_b, s)?, "b");
                    Ok(())
                });
            });
        }
        // A's scope counts as open again, so B's scopes are refused again.
        let result = std::panic::catch_unwind(AssertUnwindSafe(|| {
            b.run_in_context(|_lock_b, _ctx| Ok(()));
        }));
        assert!(
            result.is_err(),
            "entering B after the escape hatch returned must panic"
        );
        let s = "a".to_js(lock_a);
        assert_eq!(String::from_js(lock_a, s)?, "a");
        Ok(())
    });
}

#[test]
fn set_aside_scopes_count_as_open_again_after_a_panic() {
    let a = crate::Harness::new();
    let b = crate::Harness::new();
    a.run_in_context(|lock_a, _ctx| {
        let unwind = std::panic::catch_unwind(AssertUnwindSafe(|| {
            // SAFETY: no handle crosses between the isolates.
            unsafe {
                jsg::allow_scopes_of_another_isolate(|| panic!("intentional panic"));
            }
        }));
        assert!(unwind.is_err());
        // A's scope counts as open again, so B's scopes are refused.
        let result = std::panic::catch_unwind(AssertUnwindSafe(|| {
            b.run_in_context(|_lock_b, _ctx| Ok(()));
        }));
        assert!(
            result.is_err(),
            "entering B after the escape hatch panicked must panic"
        );
        let s = "a".to_js(lock_a);
        assert_eq!(String::from_js(lock_a, s)?, "a");
        Ok(())
    });
}

#[test]
fn global_of_another_isolate_panics() {
    let a = crate::Harness::new();
    let b = crate::Harness::new();
    let mut global: Option<v8::Global<v8::Value>> = None;
    a.run_in_context(|lock, _ctx| {
        let value = "from a".to_js(lock);
        global = Some(v8::Global::new(lock, value));
        Ok(())
    });
    let global = global.expect("created in A");

    let result = std::panic::catch_unwind(AssertUnwindSafe(|| {
        b.run_in_context(|lock, _ctx| {
            let _local = global.as_local(lock);
            Ok(())
        });
    }));
    let payload = result.expect_err("opening A's Global in B must panic");
    assert_eq!(
        panic_message(&*payload),
        "attempt to use a Global in an isolate other than the one it was created in"
    );

    let result = std::panic::catch_unwind(AssertUnwindSafe(|| {
        b.run_in_context(|lock, _ctx| {
            let _copy = global.clone(lock);
            Ok(())
        });
    }));
    assert!(result.is_err(), "cloning A's Global in B must panic");

    // The Global still works in its own isolate, and is dropped there.
    a.run_in_context(move |lock, _ctx| {
        let local = global.as_local(lock);
        assert_eq!(String::from_js(lock, local)?, "from a");
        drop(global);
        Ok(())
    });
}

#[test]
fn callback_entered_while_another_isolate_has_scopes_throws() {
    let a = crate::Harness::new();
    let b = crate::Harness::new();
    b.run_in_context(|lock_b, ctx_b| {
        let thing = jsg::Rc::new(Thing).to_js(lock_b);
        ctx_b.set_global(lock_b, "thing", thing);
        let mut result = None;
        // SAFETY: B's code runs while A's scope is open, but no handle crosses between them.
        unsafe {
            jsg::allow_scopes_of_another_isolate(|| {
                a.run_in_context(|_lock_a, _ctx| {
                    // The callback of B cannot open its scope, and must throw instead of
                    // unwinding into V8.
                    result = Some(ctx_b.eval_raw(lock_b, "thing.ping()"));
                    Ok(())
                });
            });
        }
        let err = result
            .expect("ran in A")
            .expect_err("calling into B while A's scope is open must throw")
            .unwrap_jsg_err(lock_b);
        assert!(
            err.message.contains("internal error"),
            "unexpected error: {:?}",
            err.message
        );
        Ok(())
    });
}

#[jsg_resource]
struct Thing;

#[jsg_resource]
impl Thing {
    #[jsg_method]
    pub fn ping(&self) {}
}

#[test]
fn resource_wrapped_in_another_isolate_panics() {
    let a = crate::Harness::new();
    let b = crate::Harness::new();
    let thing = jsg::Rc::new(Thing);
    let mut wrapper: Option<v8::Global<v8::Value>> = None;
    a.run_in_context(|lock, _ctx| {
        let value = thing.clone().to_js(lock);
        // Keep the wrapper alive so it is not collected before B tries to reuse it.
        wrapper = Some(v8::Global::new(lock, value));
        Ok(())
    });

    let result = std::panic::catch_unwind(AssertUnwindSafe(|| {
        b.run_in_context(|lock, _ctx| {
            let _value = thing.clone().to_js(lock);
            Ok(())
        });
    }));
    let payload = result.expect_err("wrapping a resource of A in B must panic");
    assert_eq!(
        panic_message(&*payload),
        "attempt to wrap a resource in an isolate other than the one it was wrapped in"
    );

    a.run_in_context(move |lock, _ctx| {
        // Wrapping it again in A returns the existing wrapper.
        let value = thing.to_js(lock);
        let existing = wrapper.take().expect("set above");
        assert_eq!(value, existing.as_local(lock));
        Ok(())
    });
}

#[cfg(not(debug_assertions))]
#[jsg_resource]
struct GlobalHolder {
    handle: Cell<Option<v8::Global<v8::Value>>>,
}

#[cfg(not(debug_assertions))]
#[jsg_resource]
impl GlobalHolder {}

/// GC of one isolate leaves alone a `Global` of another that a resource of the first holds.
/// Visiting it would make its strong handle weak, and leave the value to a traced node in the
/// wrong isolate. Debug builds assert instead, in a visit made from C++, where a panic aborts.
#[test]
#[cfg(not(debug_assertions))]
fn gc_of_another_isolate_leaves_a_global_alone() {
    let a = crate::Harness::new();
    let b = crate::Harness::new();
    let mut global: Option<v8::Global<v8::Value>> = None;
    a.run_in_context(|lock, _ctx| {
        // The `Global` is the object's only handle once the scope closes.
        let object: v8::Local<'_, v8::Value> = lock.new_object().into();
        global = Some(object.to_global(lock));
        Ok(())
    });
    let holder = jsg::Rc::new(GlobalHolder {
        handle: Cell::new(global),
    });

    b.run_in_context(|lock, ctx| {
        // Wrapping the holder in B visits the `Global`, and so does every GC of B.
        ctx.set_global(lock, "holder", holder.clone());
        crate::Harness::request_gc(lock);
        Ok(())
    });

    a.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        let global = holder.handle.take().expect("set above");
        assert!(global.as_local(lock).is_object());
        drop(global);
        Ok(())
    });
}
