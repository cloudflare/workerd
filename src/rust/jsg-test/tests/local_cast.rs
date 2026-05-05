// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Tests for `impl_local_cast!` conversions added for Float/BigInt typed arrays,
//! and upcasts to Object (Function -> Object, Array -> Object, `TypedArray` -> Object).

use jsg::ToJS;
use jsg::v8;

// =============================================================================
// Float/BigInt typed arrays -> Value
// =============================================================================

/// `Local<Float32Array>` → `Local<Value>` via `Into`, preserving the type tag.
#[test]
fn float32_array_into_value() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let data: Vec<f32> = vec![1.0, 2.0, 3.0];
        let js_val = data.to_js(lock);
        // SAFETY: The isolate is locked and the FFI handle is from a valid eval result.
        let typed: v8::Local<'_, v8::Float32Array> =
            unsafe { v8::Local::from_ffi(lock.isolate(), js_val.into_ffi()) };
        assert_eq!(typed.len(), 3);
        let value: v8::Local<v8::Value> = typed.into();
        assert!(value.is_float32_array());
        assert!(value.is_object());
        assert!(!value.is_float64_array());
        Ok(())
    });
}

/// `Local<Float64Array>` → `Local<Value>` via `Into`, preserving the type tag.
#[test]
fn float64_array_into_value() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let data: Vec<f64> = vec![1.0, 2.0, 3.0];
        let js_val = data.to_js(lock);
        // SAFETY: The isolate is locked and the FFI handle is from a valid eval result.
        let typed: v8::Local<'_, v8::Float64Array> =
            unsafe { v8::Local::from_ffi(lock.isolate(), js_val.into_ffi()) };
        assert_eq!(typed.len(), 3);
        let value: v8::Local<v8::Value> = typed.into();
        assert!(value.is_float64_array());
        assert!(value.is_object());
        assert!(!value.is_float32_array());
        Ok(())
    });
}

/// `Local<BigInt64Array>` → `Local<Value>` via `Into`, preserving the type tag.
#[test]
fn bigint64_array_into_value() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let data: Vec<i64> = vec![1, 2, 3];
        let js_val = data.to_js(lock);
        // SAFETY: The isolate is locked and the FFI handle is from a valid eval result.
        let typed: v8::Local<'_, v8::BigInt64Array> =
            unsafe { v8::Local::from_ffi(lock.isolate(), js_val.into_ffi()) };
        assert_eq!(typed.len(), 3);
        let value: v8::Local<v8::Value> = typed.into();
        assert!(value.is_bigint64_array());
        assert!(value.is_object());
        assert!(!value.is_biguint64_array());
        Ok(())
    });
}

/// `Local<BigUint64Array>` → `Local<Value>` via `Into`, preserving the type tag.
#[test]
fn biguint64_array_into_value() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let data: Vec<u64> = vec![1, 2, 3];
        let js_val = data.to_js(lock);
        // SAFETY: The isolate is locked and the FFI handle is from a valid eval result.
        let typed: v8::Local<'_, v8::BigUint64Array> =
            unsafe { v8::Local::from_ffi(lock.isolate(), js_val.into_ffi()) };
        assert_eq!(typed.len(), 3);
        let value: v8::Local<v8::Value> = typed.into();
        assert!(value.is_biguint64_array());
        assert!(value.is_object());
        assert!(!value.is_bigint64_array());
        Ok(())
    });
}

// =============================================================================
// Float/BigInt typed arrays -> TypedArray
// =============================================================================

/// `Local<Float32Array>` → `Local<TypedArray>` preserves length.
#[test]
fn float32_array_into_typed_array() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let data: Vec<f32> = vec![1.5, 2.5];
        let js_val = data.to_js(lock);
        // SAFETY: The isolate is locked and the FFI handle is from a valid eval result.
        let typed: v8::Local<'_, v8::Float32Array> =
            unsafe { v8::Local::from_ffi(lock.isolate(), js_val.into_ffi()) };
        assert_eq!(typed.len(), 2);
        let ta: v8::Local<v8::TypedArray> = typed.into();
        assert_eq!(ta.len(), 2);
        assert!(!ta.is_empty());
        // Round-trip back and verify elements
        let back: v8::Local<v8::Float32Array> = ta.into();
        assert!((back.get(0) - 1.5).abs() < f32::EPSILON);
        assert!((back.get(1) - 2.5).abs() < f32::EPSILON);
        Ok(())
    });
}

