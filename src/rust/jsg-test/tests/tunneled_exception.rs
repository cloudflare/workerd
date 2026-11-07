// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! `jsg::TunneledException`, checked against C++ JSG's conversion of an exception to a
//! `kj::Exception` and back.

use jsg::v8;

use crate::EvalContext;
use crate::Harness;

/// Whether two copies of an exception are alike: of the same type, with the same own
/// properties, compared alike in turn, and the same `name`, `message` and `code`, which a
/// `DOMException` inherits. Stacks are not compared, since each copy gets its own, and nor are
/// the reference IDs of internal errors.
const SAME: &str = r"
globalThis.same = (a, b) => {
  if (typeof a !== 'object' || a === null) return Object.is(a, b);
  if (typeof b !== 'object' || b === null) return false;
  if (Object.getPrototypeOf(a) !== Object.getPrototypeOf(b)) return false;
  const keys = (o) => Reflect.ownKeys(o).filter((k) => k !== 'stack').sort();
  const ka = keys(a);
  const kb = keys(b);
  if (ka.join() !== kb.join()) return false;
  if (!ka.every((k) => same(a[k], b[k]))) return false;
  const message = (o) => String(o.message).replace(/reference = \w+/, 'reference = ID');
  return a.name === b.name && message(a) === message(b) && a.code === b.code;
};
";

/// The exceptions converted, each a JavaScript expression, with a JavaScript expression of `v`,
/// the copy, that must hold whether or not `enhanced_error_serialization` is on.
const EXCEPTIONS: &[(&str, &str)] = &[
    (
        "new Error('g')",
        "Object.getPrototypeOf(v) === Error.prototype && v.message === 'g'",
    ),
    (
        "new (class Custom extends Error {})('c')",
        "Object.getPrototypeOf(v) === Error.prototype && v.message === 'c'",
    ),
    (
        "Object.assign(new TypeError('t'), { code: 42 })",
        "v instanceof TypeError && v.message === 't'",
    ),
    ("'thrown'", "v instanceof Error && v.message === 'thrown'"),
    ("42", "v instanceof Error && v.message === '42'"),
    (
        "undefined",
        "v instanceof Error && v.message === 'undefined'",
    ),
    (
        "Symbol('s')",
        "v instanceof Error && v.message === 'Symbol(s)'",
    ),
    (
        "abortError",
        "Object.getPrototypeOf(v) === Object.getPrototypeOf(abortError) && \
         v.name === 'AbortError' && v.message === 'm' && v !== abortError",
    ),
    (
        "Object.create(dataError)",
        "Object.getPrototypeOf(v) === Object.getPrototypeOf(dataError) && \
         v.name === 'DataError' && v.message === 'd'",
    ),
    (
        "({ retryable: true, message: 'x' })",
        "v.retryable === true && v.overloaded === undefined",
    ),
    (
        "Object.assign(new Error('o'), { overloaded: 1, retryable: 1 })",
        "v.message === 'o' && v.overloaded === true",
    ),
    (
        "Object.assign(new RangeError('r'), { retryable: 'yes' })",
        "v instanceof RangeError && v.retryable === true",
    ),
    (
        "Object.assign(new Error('d'), { name: 'DOMException' })",
        "v instanceof Error",
    ),
    (
        "Object.assign(new Error('m'), { name: 'ErrorX' })",
        "v instanceof Error",
    ),
    (
        "Object.assign(new Error('m'), { name: 'DOMException(Name)' })",
        "v instanceof Error",
    ),
    (
        "new AggregateError([new SyntaxError('a')], 'agg')",
        "v instanceof AggregateError && v.message === 'agg'",
    ),
    (
        "new URIError('u')",
        "v instanceof URIError && v.message === 'u'",
    ),
    (
        "new EvalError('e')",
        "v instanceof EvalError && v.message === 'e'",
    ),
    (
        "new SuppressedError(1, 2, 's')",
        "v instanceof SuppressedError && v.message === 's'",
    ),
    (
        "Object.assign(new Error('e'), { stack: 'original' })",
        "v.message === 'e' && v.stack !== 'original'",
    ),
    (
        "({ toString() { return 'Error: custom'; } })",
        "v !== undefined",
    ),
];

fn eval<'s>(
    lock: &mut jsg::Lock<'s>,
    ctx: &EvalContext<'_>,
    expr: &str,
) -> v8::Local<'s, v8::Value> {
    ctx.eval_raw(lock, expr)
        .unwrap_or_else(|_| panic!("{expr} should evaluate"))
}

fn tunnel<'s>(
    lock: &mut jsg::Lock<'s>,
    exception: v8::Local<'s, v8::Value>,
) -> jsg::Result<v8::Local<'s, v8::Value>> {
    jsg::TunneledException::from_js(lock, exception)?.to_js(lock)
}

