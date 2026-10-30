// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! `BigInt` and `Date` handles, and the `i64`, `u64` and `SystemTime` conversions built on them,
//! which match C++ JSG's `int64_t`, `uint64_t` and `kj::Date` (`jsg/value.h`).

use std::time::Duration;
use std::time::SystemTime;
use std::time::UNIX_EPOCH;

use jsg::ExceptionType;
use jsg::FromJS;
use jsg::v8;
use jsg_macros::jsg_method;
use jsg_macros::jsg_property;
use jsg_macros::jsg_resource;

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
        let big_int = v8::Local::<v8::BigInt>::try_from(value).unwrap();
        assert_eq!(big_int.int64_value(), Some(-5));

        let too_big = ctx.eval_raw(lock, "2n ** 63n").unwrap();
        assert_eq!(
            v8::Local::<v8::BigInt>::try_from(too_big)
                .unwrap()
                .int64_value(),
            None
        );

        let as_u64 = |lock: &mut jsg::Lock<'_>, expr: &str| {
            v8::Local::<v8::BigInt>::try_from(ctx.eval_raw(lock, expr).unwrap())
                .unwrap()
                .uint64_value()
        };
        assert_eq!(as_u64(lock, "2n ** 64n - 1n"), Some(u64::MAX));
        assert_eq!(as_u64(lock, "-1n"), None);
        assert_eq!(as_u64(lock, "2n ** 64n"), None);

        assert!(v8::Local::<v8::BigInt>::try_from(ctx.eval_raw(lock, "5").unwrap()).is_err());
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
        assert!(
            (v8::Local::<v8::Date>::try_from(value).unwrap().value_of() - 1234.0).abs()
                < f64::EPSILON
        );

        let invalid = ctx.eval_raw(lock, "new Date(NaN)").unwrap();
        assert!(
            v8::Local::<v8::Date>::try_from(invalid)
                .unwrap()
                .value_of()
                .is_nan()
        );

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
fn u64_converts_big_ints_and_numbers() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        assert_eq!(convert::<u64>(lock, ctx, "5n").unwrap(), 5);
        assert_eq!(
            convert::<u64>(lock, ctx, "2n ** 64n - 1n").unwrap(),
            u64::MAX
        );
        assert_eq!(convert::<u64>(lock, ctx, "1.9").unwrap(), 1);
        assert_eq!(convert::<u64>(lock, ctx, "'12'").unwrap(), 12);
        assert_eq!(convert::<u64>(lock, ctx, "null").unwrap(), 0);
        assert_eq!(
            convert::<u64>(lock, ctx, "2 ** 63").unwrap(),
            9_223_372_036_854_775_808
        );
        Ok(())
    });
}

#[test]
fn u64_rejects_what_cpp_jsg_rejects() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let truncated = "The value cannot be converted because it is either negative and this \
                         API expects a positive bigint, or the value would be truncated.";
        assert_type_error::<u64>(lock, ctx, "-1n", truncated);
        assert_type_error::<u64>(lock, ctx, "2n ** 64n", truncated);
        let not_an_integer = "The value cannot be converted because it is not an integer.";
        assert_type_error::<u64>(lock, ctx, "NaN", not_an_integer);
        assert_type_error::<u64>(lock, ctx, "Infinity", not_an_integer);
        assert_type_error::<u64>(lock, ctx, "undefined", not_an_integer);
        let negative = "The value cannot be converted because it is negative and this API \
                        expects a positive bigint.";
        assert_type_error::<u64>(lock, ctx, "-1", negative);
        assert_type_error::<u64>(lock, ctx, "-0.5", negative);
        assert_type_error::<u64>(
            lock,
            ctx,
            "2e19",
            "Value out of range. Must be less than or equal to 18446744073709551615.",
        );
        assert_type_error::<u64>(
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

/// Sets `value` as the global `v` and evaluates `expr`.
fn eval_with<'s>(
    lock: &mut jsg::Lock<'s>,
    ctx: &crate::EvalContext<'_>,
    value: impl jsg::ToJS<'s>,
    expr: &str,
) -> String {
    ctx.set_global(lock, "v", value);
    ctx.eval(lock, expr).unwrap()
}

#[test]
fn big_ints_are_created_exactly() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let value = v8::BigInt::new(lock, i64::MIN);
        assert_eq!(value.int64_value(), Some(i64::MIN));
        assert_eq!(
            eval_with(lock, ctx, value, "String(v === -(2n ** 63n))"),
            "true"
        );
        let value = v8::BigInt::from_unsigned(lock, u64::MAX);
        assert_eq!(value.int64_value(), None);
        assert_eq!(
            eval_with(lock, ctx, value, "String(v === 2n ** 64n - 1n)"),
            "true"
        );
        Ok(())
    });
}

/// Like C++ JSG's `int64_t`, an `i64` converts to a `BigInt`.
#[test]
fn i64_converts_to_a_big_int() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        assert_eq!(
            eval_with(
                lock,
                ctx,
                i64::MAX,
                "String(typeof v === 'bigint' && v === 2n ** 63n - 1n)"
            ),
            "true"
        );
        assert_eq!(
            eval_with(lock, ctx, i64::MIN, "String(v === -(2n ** 63n))"),
            "true"
        );
        assert_eq!(eval_with(lock, ctx, 0i64, "String(v === 0n)"), "true");
        Ok(())
    });
}

