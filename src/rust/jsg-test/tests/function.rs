// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Tests for `Local<Function>::call()` and the casts between `Local` handle types.

use jsg::FromJS;
use jsg::Number;
use jsg::ToJS;
use jsg::v8;

/// `Local<Function>::try_from` succeeds for a JS function value.
#[test]
fn try_from_function_succeeds() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let value = ctx.eval_raw(lock, "(x => x * 2)").unwrap();
        assert!(v8::Local::<v8::Function>::try_from(value).is_ok());
        Ok(())
    });
}

/// `Local<Function>::try_from` fails for a non-function value.
#[test]
fn try_from_function_fails_for_number() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let value = ctx.eval_raw(lock, "42").unwrap();
        assert!(v8::Local::<v8::Function>::try_from(value).is_err());
        Ok(())
    });
}

/// `Local<Object>::try_from` succeeds for a plain object.
#[test]
fn try_from_object_succeeds() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let value = ctx.eval_raw(lock, "({a: 1})").unwrap();
        assert!(v8::Local::<v8::Object>::try_from(value).is_ok());
        Ok(())
    });
}

/// `Local<Object>::try_from` fails for a primitive.
#[test]
fn try_from_object_fails_for_string() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let value = ctx.eval_raw(lock, "'hello'").unwrap();
        assert!(v8::Local::<v8::Object>::try_from(value).is_err());
        Ok(())
    });
}

/// `Local<Array>::try_from` succeeds for an array.
#[test]
fn try_from_array_succeeds() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let value = ctx.eval_raw(lock, "[1, 2, 3]").unwrap();
        assert!(v8::Local::<v8::Array>::try_from(value).is_ok());
        Ok(())
    });
}

/// `Local<Array>::try_from` fails for a non-array object.
#[test]
fn try_from_array_fails_for_object() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let value = ctx.eval_raw(lock, "({length: 3})").unwrap();
        assert!(v8::Local::<v8::Array>::try_from(value).is_err());
        Ok(())
    });
}

/// `Local<Function>::call` with zero args returns the correct value via `FromJS`.
#[test]
fn call_zero_args() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let value = ctx.eval_raw(lock, "(() => 42)").unwrap();
        let func = v8::Local::<v8::Function>::try_from(value).unwrap();
        let result = func.call::<Number>(lock, None::<v8::Local<v8::Value>>, &[])?;
        assert!((result.value() - 42.0).abs() < f64::EPSILON);
        Ok(())
    });
}

/// `Local<Function>::call` with arguments.
#[test]
fn call_with_args() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let value = ctx.eval_raw(lock, "((a, b) => a - b)").unwrap();
        let func = v8::Local::<v8::Function>::try_from(value).unwrap();

        let a = Number::new(10.0).to_js(lock);
        let b = Number::new(32.0).to_js(lock);
        let result = func.call::<Number>(lock, None::<v8::Local<v8::Value>>, &[a, b])?;
        assert!((result.value() + 22.0).abs() < f64::EPSILON);
        Ok(())
    });
}

/// `Local<Function>::call` returning a string via `FromJS`.
#[test]
fn call_returns_string() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let value = ctx.eval_raw(lock, "(s => s.toUpperCase())").unwrap();
        let func = v8::Local::<v8::Function>::try_from(value).unwrap();

        let arg = String::from("hello").to_js(lock);
        let result = func.call::<String>(lock, None::<v8::Local<v8::Value>>, &[arg])?;
        assert_eq!(result, "HELLO");
        Ok(())
    });
}

/// `Local<Function>::call` with an object receiver (`this`).
#[test]
fn call_with_receiver() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        // Create an object with a property and a function that reads `this.x`
        let obj = ctx.eval_raw(lock, "({x: 100})").unwrap();
        let func_val = ctx
            .eval_raw(lock, "(function() { return this.x; })")
            .unwrap();
        let func = v8::Local::<v8::Function>::try_from(func_val).unwrap();

        let result = func.call::<Number>(lock, Some(obj), &[])?;
        assert!((result.value() - 100.0).abs() < f64::EPSILON);
        Ok(())
    });
}

/// `Local<Function>::call` with a `Local<Object>` receiver via `.into()`.
#[test]
fn call_with_object_receiver_via_into() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let obj_val = ctx.eval_raw(lock, "({name: 'world'})").unwrap();
        let obj = v8::Local::<v8::Object>::try_from(obj_val).unwrap();

        let func_val = ctx
            .eval_raw(lock, "(function() { return 'hello ' + this.name; })")
            .unwrap();
        let func = v8::Local::<v8::Function>::try_from(func_val).unwrap();

        // Pass Local<Object> as receiver — the Into<Local<Value>> bound handles conversion.
        let result = func.call::<String>(lock, Some(obj.into()), &[])?;
        assert_eq!(result, "hello world");
        Ok(())
    });
}