/// `Local<Float64Array>` → `Local<TypedArray>` preserves length.
#[test]
fn float64_array_into_typed_array() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let data: Vec<f64> = vec![1.5, 2.5];
        let js_val = data.to_js(lock);
        // SAFETY: The isolate is locked and the FFI handle is from a valid eval result.
        let typed: v8::Local<'_, v8::Float64Array> =
            unsafe { v8::Local::from_ffi(lock.isolate(), js_val.into_ffi()) };
        assert_eq!(typed.len(), 2);
        let ta: v8::Local<v8::TypedArray> = typed.into();
        assert_eq!(ta.len(), 2);
        assert!(!ta.is_empty());
        // Round-trip back and verify elements
        let back: v8::Local<v8::Float64Array> = ta.into();
        assert!((back.get(0) - 1.5).abs() < f64::EPSILON);
        assert!((back.get(1) - 2.5).abs() < f64::EPSILON);
        Ok(())
    });
}

/// `Local<BigInt64Array>` → `Local<TypedArray>` preserves length.
#[test]
fn bigint64_array_into_typed_array() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let data: Vec<i64> = vec![10, -20];
        let js_val = data.to_js(lock);
        // SAFETY: The isolate is locked and the FFI handle is from a valid eval result.
        let typed: v8::Local<'_, v8::BigInt64Array> =
            unsafe { v8::Local::from_ffi(lock.isolate(), js_val.into_ffi()) };
        assert_eq!(typed.len(), 2);
        let ta: v8::Local<v8::TypedArray> = typed.into();
        assert_eq!(ta.len(), 2);
        assert!(!ta.is_empty());
        // Round-trip back and verify elements
        let back: v8::Local<v8::BigInt64Array> = ta.into();
        assert_eq!(back.get(0), 10);
        assert_eq!(back.get(1), -20);
        Ok(())
    });
}

/// `Local<BigUint64Array>` → `Local<TypedArray>` preserves length.
#[test]
fn biguint64_array_into_typed_array() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let data: Vec<u64> = vec![10, 20];
        let js_val = data.to_js(lock);
        // SAFETY: The isolate is locked and the FFI handle is from a valid eval result.
        let typed: v8::Local<'_, v8::BigUint64Array> =
            unsafe { v8::Local::from_ffi(lock.isolate(), js_val.into_ffi()) };
        assert_eq!(typed.len(), 2);
        let ta: v8::Local<v8::TypedArray> = typed.into();
        assert_eq!(ta.len(), 2);
        assert!(!ta.is_empty());
        // Round-trip back and verify elements
        let back: v8::Local<v8::BigUint64Array> = ta.into();
        assert_eq!(back.get(0), 10);
        assert_eq!(back.get(1), 20);
        Ok(())
    });
}

// =============================================================================
// Upcasts to Object
// =============================================================================

/// `Local<Function>` → `Local<Object>` via `Into`; verify `has()` sees `name` property.
#[test]
fn function_into_object() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let func_val = ctx.eval_raw("(function myFunc() {})").unwrap();
        assert!(func_val.is_function());
        let func = func_val.try_as::<v8::Function>().unwrap();
        let obj: v8::Local<v8::Object> = func.into();
        // Functions are objects — the `name` property should be accessible.
        assert!(obj.has(lock, "name"));
        let prop = obj.get(lock, "name").unwrap();
        let name: String = <String as jsg::FromJS>::from_js(lock, prop)?;
        assert_eq!(name, "myFunc");
        Ok(())
    });
}

/// `Local<Array>` → `Local<Object>` via `Into`; verify `length` property preserved.
#[test]
fn array_into_object() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let arr_val = ctx.eval_raw("[1, 2, 3]").unwrap();
        assert!(arr_val.is_array());
        let arr = arr_val.try_as::<v8::Array>().unwrap();
        assert_eq!(arr.len(), 3);
        let obj: v8::Local<v8::Object> = arr.into();
        assert!(obj.has(lock, "length"));
        let prop = obj.get(lock, "length").unwrap();
        let len: jsg::Number = <jsg::Number as jsg::FromJS>::from_js(lock, prop)?;
        assert!((len.value() - 3.0).abs() < f64::EPSILON);
        Ok(())
    });
}

