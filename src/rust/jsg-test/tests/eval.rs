// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use jsg::FromJS;
use jsg_macros::jsg_constructor;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;

use crate::EvalError;

/// A panic in a `run_in_context` callback reaches the caller without unwinding through C++,
/// and the harness stays usable.
#[test]
fn run_in_context_resumes_panic_and_stays_usable() {
    let harness = crate::Harness::new();
    let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        harness.run_in_context(|_lock, _ctx| -> Result<(), jsg::Error> {
            panic!("intentional panic in run_in_context");
        });
    }));
    let payload = result.expect_err("the panic must reach the caller");
    assert_eq!(
        payload.downcast_ref::<&str>(),
        Some(&"intentional panic in run_in_context")
    );
    harness.run_in_context(|lock, ctx| {
        assert_eq!(ctx.eval::<u32>(lock, "1 + 1").unwrap(), 2);
        Ok(())
    });
}

#[test]
fn eval_returns_correct_type() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let result: String = ctx.eval(lock, "'Hello, World!'").unwrap();
        assert_eq!(result, "Hello, World!");
        Ok(())
    });
}

#[test]
fn eval_string_concatenation() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let result: String = ctx.eval(lock, "'Hello' + ', ' + 'World!'").unwrap();
        assert_eq!(result, "Hello, World!");
        Ok(())
    });
}

#[test]
fn eval_number_returns_number_type() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        assert_eq!(ctx.eval::<u32>(lock, "42").unwrap(), 42);
        Ok(())
    });
}

#[test]
fn eval_arithmetic_expression() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        assert_eq!(ctx.eval::<u32>(lock, "1 + 2 + 3").unwrap(), 6);
        Ok(())
    });
}

#[test]
fn eval_boolean_true() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let result: bool = ctx.eval(lock, "true").unwrap();
        assert!(result);
        Ok(())
    });
}

#[test]
fn eval_boolean_false() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let result: bool = ctx.eval(lock, "false").unwrap();
        assert!(!result);
        Ok(())
    });
}

#[test]
fn eval_comparison_expression() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let result: bool = ctx.eval(lock, "5 > 3").unwrap();
        assert!(result);
        Ok(())
    });
}

#[test]
fn eval_null() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let result: jsg::Nullable<bool> = ctx.eval(lock, "null").unwrap();
        assert!(result.is_null());
        Ok(())
    });
}

#[test]
fn eval_undefined() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let result: Option<jsg::Nullable<bool>> = ctx.eval(lock, "undefined").unwrap();
        assert!(result.is_none());
        let result: jsg::Nullable<bool> = ctx.eval(lock, "undefined").unwrap();
        assert!(result.is_null());
        Ok(())
    });
}

#[test]
fn eval_some() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let result: jsg::Nullable<bool> = ctx.eval(lock, "true").unwrap();
        assert!(result.is_some());
        assert!(result.unwrap());
        Ok(())
    });
}

#[test]
fn eval_throws_on_error() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let result = ctx.eval::<bool>(lock, "throw new Error('test error')");
        match result.unwrap_err() {
            EvalError::Exception(value) => {
                assert_eq!(String::from_js(lock, value).unwrap(), "Error: test error");
            }
            _ => panic!("Unexpected error type"),
        }
        Ok(())
    });
}

#[test]
fn eval_throws_string_preserves_message() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let result = ctx.eval::<bool>(lock, "throw 'custom string error'");
        match result.unwrap_err() {
            EvalError::Exception(value) => {
                assert_eq!(String::from_js(lock, value).unwrap(), "custom string error");
            }
            _ => panic!("Unexpected error type"),
        }
        Ok(())
    });
}

#[test]
fn eval_function_call() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let result: String = ctx
            .eval(lock, "(function() { return 'from function'; })()")
            .unwrap();
        assert_eq!(result, "from function");
        Ok(())
    });
}

#[test]
fn eval_typeof_string() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let result: String = ctx.eval(lock, "typeof 'hello'").unwrap();
        assert_eq!(result, "string");
        Ok(())
    });
}

#[test]
fn eval_typeof_number() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let result: String = ctx.eval(lock, "typeof 42").unwrap();
        assert_eq!(result, "number");
        Ok(())
    });
}

#[test]
fn eval_typeof_boolean() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let result: String = ctx.eval(lock, "typeof true").unwrap();
        assert_eq!(result, "boolean");
        Ok(())
    });
}

#[test]
fn eval_unicode_string() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let result: String = ctx.eval(lock, "'こんにちは'").unwrap();
        assert_eq!(result, "こんにちは");
        Ok(())
    });
}

#[test]
fn eval_emoji_string() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let result: String = ctx.eval(lock, "'😀🎉'").unwrap();
        assert_eq!(result, "😀🎉");
        Ok(())
    });
}

#[test]
fn eval_number_string() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let result: String = ctx.eval(lock, "42").unwrap();
        assert_eq!(result, "42");
        Ok(())
    });
}

#[test]
fn eval_function_string() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let result: String = ctx.eval(lock, "(() => {})").unwrap();
        assert_eq!(result, r"() => {}");
        Ok(())
    });
}

