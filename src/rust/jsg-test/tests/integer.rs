// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Integer conversion matches C++ JSG (`jsg/value.h`): an integer converts to a
//! number, and from JS, out-of-range values are a `TypeError`, and in-range
//! fractions truncate.

use jsg::ExceptionType;
use jsg::FromJS;
use jsg::NonCoercible;
use jsg::ToJS;

/// Evaluates `expr` and converts it to `T`, as an argument would be.
fn convert<'s, T: FromJS<'s>>(
    lock: &mut jsg::Lock<'s>,
    ctx: &crate::EvalContext<'_>,
    expr: &str,
) -> Result<T::ResultType, jsg::Error> {
    let value = ctx
        .eval_raw(lock, expr)
        .expect("expression should evaluate");
    T::from_js(lock, value)
}

/// Asserts that converting `expr` to `T` is a `TypeError` with `message`.
fn assert_type_error<'s, T: FromJS<'s>>(
    lock: &mut jsg::Lock<'s>,
    ctx: &crate::EvalContext<'_>,
    expr: &str,
    message: &str,
) {
    let Err(err) = convert::<T>(lock, ctx, expr) else {
        panic!("{expr} should not convert");
    };
    assert_eq!(err.name, ExceptionType::TypeError, "{expr}");
    assert_eq!(err.message, message, "{expr}");
}

#[test]
fn integers_truncate_toward_zero() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        assert_eq!(convert::<u8>(lock, ctx, "3.7").unwrap(), 3);
        assert_eq!(convert::<i8>(lock, ctx, "-2.9").unwrap(), -2);
        assert_eq!(convert::<u32>(lock, ctx, "'7'").unwrap(), 7);
        assert_eq!(convert::<u16>(lock, ctx, "-0").unwrap(), 0);
        Ok(())
    });
}

#[test]
fn integers_out_of_range_are_type_errors() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let below = "Value out of range. Must be less than or equal to";
        assert_type_error::<u8>(lock, ctx, "256", &format!("{below} 255."));
        assert_type_error::<u8>(lock, ctx, "255.5", &format!("{below} 255."));
        assert_type_error::<u16>(lock, ctx, "65536", &format!("{below} 65535."));
        assert_type_error::<u32>(lock, ctx, "2 ** 32", &format!("{below} 4294967295."));
        let between = "Value out of range. Must be between";
        assert_type_error::<i8>(
            lock,
            ctx,
            "-129",
            &format!("{between} -128 and 127 (inclusive)."),
        );
        assert_type_error::<i16>(
            lock,
            ctx,
            "32768",
            &format!("{between} -32768 and 32767 (inclusive)."),
        );
        assert_type_error::<i32>(
            lock,
            ctx,
            "2 ** 31",
            &format!("{between} -2147483648 and 2147483647 (inclusive)."),
        );
        assert_eq!(convert::<u8>(lock, ctx, "255").unwrap(), 255);
        assert_eq!(convert::<i32>(lock, ctx, "-(2 ** 31)").unwrap(), i32::MIN);
        Ok(())
    });
}

#[test]
fn unsigned_integers_reject_negatives() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let message = "The value cannot be converted because it is negative and this API \
                       expects a positive number.";
        assert_type_error::<u8>(lock, ctx, "-1", message);
        assert_type_error::<u32>(lock, ctx, "-0.5", message);
        Ok(())
    });
}

#[test]
fn non_finite_integers() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let message = "The value cannot be converted because it is not an integer.";
        assert_type_error::<u8>(lock, ctx, "NaN", message);
        assert_type_error::<i16>(lock, ctx, "Infinity", message);
        assert_type_error::<u32>(lock, ctx, "undefined", message);
        // Like C++ JSG's int, i32 reads a non-finite number as 0.
        assert_eq!(convert::<i32>(lock, ctx, "NaN").unwrap(), 0);
        assert_eq!(convert::<i32>(lock, ctx, "-Infinity").unwrap(), 0);
        Ok(())
    });
}

/// Converts `value` to JavaScript, stores it as the global `v` and evaluates `expr`.
fn eval_with<'s, T: FromJS<'s, ResultType = T>>(
    lock: &mut jsg::Lock<'s>,
    ctx: &crate::EvalContext<'_>,
    value: impl ToJS<'s>,
    expr: &str,
) -> T {
    ctx.set_global(lock, "v", value);
    ctx.eval(lock, expr).unwrap()
}

