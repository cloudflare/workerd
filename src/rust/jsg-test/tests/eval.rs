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
        let result: f64 = ctx.eval(lock, "1 + 1").unwrap();
        assert!((result - 2.0).abs() < f64::EPSILON);
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
        let result: f64 = ctx.eval(lock, "42").unwrap();
        assert!((result - 42.0).abs() < f64::EPSILON);
        Ok(())
    });
}

#[test]
fn eval_arithmetic_expression() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let result: f64 = ctx.eval(lock, "1 + 2 + 3").unwrap();
        assert!((result - 6.0).abs() < f64::EPSILON);
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
// Lenient<T> tests
// =============================================================================

/// A minimal resource for testing `Lenient<Rc<R>>`.
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
    fn take_lenient(&self, _lock: &mut jsg::Lock, num: jsg::Lenient<jsg::Rc<NumberBox>>) -> f64 {
        match num {
            jsg::Lenient::Some(b) => b.value,
            jsg::Lenient::Unconvertable(_) => 321.0,
            jsg::Lenient::Null => 42.97,
            jsg::Lenient::Undefined => -12.6,
        }
    }
}

#[test]
fn lenient_string_some() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let result: jsg::Lenient<String> = ctx.eval(lock, "'hello'").unwrap();
        assert!(matches!(result, jsg::Lenient::Some(ref s) if s == "hello"));
        Ok(())
    });
}

#[test]
fn lenient_string_undefined() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let result: jsg::Lenient<String> = ctx.eval(lock, "undefined").unwrap();
        assert!(matches!(result, jsg::Lenient::Undefined));
        Ok(())
    });
}

#[test]
fn lenient_string_null() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let result: jsg::Lenient<String> = ctx.eval(lock, "null").unwrap();
        assert!(matches!(result, jsg::Lenient::Null));
        Ok(())
    });
}

#[test]
fn lenient_string_number() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let result: jsg::Lenient<String> = ctx.eval(lock, "42").unwrap();
        let result: Option<jsg::Nullable<String>> = result.into();
        assert_eq!(result, Some(jsg::Nullable::Some("42".to_owned())));
        Ok(())
    });
}

#[test]
fn lenient_array_top_level_mismatch_is_unconvertable() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let result: jsg::Lenient<Vec<f64>> = ctx.eval(lock, "42").unwrap();
        assert!(matches!(result, jsg::Lenient::Unconvertable(_)));
        Ok(())
    });
}

#[test]
fn lenient_array_nested_conversion_error_propagates() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let result = ctx.eval::<jsg::Lenient<Vec<f64>>>(lock, "[Symbol()]");
        assert!(result.is_err());
        Ok(())
    });
}

#[test]
fn lenient_resource_some() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let boxed = jsg::Rc::new(NumberBox { value: 42.0 });
        ctx.set_global(lock, "box", boxed);
        let result: f64 = ctx.eval(lock, "box.takeLenient(box)").unwrap();
        assert!((result - 42.0).abs() < f64::EPSILON);
        Ok(())
    });
}

#[test]
fn lenient_resource_undefined() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let boxed = jsg::Rc::new(NumberBox { value: 42.0 });
        ctx.set_global(lock, "box", boxed);
        let result: f64 = ctx.eval(lock, "box.takeLenient()").unwrap();
        assert!((result - -12.6).abs() < f64::EPSILON);
        Ok(())
    });
}

#[test]
fn lenient_resource_undefined_explicit() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let boxed = jsg::Rc::new(NumberBox { value: 42.0 });
        ctx.set_global(lock, "box", boxed);
        let result: f64 = ctx.eval(lock, "box.takeLenient(undefined)").unwrap();
        assert!((result - -12.6).abs() < f64::EPSILON);
        Ok(())
    });
}

#[test]
fn lenient_resource_null() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let boxed = jsg::Rc::new(NumberBox { value: 42.0 });
        ctx.set_global(lock, "box", boxed);
        let result: f64 = ctx.eval(lock, "box.takeLenient(null)").unwrap();
        assert!((result - 42.97).abs() < f64::EPSILON);
        Ok(())
    });
}

#[test]
fn lenient_resource_wrong_type() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let boxed = jsg::Rc::new(NumberBox { value: 42.0 });
        ctx.set_global(lock, "box", boxed);
        // Passing a function instead of a NumberBox — should be silently treated as absent.
        let result: f64 = ctx.eval(lock, "box.takeLenient(() => {})").unwrap();
        assert!((result - 321.0).abs() < f64::EPSILON);
        Ok(())
    });
}
