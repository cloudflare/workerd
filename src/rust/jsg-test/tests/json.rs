// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! `jsg::json`, checked against JavaScript's `JSON.parse()` and `JSON.stringify()`.

use std::sync::atomic::AtomicBool;
use std::sync::atomic::Ordering;

use jsg::v8;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;

use crate::EvalContext;
use crate::Harness;

fn eval<'s>(
    lock: &mut jsg::Lock<'s>,
    ctx: &EvalContext<'_>,
    expr: &str,
) -> v8::Local<'s, v8::Value> {
    ctx.eval_raw(lock, expr)
        .unwrap_or_else(|_| panic!("{expr} should evaluate"))
}

/// Asserts that `exception`, an error, has `name` and `message`.
fn assert_error<'s>(
    lock: &mut jsg::Lock<'s>,
    ctx: &EvalContext<'_>,
    exception: v8::Local<'s, v8::Value>,
    name: &str,
    message: &str,
) {
    ctx.set_global(lock, "e", exception);
    let actual: String = ctx.eval(lock, "`${e.name}: ${e.message}`").unwrap();
    assert_eq!(actual, format!("{name}: {message}"));
}

/// Parses `text`, which must throw, and returns the exception.
fn parse_exception<'s>(lock: &mut jsg::Lock<'s>, text: &str) -> v8::Local<'s, v8::Value> {
    let caught = lock
        .try_catch(|lock| {
            let result = jsg::json::parse(lock, text);
            assert!(result.as_ref().is_err_and(jsg::Error::is_pending), "{text}");
            result
        })
        .unwrap();
    caught.expect_err(text)
}

/// Stringifies the value of `expr`, which must throw, and returns the exception.
fn stringify_exception<'s>(
    lock: &mut jsg::Lock<'s>,
    ctx: &EvalContext<'_>,
    expr: &str,
) -> v8::Local<'s, v8::Value> {
    let value = eval(lock, ctx, expr);
    let caught = lock
        .try_catch(|lock| {
            let result = jsg::json::stringify(lock, value);
            assert!(result.as_ref().is_err_and(jsg::Error::is_pending), "{expr}");
            result
        })
        .unwrap();
    caught.expect_err(expr)
}

#[test]
fn parse_gives_what_json_parse_gives() {
    Harness::new().run_in_context(|lock, ctx| {
        for (text, check) in [
            ("123", "v === 123"),
            ("-0.5e3", "v === -500"),
            ("\"h\u{e9}llo \u{1f600}\"", "v === 'h\u{e9}llo \u{1f600}'"),
            // An escaped lone surrogate stays one.
            (
                "\"\\ud800\"",
                "v.length === 1 && v.charCodeAt(0) === 0xd800",
            ),
            ("null", "v === null"),
            ("true", "v === true"),
            (
                " { \"a\": [1, \"x\", null], \"b\": { \"c\": false } } ",
                "v.a[1] === 'x' && v.a[2] === null && v.b.c === false",
            ),
            // `__proto__` is an own property, not the prototype.
            (
                "{\"__proto__\": 1}",
                "Object.hasOwn(v, '__proto__') && Object.getPrototypeOf(v) === Object.prototype",
            ),
        ] {
            let value = jsg::json::parse(lock, text).unwrap();
            ctx.set_global(lock, "v", value);
            assert!(ctx.eval::<bool>(lock, check).unwrap(), "{text}: {check}");
        }
        Ok(())
    });
}

#[test]
fn invalid_json_leaves_a_syntax_error_pending() {
    Harness::new().run_in_context(|lock, ctx| {
        for text in ["", "{", "{'a': 1}", "[1,]", "undefined", "1 2"] {
            let exception = parse_exception(lock, text);
            ctx.set_global(lock, "e", exception);
            assert!(
                ctx.eval::<bool>(lock, "e instanceof SyntaxError").unwrap(),
                "{text:?}"
            );
        }
        Ok(())
    });
}

#[test]
fn stringify_gives_what_json_stringify_gives() {
    Harness::new().run_in_context(|lock, ctx| {
        for (expr, expected) in [
            ("123", "123"),
            // A newline, escaped.
            ("'h\u{e9}llo \"q\"\\n'", "\"h\u{e9}llo \\\"q\\\"\\n\""),
            // A lone surrogate is escaped, so the result is valid UTF-8.
            ("'\\ud800'", "\"\\ud800\""),
            ("null", "null"),
            ("NaN", "null"),
            (
                "({ a: [1, 'x', undefined, () => 1], b: undefined, c: { d: false } })",
                "{\"a\":[1,\"x\",null,null],\"c\":{\"d\":false}}",
            ),
            ("new Date(0)", "\"1970-01-01T00:00:00.000Z\""),
            ("({ toJSON() { return [1]; } })", "[1]"),
            ("new Map([[1, 2]])", "{}"),
        ] {
            let value = eval(lock, ctx, expr);
            assert_eq!(
                jsg::json::stringify(lock, value).unwrap(),
                expected,
                "{expr}"
            );
        }
        Ok(())
    });
}

