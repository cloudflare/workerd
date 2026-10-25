// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! `BigInt` and `Date` handles, and the `i64` and `SystemTime` conversions built on them, which
//! match C++ JSG's `int64_t` and `kj::Date` (`jsg/value.h`).

use std::time::Duration;
use std::time::SystemTime;
use std::time::UNIX_EPOCH;

use jsg::ExceptionType;
use jsg::FromJS;
use jsg::v8;

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

/// Asserts that converting `expr` to `T` throws a `TypeError` with `message`: either an error
/// the conversion fails with, or what V8 threw during the coercion, which stays pending.
fn assert_type_error<'s, T: FromJS<'s>>(
    lock: &mut jsg::Lock<'s>,
    ctx: &crate::EvalContext<'_>,
    expr: &str,
    message: &str,
) {
    let value = ctx
        .eval_raw(lock, expr)
        .expect("expression should evaluate");
    let caught = lock
        .try_catch(|lock| T::from_js(lock, value).map(drop))
        .expect("execution should not be terminating");
    let Err(exception) = caught else {
        panic!("{expr} should not convert");
    };
    let err = jsg::Error::from_js(lock, exception).unwrap();
    assert_eq!(err.name, ExceptionType::TypeError, "{expr}");
    assert_eq!(err.message, message, "{expr}");
}

#[test]
fn big_int_handles() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let value = ctx.eval_raw(lock, "-5n").unwrap();
        assert!(value.is_big_int());
        assert!(!value.is_number());
        let big_int = value.try_as::<v8::BigInt>().unwrap();
        assert_eq!(big_int.int64_value(), Some(-5));

        let too_big = ctx.eval_raw(lock, "2n ** 63n").unwrap();
        assert_eq!(too_big.try_as::<v8::BigInt>().unwrap().int64_value(), None);

        assert!(
            ctx.eval_raw(lock, "5")
                .unwrap()
                .try_as::<v8::BigInt>()
                .is_none()
        );
        Ok(())
    });
}

#[test]
fn date_handles() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let value = ctx.eval_raw(lock, "new Date(1234.5)").unwrap();
        assert!(value.is_date());
        assert!(value.is_object());
        assert!((value.try_as::<v8::Date>().unwrap().value_of() - 1234.0).abs() < f64::EPSILON);

        let invalid = ctx.eval_raw(lock, "new Date(NaN)").unwrap();
        assert!(invalid.try_as::<v8::Date>().unwrap().value_of().is_nan());

        assert!(!ctx.eval_raw(lock, "1234").unwrap().is_date());
        assert!(
            !ctx.eval_raw(lock, "({valueOf() { return 1; }})")
                .unwrap()
                .is_date()
        );
        Ok(())
    });
}

#[test]
fn i64_converts_big_ints_and_numbers() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        assert_eq!(convert::<i64>(lock, ctx, "5n").unwrap(), 5);
        assert_eq!(convert::<i64>(lock, ctx, "-(2n ** 63n)").unwrap(), i64::MIN);
        assert_eq!(convert::<i64>(lock, ctx, "1.9").unwrap(), 1);
        assert_eq!(convert::<i64>(lock, ctx, "-1.9").unwrap(), -1);
        assert_eq!(convert::<i64>(lock, ctx, "'12'").unwrap(), 12);
        assert_eq!(convert::<i64>(lock, ctx, "null").unwrap(), 0);
        assert_eq!(
            convert::<i64>(lock, ctx, "2 ** 53").unwrap(),
            9_007_199_254_740_992
        );
        Ok(())
    });
}

#[test]
fn i64_rejects_what_cpp_jsg_rejects() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let truncated = "The value cannot be converted because it would be truncated.";
        assert_type_error::<i64>(lock, ctx, "2n ** 63n", truncated);
        assert_type_error::<i64>(lock, ctx, "2n ** 64n", truncated);
        let not_an_integer = "The value cannot be converted because it is not an integer.";
        assert_type_error::<i64>(lock, ctx, "NaN", not_an_integer);
        assert_type_error::<i64>(lock, ctx, "-Infinity", not_an_integer);
        assert_type_error::<i64>(lock, ctx, "undefined", not_an_integer);
        assert_type_error::<i64>(
            lock,
            ctx,
            "1e19",
            "Value out of range. Must be between -9223372036854775808 and 9223372036854775807 \
             (inclusive).",
        );
        assert_type_error::<i64>(
            lock,
            ctx,
            "Symbol()",
            "Cannot convert a Symbol value to a number",
        );
        Ok(())
    });
}

#[test]
fn system_time_converts_dates_and_numbers() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let at = |millis: u64| UNIX_EPOCH + Duration::from_millis(millis);
        assert_eq!(
            convert::<SystemTime>(lock, ctx, "new Date(1234)").unwrap(),
            at(1234)
        );
        assert_eq!(
            convert::<SystemTime>(lock, ctx, "1234.9").unwrap(),
            at(1234)
        );
        assert_eq!(convert::<SystemTime>(lock, ctx, "0").unwrap(), UNIX_EPOCH);
        assert_eq!(
            convert::<SystemTime>(lock, ctx, "-1234.9").unwrap(),
            UNIX_EPOCH - Duration::from_millis(1234)
        );
        Ok(())
    });
}

#[test]
fn system_time_rejects_what_cpp_jsg_rejects() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let invalid = "The value cannot be converted because it is not a valid Date.";
        assert_type_error::<SystemTime>(lock, ctx, "NaN", invalid);
        assert_type_error::<SystemTime>(lock, ctx, "new Date(NaN)", invalid);
        assert_type_error::<SystemTime>(
            lock,
            ctx,
            "1e20",
            "This API doesn't support dates after 2189.",
        );
        assert_type_error::<SystemTime>(
            lock,
            ctx,
            "-1e20",
            "This API doesn't support dates before 1687.",
        );
        assert_type_error::<SystemTime>(lock, ctx, "'1234'", "Expected date but got string");

        // Only a Date or a number is a date; anything else is a type mismatch.
        for expr in [
            "'1234'",
            "null",
            "undefined",
            "({valueOf() { return 1; }})",
            "1n",
        ] {
            let value = ctx.eval_raw(lock, expr).unwrap();
            assert!(
                SystemTime::try_from_js(lock, value).unwrap().is_none(),
                "{expr}"
            );
        }
        Ok(())
    });
}
