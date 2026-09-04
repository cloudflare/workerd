// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use jsg::Number;
use jsg::ToJS;
use jsg_macros::jsg_constructor;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;

use crate::EvalError;

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
        let result: Number = ctx.eval(lock, "42").unwrap();
        assert!((result.value() - 42.0).abs() < f64::EPSILON);
        Ok(())
    });
}

#[test]
fn eval_arithmetic_expression() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let result: Number = ctx.eval(lock, "1 + 2 + 3").unwrap();
        assert!((result.value() - 6.0).abs() < f64::EPSILON);
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
        let result: jsg::Nullable<bool> = ctx.eval(lock, "undefined").unwrap();
        assert!(result.is_undefined());
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
                assert_eq!(value.to_string(), "Error: test error");
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
                assert_eq!(value.to_string(), "custom string error");
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
    value: jsg::Number,
}

#[jsg_resource]
impl NumberBox {
    #[jsg_constructor]
    fn new(value: jsg::Number) -> Self {
        Self { value }
    }

    #[jsg_method]
    fn take_lenient(
        &self,
        _lock: &mut jsg::Lock,
        num: jsg::Lenient<jsg::Rc<NumberBox>>,
    ) -> jsg::Number {
        match num {
            jsg::Lenient::Some(b) => b.value,
            jsg::Lenient::Unconvertable(_) => jsg::Number::new(321.0),
            jsg::Lenient::Null => jsg::Number::new(42.97),
            jsg::Lenient::Undefined => jsg::Number::new(-12.6),
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
        let result: jsg::Nullable<String> = result.into();
        assert_eq!(&result.unwrap(), "42");
        Ok(())
    });
}

#[test]
fn lenient_resource_some() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let boxed = jsg::Rc::new(NumberBox {
            value: jsg::Number::new(42.0),
        });
        ctx.set_global("box", boxed.to_js(lock));
        let result: jsg::Number = ctx.eval(lock, "box.takeLenient(box)").unwrap();
        assert!((result.value() - 42.0).abs() < f64::EPSILON);
        Ok(())
    });
}

#[test]
fn lenient_resource_undefined() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let boxed = jsg::Rc::new(NumberBox {
            value: jsg::Number::new(42.0),
        });
        ctx.set_global("box", boxed.to_js(lock));
        let result: jsg::Number = ctx.eval(lock, "box.takeLenient()").unwrap();
        assert!((result.value() - -12.6).abs() < f64::EPSILON);
        Ok(())
    });
}

#[test]
fn lenient_resource_undefined_explicit() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let boxed = jsg::Rc::new(NumberBox {
            value: jsg::Number::new(42.0),
        });
        ctx.set_global("box", boxed.to_js(lock));
        let result: jsg::Number = ctx.eval(lock, "box.takeLenient(undefined)").unwrap();
        assert!((result.value() - -12.6).abs() < f64::EPSILON);
        Ok(())
    });
}

#[test]
fn lenient_resource_null() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let boxed = jsg::Rc::new(NumberBox {
            value: jsg::Number::new(42.0),
        });
        ctx.set_global("box", boxed.to_js(lock));
        let result: jsg::Number = ctx.eval(lock, "box.takeLenient(null)").unwrap();
        assert!((result.value() - 42.97).abs() < f64::EPSILON);
        Ok(())
    });
}

#[test]
fn lenient_resource_wrong_type() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let boxed = jsg::Rc::new(NumberBox {
            value: jsg::Number::new(42.0),
        });
        ctx.set_global("box", boxed.to_js(lock));
        // Passing a function instead of a NumberBox — should be silently treated as absent.
        let result: jsg::Number = ctx.eval(lock, "box.takeLenient(() => {})").unwrap();
        assert!((result.value() - 321.0).abs() < f64::EPSILON);
        Ok(())
    });
}
