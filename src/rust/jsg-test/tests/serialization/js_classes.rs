// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Instances of the runtime's classes implemented in JavaScript, which `jsg::ser` writes and reads
//! as C++ JSG does (`SERIALIZATION_TAG_JS_SERIALIZABLE` in `workerd/jsg/ser.h`).

use jsg::v8;

use super::Unserializable;
use super::assert_deserialization_throws;
use super::assert_error;
use super::assert_read;
use super::assert_round_trip;
use super::deserialize_exception;
use super::eval;
use super::header;
use super::serialize_exception;
use crate::EvalContext;
use crate::Harness;

/// Classes implemented in JavaScript, as the per-isolate bootstrap's `serialization.ts` sets them
/// up. Evaluates to the exports of the module "test", among which the deserializers look them up
/// by their `deserializeInfo`. `constructed` counts constructor calls, `log` records the `clone`
/// and `deserialize` calls, and `getterCalls` counts calls of getters that no serializer may call.
const JS_CLASSES: &str = r"
  let constructed = 0;
  const log = [];
  let getterCalls = 0;

  // A class's descriptor, as makeCloneable() makes it.
  function descriptor(deserializeInfo, clone, deserialize) {
    return Object.freeze({ __proto__: null, deserializeInfo, clone, deserialize });
  }
  function makeCloneable(ctor, deserializeInfo, clone, deserialize) {
    return ctor.prototype[kCloneable] = descriptor(deserializeInfo, clone, deserialize);
  }

  class Point {
    constructor(x) { constructed++; this[kMode] = pointCloneable; this[kX] = x; }
    get x() { return this[kX]; }
  }
  const pointCloneable = makeCloneable(Point, 'test:Point', (p) => {
    log.push(`clone ${p[kX]}`);
    return p[kX];
  }, (p, data) => {
    log.push(`deserialize ${data}`);
    // Shows that the data went through deserialize().
    p[kX] = data + 1;
  });

  // Holds any value as its data, which may be the instance itself.
  class Holder {
    constructor(value) { this[kMode] = holderCloneable; this[kX] = value; }
    get value() { return this[kX]; }
  }
  const holderCloneable = makeCloneable(Holder, 'test:Holder', (h) => ({ value: h[kX] }),
      (h, data) => { h[kX] = data.value; });

  class TestError extends Error {
    constructor(message) { super(message); this[kMode] = testErrorCloneable; }
  }
  TestError.prototype.name = 'TestError';
  const testErrorCloneable = makeCloneable(TestError, 'test:TestError', (e) => e.message,
      (e, data) => {
    Object.defineProperty(e, 'message', { value: data, writable: true, configurable: true });
  });

  // Stands in for the bootstrap's DOMException.prototype, whose instances hold their name and
  // message as the bootstrap's do.
  const domExceptionPrototype = Object.create(Error.prototype, {
    name: { get() { return this[kName]; } },
    message: { get() { return this[kMessage]; } },
  });
  domExceptionPrototype[kDomExceptionPrototype] = true;

  // A DOMException with an amount, as the bootstrap's QuotaExceededError is one with a quota.
  // newAmountError() makes its instances.
  class AmountError {
    get amount() { return this[kX]; }
  }
  Object.setPrototypeOf(AmountError.prototype, domExceptionPrototype);
  const amountErrorCloneable = makeCloneable(AmountError, 'test:AmountError',
      (e) => ({ message: e.message, amount: e[kX] }), (e, data) => {
    // In the bootstrap's constructors' order.
    e[kBrand] = true;
    e[kName] = 'AmountError';
    e[kMessage] = data.message;
    e[kX] = data.amount;
  });

  // Accepts only a number as its data.
  class Strict {
    constructor(x) { this[kMode] = strictCloneable; this[kX] = x; }
    get x() { return this[kX]; }
  }
  const strictCloneable = makeCloneable(Strict, 'test:Strict', (s) => s[kX], (s, data) => {
    if (typeof data !== 'number') throw new TypeError('Strict data is not a number');
    s[kX] = data;
  });

  class Opaque { constructor() { this[kMode] = 0; } }

  // Cloneable, but written as `deserializeInfo` with `data`, so as anything a peer could send.
  class Forged {
    constructor(deserializeInfo, data = 1) {
      this[kMode] = descriptor(deserializeInfo, () => data, () => {});
    }
  }

  class CloneThrows { constructor() { this[kMode] = cloneThrowsCloneable; } }
  const cloneThrowsCloneable = makeCloneable(CloneThrows, 'test:CloneThrows', () => {
    throw new RangeError('throw from clone');
  }, () => {});

  class Throws { constructor() { this[kMode] = throwsCloneable; } }
  const throwsCloneable = makeCloneable(Throws, 'test:Throws', () => 1, () => {
    throw new Error('throw from deserialize');
  });

  class NoDeserialize {}
  class NotADescriptor {}
  NotADescriptor.prototype[kCloneable] = 1;
  class NoDeserializeFunction {}
  NoDeserializeFunction.prototype[kCloneable] = descriptor('test:NoDeserializeFunction', () => 1);
  class LazyDeserialize {}
  LazyDeserialize.prototype[kCloneable] = Object.freeze({ __proto__: null,
      deserializeInfo: 'test:LazyDeserialize', clone: () => 1,
      get deserialize() { getterCalls++; return () => {}; } });
  // Exported only through accessors.
  class Lazy {}
  Lazy.prototype[kCloneable] = descriptor('lazy:Lazy', () => 1, () => {});

  ({ Point, Holder, TestError, AmountError, Strict, Throws, NoDeserialize, NotADescriptor,
      NoDeserializeFunction, LazyDeserialize, notAFunction: 1 })
