// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Integer conversion matches C++ JSG (`jsg/value.h`): an integer converts to a
//! number, and from JS, out-of-range values are a `TypeError`, and in-range
//! fractions truncate. `Integer`, `Int32` and `Uint32` handles are the counterparts of the V8
//! classes, and `int32_value()` and the like convert as `v8::Value::Int32Value()` does.

use jsg::ExceptionType;
use jsg::FromJS;
use jsg::NonCoercible;
use jsg::ToJS;
use jsg::v8;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;
use jsg_macros::jsg_struct;

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
    // Without the JSG_INTEGER_ENFORCE_RANGE autogate, a fraction past a limit is out of range.
    let _autogates = crate::set_autogates(&[]);
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
    // Without the JSG_INTEGER_ENFORCE_RANGE autogate, `-0.5` is negative, not `0`.
    let _autogates = crate::set_autogates(&[]);
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let message = "The value cannot be converted because it is negative and this API \
                       expects a positive number.";
        assert_type_error::<u8>(lock, ctx, "-1", message);
        assert_type_error::<u32>(lock, ctx, "-0.5", message);
        Ok(())
    });
}

/// A fraction just past a range limit is out of range without the `JSG_INTEGER_ENFORCE_RANGE`
/// autogate and truncates with it, as in C++ JSG (`jsg/value.h`). What is out of range either
/// way, and what is not finite, converts the same in both states.
#[test]
fn fractions_past_a_range_limit_follow_the_enforce_range_autogate() {
    let negative = "The value cannot be converted because it is negative and this API \
                    expects a positive number.";
    let not_an_integer = "The value cannot be converted because it is not an integer.";
    let at_most = |max: u32| format!("Value out of range. Must be less than or equal to {max}.");
    let between = |min: i32, max: i32| {
        format!("Value out of range. Must be between {min} and {max} (inclusive).")
    };
    let harness = crate::Harness::new();

    {
        let _autogates = crate::set_autogates(&[]);
        harness.run_in_context(|lock, ctx| {
            assert_type_error::<u8>(lock, ctx, "255.5", &at_most(255));
            assert_type_error::<u8>(lock, ctx, "-0.5", negative);
            assert_type_error::<u16>(lock, ctx, "65535.5", &at_most(65535));
            assert_type_error::<u16>(lock, ctx, "-0.5", negative);
            assert_type_error::<u32>(lock, ctx, "2 ** 32 - 0.5", &at_most(u32::MAX));
            assert_type_error::<u32>(lock, ctx, "-0.5", negative);
            assert_type_error::<i8>(lock, ctx, "127.5", &between(-128, 127));
            assert_type_error::<i8>(lock, ctx, "-128.5", &between(-128, 127));
            assert_type_error::<i16>(lock, ctx, "32767.5", &between(-32768, 32767));
            assert_type_error::<i16>(lock, ctx, "-32768.5", &between(-32768, 32767));
            assert_type_error::<i32>(lock, ctx, "2 ** 31 - 0.5", &between(i32::MIN, i32::MAX));
            assert_type_error::<i32>(lock, ctx, "-(2 ** 31) - 0.5", &between(i32::MIN, i32::MAX));
            Ok(())
        });
    }

    {
        let _autogates = crate::set_autogates(&["jsg-integer-enforce-range"]);
        harness.run_in_context(|lock, ctx| {
            assert_eq!(convert::<u8>(lock, ctx, "255.5").unwrap(), 255);
            assert_eq!(convert::<u8>(lock, ctx, "-0.5").unwrap(), 0);
            assert_eq!(convert::<u16>(lock, ctx, "65535.5").unwrap(), 65535);
            assert_eq!(convert::<u16>(lock, ctx, "-0.5").unwrap(), 0);
            assert_eq!(
                convert::<u32>(lock, ctx, "2 ** 32 - 0.5").unwrap(),
                u32::MAX
            );
            assert_eq!(convert::<u32>(lock, ctx, "-0.5").unwrap(), 0);
            assert_eq!(convert::<i8>(lock, ctx, "127.5").unwrap(), 127);
            assert_eq!(convert::<i8>(lock, ctx, "-128.5").unwrap(), -128);
            assert_eq!(convert::<i16>(lock, ctx, "32767.5").unwrap(), 32767);
            assert_eq!(convert::<i16>(lock, ctx, "-32768.5").unwrap(), -32768);
            assert_eq!(
                convert::<i32>(lock, ctx, "2 ** 31 - 0.5").unwrap(),
                i32::MAX
            );
            assert_eq!(
                convert::<i32>(lock, ctx, "-(2 ** 31) - 0.5").unwrap(),
                i32::MIN
            );
            assert_eq!(convert::<i32>(lock, ctx, "-0.5").unwrap(), 0);

            // Unchanged by the autogate.
            assert_eq!(convert::<u8>(lock, ctx, "3.7").unwrap(), 3);
            assert_type_error::<u8>(lock, ctx, "256", &at_most(255));
            assert_type_error::<u8>(lock, ctx, "-1", negative);
            assert_type_error::<i32>(lock, ctx, "2 ** 31", &between(i32::MIN, i32::MAX));
            assert_type_error::<i32>(lock, ctx, "-(2 ** 31) - 1", &between(i32::MIN, i32::MAX));
            assert_type_error::<u32>(lock, ctx, "NaN", not_an_integer);
            assert_type_error::<i16>(lock, ctx, "-Infinity", not_an_integer);
            assert_eq!(convert::<i32>(lock, ctx, "NaN").unwrap(), 0);
            assert_eq!(convert::<i32>(lock, ctx, "Infinity").unwrap(), 0);
            Ok(())
        });
    }
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

/// An integer returned in an `Option`, a `Nullable`, a `Vec` or a `#[jsg_struct]` field is a
/// number too.
#[test]
fn integers_in_containers_convert_to_numbers() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        install(lock, ctx);
        assert!(ctx.eval::<bool>(lock, "r.first([7, 8]) === 7").unwrap());
        assert!(
            ctx.eval::<bool>(lock, "r.first([-(2 ** 31)]) === -2147483648")
                .unwrap()
        );
        assert!(ctx.eval::<bool>(lock, "r.first([]) === undefined").unwrap());
        assert!(
            ctx.eval::<bool>(lock, "r.max([1, 2 ** 32 - 1]) === 4294967295")
                .unwrap()
        );
        assert!(ctx.eval::<bool>(lock, "r.max([]) === null").unwrap());
        assert!(
            ctx.eval::<bool>(
                lock,
                "const b = r.bounds();
             b.min === -2147483648 && b.max === 4294967295 && b.small === -1"
            )
            .unwrap()
        );
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
            convert::<Vec<i16>>(lock, ctx, "[-1, 2]").unwrap(),
            vec![-1, 2]
        );
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

