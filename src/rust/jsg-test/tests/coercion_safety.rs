// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Regression tests for the Rust JSG `unwrap_*` boundary: hostile JS values
//! must not abort the process (SIGABRT) as they previously did. Specifically:
//! coercion failures and wrong types must yield an `Err`, while lone
//! surrogates are a deliberate exception -- they decode *lossily* to U+FFFD
//! (`Ok`), rather than erroring.
//!
//! What JavaScript throws during a coercion or property access stays pending, as for
//! `Local<Function>::call`: the `Err` is one for which `is_pending()` is true, and the tests
//! catch the thrown value with `Lock::try_catch`.
//!
//! `unwrap_string`/`unwrap_number` call V8's `ToString`/`ToNumber`, which run
//! arbitrary JS whenever the value isn't already a primitive of the right
//! type (`toString`/`valueOf`/`Symbol.toPrimitive`, including monkeypatched
//! ones on built-ins). That JS can do anything, including calling back into a
//! Rust-backed `#[jsg_method]`, which may itself trigger another coercion
//! (nested `unwrap_string`/`unwrap_number`) or panic. The tests below cover
//! that "arbitrary JS during coercion" path specifically, as opposed to the
//! simple throw/reject cases above.

use std::cell::Cell;

use jsg::ExceptionType;
use jsg::FromJS;
use jsg::ToJS;
use jsg::v8;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;

/// Runs `f` in `try_catch`. `f` must fail with an error for a pending exception; returns what
/// JavaScript threw.
fn thrown_by<'s, T: std::fmt::Debug>(
    lock: &mut jsg::Lock<'s>,
    f: impl FnOnce(&mut jsg::Lock<'s>) -> jsg::Result<T>,
) -> v8::Local<'s, v8::Value> {
    let caught = lock
        .try_catch(|lock| {
            let err = f(lock).expect_err("should fail");
            assert!(err.is_pending(), "not pending: {err}");
            Err::<(), _>(err)
        })
        .expect("execution should not be terminating");
    caught.expect_err("should have caught an exception")
}

/// Converts the value of `expr` to `T`, which must throw, and returns what JavaScript threw.
fn conversion_throws<'s, T: FromJS<'s>>(
    lock: &mut jsg::Lock<'s>,
    ctx: &crate::EvalContext<'_>,
    expr: &str,
) -> v8::Local<'s, v8::Value>
where
    T::ResultType: std::fmt::Debug,
{
    let value = ctx
        .eval_raw(lock, expr)
        .expect("expression should evaluate");
    thrown_by(lock, |lock| T::from_js(lock, value))
}

/// Returns whether `value` is the value of the global `thrown`, by `Object.is()`.
fn is_thrown<'s>(
    lock: &mut jsg::Lock<'s>,
    ctx: &crate::EvalContext<'_>,
    value: v8::Local<'s, v8::Value>,
) -> bool {
    ctx.set_global(lock, "caught", value);
    ctx.eval::<bool>(lock, "Object.is(caught, thrown)")
        .expect("should evaluate")
}

/// Coercion failures must surface as an error, never abort.
#[test]
fn string_from_hostile_values_returns_err() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        // Values with no primitive representation / that throw during coercion.
        let hostile = [
            "Object.create(null)",
            "Symbol('s')",
            "({ toString() { throw new TypeError('boom'); } })",
            "({ [Symbol.toPrimitive]() { throw new RangeError('nope'); } })",
            "new Proxy({}, { get() { throw new Error('trap'); } })",
        ];
        for expr in hostile {
            conversion_throws::<String>(lock, ctx, expr);
        }
        Ok(())
    });
}

/// Converting to a string or a number throws what `toString`, `valueOf`, `Symbol.toPrimitive`
/// or a Proxy `get` trap (which `ToPrimitive` runs to look up `Symbol.toPrimitive`) threw,
/// whether or not it is an `Error`.
#[test]
fn coercion_throws_thrown_value() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        for setup in ["new RangeError('boom')", "({ code: 7 })", "42"] {
            ctx.eval_raw(lock, &format!("globalThis.thrown = {setup}"))
                .expect("setup should evaluate");
            for expr in [
                "({ toString() { throw thrown; } })",
                "({ [Symbol.toPrimitive]() { throw thrown; } })",
                "new Proxy({}, { get() { throw thrown; } })",
            ] {
                let caught = conversion_throws::<String>(lock, ctx, expr);
                assert!(is_thrown(lock, ctx, caught), "{setup} {expr}");
            }
            for expr in [
                "({ valueOf() { throw thrown; } })",
                "new Proxy({}, { get() { throw thrown; } })",
            ] {
                let caught = conversion_throws::<f64>(lock, ctx, expr);
                assert!(is_thrown(lock, ctx, caught), "{setup} {expr}");
                let caught = conversion_throws::<i32>(lock, ctx, expr);
                assert!(is_thrown(lock, ctx, caught), "{setup} {expr}");
            }
        }
        Ok(())
    });
}