";

/// Defines `JS_CLASSES` in the context, with the private symbols it uses as globals, and returns
/// the exports of the module "test".
fn define_classes<'s>(lock: &mut jsg::Lock<'s>, ctx: &EvalContext<'_>) -> v8::Local<'s, v8::Value> {
    for (global, name) in [
        ("kMode", "workerd:serializeMode"),
        ("kCloneable", "workerd:cloneable"),
        ("kDomExceptionPrototype", "workerd:domExceptionPrototype"),
        ("kBrand", "workerd:domException"),
        ("kName", "workerd:errorName"),
        ("kMessage", "workerd:errorMessage"),
        ("kX", "test:x"),
    ] {
        let symbol = Harness::private_symbol(lock, name);
        ctx.set_global(lock, global, symbol);
    }
    eval(lock, ctx, JS_CLASSES)
}

/// Defines `JS_CLASSES` in the context, as a loaded bootstrap module named "test".
fn set_up_classes(lock: &mut jsg::Lock<'_>, ctx: &EvalContext<'_>) {
    let exports = define_classes(lock, ctx);
    ctx.set_bootstrap_module(lock, "test", exports);
}

/// Makes an `AmountError` as the bootstrap's `QuotaExceededError` constructor makes one: an
/// instance of the bootstrap's `DOMException`, which [`Harness::new_bootstrap_dom_exception`]
/// stands in for, marked as cloneable.
fn new_amount_error<'s>(
    lock: &mut jsg::Lock<'s>,
    ctx: &EvalContext<'_>,
    message: &str,
    amount: u32,
) -> v8::Local<'s, v8::Value> {
    let prototype = eval(lock, ctx, "AmountError.prototype");
    let error = Harness::new_bootstrap_dom_exception(
        lock,
        prototype,
        "AmountError",
        message,
        "sender stack",
    );
    ctx.set_global(lock, "newError", error);
    eval(
        lock,
        ctx,
        &format!("newError[kMode] = amountErrorCloneable; newError[kX] = {amount}; newError"),
    )
}

/// Returns the calls that `log` records, and clears it.
fn take_log(lock: &mut jsg::Lock<'_>, ctx: &EvalContext<'_>) -> String {
    ctx.eval(lock, "log.splice(0).join()").unwrap()
}

