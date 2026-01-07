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
