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
fn shared_array_buffers_are_shared_with_the_deserializer() {
    Harness::new().run_in_context(|lock, ctx| {
        let value = eval(
            lock,
            ctx,
            "globalThis.b = new SharedArrayBuffer(1); [b, new SharedArrayBuffer(2), b]",
        );
        let serialized =
            jsg::ser::serialize_with(lock, value, &jsg::ser::SerializeOptions::default()).unwrap();
        let cpp = Harness::cpp_serialize_with(lock, value, &jsg::ser::SerializeOptions::default());
        assert_eq!(serialized.data, cpp.data);
        assert_eq!(serialized.shared_array_buffers.len(), 2);
        assert!(
            serialized
                .shared_array_buffers
                .iter()
                .all(v8::BackingStore::is_shared)
        );

        // Each reader reads the other's data with its backing stores, and shares the buffers.
        let check = "v[0] === v[2] && v[0] !== b && v[1].byteLength === 2 && \
                     (new Uint8Array(b)[0] = 5, new Uint8Array(v[0])[0] === 5)";
        let copy = jsg::ser::deserialize_with(lock, &cpp.data, &cpp.deserialize_options()).unwrap();
        ctx.set_global(lock, "v", copy);
        assert!(ctx.eval::<bool>(lock, check).unwrap(), "Rust JSG read");
        let copy = Harness::cpp_deserialize_with(
            lock,
            &serialized.data,
            &serialized.deserialize_options(),
        );
        ctx.set_global(lock, "v", copy);
        assert!(ctx.eval::<bool>(lock, check).unwrap(), "C++ JSG read");

        // Without the backing stores, the data can't be read.
        deserialize_exception(lock, &serialized.data);
        Ok(())
    });
}

/// Serializes the value of `expr` with `options` and checks that the bytes are those C++ JSG writes
/// with the same options, and that what Rust JSG and C++ JSG read from them with `read` satisfies
/// `check`, a JavaScript expression of `v`.
fn assert_round_trip_with<'s>(
    lock: &mut jsg::Lock<'s>,
    ctx: &EvalContext<'_>,
    expr: &str,
    options: &jsg::ser::SerializeOptions<'_, 's>,
    read: &jsg::ser::DeserializeOptions<'_>,
    check: &str,
) {
    let value = eval(lock, ctx, expr);
    let data = jsg::ser::serialize_with(lock, value, options).unwrap().data;
    assert_eq!(
        data,
        Harness::cpp_serialize_with(lock, value, options).data,
        "{expr}: {options:?}"
    );

    let copy = jsg::ser::deserialize_with(lock, &data, read).unwrap();
    ctx.set_global(lock, "v", copy);
    assert!(
        ctx.eval::<bool>(lock, check).unwrap(),
        "{expr}: {options:?}: Rust JSG read: {check}"
    );

    let copy = Harness::cpp_deserialize_with(lock, &data, read);
    ctx.set_global(lock, "v", copy);
    assert!(
        ctx.eval::<bool>(lock, check).unwrap(),
        "{expr}: {options:?}: C++ JSG read: {check}"
    );
}

#[test]
fn versions_and_headers_are_written_and_read_as_cpp_jsg_does() {
    Harness::new().run_in_context(|lock, ctx| {
        let expr = "({ a: [1, 'x'], m: new Map([[1, 2]]), d: new Date(7) })";
        let check = "v.a[1] === 'x' && v.m.get(1) === 2 && v.d.getTime() === 7";
        for version in [None, Some(13), Some(14), Some(15)] {
            let options = jsg::ser::SerializeOptions {
                version,
                ..Default::default()
            };
            assert_round_trip_with(
                lock,
                ctx,
                expr,
                &options,
                &jsg::ser::DeserializeOptions::default(),
                check,
            );

            // Without a header, the reader is told the version, as storage reads data it wrote
            // before it wrote headers.
            let options = jsg::ser::SerializeOptions {
                version: Some(version.unwrap_or(15)),
                omit_header: true,
                ..Default::default()
            };
            let read = jsg::ser::DeserializeOptions {
                version: options.version,
                read_header: false,
                ..Default::default()
            };
            assert_round_trip_with(lock, ctx, expr, &options, &read, check);
        }

        // The header says the data is version 13, and the version given overrides it, as in C++.
        let options = jsg::ser::SerializeOptions {
            version: Some(13),
            ..Default::default()
        };
        let read = jsg::ser::DeserializeOptions {
            version: Some(13),
            ..Default::default()
        };
        assert_round_trip_with(lock, ctx, expr, &options, &read, check);
        Ok(())
    });
}