/// Serializes the value of `expr` with Rust JSG and with C++ JSG, both of which must throw
/// `name: message`.
fn assert_serialization_throws(
    lock: &mut jsg::Lock<'_>,
    ctx: &EvalContext<'_>,
    expr: &str,
    name: &str,
    message: &str,
) {
    let value = eval(lock, ctx, expr);
    ctx.set_global(lock, "value", value);
    let exception = serialize_exception(lock, ctx, "value");
    assert_error(lock, ctx, exception, name, message);
    let caught = lock
        .try_catch(|lock| Harness::cpp_try_serialize(lock, value))
        .unwrap();
    let exception = caught.expect_err("C++ JSG should throw");
    assert_error(lock, ctx, exception, name, message);
}

/// What `jsg::Serializer` writes for an instance of a class implemented in JavaScript, after the
/// header: V8's tag for a host object, then the record, which is its tag, its length-delimited
/// `deserializeInfo`, its flags, and the values the flags say follow. Each length and the flags
/// must be below 128, a byte each as varints.
fn record(
    lock: &mut jsg::Lock<'_>,
    deserialize_info: &str,
    flags: u8,
    fields: &[&[u8]],
) -> Vec<u8> {
    let length = u8::try_from(deserialize_info.len()).unwrap();
    assert!(length < 128 && flags < 128);
    let mut data = header(lock);
    data.extend_from_slice(&[b'\\', 0x17, length]);
    data.extend_from_slice(deserialize_info.as_bytes());
    data.push(flags);
    for field in fields {
        data.extend_from_slice(field);
    }
    data
}

/// `string`, which must be ASCII and shorter than 128 bytes, in V8's format.
fn v8_string(string: &str) -> Vec<u8> {
    assert!(string.is_ascii() && string.len() < 128);
    let mut data = vec![b'"', u8::try_from(string.len()).unwrap()];
    data.extend_from_slice(string.as_bytes());
    data
}

/// Describes `exception`, which may be anything, by its name and message.
fn describe<'s>(
    lock: &mut jsg::Lock<'s>,
    ctx: &EvalContext<'_>,
    exception: v8::Local<'s, v8::Value>,
) -> String {
    ctx.set_global(lock, "e", exception);
    ctx.eval(lock, "`${e?.name}: ${e?.message}`").unwrap()
}

/// The integer 1 in V8's format.
const V8_ONE: &[u8] = b"I\x02";
/// An empty object in V8's format.
const V8_EMPTY_OBJECT: &[u8] = b"o{\x00";

const UNKNOWN_CLASS: &str = "Deserialization failed: unknown class implemented in JavaScript.";

#[test]
fn instances_round_trip() {
    Harness::new().run_in_context(|lock, ctx| {
        set_up_classes(lock, ctx);
        // An instance is made from its class's prototype without calling the constructor, and
        // initialized from its data.
        assert_round_trip(
            lock,
            ctx,
            "new Point(1)",
            "v instanceof Point && Object.getPrototypeOf(v) === Point.prototype && v.x === 2 && \
             constructed === 1",
        );

        // The copy is cloneable too.
        let value = eval(lock, ctx, "new Point(1)");
        let data = jsg::ser::serialize(lock, value)?;
        let copy = jsg::ser::deserialize(lock, &data)?;
        ctx.set_global(lock, "copy", copy);
        assert_round_trip(lock, ctx, "copy", "v.x === 3");

        for (expr, check) in [
            // A subclass's instance is written as one of the class whose descriptor marked it.
            (
                "new (class Sub extends Point {})(1)",
                "Object.getPrototypeOf(v) === Point.prototype && v.x === 2",
            ),
            // So is an instance whose prototype has changed since.
            (
                "Object.setPrototypeOf(new Point(1), Holder.prototype)",
                "Object.getPrototypeOf(v) === Point.prototype && v.x === 2",
            ),
            // An instance that appears twice is written once, and read back as one object.
            (
                "(() => { const p = new Point(1); return [p, p]; })()",
                "v[0] === v[1] && v[0].x === 2",
            ),
            // Its data may refer to the instance itself, and to other instances.
            (
                "(() => { const h = new Holder(null); h[kX] = h; return h; })()",
                "v.value === v",
            ),
            (
                "new Holder([new Point(1), new Holder(new Point(5))])",
                "v.value[0].x === 2 && v.value[1].value.x === 6",
            ),
            (
                "(() => { const p = new Point(1); return { p, h: new Holder(p) }; })()",
                "v.h.value === v.p",
            ),
            // A plain class instance is still written as a plain object.
            (
                "new (class Plain { constructor() { this.a = 1; } })()",
                "JSON.stringify(v) === '{\"a\":1}'",
            ),
            // A mark on an object whose prototype is Object.prototype or null is ignored.
            (
                "(() => { const o = { a: 1 }; o[kMode] = 0; return o; })()",
                "v.a === 1",
            ),
            (
                "(() => { const n = Object.create(null); n[kMode] = 0; return n; })()",
                "Object.keys(v).length === 0",
            ),
        ] {
            assert_round_trip(lock, ctx, expr, check);
        }

        // The data of each instance follows the top-level value, in the order the instances were
        // written. Once everything has been read, the deserialize functions run in that order.
        let value = eval(
            lock,
            ctx,
            "[new Point(1), new Holder(new Point(2)), new Point(3)]",
        );
        take_log(lock, ctx);
        let data = jsg::ser::serialize(lock, value)?;
        assert_eq!(take_log(lock, ctx), "clone 1,clone 3,clone 2");
        assert_eq!(data, Harness::cpp_serialize(lock, value));
        take_log(lock, ctx);
        jsg::ser::deserialize(lock, &data)?;
        assert_eq!(
            take_log(lock, ctx),
            "deserialize 1,deserialize 3,deserialize 2"
        );
        Harness::cpp_deserialize(lock, &data);
        assert_eq!(
            take_log(lock, ctx),
            "deserialize 1,deserialize 3,deserialize 2"
        );
        Ok(())
    });
}

