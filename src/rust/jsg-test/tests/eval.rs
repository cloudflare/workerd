use crate::EvalResult;

#[test]
fn eval_string_returns_string_type() {
    let harness = crate::Harness::new();
    harness.run_in_context(|_isolate, ctx| {
        assert_eq!(
            ctx.eval_safe("'Hello, World!'"),
            EvalResult {
                success: true,
                result_type: "string".to_owned(),
                result_value: "Hello, World!".to_owned(),
            }
        );
    });
}

#[test]
fn eval_string_concatenation() {
    let harness = crate::Harness::new();
    harness.run_in_context(|_isolate, ctx| {
        assert_eq!(
            ctx.eval_safe("'Hello' + ', ' + 'World!'"),
            EvalResult {
                success: true,
                result_type: "string".to_owned(),
                result_value: "Hello, World!".to_owned(),
            }
        );
    });
}

#[test]
fn eval_number_returns_number_type() {
    let harness = crate::Harness::new();
    harness.run_in_context(|_isolate, ctx| {
        assert_eq!(
            ctx.eval_safe("42"),
            EvalResult {
                success: true,
                result_type: "number".to_owned(),
                result_value: "42".to_owned(),
            }
        );
    });
}

#[test]
fn eval_arithmetic_expression() {
    let harness = crate::Harness::new();
    harness.run_in_context(|_isolate, ctx| {
        assert_eq!(
            ctx.eval_safe("1 + 2 + 3"),
            EvalResult {
                success: true,
                result_type: "number".to_owned(),
                result_value: "6".to_owned(),
            }
        );
    });
}

#[test]
fn eval_boolean_true() {
    let harness = crate::Harness::new();
    harness.run_in_context(|_isolate, ctx| {
        assert_eq!(
            ctx.eval_safe("true"),
            EvalResult {
                success: true,
                result_type: "boolean".to_owned(),
                result_value: "true".to_owned(),
            }
        );
    });
}

#[test]
fn eval_boolean_false() {
    let harness = crate::Harness::new();
    harness.run_in_context(|_isolate, ctx| {
        assert_eq!(
            ctx.eval_safe("false"),
            EvalResult {
                success: true,
                result_type: "boolean".to_owned(),
                result_value: "false".to_owned(),
            }
        );
    });
}

#[test]
fn eval_comparison_expression() {
    let harness = crate::Harness::new();
    harness.run_in_context(|_isolate, ctx| {
        assert_eq!(
            ctx.eval_safe("5 > 3"),
            EvalResult {
                success: true,
                result_type: "boolean".to_owned(),
                result_value: "true".to_owned(),
            }
        );
    });
}

#[test]
fn eval_undefined() {
    let harness = crate::Harness::new();
    harness.run_in_context(|_isolate, ctx| {
        assert_eq!(
            ctx.eval_safe("undefined"),
            EvalResult {
                success: true,
                result_type: "undefined".to_owned(),
                result_value: "undefined".to_owned(),
            }
        );
    });
}

#[test]
fn eval_null() {
    let harness = crate::Harness::new();
    harness.run_in_context(|_isolate, ctx| {
        assert_eq!(
            ctx.eval_safe("null"),
            EvalResult {
                success: true,
                result_type: "object".to_owned(),
                result_value: "null".to_owned(),
            }
        );
    });
}

#[test]
fn eval_object() {
    let harness = crate::Harness::new();
    harness.run_in_context(|_isolate, ctx| {
        assert_eq!(
            ctx.eval_safe("({})"),
            EvalResult {
                success: true,
                result_type: "object".to_owned(),
                result_value: "[object Object]".to_owned(),
            }
        );
    });
}

#[test]
fn eval_array() {
    let harness = crate::Harness::new();
    harness.run_in_context(|_isolate, ctx| {
        assert_eq!(
            ctx.eval_safe("[1, 2, 3]"),
            EvalResult {
                success: true,
                result_type: "object".to_owned(),
                result_value: "1,2,3".to_owned(),
            }
        );
    });
}

#[test]
fn eval_throws_on_error() {
    let harness = crate::Harness::new();
    harness.run_in_context(|_isolate, ctx| {
        assert_eq!(
            ctx.eval_safe("throw new Error('test error')"),
            EvalResult {
                success: false,
                result_type: "throws".to_owned(),
                result_value: "Error: test error".to_owned(),
            }
        );
    });
}

#[test]
fn eval_throws_type_error() {
    let harness = crate::Harness::new();
    harness.run_in_context(|_isolate, ctx| {
        assert_eq!(
            ctx.eval_safe("null.foo"),
            EvalResult {
                success: false,
                result_type: "throws".to_owned(),
                result_value: "TypeError: Cannot read properties of null (reading 'foo')"
                    .to_owned(),
            }
        );
    });
}

#[test]
fn eval_function_call() {
    let harness = crate::Harness::new();
    harness.run_in_context(|_isolate, ctx| {
        assert_eq!(
            ctx.eval_safe("(function() { return 'from function'; })()"),
            EvalResult {
                success: true,
                result_type: "string".to_owned(),
                result_value: "from function".to_owned(),
            }
        );
    });
}

#[test]
fn eval_typeof_string() {
    let harness = crate::Harness::new();
    harness.run_in_context(|_isolate, ctx| {
        assert_eq!(
            ctx.eval_safe("typeof 'hello'"),
            EvalResult {
                success: true,
                result_type: "string".to_owned(),
                result_value: "string".to_owned(),
            }
        );
    });
}

#[test]
fn eval_typeof_number() {
    let harness = crate::Harness::new();
    harness.run_in_context(|_isolate, ctx| {
        assert_eq!(
            ctx.eval_safe("typeof 42"),
            EvalResult {
                success: true,
                result_type: "string".to_owned(),
                result_value: "number".to_owned(),
            }
        );
    });
}

#[test]
fn eval_typeof_boolean() {
    let harness = crate::Harness::new();
    harness.run_in_context(|_isolate, ctx| {
        assert_eq!(
            ctx.eval_safe("typeof true"),
            EvalResult {
                success: true,
                result_type: "string".to_owned(),
                result_value: "boolean".to_owned(),
            }
        );
    });
}

#[test]
fn eval_unicode_string() {
    let harness = crate::Harness::new();
    harness.run_in_context(|_isolate, ctx| {
        assert_eq!(
            ctx.eval_safe("'こんにちは'"),
            EvalResult {
                success: true,
                result_type: "string".to_owned(),
                result_value: "こんにちは".to_owned(),
            }
        );
    });
}

#[test]
fn eval_emoji_string() {
    let harness = crate::Harness::new();
    harness.run_in_context(|_isolate, ctx| {
        assert_eq!(
            ctx.eval_safe("'😀🎉'"),
            EvalResult {
                success: true,
                result_type: "string".to_owned(),
                result_value: "😀🎉".to_owned(),
            }
        );
    });
}
