// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Tests for `Global`: its layout, dropping it under the isolate lock, without it, on
//! another thread or after its isolate is gone, and turning it into a `TracedGlobal`.

use std::cell::Cell;
use std::panic::AssertUnwindSafe;
use std::sync::atomic::AtomicUsize;
use std::sync::atomic::Ordering;

use jsg::FromJS;
use jsg::ToJS;
use jsg::v8;
use jsg_macros::jsg_resource;

/// A resource whose drops the tests count.
#[jsg_resource]
struct GlobalCounted;

#[jsg_resource]
impl GlobalCounted {}

static GLOBAL_COUNTED_DROPS: AtomicUsize = AtomicUsize::new(0);

impl Drop for GlobalCounted {
    fn drop(&mut self) {
        GLOBAL_COUNTED_DROPS.fetch_add(1, Ordering::SeqCst);
    }
}

/// A resource that owns a `TracedGlobal`, which GC traces.
#[jsg_resource]
struct TracedHolder {
    handle: Cell<Option<v8::TracedGlobal<v8::Value>>>,
}

#[jsg_resource]
impl TracedHolder {}

static TRACED_HOLDER_DROPS: AtomicUsize = AtomicUsize::new(0);

impl Drop for TracedHolder {
    fn drop(&mut self) {
        TRACED_HOLDER_DROPS.fetch_add(1, Ordering::SeqCst);
    }
}

/// A strong handle to the wrapper of a new `GlobalCounted`. Once the lock's scope closes, only
/// the handle keeps the wrapper, and with it the resource, alive.
fn counted_wrapper(lock: &mut jsg::Lock<'_>) -> v8::Global<v8::Value> {
    let wrapper = jsg::Rc::new(GlobalCounted).to_js(lock);
    v8::Global::new(lock, wrapper)
}

/// A `Global` is two words: its handle, inline, and the `Arc` of its isolate's state.
/// (A `TracedGlobal` is two words as well, with a box in place of the handle.)
#[test]
fn global_is_two_words() {
    let two_words = 2 * std::mem::size_of::<usize>();
    assert_eq!(std::mem::size_of::<v8::Global<v8::Value>>(), two_words);
    assert_eq!(
        std::mem::size_of::<Option<v8::Global<v8::Value>>>(),
        two_words
    );
}

/// Under the isolate lock, a drop resets the handle at once.
#[test]
fn global_dropped_under_the_lock_is_reset_at_once() {
    GLOBAL_COUNTED_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    let mut wrapper = None;
    harness.run_in_context(|lock, _ctx| {
        wrapper = Some(counted_wrapper(lock));
        Ok(())
    });
    let wrapper = wrapper.expect("created in the context");

    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(
            GLOBAL_COUNTED_DROPS.load(Ordering::SeqCst),
            0,
            "the Global roots the wrapper"
        );
        let with_wrapper = crate::Harness::used_global_handles_size(lock);

        drop(wrapper);
        assert_eq!(lock.globals_awaiting_lock(), 0);
        assert!(crate::Harness::used_global_handles_size(lock) < with_wrapper);
        crate::Harness::request_gc(lock);
        assert_eq!(GLOBAL_COUNTED_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
}

/// Without the isolate lock, a drop queues the handle, which keeps its value alive until the
/// next lock resets it.
#[test]
fn global_dropped_without_the_lock_is_reset_at_the_next_lock() {
    GLOBAL_COUNTED_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    let mut baseline = 0;
    let mut wrapper = None;
    harness.run_in_context(|lock, _ctx| {
        // Created ahead of the measurements below, so that they only see the wrapper's handle.
        jsg::resource::function_template_of::<GlobalCounted>(lock);
        baseline = crate::Harness::used_global_handles_size(lock);
        wrapper = Some(counted_wrapper(lock));
        Ok(())
    });
    let wrapper = wrapper.expect("created in the context");

    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        let with_wrapper = crate::Harness::used_global_handles_size(lock);
        assert!(with_wrapper > baseline);

        crate::Harness::with_isolate_unlocked(lock, || drop(wrapper));
        assert_eq!(lock.globals_awaiting_lock(), 1);

        crate::Harness::request_gc(lock);
        assert_eq!(
            GLOBAL_COUNTED_DROPS.load(Ordering::SeqCst),
            0,
            "the queued handle still roots the wrapper"
        );
        assert_eq!(crate::Harness::used_global_handles_size(lock), with_wrapper);
        Ok(())
    });

    harness.run_in_context(|lock, _ctx| {
        assert_eq!(lock.globals_awaiting_lock(), 0);
        crate::Harness::request_gc(lock);
        assert_eq!(GLOBAL_COUNTED_DROPS.load(Ordering::SeqCst), 1);
        assert_eq!(crate::Harness::used_global_handles_size(lock), baseline);
        Ok(())
    });
}