#[test]
fn errors_round_trip() {
    for enhanced in [false, true] {
        let harness = Harness::with_flags(move |mut flags| {
            flags.set_enhanced_error_serialization(enhanced);
        });
        harness.run_in_context(|lock, ctx| {
            set_up_classes(lock, ctx);
            for (expr, check) in [
                // A class inheriting from Error makes native errors, with no own message until
                // initialized.
                (
                    "new TestError('boom')",
                    "v instanceof TestError && Object.prototype.toString.call(v) === \
                     '[object Error]' && v.message === 'boom'",
                ),
                // Its stack is written as a native error's is, and restored as an own data
                // property that, like the stack it replaces, is not enumerable.
                (
                    "Object.assign(new TestError('boom'), { stack: 'sender stack' })",
                    "(() => { \
                       const { value, enumerable } = Object.getOwnPropertyDescriptor(v, 'stack'); \
                       return value === 'sender stack' && !enumerable; \
                     })()",
                ),
                // Without one, it keeps the stack captured when it was made, whose first line, as
                // a native error's does, gives its name and the writer's message.
                (
                    "(() => { const e = new TestError('boom'); delete e.stack; return e; })()",
                    "v.stack.split('\\n')[0] === 'TestError: boom' && \
                     !Object.getOwnPropertyDescriptor(v, 'stack').enumerable",
                ),
                // A stack that is not a string is not written, so one that refers back to the
                // error, which a reader could not resolve, does not make the error unreadable.
                (
                    "(() => { const e = new TestError('boom'); e.stack = { e }; return e; })()",
                    "v.stack.split('\\n')[0] === 'TestError: boom'",
                ),
            ] {
                assert_round_trip(lock, ctx, expr, check);
            }

            // A class whose prototype inherits from the bootstrap's DOMException.prototype makes
            // instances of the bootstrap's DOMException.
            let error = new_amount_error(lock, ctx, "over", 5);
            ctx.set_global(lock, "error", error);
            eval(
                lock,
                ctx,
                "globalThis.isCopy = (e) => \
                   Object.getPrototypeOf(e) === AmountError.prototype && e[kBrand] === true && \
                   e.name === 'AmountError' && e.message === 'over' && e.amount === 5 && \
                   e.stack === 'sender stack' && \
                   !Object.getOwnPropertyDescriptor(e, 'stack').enumerable",
            );
            for (expr, check) in [
                ("error", "isCopy(v)"),
                (
                    "({ error, again: error })",
                    "isCopy(v.error) && v.again === v.error",
                ),
            ] {
                assert_round_trip(lock, ctx, expr, check);
            }
            Ok(())
        });
    }

    // With enhanced error serialization, an instance can be inside a native error, which the
    // serializer writes itself.
    let harness = Harness::with_flags(|mut flags| flags.set_enhanced_error_serialization(true));
    harness.run_in_context(|lock, ctx| {
        set_up_classes(lock, ctx);
        let error = new_amount_error(lock, ctx, "over", 5);
        ctx.set_global(lock, "error", error);
        assert_round_trip(
            lock,
            ctx,
            "new Error('outer', { cause: [new Point(1), error] })",
            "v.cause[0] instanceof Point && v.cause[0].x === 2 && v.cause[1].amount === 5",
        );
        Ok(())
    });
}