/// A coercion that V8 itself rejects throws V8's `TypeError`.
#[test]
fn coercion_throws_v8_type_error() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let caught = conversion_throws::<String>(lock, ctx, "Symbol('s')");
        let err = jsg::Error::from_js(lock, caught)?;
        assert_eq!(err.name, ExceptionType::TypeError);
        assert_eq!(err.message, "Cannot convert a Symbol value to a string");
        Ok(())
    });
}

/// A lone UTF-16 surrogate must not abort; it decodes lossily to U+FFFD.
#[test]
fn string_from_lone_surrogate_is_lossy_not_abort() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let value = ctx
            .eval_raw(lock, "'\\uD800'")
            .expect("expression should evaluate");
        let s = <String as FromJS>::from_js(lock, value)
            .expect("lone surrogate should decode lossily, not error");
        assert_eq!(s, "\u{FFFD}");

        // A surrogate embedded in otherwise valid text.
        let value = ctx
            .eval_raw(lock, "'a\\uD800b'")
            .expect("expression should evaluate");
        let s = <String as FromJS>::from_js(lock, value).expect("should decode lossily");
        assert_eq!(s, "a\u{FFFD}b");
        Ok(())
    });
}

/// `ToNumber` on a `Symbol`/`BigInt` throws in JS.
#[test]
fn number_from_hostile_values_returns_err() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let hostile = [
            "Symbol('s')",
            "123n",
            "({ valueOf() { throw new TypeError('no number'); } })",
        ];
        for expr in hostile {
            conversion_throws::<f64>(lock, ctx, expr);
        }
        Ok(())
    });
}

/// Integer conversions go through the same `unwrap_number`.
#[test]
fn integer_from_symbol_returns_err() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        conversion_throws::<u32>(lock, ctx, "Symbol('s')");
        Ok(())
    });
}

/// A wrong-typed typed-array argument must be an error, not an abort. No JavaScript runs, so
/// the error is not pending.
#[test]
fn typed_array_from_wrong_type_returns_err() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        // Not a Uint8Array: a plain array, a number, and a Symbol.
        for expr in ["[1, 2, 3]", "42", "Symbol('s')"] {
            let value = ctx.eval_raw(lock, expr).expect("should evaluate");
            let result = <v8::Local<v8::Uint8Array> as FromJS>::from_js(lock, value);
            assert!(
                result.as_ref().is_err_and(|err| !err.is_pending()),
                "Local::<Uint8Array>::from_js({expr}) should be a non-pending Err, got {result:?}"
            );
        }
        Ok(())
    });
}

/// An accessor property that throws while `Vec<T>::from_js`'s underlying
/// `Array::Iterate()` reads an element throws what the accessor threw, and the
/// isolate stays usable.
#[test]
fn array_iterate_throwing_element_getter_returns_err() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        ctx.eval_raw(lock, "globalThis.thrown = new RangeError('trap')")
            .expect("setup should evaluate");
        let caught = conversion_throws::<Vec<String>>(
            lock,
            ctx,
            "(() => { \
                 const arr = [1, 2, 3]; \
                 Object.defineProperty(arr, 1, { get() { throw thrown; } }); \
                 return arr; \
             })()",
        );
        assert!(is_thrown(lock, ctx, caught));

        // The isolate must still be usable afterwards.
        let ok = ctx.eval_raw(lock, "'still fine'").expect("should evaluate");
        let s = <String as FromJS>::from_js(lock, ok).expect("should coerce cleanly");
        assert_eq!(s, "still fine");
        Ok(())
    });
}

/// A coercion failure must leave the isolate usable for later evaluations.
#[test]
fn coercion_failure_does_not_poison_isolate() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        conversion_throws::<String>(lock, ctx, "Symbol('s')");

        // A subsequent, well-formed coercion still works.
        let ok = ctx.eval_raw(lock, "'still fine'").expect("should evaluate");
        let s = <String as FromJS>::from_js(lock, ok).expect("should coerce cleanly");
        assert_eq!(s, "still fine");
        Ok(())
    });
}