#[test]
fn versions_out_of_range_are_rejected() {
    Harness::new().run_in_context(|lock, ctx| {
        let value = lock.undefined();
        for version in [0, 12] {
            let options = jsg::ser::SerializeOptions {
                version: Some(version),
                ..Default::default()
            };
            let err = jsg::ser::serialize_with(lock, value, &options).unwrap_err();
            assert!(!err.is_pending(), "{version}");
            assert!(
                err.message
                    .contains("The minimum serialization version is 13."),
                "{version}: {}",
                err.message
            );

            let read = jsg::ser::DeserializeOptions {
                version: Some(version),
                read_header: false,
                ..Default::default()
            };
            let err = jsg::ser::deserialize_with(lock, b"_", &read).unwrap_err();
            assert!(!err.is_pending(), "{version}");
            assert!(
                err.message
                    .contains("The minimum serialization version is 13."),
                "{version}: {}",
                err.message
            );
        }

        // V8 throws for a version newer than its own.
        let caught = lock
            .try_catch(|lock| {
                let options = jsg::ser::SerializeOptions {
                    version: Some(1000),
                    ..Default::default()
                };
                let result = jsg::ser::serialize_with(lock, value, &options);
                assert!(result.as_ref().is_err_and(jsg::Error::is_pending));
                result
            })
            .unwrap();
        ctx.set_global(lock, "e", caught.unwrap_err());
        assert!(
            ctx.eval::<bool>(lock, "e.name === 'DataCloneError'")
                .unwrap()
        );
        Ok(())
    });
}

#[test]
fn errors_can_be_read_without_their_stack() {
    let harness = Harness::with_flags(|mut flags| flags.set_enhanced_error_serialization(true));
    harness.run_in_context(|lock, ctx| {
        let read = jsg::ser::DeserializeOptions {
            preserve_stack_in_errors: false,
            ..Default::default()
        };
        assert_round_trip_with(
            lock,
            ctx,
            "Object.assign(new TypeError('t'), { stack: 'original', code: 1 })",
            &jsg::ser::SerializeOptions::default(),
            &read,
            "v instanceof TypeError && v.code === 1 && typeof v.stack === 'string' && \
             v.stack !== 'original'",
        );
        Ok(())
    });
}

#[test]
fn header_errors_name_the_diagnostic_context() {
    Harness::new().run_in_context(|lock, ctx| {
        let read = jsg::ser::DeserializeOptions {
            diagnostic_context: Some("RPC result"),
            ..Default::default()
        };
        let err = jsg::ser::deserialize_with(lock, b"\x49\x02", &read).unwrap_err();
        assert!(!err.is_pending());
        assert!(err.message.contains("RPC result"), "{}", err.message);

        // Without a header to read, the same bytes are a value.
        let read = jsg::ser::DeserializeOptions {
            version: Some(15),
            read_header: false,
            ..Default::default()
        };
        let value = jsg::ser::deserialize_with(lock, b"\x49\x02", &read).unwrap();
        ctx.set_global(lock, "v", value);
        assert!(ctx.eval::<bool>(lock, "v === 1").unwrap());
        Ok(())
    });
}

#[test]
fn class_instances_are_plain_objects_or_rejected() {
    let reject = jsg::ser::SerializeOptions {
        class_instances: jsg::ser::ClassInstances::Reject,
        ..Default::default()
    };
    let read = jsg::ser::DeserializeOptions::default();
    for enhanced_errors in [false, true] {
        let harness = Harness::with_flags(|mut flags| {
            flags.set_enhanced_error_serialization(enhanced_errors);
        });
        harness.run_in_context(|lock, ctx| {
            ctx.eval_raw(
                lock,
                "globalThis.Point = class Point { constructor() { this.x = 1; } }",
            )
            .unwrap();

            // By default a class instance is written as a plain object.
            assert_round_trip_with(
                lock,
                ctx,
                "new Point()",
                &jsg::ser::SerializeOptions::default(),
                &read,
                "Object.getPrototypeOf(v) === Object.prototype && v.x === 1",
            );

            // Rejecting class instances leaves plain objects, built-in types, errors and API
            // objects alone.
            assert_round_trip_with(
                lock,
                ctx,
                "({ a: [1], m: new Map([[1, 2]]), d: new Date(3), e: new RangeError('r') })",
                &reject,
                &read,
                "v.a[0] === 1 && v.m.get(1) === 2 && v.d.getTime() === 3 && \
                 v.e instanceof RangeError",
            );
            let exception = serialize_exception(lock, ctx, "() => 1");
            ctx.set_global(lock, "exception", exception);
            assert_round_trip_with(
                lock,
                ctx,
                "[exception]",
                &reject,
                &read,
                "v[0].name === 'DataCloneError'",
            );

            // V8 writes an instance of a subclass of a built-in type as the built-in type.
            assert_round_trip_with(
                lock,
                ctx,
                "new (class extends Map {})([[1, 2]])",
                &reject,
                &read,
                "Object.getPrototypeOf(v) === Map.prototype && v.get(1) === 2",
            );

            // writeApiObject() rejects these with the DataCloneError jsg::Serializer throws.
            for (expr, type_name) in [
                ("({ p: new Point() })", "Point"),
                ("[Object.create(null)]", "Object"),
            ] {
                let value = eval(lock, ctx, expr);
                let caught = lock
                    .try_catch(|lock| {
                        let result = jsg::ser::serialize_with(lock, value, &reject);
                        assert!(result.as_ref().is_err_and(jsg::Error::is_pending), "{expr}");
                        result
                    })
                    .unwrap();
                assert_error(
                    lock,
                    ctx,
                    caught.unwrap_err(),
                    "DataCloneError",
                    &format!(
                        "Could not serialize object of type \"{type_name}\". This type does not \
                         support serialization."
                    ),
                );
            }
            Ok(())
        });
    }
}