#[jsg_struct]
struct Bounds {
    pub min: i32,
    pub max: u32,
    pub small: i8,
}

#[jsg_resource]
struct Integers;

#[jsg_resource]
impl Integers {
    #[jsg_method]
    pub fn int32(&self, value: v8::Local<'_, v8::Int32>) -> i32 {
        value.value()
    }

    #[jsg_method]
    pub fn uint32(&self, value: v8::Local<'_, v8::Uint32>) -> u32 {
        value.value()
    }

    #[jsg_method]
    pub fn integer(&self, value: v8::Local<'_, v8::Integer>) -> i64 {
        value.value()
    }

    #[jsg_method]
    pub fn first(&self, values: Vec<i32>) -> Option<i32> {
        values.first().copied()
    }

    #[jsg_method]
    pub fn max(&self, values: Vec<u32>) -> jsg::Nullable<u32> {
        values.iter().max().copied().into()
    }

    #[jsg_method]
    pub fn bounds(&self) -> Bounds {
        Bounds {
            min: i32::MIN,
            max: u32::MAX,
            small: -1,
        }
    }

    #[jsg_method]
    pub fn to_int32<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        value: v8::Local<'s, v8::Value>,
    ) -> Result<i32, jsg::Error> {
        value.int32_value(lock)
    }
}

fn install(lock: &mut jsg::Lock<'_>, ctx: &crate::EvalContext<'_>) {
    let resource = jsg::Rc::new(Integers).to_js(lock).unwrap();
    ctx.set_global(lock, "r", resource);
}