// =============================================================================
// Coercion callbacks running arbitrary JS, including re-entrant calls back
// into Rust-backed `#[jsg_method]`s.
// =============================================================================

/// A Rust-backed resource whose methods are called from *inside* a
/// `toString`/`valueOf`/`Symbol.toPrimitive` callback below, to exercise real
/// re-entrancy across the Rust/C++/V8 FFI boundary during coercion.
#[jsg_resource]
struct ReentrantResource {
    call_count: Cell<u32>,
}

#[jsg_resource]
impl ReentrantResource {
    /// Takes a `String` argument. When called with a non-string value, argument
    /// conversion itself goes through the slow (`ToString`-coercing) path of
    /// `unwrap_string`, nesting a second coercion inside the caller's.
    #[jsg_method]
    pub fn echo(&self, message: String) -> String {
        self.call_count.set(self.call_count.get() + 1);
        format!("[{message}]")
    }

    /// Takes an `f64` argument; same nesting trick as `echo` but for
    /// `unwrap_number`/`ToNumber`.
    #[jsg_method]
    pub fn double(&self, n: f64) -> f64 {
        self.call_count.set(self.call_count.get() + 1);
        n * 2.0
    }

    #[jsg_method]
    pub fn call_count(&self) -> f64 {
        f64::from(self.call_count.get())
    }

    /// Unconditionally panics, to test that a panic triggered *while V8 is
    /// running `ToString`/`ToNumber` on behalf of `unwrap_string`/`unwrap_number`*
    /// is still converted into a catchable error rather than aborting.
    #[jsg_method]
    pub fn panic_now(&self) {
        panic!("intentional panic during coercion re-entrancy test");
    }
}

/// A monkeypatched `toString` that runs arbitrary JS with an observable side
/// effect must actually execute (not merely fail safely) and its return value
/// must be used for the coercion.
#[test]
fn string_from_tostring_side_effects_are_observed() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        ctx.eval_raw(lock, "globalThis.__sideEffect = 0;")
            .expect("should evaluate");
        let value = ctx
            .eval_raw(
                lock,
                "({ toString() { \
                     globalThis.__sideEffect += 1; \
                     return 'computed-' + globalThis.__sideEffect; \
                 } })",
            )
            .expect("expression should evaluate");

        let s = <String as FromJS>::from_js(lock, value).expect("coercion should succeed");
        assert_eq!(s, "computed-1");

        let side_effect: f64 = ctx.eval(lock, "globalThis.__sideEffect").unwrap();
        assert!((side_effect - 1.0).abs() < f64::EPSILON);
        Ok(())
    });
}

/// Same as above, but for `ToNumber`/`valueOf`.
#[test]
fn number_from_valueof_side_effects_are_observed() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        ctx.eval_raw(lock, "globalThis.__sideEffect = 0;")
            .expect("should evaluate");
        let value = ctx
            .eval_raw(
                lock,
                "({ valueOf() { \
                     globalThis.__sideEffect += 1; \
                     return 41 + globalThis.__sideEffect; \
                 } })",
            )
            .expect("expression should evaluate");

        let n = <f64 as FromJS>::from_js(lock, value).expect("coercion should succeed");
        assert!((n - 42.0).abs() < f64::EPSILON);

        let side_effect: f64 = ctx.eval(lock, "globalThis.__sideEffect").unwrap();
        assert!((side_effect - 1.0).abs() < f64::EPSILON);
        Ok(())
    });
}

/// A `toString` callback that calls back into a Rust-backed method, whose
/// argument itself requires coercion, nests a second `unwrap_string` call
/// inside the first. Both must complete correctly and the resource method
/// must observe exactly one call.
#[test]
fn string_coercion_reentrant_into_rust_resource_method() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Rc::new(ReentrantResource {
            call_count: Cell::new(0),
        });
        let wrapped = resource.to_js(lock)?;
        ctx.set_global(lock, "resource", wrapped);

        let value = ctx
            .eval_raw(
                lock,
                "({ toString() { \
                     return resource.echo({ toString() { return 'inner'; } }); \
                 } })",
            )
            .expect("expression should evaluate");

        let s = <String as FromJS>::from_js(lock, value).expect("nested coercion should succeed");
        assert_eq!(s, "[inner]");

        let count: f64 = ctx.eval(lock, "resource.callCount()").unwrap();
        assert!((count - 1.0).abs() < f64::EPSILON);
        Ok(())
    });
}