/// Runs `setup`, which sets the globals `value` and `transfer`, an array of `count` values, and
/// returns them.
fn transfer_setup<'s>(
    lock: &mut jsg::Lock<'s>,
    ctx: &EvalContext<'_>,
    setup: &str,
    count: usize,
) -> (v8::Local<'s, v8::Value>, Vec<v8::Local<'s, v8::Value>>) {
    ctx.eval_raw(lock, setup).unwrap();
    let value = eval(lock, ctx, "value");
    let transfer = (0..count)
        .map(|i| eval(lock, ctx, &format!("transfer[{i}]")))
        .collect();
    (value, transfer)
}

/// Serializes with each serializer what `setup` sets up, transferring `count` buffers, and checks
/// that the bytes are the same, that `detached` holds after each, and that each reader reads with
/// the other's buffers a value for which `check` holds.
fn assert_transfer(
    lock: &mut jsg::Lock<'_>,
    ctx: &EvalContext<'_>,
    setup: &str,
    count: usize,
    detached: &str,
    check: &str,
) {
    let (value, transfer) = transfer_setup(lock, ctx, setup, count);
    let options = jsg::ser::SerializeOptions {
        transfer: &transfer,
        ..Default::default()
    };
    let serialized = jsg::ser::serialize_with(lock, value, &options).unwrap();
    assert!(
        ctx.eval::<bool>(lock, detached).unwrap(),
        "{setup}: Rust JSG detached"
    );

    let (value, transfer) = transfer_setup(lock, ctx, setup, count);
    let options = jsg::ser::SerializeOptions {
        transfer: &transfer,
        ..Default::default()
    };
    let cpp = Harness::cpp_serialize_with(lock, value, &options);
    assert!(
        ctx.eval::<bool>(lock, detached).unwrap(),
        "{setup}: C++ JSG detached"
    );
    assert_eq!(serialized.data, cpp.data, "{setup}");
    assert_eq!(
        serialized.transferred_array_buffers.len(),
        cpp.transferred_array_buffers.len(),
        "{setup}"
    );

    let copy = jsg::ser::deserialize_with(lock, &cpp.data, &cpp.deserialize_options()).unwrap();
    ctx.set_global(lock, "v", copy);
    assert!(
        ctx.eval::<bool>(lock, check).unwrap(),
        "{setup}: Rust JSG read: {check}"
    );
    let copy =
        Harness::cpp_deserialize_with(lock, &serialized.data, &serialized.deserialize_options());
    ctx.set_global(lock, "v", copy);
    assert!(
        ctx.eval::<bool>(lock, check).unwrap(),
        "{setup}: C++ JSG read: {check}"
    );
}