/// `Integer::new` and `new_from_unsigned` make numbers, which cast to `Int32` or `Uint32` when
/// they are in range, and the checks for all three match V8's.
#[test]
fn integer_handles() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let signed = v8::Integer::new(lock, -7);
        assert_eq!(signed.value(), -7);
        let number: v8::Local<v8::Number> = signed.into();
        assert_eq!(number.value().to_bits(), (-7.0_f64).to_bits());
        let value: v8::Local<v8::Value> = signed.into();
        assert!(value.is_number());
        assert!(value.is_int32());
        assert!(!value.is_uint32());
        assert_eq!(v8::Local::<v8::Int32>::try_from(value).unwrap().value(), -7);
        assert!(v8::Local::<v8::Uint32>::try_from(value).is_err());
        let integer: v8::Local<v8::Integer> = number.try_into().unwrap();
        assert!(v8::Local::<v8::Int32>::try_from(integer).is_ok());
        assert!(v8::Local::<v8::Uint32>::try_from(integer).is_err());
        ctx.set_global(lock, "v", signed);
        assert!(ctx.eval::<bool>(lock, "v === -7").unwrap());

        let unsigned: v8::Local<v8::Value> = v8::Integer::new_from_unsigned(lock, u32::MAX).into();
        assert!(!unsigned.is_int32());
        assert!(unsigned.is_uint32());
        let uint32 = v8::Local::<v8::Uint32>::try_from(unsigned).unwrap();
        assert_eq!(uint32.value(), u32::MAX);
        assert_eq!(
            v8::Local::<v8::Integer>::from(uint32).value(),
            i64::from(u32::MAX)
        );
        assert!(v8::Local::<v8::Integer>::try_from(unsigned).is_ok());
        assert!(v8::Local::<v8::Int32>::try_from(unsigned).is_err());
        let traced = v8::TracedGlobal::new(lock, uint32);
        assert_eq!(traced.as_local(lock).value(), u32::MAX);
        ctx.set_global(lock, "v", unsigned);
        assert!(ctx.eval::<bool>(lock, "v === 4294967295").unwrap());

        let zero: v8::Local<v8::Value> = v8::Integer::new(lock, 0).into();
        assert!(zero.is_int32());
        assert!(zero.is_uint32());

        // A number out of both ranges, a fraction, `-0`, or anything that is not a number, is
        // none of the three.
        for expr in [
            "2 ** 32",
            "-(2 ** 31) - 1",
            "1.5",
            "-0",
            "NaN",
            "Infinity",
            "'1'",
            "1n",
            "new Number(1)",
        ] {
            let not_integer = ctx.eval_raw(lock, expr).unwrap();
            assert!(!not_integer.is_int32(), "{expr}");
            assert!(!not_integer.is_uint32(), "{expr}");
            assert!(
                v8::Local::<v8::Integer>::try_from(not_integer).is_err(),
                "{expr}"
            );
        }
        Ok(())
    });
}

/// A handle parameter takes a number in range as it is, without `ToNumber` or truncation.
#[test]
fn integer_handle_parameters_do_not_coerce() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        install(lock, ctx);
        assert_eq!(
            ctx.eval::<i32>(lock, "r.int32(-(2 ** 31))").unwrap(),
            i32::MIN
        );
        assert_eq!(
            ctx.eval::<u32>(lock, "r.uint32(2 ** 32 - 1)").unwrap(),
            u32::MAX
        );
        assert_eq!(
            ctx.eval::<i64>(lock, "r.integer(2 ** 31)").unwrap(),
            1 << 31
        );
        assert_eq!(ctx.eval::<i64>(lock, "r.integer(-1)").unwrap(), -1);

        for (expr, expected) in [
            ("r.int32(1.5)", "Int32"),
            ("r.int32(2 ** 31)", "Int32"),
            ("r.int32('7')", "Int32"),
            ("r.int32({ valueOf() { return 7; } })", "Int32"),
            ("r.uint32(-1)", "Uint32"),
            ("r.uint32(2 ** 32)", "Uint32"),
            ("r.integer(2 ** 32)", "Integer"),
            ("r.integer(-(2 ** 31) - 1)", "Integer"),
            ("r.integer(0.5)", "Integer"),
        ] {
            let err = ctx
                .eval::<f64>(lock, expr)
                .unwrap_err()
                .unwrap_jsg_err(lock);
            assert_eq!(err.name, ExceptionType::TypeError, "{expr}");
            assert!(err.message.contains(expected), "{expr}: {}", err.message);
        }

        // `Lenient` applies the same check through `try_from_js`.
        let lenient: jsg::Lenient<v8::Local<v8::Uint32>> = ctx.eval(lock, "7").unwrap();
        assert!(matches!(lenient, jsg::Lenient::Some(n) if n.value() == 7));
        let lenient: jsg::Lenient<v8::Local<v8::Uint32>> = ctx.eval(lock, "-1").unwrap();
        assert!(matches!(lenient, jsg::Lenient::Unconvertable(_)));
        Ok(())
    });
}