/// Without `preserve_stack_in_errors`, the reader keeps an instance's stack only if the writer was
/// not using enhanced error serialization, which keeps a native error's stack, as
/// `jsg::Deserializer` does.
#[test]
fn errors_keep_their_stacks_as_native_errors_do() {
    for (enhanced, check) in [
        (false, "v.stack === 'sender stack'"),
        (true, "v.stack.split('\\n')[0] === 'TestError: boom'"),
    ] {
        let harness = Harness::with_flags(move |mut flags| {
            flags.set_enhanced_error_serialization(enhanced);
        });
        harness.run_in_context(|lock, ctx| {
            set_up_classes(lock, ctx);
            let value = eval(
                lock,
                ctx,
                "Object.assign(new TestError('boom'), { stack: 'sender stack' })",
            );
            let data = jsg::ser::serialize(lock, value)?;
            assert_eq!(data, Harness::cpp_serialize(lock, value));
            let options = jsg::ser::DeserializeOptions {
                preserve_stack_in_errors: false,
            };
            let copy = jsg::ser::deserialize_with(lock, &data, options)?;
            ctx.set_global(lock, "v", copy);
            assert!(ctx.eval::<bool>(lock, check).unwrap(), "{check}");
            Ok(())
        });
    }
}

/// The record's flags say whether a stack follows (1), whether the writer was not using enhanced
/// error serialization (2), and whether a message follows (4).
#[test]
fn records_are_read_as_their_flags_say() {
    Harness::new().run_in_context(|lock, ctx| {
        set_up_classes(lock, ctx);
        let message = v8_string("header");
        let stack = v8_string("forged stack");
        let data = v8_string("m");

        // A stack is kept, with preserve_stack_in_errors.
        let mut bytes = record(lock, "test:TestError", 1, &[&stack]);
        bytes.extend_from_slice(&data);
        assert_read(
            lock,
            ctx,
            &bytes,
            "v.stack === 'forged stack' && v.message === 'm'",
        );

        // A stack or message of a class that doesn't inherit from Error is ignored.
        let mut bytes = record(lock, "test:Point", 5, &[&message, &stack]);
        bytes.extend_from_slice(V8_ONE);
        assert_read(
            lock,
            ctx,
            &bytes,
            "!Object.hasOwn(v, 'stack') && !Object.hasOwn(v, 'message') && v.x === 2",
        );

        // Without preserve_stack_in_errors, a stack is kept only with flag 2. Otherwise the stack
        // captured when the error is made begins with the record's message, not the one
        // deserialize() sets.
        for (flags, fields, check) in [
            (
                5,
                &[&message[..], &stack][..],
                "v.stack.split('\\n')[0] === 'TestError: header' && !v.stack.includes('forged') \
                 && v.message === 'm'",
            ),
            (
                1,
                &[&stack[..]],
                "v.stack.split('\\n')[0] === 'TestError' && v.message === 'm'",
            ),
            (
                3,
                &[&stack[..]],
                "v.stack === 'forged stack' && v.message === 'm'",
            ),
        ] {
            let mut bytes = record(lock, "test:TestError", flags, fields);
            bytes.extend_from_slice(&data);
            let options = jsg::ser::DeserializeOptions {
                preserve_stack_in_errors: false,
            };
            let copy = jsg::ser::deserialize_with(lock, &bytes, options)?;
            ctx.set_global(lock, "v", copy);
            assert!(ctx.eval::<bool>(lock, check).unwrap(), "{flags}: {check}");
        }

        // A message or stack that isn't a string is rejected, and so are flags this version
        // doesn't know.
        for (bytes, message) in [
            (
                record(lock, "test:TestError", 4, &[V8_ONE]),
                "Deserialization failed: message of an object implemented in JavaScript is not a \
                 string.",
            ),
            (
                record(lock, "test:TestError", 1, &[V8_EMPTY_OBJECT]),
                "Deserialization failed: stack of an object implemented in JavaScript is not a \
                 string.",
            ),
            (
                record(lock, "test:Point", 8, &[]),
                "Deserialization failed: unsupported object implemented in JavaScript.",
            ),
        ] {
            assert_deserialization_throws(lock, ctx, &bytes, "DataCloneError", message);
        }

        // A record that ends too soon is malformed wherever it ends: in the flags, in the
        // deserializeInfo, in its length, or in the tag before them.
        let bytes = record(lock, "test:Point", 0, &[]);
        for drop in [1, 2, 12, 13] {
            assert_deserialization_throws(
                lock,
                ctx,
                &bytes[..bytes.len() - drop],
                "DataCloneError",
                "Deserialization failed: the data is malformed.",
            );
        }
        // Cut off before V8's tag, it is V8's own error.
        deserialize_exception(lock, &bytes[..bytes.len() - 14]);
        Ok(())
    });
}