/// Same as above, but for `ToNumber`/`valueOf`/`unwrap_number`.
#[test]
fn number_coercion_reentrant_into_rust_resource_method() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Rc::new(ReentrantResource {
            call_count: Cell::new(0),
        });
        let wrapped = resource.to_js(lock)?;
        ctx.set_global(lock, "resource", wrapped);

        let value = ctx
            .eval_raw(
                lock,
                "({ valueOf() { \
                     return resource.double({ valueOf() { return 21; } }); \
                 } })",
            )
            .expect("expression should evaluate");

        let n = <f64 as FromJS>::from_js(lock, value).expect("nested coercion should succeed");
        assert!((n - 42.0).abs() < f64::EPSILON);

        let count: f64 = ctx.eval(lock, "resource.callCount()").unwrap();
        assert!((count - 1.0).abs() < f64::EPSILON);
        Ok(())
    });
}

/// A three-level-deep chain of coercions, each crossing the FFI boundary via
/// `resource.echo`, to ensure nested coercions don't corrupt each other or the
/// isolate.
#[test]
fn string_coercion_handles_deeply_nested_reentrancy() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Rc::new(ReentrantResource {
            call_count: Cell::new(0),
        });
        let wrapped = resource.to_js(lock)?;
        ctx.set_global(lock, "resource", wrapped);

        let value = ctx
            .eval_raw(
                lock,
                "({ toString() { \
                     return resource.echo({ toString() { \
                         return resource.echo({ toString() { \
                             return resource.echo('deepest'); \
                         } }); \
                     } }); \
                 } })",
            )
            .expect("expression should evaluate");

        let s = <String as FromJS>::from_js(lock, value).expect("deep nesting should succeed");
        assert_eq!(s, "[[[deepest]]]");
        Ok(())
    });
}

/// A panic triggered by a Rust-backed method called *during* `ToString`
/// coercion must surface as an `Err` at the `unwrap_string` FFI boundary, not
/// abort the process. The panic throws an internal error and requests
/// termination, so the error is pending; `try_catch` either catches the
/// internal error or fails because execution is terminating.
#[test]
fn string_coercion_reentrant_panic_is_err_not_abort() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Rc::new(ReentrantResource {
            call_count: Cell::new(0),
        });
        let wrapped = resource.to_js(lock)?;
        ctx.set_global(lock, "resource", wrapped);

        let value = ctx
            .eval_raw(
                lock,
                "({ toString() { resource.panicNow(); return 'unreachable'; } })",
            )
            .expect("expression should evaluate");

        let result = lock.try_catch(|lock| <String as FromJS>::from_js(lock, value));
        assert!(
            !matches!(result, Ok(Ok(_))),
            "panic during ToString coercion must be an Err, got {result:?}"
        );
        Ok(())
    });
}

/// Same as above, but for `ToNumber`/`valueOf`/`unwrap_number`.
#[test]
fn number_coercion_reentrant_panic_is_err_not_abort() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Rc::new(ReentrantResource {
            call_count: Cell::new(0),
        });
        let wrapped = resource.to_js(lock)?;
        ctx.set_global(lock, "resource", wrapped);

        let value = ctx
            .eval_raw(lock, "({ valueOf() { resource.panicNow(); return 0; } })")
            .expect("expression should evaluate");

        let result = lock.try_catch(|lock| <f64 as FromJS>::from_js(lock, value));
        assert!(
            !matches!(result, Ok(Ok(_))),
            "panic during ToNumber coercion must be an Err, got {result:?}"
        );
        Ok(())
    });
}