/// A `Global` is `Send`: another thread may drop it while this one holds the isolate lock
/// and creates handles. The drop is queued, and the next lock resets it.
#[test]
fn global_dropped_on_another_thread_is_reset_at_the_next_lock() {
    GLOBAL_COUNTED_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    let mut wrapper = None;
    harness.run_in_context(|lock, _ctx| {
        wrapper = Some(counted_wrapper(lock));
        Ok(())
    });
    let wrapper = wrapper.expect("created in the context");

    harness.run_in_context(|lock, _ctx| {
        let barrier = std::sync::Arc::new(std::sync::Barrier::new(2));
        let dropper = std::thread::spawn({
            let barrier = std::sync::Arc::clone(&barrier);
            move || {
                barrier.wait();
                drop(wrapper);
            }
        });
        barrier.wait();
        // Meanwhile this thread, which holds the lock, keeps creating and dropping handles.
        for _ in 0..1000 {
            let object = lock.new_object();
            drop(v8::Global::new(lock, object));
        }
        dropper.join().expect("the drop does not panic");
        assert_eq!(lock.globals_awaiting_lock(), 1);
        crate::Harness::request_gc(lock);
        assert_eq!(GLOBAL_COUNTED_DROPS.load(Ordering::SeqCst), 0);
        Ok(())
    });

    harness.run_in_context(|lock, _ctx| {
        assert_eq!(lock.globals_awaiting_lock(), 0);
        crate::Harness::request_gc(lock);
        assert_eq!(GLOBAL_COUNTED_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
}

/// A `Global` is `'static`, so it can outlive the harness, and with it the isolate.
/// Dropping it then must leave the handle alone: `v8::Isolate::Dispose` freed it with the
/// isolate, and a reset would touch freed memory, which a memory checker reports.
#[test]
fn global_dropped_after_its_isolate_is_disposed() {
    let harness = crate::Harness::new();
    let mut global = None;
    harness.run_in_context(|lock, _ctx| {
        let value = "escaped".to_js(lock);
        global = Some(v8::Global::new(lock, value));
        Ok(())
    });
    drop(harness);
    drop(global.expect("created in the context"));
}

/// Once the `Realm` is dropped a `Global`'s handle is left to `Dispose`, while GC can
/// still run; the drop must neither reset the handle nor keep anything for V8 to write into.
#[test]
fn global_dropped_after_its_realm_is_left_to_dispose() {
    let harness = crate::Harness::new();
    let mut global = None;
    harness.run_in_context(|lock, _ctx| {
        let value = lock.new_object();
        global = Some(v8::Global::new(lock, value));
        Ok(())
    });
    let global = global.expect("created in the context");
    harness.drop_realm_then_gc(|| drop(global));
}

/// Opening or cloning a `Global` in another isolate panics; it still works in its own.
#[test]
fn global_of_another_isolate_panics() {
    let a = crate::Harness::new();
    let b = crate::Harness::new();
    let mut global = None;
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
    assert!(result.is_err(), "opening A's Global in B must panic");
    let result = std::panic::catch_unwind(AssertUnwindSafe(|| {
        b.run_in_context(|lock, _ctx| {
            let _copy = global.clone(lock);
            Ok(())
        });
    }));
    assert!(result.is_err(), "cloning A's Global in B must panic");

    a.run_in_context(move |lock, _ctx| {
        let copy = global.clone(lock);
        drop(global);
        let local = copy.as_local(lock);
        assert_eq!(String::from_js(lock, local)?, "from a");
        Ok(())
    });
}

/// A `Global` turned into a `TracedGlobal` can be traced: a resource that holds its own wrapper
/// through it is collected once nothing else refers to it.
#[test]
fn global_into_traced_global_is_traced() {
    TRACED_HOLDER_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let resource = jsg::Rc::new(TracedHolder {
            handle: Cell::new(None),
        });
        let wrapper = resource.clone().to_js(lock);
        let global = v8::Global::new(lock, wrapper);
        let traced = v8::TracedGlobal::from(global);
        assert!(traced.as_local(lock).is_object());
        resource.handle.set(Some(traced));
        drop(resource);
        Ok(())
    });

    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(
            TRACED_HOLDER_DROPS.load(Ordering::SeqCst),
            1,
            "the cycle through the TracedGlobal is collected"
        );
        Ok(())
    });
}
