// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Tests for `Lock::with_handle_scope()`.

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
