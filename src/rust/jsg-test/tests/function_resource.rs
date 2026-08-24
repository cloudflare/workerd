// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Tests for `jsg::Function` held in resources: the `#[jsg_method]` parameter
//! pipeline, GC tracing of stored callbacks, and cycle collection.

use std::cell::Cell;
use std::sync::atomic::AtomicUsize;
use std::sync::atomic::Ordering;

use jsg::Number;
use jsg::ToJS;
use jsg::v8;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;

static HOLDER_DROPS: AtomicUsize = AtomicUsize::new(0);

#[jsg_resource]
struct CallbackHolder {
    pub callback: Cell<Option<jsg::Function<(), Number>>>,
}

impl Drop for CallbackHolder {
    fn drop(&mut self) {
        HOLDER_DROPS.fetch_add(1, Ordering::SeqCst);
    }
}

#[jsg_resource]
impl CallbackHolder {
    #[jsg_method]
    fn set_callback(&self, callback: jsg::Function<(), Number>) {
        self.callback.set(Some(callback));
    }

    #[jsg_method]
    fn invoke(&self, lock: &mut jsg::Lock) -> Result<Number, jsg::Error> {
        let callback = self
            .callback
            .take()
            .ok_or_else(|| jsg::Error::new_error("no callback set"))?;
        let result = callback.call(lock, ());
        self.callback.set(Some(callback));
        result
    }
}

/// `Function` as a `#[jsg_method]` parameter: JS registers a callback through
/// the macro pipeline and Rust invokes it.
#[test]
fn function_as_method_parameter() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let holder = jsg::Rc::new(CallbackHolder {
            callback: Cell::new(None),
        });
        let wrapped = holder.to_js(lock);
        ctx.set_global("holder", wrapped);

        ctx.eval_raw("holder.setCallback(() => 42)").unwrap();
        let result: Number = ctx.eval(lock, "holder.invoke()").unwrap();
        assert!((result.value() - 42.0).abs() < f64::EPSILON);
        Ok(())
    });
}

/// A stored callback survives a full GC while the resource is only reachable
/// from JS (traced mode): if `Traced` tracing of the inner `Global` were
/// broken, the traced handle would dangle and this would crash or fail.
#[test]
fn stored_callback_survives_gc_in_traced_mode() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let holder = jsg::Rc::new(CallbackHolder {
            callback: Cell::new(None),
        });
        let wrapped = holder.clone().to_js(lock);
        ctx.set_global("holder", wrapped);
        ctx.eval_raw("holder.setCallback(() => 7)").unwrap();

        // Drop the strong Rust ref so the resource (and its stored Global)
        // downgrades to traced mode, then force a full GC.
        std::mem::drop(holder);
        crate::Harness::request_gc(lock);

        let result: Number = ctx.eval(lock, "holder.invoke()").unwrap();
        assert!((result.value() - 7.0).abs() < f64::EPSILON);
        Ok(())
    });
}

/// A cycle between a resource and its stored callback (the JS closure captures
/// the resource's own wrapper) is collected once nothing else references it.
#[test]
fn callback_cycle_collected() {
    HOLDER_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let holder = jsg::Rc::new(CallbackHolder {
            callback: Cell::new(None),
        });
        let wrapped = holder.clone().to_js(lock);

        // Build the cycle wrapper -> Function -> closure -> wrapper without
        // leaving any global variable referencing the holder.
        let setter = ctx
            .eval_raw("(h => { h.setCallback(() => { h; return 1; }); })")
            .unwrap();
        setter.try_as::<v8::Function>().unwrap().call::<(), _>(
            lock,
            None::<v8::Local<'_, v8::Value>>,
            &[wrapped],
        )?;

        std::mem::drop(holder);
        crate::Harness::request_gc(lock);
        // Still alive: the Local from to_js pins the wrapper for this scope.
        assert_eq!(HOLDER_DROPS.load(Ordering::SeqCst), 0);
        Ok(())
    });

    // New context: the old scope's Locals are gone; only the cycle remains.
    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(HOLDER_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
}
