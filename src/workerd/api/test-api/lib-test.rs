// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use jsg_test::EvalContext;
use jsg_test::Harness;

use super::*;

/// Evaluates `code`, a handler, then runs it with the globals `receiver`, `env` and `ctx`, which
/// it defines, and catches what it throws.
fn run_code<'s>(
    lock: &mut jsg::Lock<'s>,
    ctx: &EvalContext<'_>,
    code: &str,
) -> Result<v8::Local<'s, v8::Promise>, v8::Local<'s, v8::Value>> {
    let handler = ctx.eval_raw(lock, code).unwrap();
    let handler = v8::Local::<v8::Function>::try_from(handler).unwrap();
    let [receiver, env, context] = ["receiver", "env", "ctx"].map(|name| {
        ctx.eval_raw(lock, &format!("globalThis.{name} ??= {{ name: '{name}' }}"))
            .unwrap()
    });
    lock.try_catch(|lock| run(lock, handler, receiver, env, context))
        .unwrap()
}

fn check(lock: &mut jsg::Lock<'_>, ctx: &EvalContext<'_>, condition: &str) {
    assert!(ctx.eval::<bool>(lock, condition).unwrap(), "{condition}");
}

/// A handler that records its calls in the global `calls`.
const RECORD: &str = "(function (c, e, x) {
    (globalThis.calls ??= []).push({ self: this, c, e, x, n: arguments.length });
})";

#[test]
fn calls_the_handler_on_the_receiver_with_a_new_controller() {
    Harness::new().run_in_context(|lock, ctx| {
        run_code(lock, ctx, RECORD).unwrap();
        run_code(lock, ctx, RECORD).unwrap();
        check(lock, ctx, "calls.length === 2");
        for call in ["calls[0]", "calls[1]"] {
            check(
                lock,
                ctx,
                &format!(
                    "{call}.self === receiver && {call}.e === env && {call}.x === ctx && \
                     {call}.n === 3"
                ),
            );
        }
        check(lock, ctx, "calls[0].c !== calls[1].c");
        check(
            lock,
            ctx,
            "Object.getPrototypeOf(calls[0].c) === Object.getPrototypeOf(calls[1].c)",
        );
        check(
            lock,
            ctx,
            "calls[0].c.constructor.name === 'TestController'",
        );
        check(lock, ctx, "Reflect.ownKeys(calls[0].c).length === 0");
        Ok(())
    });
}

#[test]
fn the_controller_cannot_be_constructed() {
    Harness::new().run_in_context(|lock, ctx| {
        run_code(lock, ctx, RECORD).unwrap();
        for construct in [
            "new C()",
            "C()",
            "Reflect.construct(C, [])",
            "new (class extends C {})()",
        ] {
            check(
                lock,
                ctx,
                &format!(
                    "(() => {{ const C = calls[0].c.constructor; try {{ {construct}; return \
                     false; }} catch (e) {{ return e instanceof TypeError && e.message === \
                     'Illegal constructor'; }} }})()"
                ),
            );
        }
        Ok(())
    });
}

#[test]
fn a_returned_promise_is_returned_as_is() {
    Harness::new().run_in_context(|lock, ctx| {
        let promise = run_code(
            lock,
            ctx,
            "(function () { return globalThis.p = new Promise(() => {}); })",
        )
        .unwrap();
        let expected = ctx.eval_raw(lock, "p").unwrap();
        assert_eq!(v8::Local::<v8::Value>::from(promise), expected);
        Ok(())
    });
}

#[test]
fn other_values_resolve_with_undefined() {
    Harness::new().run_in_context(|lock, ctx| {
        for code in ["(() => 42)", "(() => {})", "(() => ({ value: 1 }))"] {
            let promise = run_code(lock, ctx, code).unwrap();
            assert_eq!(promise.state(), v8::PromiseState::Fulfilled, "{code}");
            assert!(promise.result(lock).unwrap().is_undefined(), "{code}");
        }
        Ok(())
    });
}

#[test]
fn what_the_handler_throws_is_returned() {
    Harness::new().run_in_context(|lock, ctx| {
        let Err(exception) = run_code(lock, ctx, "(() => { throw new TypeError('boom'); })") else {
            panic!("the handler threw");
        };
        ctx.set_global(lock, "thrown", exception);
        check(
            lock,
            ctx,
            "thrown instanceof TypeError && thrown.message === 'boom'",
        );
        Ok(())
    });
}
