// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Integer conversion from JS matches C++ JSG (`jsg/value.h`): out-of-range
//! values are a `TypeError`, and in-range fractions truncate.

use jsg::ExceptionType;
use jsg::FromJS;

/// Evaluates `expr` and converts it to `T`, as an argument would be.
fn convert<T: FromJS>(
    lock: &mut jsg::Lock,
    ctx: &crate::EvalContext<'_>,
    expr: &str,
) -> Result<T::ResultType, jsg::Error> {
    let value = ctx.eval_raw(expr).expect("expression should evaluate");
    T::from_js(lock, value)
}

/// Asserts that converting `expr` to `T` is a `TypeError` with `message`.
fn assert_type_error<T: FromJS>(
    lock: &mut jsg::Lock,
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