/// `Local<TypedArray>` → `Local<Object>` via `Into`; verify `byteLength` property.
#[test]
fn typed_array_into_object() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let data: Vec<u8> = vec![1, 2, 3];
        let js_val = data.to_js(lock);
        // SAFETY: The isolate is locked and the FFI handle is from a valid eval result.
        let typed: v8::Local<'_, v8::Uint8Array> =
            unsafe { v8::Local::from_ffi(lock.isolate(), js_val.into_ffi()) };
        assert_eq!(typed.len(), 3);
        // Uint8Array -> TypedArray -> Object
        let ta: v8::Local<v8::TypedArray> = typed.into();
        assert_eq!(ta.len(), 3);
        let obj: v8::Local<v8::Object> = ta.into();
        assert!(obj.has(lock, "byteLength"));
        let prop = obj.get(lock, "byteLength").unwrap();
        let byte_len: jsg::Number = <jsg::Number as jsg::FromJS>::from_js(lock, prop)?;
        assert!((byte_len.value() - 3.0).abs() < f64::EPSILON);
        Ok(())
    });
}

// =============================================================================
// Property access on Object obtained from upcast
// =============================================================================

/// Upcast `Function` to `Object`, then read a custom property via `.get()`.
/// Functions are objects in JS, so custom properties can be set on them.
#[test]
fn function_as_object_property_access() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let func_val = ctx
            .eval_raw("var f = () => {}; f.custom = 'hello'; f")
            .unwrap();
        let func = func_val.try_as::<v8::Function>().unwrap();
        let obj: v8::Local<v8::Object> = func.into();

        assert!(obj.has(lock, "custom"));
        let prop = obj.get(lock, "custom").unwrap();
        let val: String = <String as jsg::FromJS>::from_js(lock, prop)?;
        assert_eq!(val, "hello");
        Ok(())
    });
}

/// Upcast `Array` to `Object`, then read `length` via `.get()`.
#[test]
fn array_as_object_property_access() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let arr_val = ctx.eval_raw("[10, 20, 30]").unwrap();
        let arr = arr_val.try_as::<v8::Array>().unwrap();
        let obj: v8::Local<v8::Object> = arr.into();

        assert!(obj.has(lock, "length"));
        let prop = obj.get(lock, "length").unwrap();
        let val: jsg::Number = <jsg::Number as jsg::FromJS>::from_js(lock, prop)?;
        assert!((val.value() - 3.0).abs() < f64::EPSILON);
        Ok(())
    });
}

/// Upcast `TypedArray` (via `Uint8Array`) to `Object`, then read `byteLength`.
#[test]
fn typed_array_as_object_property_access() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let data: Vec<u8> = vec![1, 2, 3, 4, 5];
        let js_val = data.to_js(lock);
        // SAFETY: The isolate is locked and the FFI handle is from a valid eval result.
        let typed: v8::Local<'_, v8::Uint8Array> =
            unsafe { v8::Local::from_ffi(lock.isolate(), js_val.into_ffi()) };
        let ta: v8::Local<v8::TypedArray> = typed.into();
        let obj: v8::Local<v8::Object> = ta.into();

        assert!(obj.has(lock, "byteLength"));
        assert!(obj.has(lock, "length"));
        let prop = obj.get(lock, "byteLength").unwrap();
        let byte_len: jsg::Number = <jsg::Number as jsg::FromJS>::from_js(lock, prop)?;
        assert!((byte_len.value() - 5.0).abs() < f64::EPSILON);
        let prop = obj.get(lock, "length").unwrap();
        let len: jsg::Number = <jsg::Number as jsg::FromJS>::from_js(lock, prop)?;
        assert!((len.value() - 5.0).abs() < f64::EPSILON);
        Ok(())
    });
}

// =============================================================================
// Bidirectional casts (downcast from Value back to concrete type)
// =============================================================================

/// Round-trip: `Float32Array` → `Value` → `Float32Array`; verify elements preserved.
#[test]
fn float32_array_value_roundtrip() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let data: Vec<f32> = vec![1.5, 2.5, 3.5];
        let js_val = data.to_js(lock);
        // SAFETY: The isolate is locked and the FFI handle is from a valid eval result.
        let typed: v8::Local<'_, v8::Float32Array> =
            unsafe { v8::Local::from_ffi(lock.isolate(), js_val.into_ffi()) };
        assert_eq!(typed.len(), 3);
        let value: v8::Local<v8::Value> = typed.into();
        assert!(value.is_float32_array());
        let back: v8::Local<v8::Float32Array> = value.into();
        assert_eq!(back.len(), 3);
        assert!((back.get(0) - 1.5).abs() < f32::EPSILON);
        assert!((back.get(1) - 2.5).abs() < f32::EPSILON);
        assert!((back.get(2) - 3.5).abs() < f32::EPSILON);
        Ok(())
    });
}

