// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! `jsg::ser`, checked against C++ JSG's `jsg::Serializer` and `jsg::Deserializer`: the bytes
//! are the same, and each reads what the other writes.

use std::sync::atomic::AtomicBool;
use std::sync::atomic::Ordering;

use jsg::FromJS;
use jsg::v8;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;

use crate::EvalContext;
use crate::Harness;

#[jsg_resource]
struct Unserializable;

#[jsg_resource]
impl Unserializable {
    #[jsg_method]
    fn noop(&self) {}
}

fn eval<'s>(
    lock: &mut jsg::Lock<'s>,
    ctx: &EvalContext<'_>,
    expr: &str,
) -> v8::Local<'s, v8::Value> {
    ctx.eval_raw(lock, expr)
        .unwrap_or_else(|_| panic!("{expr} should evaluate"))
}

/// Serializes the value of `expr` and checks that the bytes are those C++ JSG writes, and that
/// what Rust JSG and C++ JSG read from them satisfies `check`, a JavaScript expression of `v`.
fn assert_round_trip(lock: &mut jsg::Lock<'_>, ctx: &EvalContext<'_>, expr: &str, check: &str) {
    let value = eval(lock, ctx, expr);
    let data = jsg::ser::serialize(lock, value).unwrap();
    assert_eq!(data, Harness::cpp_serialize(lock, value), "{expr}");

    let copy = jsg::ser::deserialize(lock, &data).unwrap();
    ctx.set_global(lock, "v", copy);
    assert!(
        ctx.eval::<bool>(lock, check).unwrap(),
        "{expr}: Rust JSG read: {check}"
    );

    let copy = Harness::cpp_deserialize(lock, &data);
    ctx.set_global(lock, "v", copy);
    assert!(
        ctx.eval::<bool>(lock, check).unwrap(),
        "{expr}: C++ JSG read: {check}"
    );
}

/// Serializes the value of `expr`, which must throw, and returns the exception.
fn serialize_exception<'s>(
    lock: &mut jsg::Lock<'s>,
    ctx: &EvalContext<'_>,
    expr: &str,
) -> v8::Local<'s, v8::Value> {
    let value = eval(lock, ctx, expr);
    let caught = lock
        .try_catch(|lock| {
            let result = jsg::ser::serialize(lock, value);
            assert!(result.as_ref().is_err_and(jsg::Error::is_pending), "{expr}");
            result
        })
        .unwrap();
    caught.expect_err(expr)
}

/// Asserts that `exception`, an error or a `DOMException`, has `name` and `message`.
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

#[test]
fn values_round_trip() {
    Harness::new().run_in_context(|lock, ctx| {
        for (expr, check) in [
            ("123", "v === 123"),
            ("'h\u{e9}llo'", "v === 'h\u{e9}llo'"),
            ("null", "v === null"),
            ("10n ** 30n", "v === 10n ** 30n"),
            (
                "({ a: 1, b: [1, 'x', null], c: undefined })",
                "v.a === 1 && v.b[1] === 'x' && v.b[2] === null && 'c' in v",
            ),
            ("new Map([[1, { x: 2 }]])", "v.get(1).x === 2"),
            ("new Set(['a'])", "v.has('a')"),
            ("new Date(5)", "v.getTime() === 5"),
            ("/a+/g", "v.source === 'a+' && v.flags === 'g'"),
            (
                "new Uint8Array([1, 2, 3])",
                "v instanceof Uint8Array && v[2] === 3",
            ),
            (
                "new Float64Array([0.5]).buffer",
                "new Float64Array(v)[0] === 0.5",
            ),
            (
                "(() => { const o = {}; o.self = o; return o; })()",
                "v.self === v",
            ),
            // Without enhanced_error_serialization, V8 writes errors itself.
            (
                "Object.assign(new TypeError('bad'), { code: 1 })",
                "v instanceof TypeError && v.message === 'bad' && v.code === undefined",
            ),
        ] {
            assert_round_trip(lock, ctx, expr, check);
        }
        Ok(())
    });
}