#[test]
fn unknown_classes_throw_data_clone_error() {
    Harness::new().run_in_context(|lock, ctx| {
        set_up_classes(lock, ctx);
        // Exports that are accessors, and a workerd API object.
        let exports = eval(
            lock,
            ctx,
            "({ get Lazy() { getterCalls++; return Lazy; } })",
        );
        ctx.set_bootstrap_module(lock, "lazy", exports);
        let exports = eval(
            lock,
            ctx,
            "Object.defineProperty({}, 'Lazy', { get() { getterCalls++; return Lazy; } })",
        );
        ctx.set_bootstrap_module(lock, "lazy2", exports);
        let api_object = jsg::Rc::new(lock, Unserializable);
        ctx.set_global(lock, "apiObject", api_object);
        let exports = eval(lock, ctx, "Object.assign(apiObject, { Point })");
        ctx.set_bootstrap_module(lock, "api", exports);
        // A class whose descriptor is a workerd API object.
        let api_object = jsg::Rc::new(lock, Unserializable);
        ctx.set_global(lock, "apiDescriptor", api_object);
        let exports = eval(
            lock,
            ctx,
            "class Wrapped {}
             Wrapped.prototype[kCloneable] = Object.assign(apiDescriptor, { deserialize() {} });
             ({ Wrapped })",
        );
        ctx.set_bootstrap_module(lock, "wrapped", exports);

        // Data naming anything but a loaded class whose descriptor has a deserialize function, all
        // found through own data properties of ordinary objects, is rejected.
        for deserialize_info in [
            "test:NoDeserialize",
            "test:NotADescriptor",
            "test:NoDeserializeFunction",
            "test:LazyDeserialize",
            "test:notAFunction",
            "test:Missing",
            "test:toString",
            "lazy:Lazy",
            "lazy2:Lazy",
            "api:Point",
            "wrapped:Wrapped",
            "other:Point",
            "test",
            "",
        ] {
            let value = eval(lock, ctx, &format!("new Forged('{deserialize_info}')"));
            let data = jsg::ser::serialize(lock, value)?;
            assert_eq!(
                data,
                Harness::cpp_serialize(lock, value),
                "{deserialize_info}"
            );
            assert_deserialization_throws(lock, ctx, &data, "DataCloneError", UNKNOWN_CLASS);
        }
        assert!(ctx.eval::<bool>(lock, "getterCalls === 0").unwrap());
        Ok(())
    });

    // Without the modules to find classes in, nothing can be deserialized.
    Harness::new().run_in_context(|lock, ctx| {
        define_classes(lock, ctx);
        let value = eval(lock, ctx, "new Point(1)");
        let data = jsg::ser::serialize(lock, value)?;
        assert_deserialization_throws(lock, ctx, &data, "DataCloneError", UNKNOWN_CLASS);
        Ok(())
    });
}