/// Round-trip: `Float64Array` → `Value` → `Float64Array`; verify elements preserved.
#[test]
fn float64_array_value_roundtrip() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let data: Vec<f64> = vec![1.1, 2.2, 3.3];
        let js_val = data.to_js(lock);
        // SAFETY: The isolate is locked and the FFI handle is from a valid eval result.
        let typed: v8::Local<'_, v8::Float64Array> =
            unsafe { v8::Local::from_ffi(lock.isolate(), js_val.into_ffi()) };
        assert_eq!(typed.len(), 3);
        let value: v8::Local<v8::Value> = typed.into();
        assert!(value.is_float64_array());
        let back: v8::Local<v8::Float64Array> = value.into();
        assert_eq!(back.len(), 3);
        assert!((back.get(0) - 1.1).abs() < f64::EPSILON);
        assert!((back.get(1) - 2.2).abs() < f64::EPSILON);
        assert!((back.get(2) - 3.3).abs() < f64::EPSILON);
        Ok(())
    });
}

/// Round-trip: `BigInt64Array` → `Value` → `BigInt64Array`; verify elements preserved.
#[test]
fn bigint64_array_value_roundtrip() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let data: Vec<i64> = vec![-100, 0, 100];
        let js_val = data.to_js(lock);
        // SAFETY: The isolate is locked and the FFI handle is from a valid eval result.
        let typed: v8::Local<'_, v8::BigInt64Array> =
            unsafe { v8::Local::from_ffi(lock.isolate(), js_val.into_ffi()) };
        assert_eq!(typed.len(), 3);
        let value: v8::Local<v8::Value> = typed.into();
        assert!(value.is_bigint64_array());
        let back: v8::Local<v8::BigInt64Array> = value.into();
        assert_eq!(back.len(), 3);
        assert_eq!(back.get(0), -100);
        assert_eq!(back.get(1), 0);
        assert_eq!(back.get(2), 100);
        Ok(())
    });
}

/// Round-trip: `BigUint64Array` → `Value` → `BigUint64Array`; verify elements preserved.
#[test]
fn biguint64_array_value_roundtrip() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let data: Vec<u64> = vec![0, 42, 999];
        let js_val = data.to_js(lock);
        // SAFETY: The isolate is locked and the FFI handle is from a valid eval result.
        let typed: v8::Local<'_, v8::BigUint64Array> =
            unsafe { v8::Local::from_ffi(lock.isolate(), js_val.into_ffi()) };
        assert_eq!(typed.len(), 3);
        let value: v8::Local<v8::Value> = typed.into();
        assert!(value.is_biguint64_array());
        let back: v8::Local<v8::BigUint64Array> = value.into();
        assert_eq!(back.len(), 3);
        assert_eq!(back.get(0), 0);
        assert_eq!(back.get(1), 42);
        assert_eq!(back.get(2), 999);
        Ok(())
    });
}

/// Round-trip: `Function` → `Object` → `Function`; verify callable after roundtrip.
#[test]
fn function_object_roundtrip() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let func_val = ctx.eval_raw("(function namedFn() { return 42; })").unwrap();
        assert!(func_val.is_function());
        let func = func_val.try_as::<v8::Function>().unwrap();
        let obj: v8::Local<v8::Object> = func.into();
        // Verify function-specific property is readable as Object
        assert!(obj.has(lock, "name"));
        let prop = obj.get(lock, "name").unwrap();
        let name: String = <String as jsg::FromJS>::from_js(lock, prop)?;
        assert_eq!(name, "namedFn");
        // Roundtrip back and verify it's still callable
        let back: v8::Local<v8::Function> = obj.into();
        let result = back.call::<jsg::Number, v8::Local<v8::Value>>(lock, None, &[])?;
        assert!((result.value() - 42.0).abs() < f64::EPSILON);
        Ok(())
    });
}

/// Round-trip: `Array` → `Object` → `Array`; verify length preserved.
#[test]
fn array_object_roundtrip() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let arr_val = ctx.eval_raw("[10, 20, 30]").unwrap();
        assert!(arr_val.is_array());
        let arr = arr_val.try_as::<v8::Array>().unwrap();
        assert_eq!(arr.len(), 3);
        let obj: v8::Local<v8::Object> = arr.into();
        // Verify length via Object property access
        let prop = obj.get(lock, "length").unwrap();
        let len: jsg::Number = <jsg::Number as jsg::FromJS>::from_js(lock, prop)?;
        assert!((len.value() - 3.0).abs() < f64::EPSILON);
        // Roundtrip back and verify Array-specific methods work
        let back: v8::Local<v8::Array> = obj.into();
        assert_eq!(back.len(), 3);
        assert!(!back.is_empty());
        Ok(())
    });
}