#[test]
fn errors_with_enhanced_error_serialization() {
    let harness = Harness::with_flags(|mut flags| flags.set_enhanced_error_serialization(true));
    harness.run_in_context(|lock, ctx| {
        for (expr, check) in [
            (
                "new RangeError('r')",
                "v instanceof RangeError && v.message === 'r' && !Object.hasOwn(v, 'name')",
            ),
            (
                "Object.assign(new Error('e'), { code: 42, cause: new TypeError('t') })",
                "v.code === 42 && v.cause instanceof TypeError && v.cause.message === 't'",
            ),
            (
                "Object.assign(new Error('e'), { stack: 'original' })",
                "v.stack === 'original'",
            ),
            // A name that is not a known type's is written out, and set as an own property.
            (
                "Object.assign(new Error('m'), { name: 'MyError' })",
                "v instanceof Error && v.name === 'MyError' && v.message === 'm'",
            ),
            // A subclass is read as the type whose name it inherits.
            (
                "new (class Custom extends Error {})('c')",
                "Object.getPrototypeOf(v) === Error.prototype && v.message === 'c'",
            ),
            (
                "new AggregateError([new SyntaxError('a')], 'agg')",
                "v instanceof AggregateError && v.message === 'agg' && \
                 v.errors[0] instanceof SyntaxError",
            ),
            (
                "({ inner: [new EvalError('x')] })",
                "v.inner[0] instanceof EvalError",
            ),
        ] {
            assert_round_trip(lock, ctx, expr, check);
        }
        Ok(())
    });
}

/// Where `jsg::Serializer` loses part of an error, the Rust serializer loses the same part, so
/// that the bytes stay the same.
#[test]
fn errors_lose_what_cpp_jsg_loses() {
    let harness = Harness::with_flags(|mut flags| flags.set_enhanced_error_serialization(true));
    harness.run_in_context(|lock, ctx| {
        // The name is compared with the known types' names up to its first NUL, so this one is
        // taken for `Error` and not written.
        assert_round_trip(
            lock,
            ctx,
            "Object.assign(new Error('m'), { name: 'Error\\0custom' })",
            "v.name === 'Error' && !Object.hasOwn(v, 'name') && v.message === 'm'",
        );

        // The other own properties are copied with an assignment, which runs a setter that
        // `Object.prototype` has for the property instead of copying it.
        ctx.eval_raw(
            lock,
            "globalThis.setterRuns = 0;
             Object.defineProperty(Object.prototype, 'x', {
               set(value) { globalThis.setterRuns++; },
               configurable: true,
             });",
        )
        .unwrap();
        assert_round_trip(
            lock,
            ctx,
            "Object.defineProperty(new Error('e'), 'x', { value: 1, enumerable: true })",
            "!Object.hasOwn(v, 'x')",
        );
        // Once for each serializer.
        assert!(ctx.eval::<bool>(lock, "globalThis.setterRuns === 2").unwrap());
        ctx.eval_raw(lock, "delete Object.prototype.x").unwrap();

        // An own `__proto__` sets the prototype of the object it is copied to, and is lost.
        assert_round_trip(
            lock,
            ctx,
            "Object.defineProperty(new Error('e'), '__proto__', { value: { y: 1 }, enumerable: true })",
            "!Object.hasOwn(v, '__proto__') && v.y === undefined && v.message === 'e'",
        );
        Ok(())
    });
}

#[test]
fn workerd_api_objects_round_trip() {
    Harness::new().run_in_context(|lock, ctx| {
        // A DOMException, which is serializable, is the exception serializing a function throws.
        let exception = serialize_exception(lock, ctx, "() => 1");
        ctx.set_global(lock, "exception", exception);
        assert_round_trip(
            lock,
            ctx,
            "({ exception })",
            "v.exception.name === 'DataCloneError' && v.exception.code === 25 && \
             v.exception.message === '() => 1 could not be cloned.'",
        );
        Ok(())
    });
}

#[test]
fn unserializable_values_throw_data_clone_error() {
    Harness::new().run_in_context(|lock, ctx| {
        let exception = serialize_exception(lock, ctx, "({ f() {} }).f");
        assert_error(
            lock,
            ctx,
            exception,
            "DataCloneError",
            "f() {} could not be cloned.",
        );

        let exception = serialize_exception(lock, ctx, "Symbol('s')");
        assert_error(
            lock,
            ctx,
            exception,
            "DataCloneError",
            "Symbol(s) could not be cloned.",
        );

        ctx.set_global(lock, "resource", jsg::Rc::new(Unserializable));
        let exception = serialize_exception(lock, ctx, "[resource]");
        assert_error(
            lock,
            ctx,
            exception,
            "DataCloneError",
            "Could not serialize object of type \"Unserializable\". This type does not support \
             serialization.",
        );
        Ok(())
    });
}

#[test]
fn what_serialization_runs_throws_stays_pending() {
    Harness::new().run_in_context(|lock, ctx| {
        let exception = serialize_exception(
            lock,
            ctx,
            "({ get x() { throw new RangeError('from getter'); } })",
        );
        assert_error(lock, ctx, exception, "RangeError", "from getter");
        Ok(())
    });

    let harness = Harness::with_flags(|mut flags| flags.set_enhanced_error_serialization(true));
    harness.run_in_context(|lock, ctx| {
        let exception = serialize_exception(
            lock,
            ctx,
            "Object.defineProperty(new Error('e'), 'message', { get() { throw new URIError('u'); } })",
        );
        assert_error(lock, ctx, exception, "URIError", "u");
        Ok(())
    });
}