/// Where `JSON.stringify()` returns `undefined`, `jsg::json::stringify` gives the string
/// `"undefined"`, as `jsg::Lock::serializeJson()` does.
#[test]
fn unrepresentable_values_give_undefined() {
    Harness::new().run_in_context(|lock, ctx| {
        for expr in [
            "undefined",
            "() => 1",
            "Symbol('s')",
            "({ toJSON() { return undefined; } })",
        ] {
            let value = eval(lock, ctx, expr);
            assert_eq!(
                jsg::json::stringify(lock, value).unwrap(),
                "undefined",
                "{expr}"
            );
        }
        Ok(())
    });
}

#[test]
fn what_stringify_runs_throws_stays_pending() {
    Harness::new().run_in_context(|lock, ctx| {
        let exception = stringify_exception(
            lock,
            ctx,
            "({ get x() { throw new RangeError('from getter'); } })",
        );
        assert_error(lock, ctx, exception, "RangeError", "from getter");

        let exception = stringify_exception(
            lock,
            ctx,
            "({ toJSON() { throw new URIError('from toJSON'); } })",
        );
        assert_error(lock, ctx, exception, "URIError", "from toJSON");

        let exception = stringify_exception(
            lock,
            ctx,
            "new Proxy({}, { ownKeys() { throw new EvalError('from trap'); } })",
        );
        assert_error(lock, ctx, exception, "EvalError", "from trap");

        for expr in [
            "(() => { const o = {}; o.self = o; return o; })()",
            "({ n: 1n })",
        ] {
            let exception = stringify_exception(lock, ctx, expr);
            ctx.set_global(lock, "e", exception);
            assert!(
                ctx.eval::<bool>(lock, "e instanceof TypeError").unwrap(),
                "{expr}"
            );
        }
        Ok(())
    });
}

#[test]
fn gap_indents_as_json_stringify_does() {
    Harness::new().run_in_context(|lock, ctx| {
        let value = eval(lock, ctx, "({ a: [1], b: {} })");
        for gap in ["", "  ", "\t", "--\u{e9}--", "12345678901234"] {
            ctx.set_global(lock, "gap", gap);
            let expected: String = ctx
                .eval(lock, "JSON.stringify({ a: [1], b: {} }, null, gap)")
                .unwrap();
            assert_eq!(
                jsg::json::stringify_with_gap(lock, value, gap).unwrap(),
                expected,
                "{gap:?}"
            );
        }
        // Only the first 10 code units are used.
        assert_eq!(
            jsg::json::stringify_with_gap(lock, value, "12345678901234").unwrap(),
            "{\n1234567890\"a\": [\n123456789012345678901\n1234567890],\n1234567890\"b\": {}\n}"
        );
        assert_eq!(
            jsg::json::stringify_with_gap(lock, value, "").unwrap(),
            jsg::json::stringify(lock, value).unwrap()
        );
        Ok(())
    });
}

/// Both call V8 directly, so replacing the `JSON` global changes nothing.
#[test]
fn replacing_the_json_global_changes_nothing() {
    Harness::new().run_in_context(|lock, ctx| {
        ctx.eval_raw(
            lock,
            "globalThis.JSON = { parse() { throw 1; }, stringify() { throw 2; } }",
        )
        .unwrap();
        let value = jsg::json::parse(lock, "[1, 2]").unwrap();
        assert_eq!(jsg::json::stringify(lock, value).unwrap(), "[1,2]");
        Ok(())
    });
}

static CHECKED_WHILE_TERMINATING: AtomicBool = AtomicBool::new(false);

#[jsg_resource]
struct Terminator;

#[jsg_resource]
impl Terminator {
    /// Is terminated, then parses and stringifies, which must fail and leave the termination
    /// pending.
    #[jsg_method]
    fn json_while_terminating(&self, lock: &mut jsg::Lock) {
        let value = lock.undefined();
        Harness::terminate_and_deliver(lock);
        assert!(jsg::json::parse(lock, "1").unwrap_err().is_pending());
        assert!(jsg::json::stringify(lock, value).unwrap_err().is_pending());
        assert!(lock.is_execution_terminating());
        CHECKED_WHILE_TERMINATING.store(true, Ordering::SeqCst);
    }
}

#[test]
fn nothing_runs_while_execution_is_terminating() {
    Harness::new().run_in_context(|lock, ctx| {
        let terminator = jsg::Rc::new(lock, Terminator);
        ctx.set_global(lock, "terminator", terminator);
        // The harness reports a termination as a failure or as a null exception.
        match ctx.eval_raw(lock, "terminator.jsonWhileTerminating()") {
            Err(crate::EvalError::EvalFailed) => {}
            Err(crate::EvalError::Exception(exception)) => assert!(exception.is_null()),
            other => panic!("the script was not terminated: {other:?}"),
        }
        assert!(CHECKED_WHILE_TERMINATING.load(Ordering::SeqCst));
        Ok(())
    });
}
