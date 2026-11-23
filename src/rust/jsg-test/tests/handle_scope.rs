// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Tests for `Lock::with_handle_scope()` and `Lock::with_escapable_handle_scope()`.

use jsg::FromJS;
use jsg::v8;

/// The handles the closure creates are freed when it returns, so a loop that runs its body in a
/// new scope does not grow the enclosing one, and one that doesn't does.
#[test]
fn handles_are_freed_when_the_scope_closes() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let before = crate::Harness::number_of_handles(lock);
        for _ in 0..100 {
            lock.with_handle_scope(|lock, _| {
                let _ = lock.new_object();
                let _ = lock.new_object();
            });
        }
        assert_eq!(crate::Harness::number_of_handles(lock), before);
        for _ in 0..100 {
            let _ = lock.new_object();
        }
        assert!(crate::Harness::number_of_handles(lock) >= before + 100);
        Ok(())
    });
}

/// A handle of the enclosing scope can be used in the new one, and what the closure returns
/// comes back.
#[test]
fn outer_handles_are_usable_inside() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let double = ctx.eval_raw(lock, "(x => x * 2)").unwrap();
        let double = v8::Local::<v8::Function>::try_from(double).unwrap();
        let mut sum = 0.0;
        for i in 0..1000 {
            sum += lock.with_handle_scope(|lock, _| {
                let arg = v8::Number::new(lock, f64::from(i)).into();
                let result = double.call(lock, None, &[arg]).unwrap();
                f64::from_js(lock, result).unwrap()
            });
        }
        assert!((sum - 999_000.0).abs() < f64::EPSILON);
        Ok(())
    });
}

/// Scopes nest, and a handle of an enclosing scope stays valid once an inner one closes.
#[test]
fn scopes_nest() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let outer = lock.new_object();
        lock.with_handle_scope(|lock, _| {
            let middle = lock.new_object();
            lock.with_handle_scope(|lock, _| {
                let value = v8::Number::new(lock, 1.0).into();
                middle.set(lock, "value", value).unwrap();
            });
            let value = middle.get(lock, "value").unwrap();
            outer.set(lock, "copied", value).unwrap();
        });
        let copied = outer.get(lock, "copied").unwrap();
        assert!((f64::from_js(lock, copied).unwrap() - 1.0).abs() < f64::EPSILON);
        Ok(())
    });
}

/// An exception thrown in the new scope stays pending after it closes.
#[test]
fn exception_stays_pending() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let thrower = ctx
            .eval_raw(lock, "(() => { throw new TypeError('boom'); })")
            .unwrap();
        let thrower = v8::Local::<v8::Function>::try_from(thrower).unwrap();
        let caught = lock
            .try_catch(|lock| {
                lock.with_handle_scope(|lock, _| thrower.call(lock, None, &[]).map(drop))
            })
            .unwrap();
        let Err(exception) = caught else {
            panic!("the call should have thrown");
        };
        let message = String::from_js(lock, exception).unwrap();
        assert_eq!(message, "TypeError: boom");
        Ok(())
    });
}

/// A panic in the closure closes the scope and resumes after it, and the enclosing scope can
/// still be used.
#[test]
fn panic_resumes_after_the_scope_closes() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let outer = lock.new_object();
        let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            lock.with_handle_scope(|lock, _| {
                let _ = lock.new_object();
                panic!("in the scope");
            })
        }));
        assert!(result.is_err());
        let value = v8::Number::new(lock, 1.0).into();
        outer.set(lock, "value", value).unwrap();
        Ok(())
    });
}

/// The handle the closure escapes stays valid after the scope closes, and the others it creates
/// are freed: the enclosing scope grows by the one escaped handle.
#[test]
fn escaped_handle_outlives_the_scope() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let before = crate::Harness::number_of_handles(lock);
        let object = lock.with_escapable_handle_scope(|lock, scope| {
            let object = lock.new_object();
            for i in 0..100 {
                let value = v8::Number::new(lock, f64::from(i)).into();
                object.set(lock, "value", value).unwrap();
            }
            scope.escape(object)
        });
        assert_eq!(crate::Harness::number_of_handles(lock), before + 1);
        let value = object.get(lock, "value").unwrap();
        assert!((f64::from_js(lock, value).unwrap() - 99.0).abs() < f64::EPSILON);
        Ok(())
    });
}

/// The closure can return the escaped handle in a `Result`, and an error without escaping any.
#[test]
fn escaped_handle_in_a_result() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let thrower = ctx
            .eval_raw(lock, "(() => { throw new TypeError('boom'); })")
            .unwrap();
        let thrower = v8::Local::<v8::Function>::try_from(thrower).unwrap();
        let ok = lock.with_escapable_handle_scope(|lock, scope| -> jsg::Result<_> {
            let value = v8::Number::new(lock, 2.0).into();
            Ok(scope.escape(value))
        });
        let ok: v8::Local<'_, v8::Value> = ok.unwrap();
        assert!((f64::from_js(lock, ok).unwrap() - 2.0).abs() < f64::EPSILON);
        let caught = lock
            .try_catch(|lock| {
                lock.with_escapable_handle_scope(|lock, scope| {
                    let result = thrower.call(lock, None, &[])?;
                    Ok(scope.escape(result))
                })
            })
            .unwrap();
        let Err(exception) = caught else {
            panic!("the call should have thrown");
        };
        let message = String::from_js(lock, exception).unwrap();
        assert_eq!(message, "TypeError: boom");
        Ok(())
    });
}

/// A handle escaped from a scope nested in another escapable scope reaches the outermost one
/// through both.
#[test]
fn escapable_scopes_nest() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let object = lock.with_escapable_handle_scope(|lock, outer| {
            let object = lock.with_escapable_handle_scope(|lock, inner| {
                let object = lock.new_object();
                let value = v8::Number::new(lock, 3.0).into();
                object.set(lock, "value", value).unwrap();
                inner.escape(object)
            });
            outer.escape(object)
        });
        let value = object.get(lock, "value").unwrap();
        assert!((f64::from_js(lock, value).unwrap() - 3.0).abs() < f64::EPSILON);
        Ok(())
    });
}

/// A handle created in a scope nested in the escapable one can be escaped through it, and stays
/// valid once both scopes close.
#[test]
fn handle_of_a_nested_scope_can_be_escaped() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let object = lock.with_escapable_handle_scope(|lock, scope| {
            lock.with_handle_scope(move |lock, _| {
                let object = lock.new_object();
                let value = v8::Number::new(lock, 4.0).into();
                object.set(lock, "value", value).unwrap();
                scope.escape(object)
            })
        });
        let value = object.get(lock, "value").unwrap();
        assert!((f64::from_js(lock, value).unwrap() - 4.0).abs() < f64::EPSILON);
        Ok(())
    });
}