/// The header V8 writes, which `jsg::ser::serialize` writes first.
fn header(lock: &mut jsg::Lock<'_>) -> Vec<u8> {
    let value = lock.undefined();
    let mut data = jsg::ser::serialize(lock, value).unwrap();
    data.truncate(2);
    data
}

/// Deserializes `data`, which must throw, and returns the exception.
fn deserialize_exception<'s>(lock: &mut jsg::Lock<'s>, data: &[u8]) -> v8::Local<'s, v8::Value> {
    let caught = lock
        .try_catch(|lock| {
            let result = jsg::ser::deserialize(lock, data);
            assert!(
                result.as_ref().is_err_and(jsg::Error::is_pending),
                "{data:?}"
            );
            result
        })
        .unwrap();
    caught.expect_err("deserialization should throw")
}

#[test]
fn malformed_data_throws() {
    Harness::new().run_in_context(|lock, ctx| {
        // V8's tag for a host object, then the tag of no workerd API type.
        let mut data = header(lock);
        data.extend_from_slice(b"\\\x63");
        let exception = deserialize_exception(lock, &data);
        let err = jsg::Error::from_js(lock, exception).unwrap();
        assert!(err.message.starts_with("internal error"), "{}", err.message);

        // A native error, cut short before its type.
        let mut data = header(lock);
        data.extend_from_slice(b"\\\x0a");
        let exception = deserialize_exception(lock, &data);
        let err = jsg::Error::from_js(lock, exception).unwrap();
        assert!(err.message.starts_with("internal error"), "{}", err.message);

        // A native error whose message is not a string.
        let mut data = header(lock);
        data.extend_from_slice(b"\\\x0a\x01_");
        let exception = deserialize_exception(lock, &data);
        assert_error(
            lock,
            ctx,
            exception,
            "DataCloneError",
            "Deserialization failed: error message is not a string",
        );

        // V8 reports a value cut short itself.
        let mut data = header(lock);
        data.push(b'o');
        deserialize_exception(lock, &data);
        Ok(())
    });
}

#[test]
fn missing_or_unsupported_header_is_an_internal_error() {
    Harness::new().run_in_context(|lock, _ctx| {
        for data in [&b""[..], b"\x49\x02", b"\xff\x7f\x49\x02"] {
            let err = jsg::ser::deserialize(lock, data).unwrap_err();
            assert!(!err.is_pending(), "{data:?}");
            assert!(
                err.message.contains(
                    "Unable to deserialize cloned data due to invalid or unsupported version."
                ),
                "{data:?}: {}",
                err.message
            );
        }
        Ok(())
    });
}

#[test]
fn shared_array_buffers_are_written_as_cpp_jsg_writes_them() {
    Harness::new().run_in_context(|lock, ctx| {
        let value = eval(
            lock,
            ctx,
            "(() => { const b = new SharedArrayBuffer(1); return [b, new SharedArrayBuffer(2), b]; })()",
        );
        let data = jsg::ser::serialize(lock, value).unwrap();
        assert_eq!(data, Harness::cpp_serialize(lock, value));
        // No deserializer is given the buffers.
        deserialize_exception(lock, &data);
        Ok(())
    });
}

static CHECKED_WHILE_TERMINATING: AtomicBool = AtomicBool::new(false);

#[jsg_resource]
struct Terminator;

#[jsg_resource]
impl Terminator {
    /// Is terminated, then serializes and deserializes, which must fail and leave the
    /// termination pending.
    #[jsg_method]
    fn serialize_while_terminating(&self, lock: &mut jsg::Lock) {
        let value = lock.undefined();
        let data = jsg::ser::serialize(lock, value).unwrap();
        Harness::terminate_and_deliver(lock);
        assert!(jsg::ser::serialize(lock, value).unwrap_err().is_pending());
        assert!(jsg::ser::deserialize(lock, &data).unwrap_err().is_pending());
        assert!(lock.is_execution_terminating());
        CHECKED_WHILE_TERMINATING.store(true, Ordering::SeqCst);
    }
}

#[test]
fn nothing_runs_while_execution_is_terminating() {
    Harness::new().run_in_context(|lock, ctx| {
        ctx.set_global(lock, "terminator", jsg::Rc::new(Terminator));
        // The harness reports a termination as a failure or as a null exception.
        match ctx.eval_raw(lock, "terminator.serializeWhileTerminating()") {
            Err(crate::EvalError::EvalFailed) => {}
            Err(crate::EvalError::Exception(exception)) => assert!(exception.is_null()),
            other => panic!("the script was not terminated: {other:?}"),
        }
        assert!(CHECKED_WHILE_TERMINATING.load(Ordering::SeqCst));
        Ok(())
    });
}