/// `Local<Function>::call` leaves what the function threw pending, which `try_catch` catches.
#[test]
fn call_leaves_thrown_exception_pending() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let thrown = ctx
            .eval_raw(lock, "globalThis.thrown = new RangeError('boom')")
            .unwrap();
        let func = ctx.eval_raw(lock, "(() => { throw thrown; })").unwrap();
        let func = v8::Local::<v8::Function>::try_from(func).unwrap();

        let caught = lock.try_catch(|lock| {
            let err = func
                .call::<v8::Local<v8::Value>>(lock, None, &[])
                .unwrap_err();
            assert!(err.is_pending());
            Err::<(), _>(err)
        })?;
        assert_eq!(caught.unwrap_err(), thrown);
        Ok(())
    });
}

/// A thrown value that is not an `Error` is caught as is.
#[test]
fn try_catch_catches_thrown_non_error() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let func = ctx.eval_raw(lock, "(() => { throw 42; })").unwrap();
        let func = v8::Local::<v8::Function>::try_from(func).unwrap();

        let caught = lock.try_catch(|lock| func.call::<Number>(lock, None, &[]))?;
        let thrown = Number::from_js(lock, caught.unwrap_err())?;
        assert!((thrown.value() - 42.0).abs() < f64::EPSILON);
        Ok(())
    });
}

/// Catching a thrown value runs no JavaScript: neither its getters nor proxy traps.
#[test]
fn try_catch_does_not_run_thrown_value_accessors() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        for thrown in [
            "({ get overloaded() { reads++; }, get retryable() { reads++; }, \
               get message() { reads++; }, get name() { reads++; } })",
            "new Proxy(new Error('boom'), { get() { reads++; }, has() { reads++; }, \
               getPrototypeOf() { reads++; return null; } })",
        ] {
            let thrown = ctx
                .eval_raw(
                    lock,
                    &format!("globalThis.reads = 0; globalThis.thrown = {thrown}"),
                )
                .unwrap();
            let func = ctx.eval_raw(lock, "(() => { throw thrown; })").unwrap();
            let func = v8::Local::<v8::Function>::try_from(func).unwrap();

            let caught =
                lock.try_catch(|lock| func.call::<v8::Local<v8::Value>>(lock, None, &[]))?;
            assert_eq!(caught.unwrap_err(), thrown);
            let reads = ctx.eval::<Number>(lock, "reads").unwrap();
            assert!(
                reads.value().abs() < f64::EPSILON,
                "{thrown:?} ran {reads:?} accessors"
            );
        }
        Ok(())
    });
}

/// `try_catch` returns what the closure returns when it succeeds.
#[test]
fn try_catch_returns_result_on_success() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let func = ctx.eval_raw(lock, "(() => 7)").unwrap();
        let func = v8::Local::<v8::Function>::try_from(func).unwrap();

        let result = lock.try_catch(|lock| func.call::<Number>(lock, None, &[]))?;
        assert!((result.unwrap().value() - 7.0).abs() < f64::EPSILON);
        Ok(())
    });
}

/// An error the closure returns itself is caught as the exception that throwing it creates.
#[test]
fn try_catch_catches_returned_error() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let caught = lock.try_catch(|_lock| Err::<(), _>(jsg::Error::new_range_error("nope")))?;
        let err = jsg::Error::from_js(lock, caught.unwrap_err())?;
        assert_eq!(err.name, jsg::ExceptionType::RangeError);
        assert_eq!(err.message, "nope");
        assert!(!err.is_pending());
        Ok(())
    });
}

/// A panic in the closure unwinds past `try_catch`, which leaves the isolate usable.
#[test]
fn try_catch_resumes_panic() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let panic = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            let _ = lock.try_catch(|_lock| -> jsg::Result<()> { panic!("intentional panic") });
        }));
        assert!(panic.is_err());
        assert!(ctx.eval::<bool>(lock, "true").unwrap());
        Ok(())
    });
}

/// An error that did not come from JavaScript is not pending.
#[test]
fn constructed_error_is_not_pending() {
    assert!(!jsg::Error::new_type_error("constructed").is_pending());
}

/// `impl_local_cast!` conversions: `Local<Object>` → `Local<Value>` via `Into`.
#[test]
fn local_object_into_value() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let obj_val = ctx.eval_raw(lock, "({})").unwrap();
        let obj = v8::Local::<v8::Object>::try_from(obj_val).unwrap();
        let value: v8::Local<v8::Value> = obj.into();
        assert!(value.is_object());
        Ok(())
    });
}

/// `impl_local_cast!` conversions: `Local<Function>` → `Local<Value>` via `Into`.
#[test]
fn local_function_into_value() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let func_val = ctx.eval_raw(lock, "(() => {})").unwrap();
        let func = v8::Local::<v8::Function>::try_from(func_val).unwrap();
        let value: v8::Local<v8::Value> = func.into();
        assert!(value.is_object());
        Ok(())
    });
}

/// `impl_local_cast!` conversions: `Local<Array>` → `Local<Value>` via `Into`.
#[test]
fn local_array_into_value() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let arr_val = ctx.eval_raw(lock, "[1, 2]").unwrap();
        let arr = v8::Local::<v8::Array>::try_from(arr_val).unwrap();
        let value: v8::Local<v8::Value> = arr.into();
        assert!(value.is_array());
        Ok(())
    });
}