// =============================================================================
// LenientOption<T> tests
// =============================================================================

/// A minimal resource for testing `LenientOption<Rc<R>>`.
#[jsg_resource]
struct NumberBox {
    value: f64,
}

#[jsg_resource]
impl NumberBox {
    #[jsg_constructor]
    fn new(value: f64) -> Self {
        Self { value }
    }

    #[jsg_method]
    fn take_lenient(
        &self,
        lock: &mut jsg::Lock,
        num: jsg::LenientOption<jsg::Rc<NumberBox>>,
    ) -> String {
        num.0
            .map_or_else(|| "absent".to_owned(), |b| b.get(lock).value.to_string())
    }

    #[jsg_method]
    fn echo_lenient(&self, value: jsg::LenientOption<String>) -> jsg::LenientOption<String> {
        value
    }
}

fn eval_lenient_string(
    lock: &mut jsg::Lock,
    ctx: &crate::EvalContext<'_>,
    code: &str,
) -> Option<String> {
    ctx.eval::<jsg::LenientOption<String>>(lock, code)
        .unwrap()
        .0
}

#[test]
fn lenient_string() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        assert_eq!(
            eval_lenient_string(lock, ctx, "'hello'").as_deref(),
            Some("hello")
        );
        assert_eq!(eval_lenient_string(lock, ctx, "undefined"), None);
        // As for C++ `LenientOptional<kj::String>`, null goes through the string conversion.
        assert_eq!(
            eval_lenient_string(lock, ctx, "null").as_deref(),
            Some("null")
        );
        assert_eq!(eval_lenient_string(lock, ctx, "42").as_deref(), Some("42"));
        Ok(())
    });
}

#[test]
fn lenient_non_coercible_mismatch_is_absent() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        type Lenient = jsg::LenientOption<jsg::NonCoercible<String>>;
        assert!(ctx.eval::<Lenient>(lock, "1").unwrap().is_none());
        assert!(ctx.eval::<Lenient>(lock, "null").unwrap().is_none());
        let value = ctx.eval::<Lenient>(lock, "'x'").unwrap().0;
        assert_eq!(
            value.map(jsg::NonCoercible::into_inner).as_deref(),
            Some("x")
        );
        Ok(())
    });
}

// As for C++ `LenientOptional<uint32_t>`, a coercing `T` takes any value, and a value it
// can't coerce is an error rather than absent.
#[test]
fn lenient_coercing_integer_errors_are_not_absent() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        type Lenient = jsg::LenientOption<u32>;
        assert_eq!(
            ctx.eval::<Lenient>(lock, "null").unwrap().into_inner(),
            Some(0)
        );
        assert_eq!(
            ctx.eval::<Lenient>(lock, "'7'").unwrap().into_inner(),
            Some(7)
        );
        assert!(ctx.eval::<Lenient>(lock, "undefined").unwrap().is_none());
        for code in ["'abc'", "-1"] {
            let Err(EvalError::UncoercibleResult { message, .. }) = ctx.eval::<Lenient>(lock, code)
            else {
                panic!("{code} should not convert");
            };
            assert!(message.starts_with("TypeError"), "{code}: {message}");
        }
        Ok(())
    });
}

#[test]
fn lenient_array_top_level_mismatch_is_absent() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let result: jsg::LenientOption<Vec<u32>> = ctx.eval(lock, "42").unwrap();
        assert!(result.is_none());
        Ok(())
    });
}

#[test]
fn lenient_array_nested_conversion_error_propagates() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let result = ctx.eval::<jsg::LenientOption<Vec<u32>>>(lock, "[Symbol()]");
        assert!(result.is_err());
        Ok(())
    });
}

#[test]
fn lenient_resource() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let boxed = jsg::Rc::new(lock, NumberBox { value: 42.0 });
        ctx.set_global(lock, "box", boxed);
        let take = |lock: &mut jsg::Lock, code: &str| -> String { ctx.eval(lock, code).unwrap() };
        assert_eq!(take(lock, "box.takeLenient(box)"), "42");
        assert_eq!(take(lock, "box.takeLenient()"), "absent");
        assert_eq!(take(lock, "box.takeLenient(undefined)"), "absent");
        // As for C++ `LenientOptional<Ref<T>>` (value-test.c++), null is absent for a resource.
        assert_eq!(take(lock, "box.takeLenient(null)"), "absent");
        // Passing a function instead of a NumberBox is absent too.
        assert_eq!(take(lock, "box.takeLenient(() => {})"), "absent");
        Ok(())
    });
}

#[test]
fn lenient_to_js() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let boxed = jsg::Rc::new(lock, NumberBox { value: 0.0 });
        ctx.set_global(lock, "box", boxed);
        let result: String = ctx
            .eval(
                lock,
                "[box.echoLenient('a'), box.echoLenient(), box.echoLenient(null)]
                     .map((v) => `${typeof v}:${v}`).join()",
            )
            .unwrap();
        assert_eq!(result, "string:a,undefined:undefined,string:null");
        Ok(())
    });
}