#[test]
fn invalid_instances_throw_data_clone_error() {
    Harness::new().run_in_context(|lock, ctx| {
        set_up_classes(lock, ctx);
        let not_supported = |class: &str| {
            format!(
                "Could not serialize object of type \"{class}\". This type does not support \
                 serialization."
            )
        };
        // An instance whose mark is not a descriptor.
        assert_serialization_throws(
            lock,
            ctx,
            "new Opaque()",
            "DataCloneError",
            &not_supported("Opaque"),
        );
        // Nor is one whose descriptor lacks a clone function or a deserializeInfo string, or isn't
        // frozen, so that the serializer reads each descriptor once. Nor is a descriptor that is a
        // workerd API object, whose interceptors could run code, or whose clone is an accessor.
        let api_object = jsg::Rc::new(lock, Unserializable);
        ctx.set_global(lock, "apiObject", api_object);
        for mark in [
            "'yes'",
            "{}",
            "descriptor('test:Point')",
            "descriptor(1, () => 1)",
            "{ __proto__: null, deserializeInfo: 'test:Point', clone: () => 1 }",
            "Object.freeze(Object.assign(apiObject, { deserializeInfo: 'test:Point', \
             clone: () => 1 }))",
            "Object.freeze({ __proto__: null, deserializeInfo: 'test:Point', \
             get clone() { getterCalls++; return () => 1; } })",
        ] {
            assert_serialization_throws(
                lock,
                ctx,
                &format!(
                    "(() => {{ const n = new NoDeserialize(); n[kMode] = {mark}; return n; }})()"
                ),
                "DataCloneError",
                &not_supported("NoDeserialize"),
            );
        }
        assert!(ctx.eval::<bool>(lock, "getterCalls === 0").unwrap());

        // A mark on a workerd API object is ignored.
        let resource = jsg::Rc::new(lock, Unserializable);
        ctx.set_global(lock, "resource", resource);
        assert_serialization_throws(
            lock,
            ctx,
            "(() => { resource[kMode] = descriptor('test:Point', () => 1, () => {}); \
               return [resource]; })()",
            "DataCloneError",
            &not_supported("Unserializable"),
        );

        // A Proxy is not an instance, whatever it wraps.
        assert_serialization_throws(
            lock,
            ctx,
            "new Proxy(new Point(1), {})",
            "DataCloneError",
            "#<Point> could not be cloned.",
        );
        Ok(())
    });
}

#[test]
fn what_clone_and_deserialize_throw_stays_pending() {
    let harness = Harness::with_flags(|mut flags| flags.set_enhanced_error_serialization(true));
    harness.run_in_context(|lock, ctx| {
        set_up_classes(lock, ctx);
        assert_serialization_throws(
            lock,
            ctx,
            "new CloneThrows()",
            "RangeError",
            "throw from clone",
        );
        // So does what reading an error's message throws.
        assert_serialization_throws(
            lock,
            ctx,
            "Object.defineProperty(new TestError('e'), 'message', \
               { get() { throw new URIError('u'); } })",
            "URIError",
            "u",
        );

        // What a deserialize function throws reaches the caller, such as its rejection of data
        // that no instance of its class would have written.
        for (expr, name, message) in [
            ("new Throws()", "Error", "throw from deserialize"),
            (
                "new Forged('test:Strict', 'nope')",
                "TypeError",
                "Strict data is not a number",
            ),
        ] {
            let value = eval(lock, ctx, expr);
            let data = jsg::ser::serialize(lock, value)?;
            assert_eq!(data, Harness::cpp_serialize(lock, value), "{expr}");
            assert_deserialization_throws(lock, ctx, &data, name, message);
        }
        Ok(())
    });
}