/// Like C++ JSG's `uint64_t`, a `u64` converts to a `BigInt`.
#[test]
fn u64_converts_to_a_big_int() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        assert_eq!(
            eval_with(
                lock,
                ctx,
                u64::MAX,
                "String(typeof v === 'bigint' && v === 2n ** 64n - 1n)"
            ),
            "true"
        );
        assert_eq!(eval_with(lock, ctx, 0u64, "String(v === 0n)"), "true");
        Ok(())
    });
}

#[test]
fn dates_are_created_with_their_time_value() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let date = v8::Date::new(lock, 1234.5).unwrap();
        assert!((date.value_of() - 1234.0).abs() < f64::EPSILON);
        assert!(v8::Date::new(lock, 9e15).unwrap().value_of().is_nan());
        assert_eq!(
            eval_with(lock, ctx, date, "`${v instanceof Date} ${v.getTime()}`"),
            "true 1234"
        );
        Ok(())
    });
}

// Like C++ JSG's kj::Date, the milliseconds are truncated toward zero, before the epoch too.
#[test]
fn system_time_converts_to_a_date() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let describe = "`${v instanceof Date} ${v.getTime()}`";
        assert_eq!(eval_with(lock, ctx, UNIX_EPOCH, describe), "true 0");
        let after = UNIX_EPOCH + Duration::from_micros(1_234_999);
        assert_eq!(eval_with(lock, ctx, after, describe), "true 1234");
        let before = UNIX_EPOCH - Duration::from_micros(1_234_999);
        assert_eq!(eval_with(lock, ctx, before, describe), "true -1234");
        let just_before = UNIX_EPOCH - Duration::from_nanos(1);
        assert_eq!(
            eval_with(lock, ctx, just_before, "String(Object.is(v.getTime(), 0))"),
            "true"
        );
        let at = UNIX_EPOCH + Duration::from_secs(1_700_000_000);
        assert_eq!(
            eval_with(lock, ctx, at, "v.toISOString()"),
            "2023-11-14T22:13:20.000Z"
        );
        ctx.set_global(lock, "v", at);
        assert_eq!(convert::<SystemTime>(lock, ctx, "v").unwrap(), at);
        Ok(())
    });
}

// A time beyond the range of JavaScript dates, 8.64e15 milliseconds either side of the epoch,
// gives an invalid date. Not every platform's `SystemTime` reaches that far: on Windows it ends
// in the year 30828, so the cases it cannot hold are skipped there.
#[test]
fn system_time_beyond_javascript_dates_is_an_invalid_date() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        // 8.64e15 milliseconds.
        let limit = Duration::from_hours(2_400_000_000);
        let beyond = limit + Duration::from_millis(1);
        let cases = [
            (UNIX_EPOCH.checked_add(limit), "true 8640000000000000"),
            (UNIX_EPOCH.checked_add(beyond), "true NaN"),
            (UNIX_EPOCH.checked_sub(beyond), "true NaN"),
        ];
        for (time, expected) in cases {
            if let Some(time) = time {
                assert_eq!(
                    eval_with(lock, ctx, time, "`${v instanceof Date} ${v.getTime()}`"),
                    expected
                );
            }
        }
        Ok(())
    });
}

#[jsg_resource]
struct Event {
    received_at: SystemTime,
    expires_at: Option<SystemTime>,
}

#[jsg_resource]
impl Event {
    #[jsg_property(instance, readonly)]
    pub fn get_received_at(&self) -> SystemTime {
        self.received_at
    }

    #[jsg_property(prototype, readonly)]
    pub fn get_expires_at(&self) -> Option<SystemTime> {
        self.expires_at
    }

    #[jsg_method]
    pub fn received_after(&self, millis: u32) -> SystemTime {
        self.received_at + Duration::from_millis(u64::from(millis))
    }
}

// Each read converts the time again, so it gets a new Date, as in C++ JSG.
#[test]
fn properties_and_methods_return_dates() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let at = UNIX_EPOCH + Duration::from_millis(1_700_000_000_123);
        let event = jsg::Rc::new(Event {
            received_at: at,
            expires_at: None,
        });
        ctx.set_global(lock, "event", event);
        let result: String = ctx
            .eval(
                lock,
                "[event.receivedAt instanceof Date, event.receivedAt.toISOString(), \
                  event.receivedAt === event.receivedAt, event.expiresAt, \
                  event.receivedAfter(1000).toISOString()].join(' ')",
            )
            .unwrap();
        assert_eq!(
            result,
            "true 2023-11-14T22:13:20.123Z false  2023-11-14T22:13:21.123Z"
        );

        let event = jsg::Rc::new(Event {
            received_at: at,
            expires_at: Some(at + Duration::from_mins(1)),
        });
        ctx.set_global(lock, "event", event);
        let result: String = ctx.eval(lock, "event.expiresAt.toISOString()").unwrap();
        assert_eq!(result, "2023-11-14T22:14:20.123Z");
        Ok(())
    });
}
