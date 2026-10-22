// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Tests for dropping a `Global` after its isolate is gone.

use std::cell::Cell;
use std::sync::atomic::AtomicUsize;
use std::sync::atomic::Ordering;

use jsg::ToJS;
use jsg::v8;
use jsg_macros::jsg_resource;

/// A resource whose only job is to own a `Global`, so that dropping the resource drops the
/// handle.
#[jsg_resource]
struct HandleHolder {
    handle: Cell<Option<v8::Global<v8::Value>>>,
}

#[jsg_resource]
impl HandleHolder {}

static HANDLE_HOLDER_DROPS: AtomicUsize = AtomicUsize::new(0);

impl Drop for HandleHolder {
    fn drop(&mut self) {
        HANDLE_HOLDER_DROPS.fetch_add(1, Ordering::SeqCst);
    }
}

/// A `Global` is `'static`, so it can outlive the harness, and with it the isolate. Dropping it
/// then must leave the handle alone: `v8::Isolate::Dispose` freed it with the isolate.
#[test]
fn global_dropped_after_its_isolate_is_disposed() {
    let harness = crate::Harness::new();
    let mut global: Option<v8::Global<v8::Value>> = None;
    harness.run_in_context(|lock, _ctx| {
        global = Some("escaped".to_js(lock).to_global(lock));
        Ok(())
    });
    drop(harness);
    drop(global.expect("created in the context"));
}

/// A resource that has a JavaScript wrapper and outlives its isolate is destroyed at once when
/// its last `Rc` goes, since the isolate detached its wrapper at teardown. The `Global` it owns
/// is dropped with it, after the isolate was disposed.
#[test]
fn wrapped_resource_dropped_after_its_isolate_is_disposed() {
    HANDLE_HOLDER_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    let mut holder: Option<jsg::Rc<HandleHolder>> = None;
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Rc::new(HandleHolder {
            handle: Cell::new(None),
        });
        resource
            .handle
            .set(Some("held".to_js(lock).to_global(lock)));
        // Wrapping the resource ties its `Wrappable` to the isolate, which the isolate undoes
        // when it is torn down.
        ctx.set_global(lock, "holder", resource.clone());
        holder = Some(resource);
        Ok(())
    });
    drop(harness);
    assert_eq!(HANDLE_HOLDER_DROPS.load(Ordering::SeqCst), 0);

    drop(holder.expect("created in the context"));
    assert_eq!(HANDLE_HOLDER_DROPS.load(Ordering::SeqCst), 1);
}
