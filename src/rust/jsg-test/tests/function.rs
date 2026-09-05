// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Tests for `Local<Function>::call()`, `jsg::Function`, `As<T>` trait, and
//! `impl_local_cast!` conversions.

use jsg::ExceptionType;
use jsg::FromJS;
use jsg::Function;
use jsg::Number;
use jsg::ToJS;
use jsg::v8;

/// `try_as::<Function>()` succeeds for a JS function value.
#[test]
fn try_as_function_succeeds() {
    let harness = crate::Harness::new();
    harness.run_in_context(|_lock, ctx| {
        let value = ctx.eval_raw("(x => x * 2)").unwrap();
        assert!(value.try_as::<v8::Function>().is_some());
        Ok(())
    });
}

/// `try_as::<Function>()` returns `None` for a non-function value.
#[test]
fn try_as_function_fails_for_number() {
    let harness = crate::Harness::new();
    harness.run_in_context(|_lock, ctx| {
        let value = ctx.eval_raw("42").unwrap();
        assert!(value.try_as::<v8::Function>().is_none());
        Ok(())
    });
}

/// `try_as::<Object>()` succeeds for a plain object.
#[test]
fn try_as_object_succeeds() {
    let harness = crate::Harness::new();
    harness.run_in_context(|_lock, ctx| {
        let value = ctx.eval_raw("({a: 1})").unwrap();
        assert!(value.try_as::<v8::Object>().is_some());
        Ok(())
    });
}

/// `try_as::<Object>()` returns `None` for a primitive.
#[test]
fn try_as_object_fails_for_string() {
    let harness = crate::Harness::new();
    harness.run_in_context(|_lock, ctx| {
        let value = ctx.eval_raw("'hello'").unwrap();
        assert!(value.try_as::<v8::Object>().is_none());
        Ok(())
    });
}

/// `try_as::<Array>()` succeeds for an array.
#[test]
fn try_as_array_succeeds() {
    let harness = crate::Harness::new();
    harness.run_in_context(|_lock, ctx| {
        let value = ctx.eval_raw("[1, 2, 3]").unwrap();
        assert!(value.try_as::<v8::Array>().is_some());
        Ok(())
    });
}

/// `try_as::<Array>()` returns `None` for a non-array object.
#[test]
fn try_as_array_fails_for_object() {
    let harness = crate::Harness::new();
    harness.run_in_context(|_lock, ctx| {
        let value = ctx.eval_raw("({length: 3})").unwrap();
        assert!(value.try_as::<v8::Array>().is_none());
        Ok(())
    });
}

/// `Local<Function>::call` with zero args returns the correct value via `FromJS`.
#[test]
fn call_zero_args() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let value = ctx.eval_raw("(() => 42)").unwrap();
        let func = value.try_as::<v8::Function>().unwrap();
        let result = func.call::<Number, _>(lock, None::<v8::Local<v8::Value>>, &[])?;
        assert!((result.value() - 42.0).abs() < f64::EPSILON);
        Ok(())
    });
}

/// `Local<Function>::call` with arguments.
#[test]
fn call_with_args() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let value = ctx.eval_raw("((a, b) => a + b)").unwrap();
        let func = value.try_as::<v8::Function>().unwrap();

        let a = Number::new(10.0).to_js(lock);
        let b = Number::new(32.0).to_js(lock);
        let result = func.call::<Number, _>(lock, None::<v8::Local<v8::Value>>, &[a, b])?;
        assert!((result.value() - 42.0).abs() < f64::EPSILON);
        Ok(())
    });
}

/// `Local<Function>::call` returning a string via `FromJS`.
#[test]
fn call_returns_string() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let value = ctx.eval_raw("(s => s.toUpperCase())").unwrap();
        let func = value.try_as::<v8::Function>().unwrap();

        let arg = String::from("hello").to_js(lock);
        let result = func.call::<String, _>(lock, None::<v8::Local<v8::Value>>, &[arg])?;
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
        let obj = ctx.eval_raw("({x: 100})").unwrap();
        let func_val = ctx.eval_raw("(function() { return this.x; })").unwrap();
        let func = func_val.try_as::<v8::Function>().unwrap();

        let result = func.call::<Number, _>(lock, Some(obj), &[])?;
        assert!((result.value() - 100.0).abs() < f64::EPSILON);
        Ok(())
    });
}

/// `Local<Function>::call` with a `Local<Object>` receiver via `.into()`.
#[test]
fn call_with_object_receiver_via_into() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let obj_val = ctx.eval_raw("({name: 'world'})").unwrap();
        let obj = obj_val.try_as::<v8::Object>().unwrap();

        let func_val = ctx
            .eval_raw("(function() { return 'hello ' + this.name; })")
            .unwrap();
        let func = func_val.try_as::<v8::Function>().unwrap();

        // Pass Local<Object> as receiver — the Into<Local<Value>> bound handles conversion.
        let result = func.call::<String, v8::Local<v8::Object>>(lock, Some(obj), &[])?;
        assert_eq!(result, "hello world");
        Ok(())
    });
}

/// A throwing callee surfaces as an `Err` preserving the JS error type and
/// message — it must not unwind across the FFI boundary.
#[test]
fn call_throw_returns_error() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let value = ctx
            .eval_raw("(() => { throw new TypeError('boom') })")
            .unwrap();
        let func = value.try_as::<v8::Function>().unwrap();
        let err = func
            .call::<Number, _>(lock, None::<v8::Local<v8::Value>>, &[])
            .unwrap_err();
        assert_eq!(err.name, ExceptionType::TypeError);
        assert_eq!(err.message, "boom");
        Ok(())
    });
}

