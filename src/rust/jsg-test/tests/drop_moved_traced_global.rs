// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! A `TracedGlobal` that a resource's `Drop` moves out of a field the garbage collector has put
//! in traced mode.

use std::sync::Mutex;

use jsg::v8;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;

static STASH: Mutex<Vec<v8::TracedGlobal<v8::Value>>> = Mutex::new(Vec::new());

/// Held by each test, since they share `STASH`.
static SERIAL: Mutex<()> = Mutex::new(());

#[jsg_resource]
struct StashesGlobalOnDrop {
    handle: Option<v8::TracedGlobal<v8::Value>>,
}

impl Drop for StashesGlobalOnDrop {
    fn drop(&mut self) {
        if let Some(handle) = self.handle.take() {
            STASH.lock().unwrap().push(handle);
        }
    }
}

#[jsg_resource]
impl StashesGlobalOnDrop {}

#[jsg_resource]
struct Maker;

#[jsg_resource]
impl Maker {
    /// Returns a new `StashesGlobalOnDrop` that holds `value`.
    #[jsg_method]
    fn make<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        value: v8::Local<'s, v8::Value>,
    ) -> jsg::Rc<StashesGlobalOnDrop> {
        let handle = Some(value.to_traced_global(lock));
        jsg::Rc::new(lock, StashesGlobalOnDrop { handle })
    }
}

/// Sets `globalThis.maker` to a new `Maker`.
fn set_maker(lock: &mut jsg::Lock<'_>, ctx: &crate::EvalContext<'_>) {
    let maker = jsg::Rc::new(lock, Maker);
    ctx.set_global(lock, "maker", maker);
}

/// Wraps a `StashesGlobalOnDrop` that holds the value `value_expr` evaluates to, lets GC put its
/// `TracedGlobal` in traced mode, then lets GC sweep its wrapper, so that its `Drop` stashes the
/// handle. Returns the stashed handle.
fn stash_from_swept_resource(
    lock: &mut jsg::Lock<'_>,
    ctx: &crate::EvalContext<'_>,
    value_expr: &str,
) -> v8::TracedGlobal<v8::Value> {
    ctx.eval_raw(
        lock,
        &format!("globalThis.r = maker.make({value_expr}); undefined"),
    )
    .expect("should evaluate");
    crate::Harness::request_gc(lock);
    ctx.eval_raw(lock, "globalThis.r = undefined; undefined")
        .expect("should evaluate");
    crate::Harness::request_gc(lock);
    let mut stash = STASH.lock().unwrap();
    assert_eq!(stash.len(), 1, "Drop stashed the handle");
    stash.pop().unwrap()
}

/// The value outlives the resource, held by a strong `Global`. The `TracedGlobal` that `Drop`
/// moves out is strong, so it keeps the value alive once that `Global` lets go.
#[test]
fn traced_global_moved_out_in_drop_keeps_its_value() {
    let _serial = SERIAL
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner);
    let harness = crate::Harness::new();
    let mut keeper: Option<v8::Global<v8::Value>> = None;
    let mut stashed: Option<v8::TracedGlobal<v8::Value>> = None;
    harness.run_in_context(|lock, ctx| {
        let value = ctx
            .eval_raw(lock, "globalThis.value = {}")
            .expect("should evaluate");
        keeper = Some(v8::Global::new(lock, value));
        set_maker(lock, ctx);
        stashed = Some(stash_from_swept_resource(lock, ctx, "globalThis.value"));
        Ok(())
    });
    // Each `run_in_context` has its own context, so only `keeper` and the stashed handle hold
    // the value from here on, and no `Local` does.
    drop(keeper);
    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        crate::Harness::request_gc(lock);
        Ok(())
    });
    harness.run_in_context(|lock, _ctx| {
        let stashed = stashed.take().expect("stashed above");
        assert!(stashed.as_local(lock).is_object());
        Ok(())
    });
}

/// The value dies with the resource, in the GC that sweeps it. The `TracedGlobal` that `Drop`
/// moves out is then empty, and dropping it touches no handle.
#[test]
fn traced_global_whose_value_died_with_the_resource_drops_cleanly() {
    let _serial = SERIAL
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner);
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        set_maker(lock, ctx);
        // The first round also creates the resources' templates, which the realm keeps.
        drop(stash_from_swept_resource(lock, ctx, "{}"));
        crate::Harness::request_gc(lock);
        let baseline = crate::Harness::used_global_handles_size(lock);

        // V8 reset the handle when its value died, so the drop has nothing left to release.
        drop(stash_from_swept_resource(lock, ctx, "{}"));
        crate::Harness::request_gc(lock);
        assert_eq!(crate::Harness::used_global_handles_size(lock), baseline);
        Ok(())
    });
}

/// The value is held only by the resource, and a scavenge drops the resource's young wrapper.
/// The `TracedGlobal` that `Drop` moves out still reaches the value after later full GCs.
#[test]
fn traced_global_moved_out_after_a_scavenge_keeps_its_value() {
    let _serial = SERIAL
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner);
    let harness = crate::Harness::new();
    let mut stashed: Option<v8::TracedGlobal<v8::Value>> = None;
    harness.run_in_context(|lock, ctx| {
        set_maker(lock, ctx);
        ctx.eval_raw(lock, "maker.make({}); undefined")
            .expect("should evaluate");
        crate::Harness::request_minor_gc(lock);
        stashed = STASH.lock().unwrap().pop();
        Ok(())
    });
    assert!(stashed.is_some(), "the scavenge dropped the wrapper");
    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        crate::Harness::request_gc(lock);
        Ok(())
    });
    harness.run_in_context(|lock, _ctx| {
        let stashed = stashed.take().expect("checked above");
        assert!(stashed.as_local(lock).is_object());
        Ok(())
    });
}