/// A caller that has disallowed JavaScript execution, which `deserialize` functions need, cannot
/// read instances, as with C++ JSG.
#[test]
fn instances_cannot_be_read_while_javascript_is_disallowed() {
    Harness::new().run_in_context(|lock, ctx| {
        set_up_classes(lock, ctx);
        let value = eval(lock, ctx, "new Point(1)");
        let data = jsg::ser::serialize(lock, value)?;
        take_log(lock, ctx);

        Harness::set_javascript_execution_disallowed(lock, true);
        let rust = lock
            .try_catch(|lock| jsg::ser::deserialize(lock, &data))
            .unwrap();
        let cpp = lock
            .try_catch(|lock| Harness::cpp_try_deserialize(lock, &data))
            .unwrap();
        Harness::set_javascript_execution_disallowed(lock, false);

        for caught in [rust, cpp] {
            assert_error(
                lock,
                ctx,
                caught.expect_err("deserialization should throw"),
                "DataCloneError",
                "Objects implemented in JavaScript cannot be deserialized here.",
            );
        }
        assert_eq!(take_log(lock, ctx), "");
        Ok(())
    });
}

/// Whatever class and data a peer names, Rust JSG reads it as C++ JSG does: both read a value, or
/// both throw the same exception. The peer is a `Forged` instance naming any class, or a random
/// string, with any data. The pseudo-random sequence is fixed, so every run reads the same data.
#[test]
fn arbitrary_instances_are_read_as_cpp_jsg_reads_them() {
    let harness = Harness::with_flags(|mut flags| flags.set_enhanced_error_serialization(true));
    harness.run_in_context(|lock, ctx| {
        set_up_classes(lock, ctx);
        eval(
            lock,
            ctx,
            r"
              let seed = 1;
              function random(n) {
                seed = (seed * 48271) % 2147483647;
                return seed % n;
              }
              const names = ['test:Point', 'test:Holder', 'test:TestError', 'test:AmountError',
                  'test:Strict', 'test:Throws', 'test:NoDeserialize', 'test:NotADescriptor',
                  'test:NoDeserializeFunction', 'test:notAFunction', 'test:Missing',
                  'test:toString', 'other:Point', 'test', '', ':', 'test:', 'test:Point:',
                  '\0test:Point'];
              function randomName() {
                if (random(4) > 0) return names[random(names.length)];
                let name = '';
                for (let n = random(12); n > 0; n--) name += 'test:Point\0é\u{1f600}'[random(13)];
                return name;
              }
              const values = [
                () => undefined, () => null, () => random(100), () => -0.5, () => 'x', () => ({}),
                () => [], () => ({ value: new Point(1) }), () => new Point(2),
                () => new Holder(null), () => new Strict(3), () => new TestError('e'),
                () => new Error('e'), () => new Uint8Array(2), () => new Map([[1, new Point(4)]]),
                () => ({ message: 'm', amount: 1 }), () => new Forged(randomName(), random(10)),
                () => new Forged(randomName(), new Forged(randomName())),
              ];
              function nextForged() {
                const forged = new Forged(randomName(), values[random(values.length)]());
                return [forged, forged];
              }
            ",
        );
        let mut read = 0;
        let mut threw = 0;
        for i in 0..500 {
            let value = eval(lock, ctx, "nextForged()");
            let data = jsg::ser::serialize(lock, value)?;
            assert_eq!(data, Harness::cpp_serialize(lock, value), "{i}");
            let rust = lock
                .try_catch(|lock| jsg::ser::deserialize(lock, &data))
                .unwrap();
            let cpp = lock
                .try_catch(|lock| Harness::cpp_try_deserialize(lock, &data))
                .unwrap();
            match (rust, cpp) {
                (Ok(_), Ok(_)) => read += 1,
                (Err(rust), Err(cpp)) => {
                    let rust = describe(lock, ctx, rust);
                    assert_eq!(rust, describe(lock, ctx, cpp), "{i}");
                    threw += 1;
                }
                (rust, cpp) => panic!("{i}: Rust JSG read {rust:?}, C++ JSG read {cpp:?}"),
            }
        }
        assert!(read > 0 && threw > 0, "{read} {threw}");
        Ok(())
    });
}
