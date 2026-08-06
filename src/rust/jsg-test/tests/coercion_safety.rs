// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Regression tests for the Rust JSG `unwrap_*` boundary: hostile JS values
//! (coercion failures, lone surrogates, wrong types) must yield a catchable
//! `Err`, not abort the process (SIGABRT) as they previously did.
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
use jsg::Number;
use jsg::ToJS;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;

/// Coercion failures must surface as a catchable error, never abort.
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
            let value = ctx.eval_raw(expr).expect("expression should evaluate");
            let result = <String as FromJS>::from_js(lock, value);
            assert!(
                result.is_err(),
                "String::from_js({expr}) should be Err, got {result:?}"
            );
        }
        Ok(())
    });
}

/// A throwing `toString` preserves the JS error type and message.
#[test]
fn string_from_throwing_tostring_maps_to_type_error() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let value = ctx
            .eval_raw("({ toString() { throw new TypeError('boom'); } })")
            .expect("expression should evaluate");
        let err = <String as FromJS>::from_js(lock, value)
            .expect_err("coercion failure should be an Err");
        assert_eq!(err.name, ExceptionType::TypeError);
        assert!(
            err.message.contains("boom"),
            "message should preserve the original text, got {:?}",
            err.message
        );
        Ok(())
    });
}

/// A lone UTF-16 surrogate must not abort; it decodes lossily to U+FFFD.
#[test]
fn string_from_lone_surrogate_is_lossy_not_abort() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let value = ctx
            .eval_raw("'\\uD800'")
            .expect("expression should evaluate");
        let s = <String as FromJS>::from_js(lock, value)
            .expect("lone surrogate should decode lossily, not error");
        assert_eq!(s, "\u{FFFD}");

        // A surrogate embedded in otherwise valid text.
        let value = ctx
            .eval_raw("'a\\uD800b'")
            .expect("expression should evaluate");
        let s = <String as FromJS>::from_js(lock, value).expect("should decode lossily");
        assert_eq!(s, "a\u{FFFD}b");
        Ok(())
    });
}

/// `Number` coercion of a `Symbol`/`BigInt` throws in JS; it must be catchable.
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
            let value = ctx.eval_raw(expr).expect("expression should evaluate");
            let result = <Number as FromJS>::from_js(lock, value);
            assert!(
                result.is_err(),
                "Number::from_js({expr}) should be Err, got {result:?}"
            );
        }
        Ok(())
    });
}

/// Integer conversions go through the same fallible `unwrap_number`.
#[test]
fn integer_from_symbol_returns_err() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let value = ctx.eval_raw("Symbol('s')").expect("should evaluate");
        let result = <u32 as FromJS>::from_js(lock, value);
        assert!(result.is_err(), "u32::from_js(Symbol) should be Err");
        Ok(())
    });
}

/// A wrong-typed typed-array argument must be a catchable error, not an abort.
#[test]
fn typed_array_from_wrong_type_returns_err() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        // Not a Uint8Array: a plain array, a number, and a Symbol.
        for expr in ["[1, 2, 3]", "42", "Symbol('s')"] {
            let value = ctx.eval_raw(expr).expect("should evaluate");
            let result = <Vec<u8> as FromJS>::from_js(lock, value);
            assert!(
                result.is_err(),
                "Vec::<u8>::from_js({expr}) should be Err, got {result:?}"
            );
        }
        Ok(())
    });
}

/// A coercion failure must leave the isolate usable for later evaluations.
#[test]
fn coercion_failure_does_not_poison_isolate() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let hostile = ctx.eval_raw("Symbol('s')").expect("should evaluate");
        assert!(<String as FromJS>::from_js(lock, hostile).is_err());

        // A subsequent, well-formed coercion still works.
        let ok = ctx.eval_raw("'still fine'").expect("should evaluate");
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

    /// Takes a `Number` argument; same nesting trick as `echo` but for
    /// `unwrap_number`/`ToNumber`.
    #[jsg_method]
    pub fn double(&self, n: Number) -> Number {
        self.call_count.set(self.call_count.get() + 1);
        Number::new(n.value() * 2.0)
    }

    #[jsg_method]
    pub fn call_count(&self) -> Number {
        Number::new(f64::from(self.call_count.get()))
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
        ctx.eval_raw("globalThis.__sideEffect = 0;")
            .expect("should evaluate");
        let value = ctx
            .eval_raw(
                "({ toString() { \
                     globalThis.__sideEffect += 1; \
                     return 'computed-' + globalThis.__sideEffect; \
                 } })",
            )
            .expect("expression should evaluate");

        let s = <String as FromJS>::from_js(lock, value).expect("coercion should succeed");
        assert_eq!(s, "computed-1");

        let side_effect: Number = ctx.eval(lock, "globalThis.__sideEffect").unwrap();
        assert!((side_effect.value() - 1.0).abs() < f64::EPSILON);
        Ok(())
    });
}