#[test]
fn transferred_array_buffers_move_to_the_deserializer() {
    Harness::new().run_in_context(|lock, ctx| {
        // A buffer and a view of it written after it.
        assert_transfer(
            lock,
            ctx,
            "globalThis.b = new ArrayBuffer(4); new Uint8Array(b)[1] = 9;
             globalThis.value = { b, v: new Uint8Array(b, 1) }; globalThis.transfer = [b];",
            1,
            "b.byteLength === 0",
            "v.b.byteLength === 4 && v.v.buffer === v.b && v.v[0] === 9",
        );
        // A view given for its buffer, and a value given twice, which is transferred once.
        assert_transfer(
            lock,
            ctx,
            "globalThis.b = new ArrayBuffer(2); globalThis.u = new Uint8Array(b); u[0] = 3;
             globalThis.value = [u]; globalThis.transfer = [u, u];",
            2,
            "b.byteLength === 0 && u.length === 0",
            "v[0][0] === 3 && v[0].buffer.byteLength === 2",
        );
        // A getter that serialization runs sees the buffer intact: it is detached afterwards.
        assert_transfer(
            lock,
            ctx,
            "globalThis.b = new ArrayBuffer(8);
             globalThis.value = { get size() { return b.byteLength; }, b };
             globalThis.transfer = [b];",
            1,
            "b.byteLength === 0",
            "v.size === 8 && v.b.byteLength === 8",
        );
        // A buffer that the value doesn't hold is still detached, and passed on.
        assert_transfer(
            lock,
            ctx,
            "globalThis.b = new ArrayBuffer(1); globalThis.value = 1; globalThis.transfer = [b];",
            1,
            "b.byteLength === 0",
            "v === 1",
        );
        Ok(())
    });
}

#[test]
fn values_that_cannot_be_transferred_throw() {
    Harness::new().run_in_context(|lock, ctx| {
        for (setup, name, message) in [
            (
                "globalThis.b = new ArrayBuffer(1); globalThis.transfer = [b, {}];",
                "TypeError",
                "Object is not transferable",
            ),
            (
                "globalThis.b = new ArrayBuffer(1);
                 globalThis.transfer = [b, new Uint8Array(new SharedArrayBuffer(1))];",
                "DataCloneError",
                "Object is not transferable.",
            ),
            (
                "globalThis.b = new ArrayBuffer(1);
                 globalThis.transfer = [b, new SharedArrayBuffer(1)];",
                "TypeError",
                "Object is not transferable",
            ),
        ] {
            let (value, transfer) =
                transfer_setup(lock, ctx, &format!("{setup} globalThis.value = b;"), 2);
            let caught = lock
                .try_catch(|lock| {
                    let options = jsg::ser::SerializeOptions {
                        transfer: &transfer,
                        ..Default::default()
                    };
                    let result = jsg::ser::serialize_with(lock, value, &options);
                    assert!(
                        result.as_ref().is_err_and(jsg::Error::is_pending),
                        "{setup}"
                    );
                    result.map(drop)
                })
                .unwrap();
            assert_error(lock, ctx, caught.unwrap_err(), name, message);
            // Nothing is detached.
            assert!(
                ctx.eval::<bool>(lock, "b.byteLength === 1").unwrap(),
                "{setup}"
            );
        }
        Ok(())
    });
}

#[test]
fn buffers_must_match_the_data() {
    Harness::new().run_in_context(|lock, ctx| {
        let (value, transfer) = transfer_setup(
            lock,
            ctx,
            "globalThis.value = new ArrayBuffer(1); globalThis.transfer = [value];",
            1,
        );
        let options = jsg::ser::SerializeOptions {
            transfer: &transfer,
            ..Default::default()
        };
        let transferred = jsg::ser::serialize_with(lock, value, &options).unwrap();
        let value = eval(lock, ctx, "new SharedArrayBuffer(1)");
        let shared =
            jsg::ser::serialize_with(lock, value, &jsg::ser::SerializeOptions::default()).unwrap();

        // Data that refers to buffers the reader isn't given.
        deserialize_exception(lock, &transferred.data);
        let read = jsg::ser::DeserializeOptions {
            shared_array_buffers: Some(&[]),
            ..Default::default()
        };
        let caught = lock
            .try_catch(|lock| jsg::ser::deserialize_with(lock, &shared.data, &read))
            .unwrap();
        let err = jsg::Error::from_js(lock, caught.unwrap_err()).unwrap();
        assert!(err.message.starts_with("internal error"), "{}", err.message);

        // Backing stores of the wrong kind.
        for read in [
            jsg::ser::DeserializeOptions {
                transferred_array_buffers: &shared.shared_array_buffers,
                ..Default::default()
            },
            jsg::ser::DeserializeOptions {
                shared_array_buffers: Some(&transferred.transferred_array_buffers),
                ..Default::default()
            },
        ] {
            let err = jsg::ser::deserialize_with(lock, &shared.data, &read).unwrap_err();
            assert!(!err.is_pending());
            assert!(
                err.message.contains("backing store must"),
                "{}",
                err.message
            );
        }
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