/// `int32_value`, `uint32_value` and `integer_value` convert any value as JavaScript's
/// `ToInt32`, `ToUint32` and `ToInteger` do, running `valueOf` where there is one.
#[test]
fn integer_coercions() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let value_of = "({ valueOf() { return -2.5; } })";
        for (expr, expected) in [
            ("7", 7),
            ("-7", -7),
            ("1.9", 1),
            ("-1.9", -1),
            ("-0", 0),
            ("NaN", 0),
            ("Infinity", 0),
            ("-Infinity", 0),
            ("2 ** 32 + 5", 5),
            ("2 ** 31", i32::MIN),
            ("'7'", 7),
            ("' 0x10 '", 16),
            ("true", 1),
            ("null", 0),
            ("undefined", 0),
            ("[3]", 3),
            (value_of, -2),
        ] {
            let value = ctx.eval_raw(lock, expr).unwrap();
            assert_eq!(value.int32_value(lock).unwrap(), expected, "{expr}");
        }
        for (expr, expected) in [
            ("7", 7),
            ("-1", u32::MAX),
            ("2 ** 32 + 5", 5),
            ("2 ** 31", 1 << 31),
            ("NaN", 0),
            ("-0", 0),
            ("'7'", 7),
            (value_of, u32::MAX - 1),
        ] {
            let value = ctx.eval_raw(lock, expr).unwrap();
            assert_eq!(value.uint32_value(lock).unwrap(), expected, "{expr}");
        }
        for (expr, expected) in [
            ("1.9", 1),
            ("-1.9", -1),
            ("NaN", 0),
            ("2 ** 32 + 5", (1 << 32) + 5),
            ("2 ** 53", 1 << 53),
            ("2 ** 63", i64::MAX),
            ("-(2 ** 63)", i64::MIN),
            ("Infinity", i64::MAX),
            ("-Infinity", i64::MIN),
            ("'7'", 7),
            (value_of, -2),
        ] {
            let value = ctx.eval_raw(lock, expr).unwrap();
            assert_eq!(value.integer_value(lock).unwrap(), expected, "{expr}");
        }
        Ok(())
    });
}

#[derive(Clone, Copy, Debug)]
enum Coercion {
    Int32,
    Uint32,
    Integer,
}

/// What `valueOf` throws, or `ToNumber` throws for a `BigInt` or a `Symbol`, stays pending, so
/// that a method passing the error on throws it to its caller.
#[test]
fn integer_coercions_pass_on_what_to_number_throws() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let thrower = "({ valueOf() { throw new RangeError('boom'); } })";
        for (expr, name, message) in [
            (thrower, ExceptionType::RangeError, "boom"),
            (
                "1n",
                ExceptionType::TypeError,
                "Cannot convert a BigInt value to a number",
            ),
            (
                "Symbol()",
                ExceptionType::TypeError,
                "Cannot convert a Symbol value to a number",
            ),
        ] {
            let value = ctx.eval_raw(lock, expr).unwrap();
            for coercion in [Coercion::Int32, Coercion::Uint32, Coercion::Integer] {
                let caught = lock
                    .try_catch(|lock| {
                        let err = match coercion {
                            Coercion::Int32 => value.int32_value(lock).map(drop),
                            Coercion::Uint32 => value.uint32_value(lock).map(drop),
                            Coercion::Integer => value.integer_value(lock).map(drop),
                        }
                        .unwrap_err();
                        assert!(err.is_pending(), "{expr}");
                        Err::<(), _>(err)
                    })
                    .expect("execution should not be terminating");
                let Err(exception) = caught else {
                    panic!("{expr} should not convert");
                };
                let err = jsg::Error::from_js(lock, exception).unwrap();
                assert_eq!(err.name, name, "{expr} ({coercion:?})");
                assert_eq!(err.message, message, "{expr} ({coercion:?})");
            }
        }

        install(lock, ctx);
        assert_eq!(ctx.eval::<i32>(lock, "r.toInt32('0x10')").unwrap(), 16);
        let err = ctx
            .eval::<i32>(lock, &format!("r.toInt32({thrower})"))
            .unwrap_err()
            .unwrap_jsg_err(lock);
        assert_eq!(err.name, ExceptionType::RangeError);
        assert_eq!(err.message, "boom");
        Ok(())
    });
}