/// Same as above, but for `Number`/`valueOf`.
#[test]
fn number_from_valueof_side_effects_are_observed() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        ctx.eval_raw("globalThis.__sideEffect = 0;")
            .expect("should evaluate");
        let value = ctx
            .eval_raw(
                "({ valueOf() { \
                     globalThis.__sideEffect += 1; \
                     return 41 + globalThis.__sideEffect; \
                 } })",
            )
            .expect("expression should evaluate");

        let n = <Number as FromJS>::from_js(lock, value).expect("coercion should succeed");
        assert!((n.value() - 42.0).abs() < f64::EPSILON);

        let side_effect: Number = ctx.eval(lock, "globalThis.__sideEffect").unwrap();
        assert!((side_effect.value() - 1.0).abs() < f64::EPSILON);
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
        let wrapped = resource.to_js(lock);
        ctx.set_global("resource", wrapped);

        let value = ctx
            .eval_raw(
                "({ toString() { \
                     return resource.echo({ toString() { return 'inner'; } }); \
                 } })",
            )
            .expect("expression should evaluate");

        let s = <String as FromJS>::from_js(lock, value).expect("nested coercion should succeed");
        assert_eq!(s, "[inner]");

        let count: Number = ctx.eval(lock, "resource.callCount()").unwrap();
        assert!((count.value() - 1.0).abs() < f64::EPSILON);
        Ok(())
    });
}

/// Same as above, but for `Number`/`valueOf`/`unwrap_number`.
#[test]
fn number_coercion_reentrant_into_rust_resource_method() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Rc::new(ReentrantResource {
            call_count: Cell::new(0),
        });
        let wrapped = resource.to_js(lock);
        ctx.set_global("resource", wrapped);

        let value = ctx
            .eval_raw(
                "({ valueOf() { \
                     return resource.double({ valueOf() { return 21; } }); \
                 } })",
            )
            .expect("expression should evaluate");

        let n = <Number as FromJS>::from_js(lock, value).expect("nested coercion should succeed");
        assert!((n.value() - 42.0).abs() < f64::EPSILON);

        let count: Number = ctx.eval(lock, "resource.callCount()").unwrap();
        assert!((count.value() - 1.0).abs() < f64::EPSILON);
        Ok(())
    });
}

/// A three-level-deep chain of coercions, each crossing the FFI boundary via
/// `resource.echo`, to ensure nested `v8::TryCatch`/`unwrapCoerce` scopes
/// don't corrupt each other or the isolate.
#[test]
fn string_coercion_handles_deeply_nested_reentrancy() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Rc::new(ReentrantResource {
            call_count: Cell::new(0),
        });
        let wrapped = resource.to_js(lock);
        ctx.set_global("resource", wrapped);

        let value = ctx
            .eval_raw(
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
/// coercion must surface as a catchable `Err` at the `unwrap_string` FFI
/// boundary, not abort the process. This exercises the "termination
/// exceptions propagate unchanged through `unwrapCoerce`'s `JSG_TRY`/
/// `JSG_CATCH`" path described in `ffi.c++`, specifically when the
/// termination originates from a re-entrant call rather than directly inside
/// `ToString` itself.
#[test]
fn string_coercion_reentrant_panic_is_catchable_not_abort() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Rc::new(ReentrantResource {
            call_count: Cell::new(0),
        });
        let wrapped = resource.to_js(lock);
        ctx.set_global("resource", wrapped);

        let value = ctx
            .eval_raw("({ toString() { resource.panicNow(); return 'unreachable'; } })")
            .expect("expression should evaluate");

        let result = <String as FromJS>::from_js(lock, value);
        assert!(
            result.is_err(),
            "panic during ToString coercion must be a catchable Err, got {result:?}"
        );
        Ok(())
    });
}

/// Same as above, but for `Number`/`valueOf`/`unwrap_number`.
#[test]
fn number_coercion_reentrant_panic_is_catchable_not_abort() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Rc::new(ReentrantResource {
            call_count: Cell::new(0),
        });
        let wrapped = resource.to_js(lock);
        ctx.set_global("resource", wrapped);

        let value = ctx
            .eval_raw("({ valueOf() { resource.panicNow(); return 0; } })")
            .expect("expression should evaluate");

        let result = <Number as FromJS>::from_js(lock, value);
        assert!(
            result.is_err(),
            "panic during ToNumber coercion must be a catchable Err, got {result:?}"
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
        let wrapped = resource.to_js(lock);
        ctx.set_global("resource", wrapped);

        let value = ctx
            .eval_raw("({ toString() { resource.panicNow(); return 'unreachable'; } })")
            .expect("expression should evaluate");
        assert!(<String as FromJS>::from_js(lock, value).is_err());
        Ok(())
    });

    // A subsequent, independent context on the same isolate still works.
    harness.run_in_context(|lock, ctx| {
        let ok = ctx.eval_raw("'still fine'").expect("should evaluate");
        let s = <String as FromJS>::from_js(lock, ok).expect("should coerce cleanly");
        assert_eq!(s, "still fine");
        Ok(())
    });
}