/// After a re-entrant panic during coercion terminates the isolate's current
/// execution, a *fresh* context on the same isolate must still be usable —
/// mirroring `coercion_failure_does_not_poison_isolate` but for the
/// re-entrant-panic path specifically.
#[test]
fn reentrant_panic_during_coercion_does_not_poison_isolate() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Rc::new(ReentrantResource {
            call_count: Cell::new(0),
        });
        let wrapped = resource.to_js(lock)?;
        ctx.set_global(lock, "resource", wrapped);

        let value = ctx
            .eval_raw(
                lock,
                "({ toString() { resource.panicNow(); return 'unreachable'; } })",
            )
            .expect("expression should evaluate");
        let result = lock.try_catch(|lock| <String as FromJS>::from_js(lock, value));
        assert!(!matches!(result, Ok(Ok(_))));
        // The panic requested termination, which takes effect when JavaScript next runs, as it
        // would when the Rust code that coerced returned to its JavaScript caller.
        assert!(ctx.eval_raw(lock, "0").is_err());
        Ok(())
    });

    // A subsequent, independent context on the same isolate still works.
    harness.run_in_context(|lock, ctx| {
        let ok = ctx.eval_raw(lock, "'still fine'").expect("should evaluate");
        let s = <String as FromJS>::from_js(lock, ok).expect("should coerce cleanly");
        assert_eq!(s, "still fine");
        Ok(())
    });
}

/// `Object::get` throws what a getter or Proxy trap threw; an absent property is `undefined`.
#[test]
fn object_get_from_throwing_getter_returns_err() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        ctx.eval_raw(lock, "globalThis.thrown = new RangeError('getter')")
            .expect("setup should evaluate");
        for expr in [
            "({ get x() { throw thrown; } })",
            "new Proxy({}, { get() { throw thrown; } })",
        ] {
            let value = ctx
                .eval_raw(lock, expr)
                .expect("expression should evaluate");
            let obj: v8::Local<'_, v8::Object> = value.try_into().unwrap();
            let caught = thrown_by(lock, |lock| obj.get(lock, "x"));
            assert!(is_thrown(lock, ctx, caught), "{expr}");
        }
        let obj: v8::Local<'_, v8::Object> =
            ctx.eval_raw(lock, "({})").unwrap().try_into().unwrap();
        assert!(obj.get(lock, "absent").unwrap().is_undefined());
        Ok(())
    });
}

/// `Object::has` throws what a Proxy `has` trap threw.
#[test]
fn object_has_from_throwing_proxy_returns_err() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        ctx.eval_raw(lock, "globalThis.thrown = new TypeError('trap')")
            .expect("setup should evaluate");
        let value = ctx
            .eval_raw(lock, "new Proxy({}, { has() { throw thrown; } })")
            .expect("expression should evaluate");
        let obj: v8::Local<'_, v8::Object> = value.try_into().unwrap();
        let caught = thrown_by(lock, |lock| obj.has(lock, "x"));
        assert!(is_thrown(lock, ctx, caught));
        Ok(())
    });
}

/// `Object::set` throws what a setter, on the object or its prototype chain, or a Proxy `set`
/// trap threw.
#[test]
fn object_set_from_throwing_setter_returns_err() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        ctx.eval_raw(lock, "globalThis.thrown = new RangeError('setter')")
            .expect("setup should evaluate");
        for expr in [
            "({ set x(v) { throw thrown; } })",
            "Object.create({ set x(v) { throw thrown; } })",
            "new Proxy({}, { set() { throw thrown; } })",
        ] {
            let value = ctx
                .eval_raw(lock, expr)
                .expect("expression should evaluate");
            let obj: v8::Local<'_, v8::Object> = value.try_into().unwrap();
            let one = 1.0_f64.to_js(lock)?;
            let caught = thrown_by(lock, |lock| obj.set(lock, "x", one));
            assert!(is_thrown(lock, ctx, caught), "{expr}");
        }
        let obj: v8::Local<'_, v8::Object> =
            ctx.eval_raw(lock, "({})").unwrap().try_into().unwrap();
        let one = 1.0_f64.to_js(lock)?;
        obj.set(lock, "x", one)?;
        let x = obj.get(lock, "x")?;
        assert!((f64::from_js(lock, x)? - 1.0).abs() < f64::EPSILON);
        Ok(())
    });
}

/// `Array::set` throws what an element's setter, on the array or `Array.prototype`, threw.
#[test]
fn array_set_from_throwing_setter_returns_err() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        ctx.eval_raw(lock, "globalThis.thrown = new RangeError('setter')")
            .expect("setup should evaluate");
        for expr in [
            "Object.defineProperty([0], 0, { set(v) { throw thrown; } })",
            "Object.setPrototypeOf([], Object.create(Array.prototype, \
                 { 0: { set(v) { throw thrown; } } }))",
        ] {
            let value = ctx
                .eval_raw(lock, expr)
                .expect("expression should evaluate");
            let array: v8::Local<'_, v8::Array> = value.try_into().unwrap();
            let one = 1.0_f64.to_js(lock)?;
            let caught = thrown_by(lock, |lock| array.set(lock, 0, one));
            assert!(is_thrown(lock, ctx, caught), "{expr}");
        }
        let array: v8::Local<'_, v8::Array> = ctx.eval_raw(lock, "[]").unwrap().try_into().unwrap();
        let one = 1.0_f64.to_js(lock)?;
        array.set(lock, 2, one)?;
        assert_eq!(array.len(), 3);
        Ok(())
    });
}

