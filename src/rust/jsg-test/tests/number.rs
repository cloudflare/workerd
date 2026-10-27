// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! `Number` handles, the counterpart of `v8::Number`.

#![expect(
    clippy::float_cmp,
    reason = "every compared value is exactly representable"
)]

use jsg::ExceptionType;
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
