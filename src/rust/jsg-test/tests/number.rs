// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! `Number` handles, the counterpart of `v8::Number`, and the `f64` conversion, which matches
//! C++ JSG's `double` (`jsg/value.h`).

#![expect(
    clippy::float_cmp,
    reason = "every compared value is exactly representable"
)]

use jsg::ExceptionType;
use jsg::FromJS;
use jsg::NonCoercible;
use jsg::ToJS;
use jsg::v8;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;

#[jsg_resource]
struct Numbers;

#[jsg_resource]
impl Numbers {
    #[jsg_method]
    pub fn double(&self, value: v8::Local<'_, v8::Number>) -> f64 {
        value.value() * 2.0
    }

    #[jsg_method]
    pub fn id(&self, value: f64) -> f64 {
        value
    }

    #[jsg_method]
    pub fn strict(&self, value: NonCoercible<f64>) -> f64 {
        value.into_inner()
    }

    #[jsg_method]
    pub fn sum(&self, values: Vec<f64>) -> f64 {
        values.iter().sum()
    }

    #[jsg_method]
    pub fn first(&self, values: Vec<f64>) -> Option<f64> {
        values.first().copied()
    }
}

fn install(lock: &mut jsg::Lock<'_>, ctx: &crate::EvalContext<'_>) {
    let resource = jsg::Rc::new(Numbers).to_js(lock).unwrap();
    ctx.set_global(lock, "r", resource);
}

/// Evaluates `expr` and converts it to an `f64`, as an argument would be.
fn convert(lock: &mut jsg::Lock<'_>, ctx: &crate::EvalContext<'_>, expr: &str) -> f64 {
    let value = ctx.eval_raw(lock, expr).unwrap();
    f64::from_js(lock, value).unwrap_or_else(|err| panic!("{expr} should convert: {err}"))
}

/// Asserts that converting `expr` to `T` throws a `TypeError` with `message`: either an error
/// the conversion fails with, or what V8 threw during the coercion, which stays pending.
fn assert_type_error<'s, T: FromJS<'s>>(
    lock: &mut jsg::Lock<'s>,
    ctx: &crate::EvalContext<'_>,
    expr: &str,
    message: &str,
) {
    let value = ctx.eval_raw(lock, expr).unwrap();
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
fn number_handles() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let number = v8::Number::new(lock, 2.5);
        assert_eq!(number.value(), 2.5);
        let traced = v8::TracedGlobal::new(lock, number);
        assert_eq!(traced.as_local(lock).value(), 2.5);

        let value: v8::Local<v8::Value> = number.into();
        assert!(value.is_number());
        assert!(!value.is_object());
        assert!(!value.is_boolean());
        ctx.set_global(lock, "n", value);
        assert!(ctx.eval::<bool>(lock, "n === 2.5").unwrap());

        // The value survives unchanged, including a sign on zero and NaN.
        let negative_zero = v8::Number::new(lock, -0.0).value();
        assert_eq!(negative_zero, 0.0);
        assert!(negative_zero.is_sign_negative());
        assert!(v8::Number::new(lock, f64::NAN).value().is_nan());
        assert_eq!(
            v8::Number::new(lock, f64::NEG_INFINITY).value(),
            f64::NEG_INFINITY
        );

        let from_js = ctx.eval_raw(lock, "2 ** 53").unwrap();
        assert_eq!(
            v8::Local::<v8::Number>::try_from(from_js).unwrap().value(),
            9_007_199_254_740_992.0
        );

        // A Number object is not a number, nor is anything that coerces to one.
        for expr in ["new Number(1)", "'1'", "1n", "true", "null", "undefined"] {
            let not_number = ctx.eval_raw(lock, expr).unwrap();
            assert!(
                v8::Local::<v8::Number>::try_from(not_number).is_err(),
                "{expr}"
            );
        }

        // `Lenient` applies the same check through `try_from_js`.
        let lenient: jsg::Lenient<v8::Local<v8::Number>> = ctx.eval(lock, "1").unwrap();
        assert!(matches!(lenient, jsg::Lenient::Some(n) if n.value() == 1.0));
        let lenient: jsg::Lenient<v8::Local<v8::Number>> = ctx.eval(lock, "'1'").unwrap();
        assert!(matches!(lenient, jsg::Lenient::Unconvertable(_)));
        Ok(())
    });
}

/// A `Local<Number>` parameter takes a number as it is, without `ToNumber`.
#[test]
fn number_handle_parameter_does_not_coerce() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Rc::new(Numbers).to_js(lock)?;
        ctx.set_global(lock, "r", resource);

        assert_eq!(
            ctx.eval::<jsg::Number>(lock, "r.double(21)")
                .unwrap()
                .value(),
            42.0
        );

        for expr in ["r.double('21')", "r.double({ valueOf() { return 21; } })"] {
            let err = ctx
                .eval::<jsg::Number>(lock, expr)
                .unwrap_err()
                .unwrap_jsg_err(lock);
            assert_eq!(err.name, ExceptionType::TypeError, "{expr}");
            assert!(err.message.contains("Number"), "{expr}: {}", err.message);
        }
        Ok(())
    });
}