/// What an argument's coercion throws reaches the JavaScript caller of a method unchanged.
#[test]
fn coercion_exception_propagates_through_method() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Rc::new(ReentrantResource {
            call_count: Cell::new(0),
        });
        ctx.set_global(lock, "resource", resource.clone());
        let caught = ctx.eval::<bool>(
            lock,
            "const thrown = new RangeError('boom');
             let caught;
             try { resource.echo({ toString() { throw thrown; } }); } catch (e) { caught = e; }
             let caughtNumber;
             try { resource.double({ valueOf() { throw 42; } }); } catch (e) { caughtNumber = e; }
             caught === thrown && caughtNumber === 42",
        );
        assert!(caught.unwrap());
        assert_eq!(resource.call_count.get(), 0);
        Ok(())
    });
}

// =============================================================================
// Termination during, and before, a conversion or property access.
// =============================================================================

/// What `ConvertingResource` does with its value.
#[derive(Clone, Copy, Debug)]
enum Conversion {
    String,
    Number,
    Get,
    Has,
    Iterate,
    Set,
    SetElement,
}

impl Conversion {
    const ALL: [Self; 7] = [
        Self::String,
        Self::Number,
        Self::Get,
        Self::Has,
        Self::Iterate,
        Self::Set,
        Self::SetElement,
    ];

    /// Converts `value`, reads or sets its property `x`, or sets its element 0.
    fn run<'s>(self, lock: &mut jsg::Lock<'s>, value: v8::Local<'s, v8::Value>) -> jsg::Result<()> {
        match self {
            Self::String => String::from_js(lock, value).map(drop),
            Self::Number => f64::from_js(lock, value).map(drop),
            Self::Get => v8::Local::<v8::Object>::try_from(value)
                .unwrap()
                .get(lock, "x")
                .map(drop),
            Self::Has => v8::Local::<v8::Object>::try_from(value)
                .unwrap()
                .has(lock, "x")
                .map(drop),
            Self::Iterate => Vec::<f64>::from_js(lock, value).map(drop),
            Self::Set => {
                let undefined = lock.undefined();
                v8::Local::<v8::Object>::try_from(value)
                    .unwrap()
                    .set(lock, "x", undefined)
            }
            Self::SetElement => {
                let undefined = lock.undefined();
                v8::Local::<v8::Array>::try_from(value)
                    .unwrap()
                    .set(lock, 0, undefined)
            }
        }
    }

    /// A value whose conversion, property `x` or element 0 calls `f`.
    fn value_calling(self, f: &str) -> String {
        match self {
            Self::String => format!("({{ toString: {f} }})"),
            Self::Number => format!("({{ valueOf: {f} }})"),
            Self::Get => format!("({{ get x() {{ return ({f})(); }} }})"),
            Self::Has => format!("new Proxy({{}}, {{ has: {f} }})"),
            Self::Iterate => {
                format!(
                    "(() => {{ const a = [0]; Object.defineProperty(a, 0, {{ get: {f} }}); return a; }})()"
                )
            }
            Self::Set => format!("({{ set x(v) {{ ({f})(); }} }})"),
            Self::SetElement => {
                format!(
                    "(() => {{ const a = [0]; Object.defineProperty(a, 0, {{ set: {f} }}); return a; }})()"
                )
            }
        }
    }
}

jsg::jsg_traced!(Conversion);

/// A Rust-backed resource that converts values, and records whether a conversion failed with a
/// pending error.
#[jsg_resource]
struct ConvertingResource {
    conversion: Cell<Option<Conversion>>,
    saw_pending: Cell<bool>,
}