#[test]
fn integers_convert_to_numbers() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        assert!(eval_with::<bool>(
            lock,
            ctx,
            u8::MAX,
            "typeof v === 'number' && v === 255"
        ));
        assert!(eval_with::<bool>(lock, ctx, u16::MAX, "v === 65535"));
        assert!(eval_with::<bool>(lock, ctx, u32::MAX, "v === 4294967295"));
        assert!(eval_with::<bool>(lock, ctx, i8::MIN, "v === -128"));
        assert!(eval_with::<bool>(lock, ctx, i16::MIN, "v === -32768"));
        assert!(eval_with::<bool>(lock, ctx, i32::MIN, "v === -2147483648"));
        assert!(eval_with::<bool>(lock, ctx, 0u32, "Object.is(v, 0)"));
        Ok(())
    });
}

/// A `Vec` or slice of integers converts from an `Array` whose elements convert, and not from a
/// typed array, which is a `Local<Uint8Array>` and the like.
#[test]
fn vec_of_integers_converts_from_an_array() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        assert_eq!(
            convert::<Vec<u8>>(lock, ctx, "[1, 2.9, '255']").unwrap(),
            vec![1, 2, 255]
        );
        assert_eq!(convert::<&[u32]>(lock, ctx, "[7]").unwrap(), vec![7]);
        assert_eq!(
            convert::<Vec<i64>>(lock, ctx, "[1n, -2, 3.9]").unwrap(),
            vec![1, -2, 3]
        );
        assert_eq!(
            convert::<Vec<u64>>(lock, ctx, "[2n ** 64n - 1n, 2, 3.9]").unwrap(),
            vec![u64::MAX, 2, 3]
        );
        assert_type_error::<Vec<u8>>(
            lock,
            ctx,
            "[1, 256]",
            "Value out of range. Must be less than or equal to 255.",
        );
        assert_type_error::<Vec<u8>>(
            lock,
            ctx,
            "new Uint8Array([1, 2])",
            "Expected Array but got object",
        );
        Ok(())
    });
}

/// A `Vec` of integers converts to an `Array` of numbers, or of `BigInt`s for `i64` and `u64`.
#[test]
fn vec_of_integers_converts_to_an_array() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        assert!(eval_with::<bool>(
            lock,
            ctx,
            vec![1u8, 2, 255],
            "Array.isArray(v) && v.length === 3 && v[2] === 255"
        ));
        assert!(eval_with::<bool>(
            lock,
            ctx,
            vec![i32::MIN],
            "Array.isArray(v) && v[0] === -2147483648"
        ));
        assert!(eval_with::<bool>(
            lock,
            ctx,
            vec![i64::MIN, 0],
            "Array.isArray(v) && v[0] === -(2n ** 63n) && v[1] === 0n"
        ));
        assert!(eval_with::<bool>(
            lock,
            ctx,
            vec![u64::MAX],
            "Array.isArray(v) && v[0] === 2n ** 64n - 1n"
        ));
        Ok(())
    });
}

/// A `NonCoercible` integer of up to 32 bits accepts only a number, and a `NonCoercible<i64>` or
/// `NonCoercible<u64>` only a `BigInt`. (C++ JSG has no `NonCoercible` integers.)
#[test]
fn non_coercible_integers_require_a_number() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        assert_eq!(*convert::<NonCoercible<u32>>(lock, ctx, "7").unwrap(), 7);
        assert_type_error::<NonCoercible<u32>>(
            lock,
            ctx,
            "'7'",
            "Expected a number value but got string",
        );
        assert_eq!(*convert::<NonCoercible<i64>>(lock, ctx, "7n").unwrap(), 7);
        assert_type_error::<NonCoercible<i64>>(
            lock,
            ctx,
            "7",
            "Expected a bigint value but got number",
        );
        assert_eq!(*convert::<NonCoercible<u64>>(lock, ctx, "7n").unwrap(), 7);
        assert_type_error::<NonCoercible<u64>>(
            lock,
            ctx,
            "7",
            "Expected a bigint value but got number",
        );
        Ok(())
    });
}
