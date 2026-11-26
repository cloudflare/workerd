// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! `Local<Object>::set_prototype`, the counterpart of `v8::Object::SetPrototype`.

use jsg::ExceptionType;
use jsg::v8;

fn eval_object<'s>(
    lock: &mut jsg::Lock<'s>,
    ctx: &crate::EvalContext,
    code: &str,
) -> v8::Local<'s, v8::Object> {
    v8::Local::<v8::Object>::try_from(ctx.eval_raw(lock, code).unwrap()).unwrap()
}

#[test]
fn set_prototype_changes_the_prototype() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let object = lock.new_object();
        let parent = lock.new_object();
        object.set_prototype(lock, Some(parent))?;
        ctx.set_global(lock, "object", object);
        ctx.set_global(lock, "parent", parent);
        assert!(
            ctx.eval::<bool>(lock, "Object.getPrototypeOf(object) === parent")
                .unwrap()
        );

        object.set_prototype(lock, None)?;
        assert!(
            ctx.eval::<bool>(lock, "Object.getPrototypeOf(object) === null")
                .unwrap()
        );
        Ok(())
    });
}

// V8 reports each refusal as `Nothing` without an exception; it must reach Rust as a
// `TypeError`, not as an error that claims an exception is pending.
fn assert_refused(result: jsg::Result<()>) {
    let err = result.unwrap_err();
    assert!(!err.is_pending(), "pending: {err}");
    assert_eq!(err.name, ExceptionType::TypeError);
}

#[test]
fn set_prototype_refuses_a_cycle() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let a = lock.new_object();
        let b = lock.new_object();
        a.set_prototype(lock, Some(b))?;
        assert_refused(b.set_prototype(lock, Some(a)));
        // Nothing was left pending, so JavaScript still runs.
        assert!(ctx.eval::<bool>(lock, "true").unwrap());
        Ok(())
    });
}

#[test]
fn set_prototype_refuses_a_non_extensible_object() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let object = eval_object(lock, ctx, "Object.preventExtensions({})");
        let parent = lock.new_object();
        assert_refused(object.set_prototype(lock, Some(parent)));
        Ok(())
    });
}

#[test]
fn set_prototype_refuses_when_a_proxy_trap_throws() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let proxy = eval_object(
            lock,
            ctx,
            "new Proxy({}, { setPrototypeOf() { throw new Error('trap'); } })",
        );
        let parent = lock.new_object();
        assert_refused(proxy.set_prototype(lock, Some(parent)));
        assert!(ctx.eval::<bool>(lock, "true").unwrap());
        Ok(())
    });
}