/// An `f64` converts with `ToNumber`, like C++ JSG's `double`.
#[test]
fn f64_converts_with_to_number() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        assert_eq!(convert(lock, ctx, "42"), 42.0);
        assert_eq!(convert(lock, ctx, "-2.5"), -2.5);
        assert_eq!(convert(lock, ctx, "'42'"), 42.0);
        assert_eq!(convert(lock, ctx, "' 0x10 '"), 16.0);
        assert_eq!(convert(lock, ctx, "true"), 1.0);
        assert_eq!(convert(lock, ctx, "null"), 0.0);
        assert_eq!(convert(lock, ctx, "[]"), 0.0);
        assert_eq!(convert(lock, ctx, "({ valueOf() { return 3; } })"), 3.0);
        assert_eq!(convert(lock, ctx, "new Number(7)"), 7.0);
        assert!(convert(lock, ctx, "undefined").is_nan());
        assert!(convert(lock, ctx, "NaN").is_nan());
        assert!(convert(lock, ctx, "'abc'").is_nan());
        assert_eq!(convert(lock, ctx, "Infinity"), f64::INFINITY);

        let negative_zero = convert(lock, ctx, "-0");
        assert_eq!(negative_zero, 0.0);
        assert!(negative_zero.is_sign_negative());
        assert!(!convert(lock, ctx, "0").is_sign_negative());

        // `ToNumber` rejects a BigInt and a Symbol, unlike JavaScript's `Number()`.
        assert_type_error::<f64>(lock, ctx, "1n", "Cannot convert a BigInt value to a number");
        assert_type_error::<f64>(
            lock,
            ctx,
            "Symbol()",
            "Cannot convert a Symbol value to a number",
        );
        Ok(())
    });
}

/// An `f64` parameter and return value pass a number through unchanged, including `-0` and NaN.
#[test]
fn f64_parameters_and_return_values() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        install(lock, ctx);
        assert_eq!(ctx.eval::<f64>(lock, "r.id(1.5)").unwrap(), 1.5);
        assert_eq!(ctx.eval::<f64>(lock, "r.id('2')").unwrap(), 2.0);
        assert!(ctx.eval::<bool>(lock, "Object.is(r.id(-0), -0)").unwrap());
        assert!(ctx.eval::<bool>(lock, "Number.isNaN(r.id(NaN))").unwrap());
        assert!(
            ctx.eval::<bool>(lock, "Number.isNaN(r.id(undefined))")
                .unwrap()
        );

        assert_eq!(ctx.eval::<f64>(lock, "r.sum([1, 2.5, '3'])").unwrap(), 6.5);
        assert_eq!(
            ctx.eval::<Vec<f64>>(lock, "[1, 2, 3]").unwrap(),
            [1.0, 2.0, 3.0]
        );
        assert_eq!(
            ctx.eval::<Option<f64>>(lock, "r.first([4])").unwrap(),
            Some(4.0)
        );
        assert_eq!(ctx.eval::<Option<f64>>(lock, "r.first([])").unwrap(), None);

        let err = ctx
            .eval::<f64>(lock, "r.id(1n)")
            .unwrap_err()
            .unwrap_jsg_err(lock);
        assert_eq!(err.name, ExceptionType::TypeError);
        Ok(())
    });
}

/// `NonCoercible<f64>` accepts only a number, including `-0` and NaN.
#[test]
fn non_coercible_f64_accepts_only_numbers() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        install(lock, ctx);
        assert_eq!(ctx.eval::<f64>(lock, "r.strict(1.5)").unwrap(), 1.5);
        assert!(
            ctx.eval::<bool>(lock, "Object.is(r.strict(-0), -0)")
                .unwrap()
        );
        assert!(
            ctx.eval::<bool>(lock, "Number.isNaN(r.strict(NaN))")
                .unwrap()
        );

        for expr in [
            "r.strict('1')",
            "r.strict(1n)",
            "r.strict(true)",
            "r.strict(null)",
            "r.strict(new Number(1))",
            "r.strict({ valueOf() { return 1; } })",
            "r.strict(undefined)",
        ] {
            let err = ctx
                .eval::<f64>(lock, expr)
                .unwrap_err()
                .unwrap_jsg_err(lock);
            assert_eq!(err.name, ExceptionType::TypeError, "{expr}");
            assert!(
                err.message
                    .ends_with("parameter 1 is not of type 'number'."),
                "{expr}: {}",
                err.message
            );
        }
        Ok(())
    });
}