fn set_up(lock: &mut jsg::Lock<'_>, ctx: &EvalContext<'_>) {
    eval(lock, ctx, SAME);
    let abort_error = lock.dom_exception("AbortError", "m").unwrap();
    ctx.set_global(lock, "abortError", abort_error);
    let data_error = lock.dom_exception("DataError", "d").unwrap();
    ctx.set_global(lock, "dataError", data_error);
}

fn assert_like_cpp(enhanced_error_serialization: bool) {
    let harness = Harness::with_flags(|mut flags| {
        flags.set_enhanced_error_serialization(enhanced_error_serialization);
    });
    harness.run_in_context(|lock, ctx| {
        set_up(lock, ctx);
        for &(expr, check) in EXCEPTIONS {
            let what =
                format!("{expr}, enhanced_error_serialization: {enhanced_error_serialization}");
            let exception = eval(lock, ctx, expr);
            let rust = tunnel(lock, exception).unwrap_or_else(|_| panic!("{what}: Rust"));
            let cpp = Harness::cpp_tunnel_exception(lock, exception)
                .unwrap_or_else(|_| panic!("{what}: C++"));
            ctx.set_global(lock, "v", rust);
            ctx.set_global(lock, "cpp", cpp);
            let holds = ctx
                .eval::<bool>(lock, check)
                .unwrap_or_else(|_| panic!("{what}: {check} threw"));
            assert!(holds, "{what}: {check}");
            assert!(
                ctx.eval::<bool>(lock, "same(v, cpp)").unwrap(),
                "{what}: unlike C++ JSG's copy"
            );
        }
        Ok(())
    });
}

#[test]
fn copies_like_cpp_jsg() {
    assert_like_cpp(false);
}

#[test]
fn copies_like_cpp_jsg_with_enhanced_error_serialization() {
    assert_like_cpp(true);
}

/// Only with `enhanced_error_serialization` does the copy come from the serialized exception,
/// keeping its type's name and its own properties.
#[test]
fn copies_own_properties_only_with_enhanced_error_serialization() {
    for enhanced in [false, true] {
        let harness =
            Harness::with_flags(|mut flags| flags.set_enhanced_error_serialization(enhanced));
        harness.run_in_context(|lock, ctx| {
            let exception = eval(lock, ctx, "Object.assign(new TypeError('t'), { code: 42 })");
            let copy = tunnel(lock, exception)?;
            ctx.set_global(lock, "v", copy);
            let code = ctx.eval::<bool>(lock, "v.code === 42").unwrap();
            assert_eq!(code, enhanced);
            Ok(())
        });
    }
}

/// The properties read run their getters as C++ JSG runs them, in the same order.
#[test]
fn reads_properties_like_cpp_jsg() {
    for enhanced in [false, true] {
        let harness =
            Harness::with_flags(|mut flags| flags.set_enhanced_error_serialization(enhanced));
        harness.run_in_context(|lock, ctx| {
            let make = "(() => {
                globalThis.reads = [];
                const e = new Error('m');
                for (const k of ['overloaded', 'retryable', 'name', 'stack', 'code']) {
                    Object.defineProperty(e, k, {
                        get() { reads.push(k); return undefined; },
                        enumerable: true,
                        configurable: true,
                    });
                }
                return e;
            })()";
            let exception = eval(lock, ctx, make);
            tunnel(lock, exception)?;
            let rust = ctx.eval::<String>(lock, "reads.join()").unwrap();
            let exception = eval(lock, ctx, make);
            Harness::cpp_tunnel_exception(lock, exception)?;
            let cpp = ctx.eval::<String>(lock, "reads.join()").unwrap();
            assert_eq!(rust, cpp, "enhanced_error_serialization: {enhanced}");
            assert!(rust.starts_with("overloaded,retryable"), "{rust}");
            Ok(())
        });
    }
}

/// What a getter throws propagates, as from C++ JSG.
#[test]
fn propagates_what_a_getter_throws() {
    let harness = Harness::new();
    harness.run_in_context(|lock, ctx| {
        let exception = eval(
            lock,
            ctx,
            "Object.defineProperty(new Error('m'), 'retryable', { \
               get() { throw new RangeError('from getter'); } })",
        );
        let rust = lock.try_catch(|lock| tunnel(lock, exception))?.unwrap_err();
        let cpp = lock
            .try_catch(|lock| Harness::cpp_tunnel_exception(lock, exception))?
            .unwrap_err();
        ctx.set_global(lock, "v", rust);
        ctx.set_global(lock, "cpp", cpp);
        assert!(
            ctx.eval::<bool>(
                lock,
                "v instanceof RangeError && v.message === 'from getter' && \
                 cpp instanceof RangeError && cpp.message === 'from getter'"
            )
            .unwrap()
        );
        Ok(())
    });
}