#[jsg_resource]
impl ConvertingResource {
    /// Converts `value`, and fails as the conversion does.
    #[jsg_method]
    pub fn convert<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        value: v8::Local<'s, v8::Value>,
    ) -> Result<(), jsg::Error> {
        let conversion = self.conversion.get().unwrap();
        conversion
            .run(lock, value)
            .inspect_err(|err| self.saw_pending.set(err.is_pending()))
    }

    /// Calls `callback`, then converts `value` even if the call failed, and fails as the call
    /// did.
    #[jsg_method]
    pub fn call_then_convert<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        callback: v8::Local<'s, v8::Value>,
        value: v8::Local<'s, v8::Value>,
    ) -> Result<(), jsg::Error> {
        let callback = v8::Local::<v8::Function>::try_from(callback)
            .map_err(|_| jsg::Error::new_type_error("not a function"))?;
        let called = callback.call::<v8::Local<v8::Value>>(lock, None, &[]);
        let conversion = self.conversion.get().unwrap();
        let converted = conversion.run(lock, value);
        self.saw_pending
            .set(converted.is_err_and(|err| err.is_pending()));
        called.map(drop)
    }
}

/// JavaScript that terminates execution: `panicNow()` throws an internal error and requests
/// termination, which takes effect at the next interrupt check, in the loop.
const TERMINATE: &str = "() => { try { panicker.panicNow(); } catch (e) {} while (true) {} }";

fn converting_resource(
    lock: &mut jsg::Lock,
    ctx: &crate::EvalContext<'_>,
    conversion: Conversion,
) -> jsg::Rc<ConvertingResource> {
    let resource = jsg::Rc::new(ConvertingResource {
        conversion: Cell::new(Some(conversion)),
        saw_pending: Cell::new(false),
    });
    ctx.set_global(lock, "obj", resource.clone());
    ctx.set_global(
        lock,
        "panicker",
        jsg::Rc::new(ReentrantResource {
            call_count: Cell::new(0),
        }),
    );
    resource
}

/// Execution terminating during a conversion or property access fails it with a pending error,
/// and the termination keeps propagating past the method: JavaScript cannot catch it.
#[test]
fn termination_during_conversion_propagates() {
    for conversion in Conversion::ALL {
        let harness = crate::Harness::new();
        harness.run_in_context(|lock, ctx| {
            let resource = converting_resource(lock, ctx, conversion);
            let value = conversion.value_calling(TERMINATE);
            let result = ctx.eval::<bool>(
                lock,
                &format!("try {{ obj.convert({value}); }} catch (e) {{}} true"),
            );
            assert!(
                result.is_err(),
                "{conversion:?}: not terminated: {result:?}"
            );
            assert!(resource.saw_pending.get(), "{conversion:?}");
            Ok(())
        });
    }
}

/// A conversion or property access while execution is terminating runs no JavaScript and fails
/// with a pending error, as `Local<Function>::call` does.
#[test]
fn conversion_while_terminating_runs_nothing() {
    for conversion in Conversion::ALL {
        let harness = crate::Harness::new();
        harness.run_in_context(|lock, ctx| {
            let resource = converting_resource(lock, ctx, conversion);
            let value =
                conversion.value_calling("() => { globalThis.secondRan = true; return 0; }");
            let result = ctx.eval::<bool>(
                lock,
                &format!(
                    "globalThis.secondRan = false;
                     obj.callThenConvert({TERMINATE}, {value});
                     true"
                ),
            );
            assert!(
                result.is_err(),
                "{conversion:?}: not terminated: {result:?}"
            );
            assert!(resource.saw_pending.get(), "{conversion:?}");
            // V8 clears the termination once no JavaScript is running.
            assert!(
                !ctx.eval::<bool>(lock, "secondRan").unwrap(),
                "{conversion:?} ran JavaScript while terminating"
            );
            Ok(())
        });
    }
}

/// `Error`'s `FromJS` passes on what reading `name` or `message` throws, rather than discarding
/// it and running more JavaScript with an exception pending.
#[test]
fn error_from_js_passes_on_what_reading_it_throws() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        ctx.eval_raw(lock, "globalThis.thrown = new RangeError('getter')")
            .expect("setup should evaluate");
        for expr in [
            "Object.defineProperty(new Error('x'), 'name', { get() { throw thrown; } })",
            "Object.defineProperty(new Error('x'), 'message', { get() { throw thrown; } })",
            "Object.assign(new Error('x'), { message: { toString() { throw thrown; } } })",
        ] {
            let caught = conversion_throws::<jsg::Error>(lock, ctx, expr);
            assert!(is_thrown(lock, ctx, caught), "{expr}");
        }
        Ok(())
    });
}
