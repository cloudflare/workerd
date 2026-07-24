// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Regression tests for the Rust JSG `unwrap_*` boundary: hostile JS values
//! (coercion failures, lone surrogates, wrong types) must yield a catchable
//! `Err`, not abort the process (SIGABRT) as they previously did.

use jsg::ExceptionType;
use jsg::FromJS;
use jsg::Number;

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