/// A non-Error throw (e.g. a string) still comes back as an `Err`.
#[test]
fn call_throw_non_error_value() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let value = ctx.eval_raw("(() => { throw 'plain string' })").unwrap();
        let func = value.try_as::<v8::Function>().unwrap();
        let result = func.call::<Number, _>(lock, None::<v8::Local<v8::Value>>, &[]);
        assert!(result.is_err());
        Ok(())
    });
}

/// `jsg::Function` converts from a JS function value and calls with typed args.
#[test]
fn typed_function_from_js_and_call() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let value = ctx.eval_raw("((a, b) => a + b)").unwrap();
        let func: Function<(Number, Number), Number> = Function::from_js(lock, value)?;
        let result = func.call(lock, (Number::new(10.0), Number::new(32.0)))?;
        assert!((result.value() - 42.0).abs() < f64::EPSILON);
        Ok(())
    });
}

/// `jsg::Function<(), ()>` — no arguments, result discarded.
#[test]
fn typed_function_unit_signature() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let value = ctx
            .eval_raw("(() => { globalThis.sideEffect = 7; })")
            .unwrap();
        let func: Function = Function::from_js(lock, value)?;
        func.call(lock, ())?;
        let observed: Number = ctx.eval(lock, "globalThis.sideEffect").unwrap();
        assert!((observed.value() - 7.0).abs() < f64::EPSILON);
        Ok(())
    });
}

/// `jsg::Function::from_js` rejects non-function values with a `TypeError`.
#[test]
fn typed_function_rejects_non_function() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let value = ctx.eval_raw("42").unwrap();
        let err = Function::<(), ()>::from_js(lock, value).unwrap_err();
        assert_eq!(err.name, ExceptionType::TypeError);
        assert_eq!(err.message, "expected function, got number");
        Ok(())
    });
}

/// A throwing callee through `jsg::Function` preserves the JS error type.
#[test]
fn typed_function_throw_preserves_error_type() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let value = ctx
            .eval_raw("(() => { throw new RangeError('out of range') })")
            .unwrap();
        let func: Function = Function::from_js(lock, value)?;
        let err = func.call(lock, ()).unwrap_err();
        assert_eq!(err.name, ExceptionType::RangeError);
        assert_eq!(err.message, "out of range");
        Ok(())
    });
}

/// `jsg::Function::call_with_receiver` passes `this`.
#[test]
fn typed_function_with_receiver() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let obj = ctx.eval_raw("({x: 100})").unwrap();
        let value = ctx.eval_raw("(function() { return this.x; })").unwrap();
        let func: Function<(), Number> = Function::from_js(lock, value)?;
        let result = func.call_with_receiver(lock, Some(obj), ())?;
        assert!((result.value() - 100.0).abs() < f64::EPSILON);
        Ok(())
    });
}

/// Isolate termination surfaces as an `Error` with `is_termination()` set —
/// distinguishable from a JS throw, and not process-fatal.
#[test]
fn call_after_terminate_returns_termination_error() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let value = ctx.eval_raw("(() => 1)").unwrap();
        let func: Function<(), Number> = Function::from_js(lock, value)?;
        lock.terminate_execution();
        let err = func.call(lock, ()).unwrap_err();
        assert!(err.is_termination());
        crate::Harness::cancel_termination(lock);
        Ok(())
    });
}

/// A clone remains callable after the original is dropped (independent
/// persistent handles).
#[test]
fn typed_function_clone_independent() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let value = ctx.eval_raw("(() => 'alive')").unwrap();
        let func: Function<(), String> = Function::from_js(lock, value)?;
        let cloned = func.clone(lock);
        drop(func);
        assert_eq!(cloned.call(lock, ())?, "alive");
        Ok(())
    });
}

/// `impl_local_cast!` conversions: `Local<Object>` → `Local<Value>` via `Into`.
#[test]
fn local_object_into_value() {
    let harness = crate::Harness::new();
    harness.run_in_context(|_lock, ctx| {
        let obj_val = ctx.eval_raw("({})").unwrap();
        let obj = obj_val.try_as::<v8::Object>().unwrap();
        // Into<Local<Value>> should compile and not panic in debug.
        let value: v8::Local<v8::Value> = obj.into();
        assert!(value.is_object());
        Ok(())
    });
}

/// `impl_local_cast!` conversions: `Local<Function>` → `Local<Value>` via `Into`.
#[test]
fn local_function_into_value() {
    let harness = crate::Harness::new();
    harness.run_in_context(|_lock, ctx| {
        let func_val = ctx.eval_raw("(() => {})").unwrap();
        let func = func_val.try_as::<v8::Function>().unwrap();
        let value: v8::Local<v8::Value> = func.into();
        assert!(value.is_object());
        Ok(())
    });
}

/// `impl_local_cast!` conversions: `Local<Array>` → `Local<Value>` via `Into`.
#[test]
fn local_array_into_value() {
    let harness = crate::Harness::new();
    harness.run_in_context(|_lock, ctx| {
        let arr_val = ctx.eval_raw("[1, 2]").unwrap();
        let arr = arr_val.try_as::<v8::Array>().unwrap();
        let value: v8::Local<v8::Value> = arr.into();
        assert!(value.is_array());
        Ok(())
    });
}
