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
            unsafe { v8::Local::from_ffi(js_val.into_ffi()) };
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
            unsafe { v8::Local::from_ffi(js_val.into_ffi()) };
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
            unsafe { v8::Local::from_ffi(js_val.into_ffi()) };
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
            unsafe { v8::Local::from_ffi(js_val.into_ffi()) };
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
            unsafe { v8::Local::from_ffi(js_val.into_ffi()) };
        assert_eq!(typed.len(), 2);
        let ta: v8::Local<v8::TypedArray> = typed.into();
        assert_eq!(ta.len(), 2);
        assert!(!ta.is_empty());
        // Round-trip back and verify elements
        let back: v8::Local<v8::Float32Array> = ta.try_into().unwrap();
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
            unsafe { v8::Local::from_ffi(js_val.into_ffi()) };
        assert_eq!(typed.len(), 2);
        let ta: v8::Local<v8::TypedArray> = typed.into();
        assert_eq!(ta.len(), 2);
        assert!(!ta.is_empty());
        // Round-trip back and verify elements
        let back: v8::Local<v8::Float64Array> = ta.try_into().unwrap();
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
            unsafe { v8::Local::from_ffi(js_val.into_ffi()) };
        assert_eq!(typed.len(), 2);
        let ta: v8::Local<v8::TypedArray> = typed.into();
        assert_eq!(ta.len(), 2);
        assert!(!ta.is_empty());
        // Round-trip back and verify elements
        let back: v8::Local<v8::BigInt64Array> = ta.try_into().unwrap();
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
            unsafe { v8::Local::from_ffi(js_val.into_ffi()) };
        assert_eq!(typed.len(), 2);
        let ta: v8::Local<v8::TypedArray> = typed.into();
        assert_eq!(ta.len(), 2);
        assert!(!ta.is_empty());
        // Round-trip back and verify elements
        let back: v8::Local<v8::BigUint64Array> = ta.try_into().unwrap();
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
        let func_val = ctx.eval_raw(lock, "(function myFunc() {})").unwrap();
        assert!(func_val.is_function());
        let func = v8::Local::<v8::Function>::try_from(func_val).unwrap();
        let obj: v8::Local<v8::Object> = func.into();
        // Functions are objects — the `name` property should be accessible.
        assert!(obj.has(lock, "name").unwrap());
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
        let arr_val = ctx.eval_raw(lock, "[1, 2, 3]").unwrap();
        assert!(arr_val.is_array());
        let arr = v8::Local::<v8::Array>::try_from(arr_val).unwrap();
        assert_eq!(arr.len(), 3);
        let obj: v8::Local<v8::Object> = arr.into();
        assert!(obj.has(lock, "length").unwrap());
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
            unsafe { v8::Local::from_ffi(js_val.into_ffi()) };
        assert_eq!(typed.len(), 3);
        // Uint8Array -> TypedArray -> Object
        let ta: v8::Local<v8::TypedArray> = typed.into();
        assert_eq!(ta.len(), 3);
        let obj: v8::Local<v8::Object> = ta.into();
        assert!(obj.has(lock, "byteLength").unwrap());
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
            .eval_raw(lock, "var f = () => {}; f.custom = 'hello'; f")
            .unwrap();
        let func = v8::Local::<v8::Function>::try_from(func_val).unwrap();
        let obj: v8::Local<v8::Object> = func.into();

        assert!(obj.has(lock, "custom").unwrap());
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
        let arr_val = ctx.eval_raw(lock, "[10, 20, 30]").unwrap();
        let arr = v8::Local::<v8::Array>::try_from(arr_val).unwrap();
        let obj: v8::Local<v8::Object> = arr.into();

        assert!(obj.has(lock, "length").unwrap());
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
            unsafe { v8::Local::from_ffi(js_val.into_ffi()) };
        let ta: v8::Local<v8::TypedArray> = typed.into();
        let obj: v8::Local<v8::Object> = ta.into();

        assert!(obj.has(lock, "byteLength").unwrap());
        assert!(obj.has(lock, "length").unwrap());
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
            unsafe { v8::Local::from_ffi(js_val.into_ffi()) };
        assert_eq!(typed.len(), 3);
        let value: v8::Local<v8::Value> = typed.into();
        assert!(value.is_float32_array());
        let back: v8::Local<v8::Float32Array> = value.try_into().unwrap();
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
            unsafe { v8::Local::from_ffi(js_val.into_ffi()) };
        assert_eq!(typed.len(), 3);
        let value: v8::Local<v8::Value> = typed.into();
        assert!(value.is_float64_array());
        let back: v8::Local<v8::Float64Array> = value.try_into().unwrap();
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
            unsafe { v8::Local::from_ffi(js_val.into_ffi()) };
        assert_eq!(typed.len(), 3);
        let value: v8::Local<v8::Value> = typed.into();
        assert!(value.is_bigint64_array());
        let back: v8::Local<v8::BigInt64Array> = value.try_into().unwrap();
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
            unsafe { v8::Local::from_ffi(js_val.into_ffi()) };
        assert_eq!(typed.len(), 3);
        let value: v8::Local<v8::Value> = typed.into();
        assert!(value.is_biguint64_array());
        let back: v8::Local<v8::BigUint64Array> = value.try_into().unwrap();
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
        let func_val = ctx
            .eval_raw(lock, "(function namedFn() { return 42; })")
            .unwrap();
        assert!(func_val.is_function());
        let func = v8::Local::<v8::Function>::try_from(func_val).unwrap();
        let obj: v8::Local<v8::Object> = func.into();
        // Verify function-specific property is readable as Object
        assert!(obj.has(lock, "name").unwrap());
        let prop = obj.get(lock, "name").unwrap();
        let name: String = <String as jsg::FromJS>::from_js(lock, prop)?;
        assert_eq!(name, "namedFn");
        // Roundtrip back and verify it's still callable
        let back: v8::Local<v8::Function> = obj.try_into().unwrap();
        let result = back.call::<jsg::Number>(lock, None, &[])?;
        assert!((result.value() - 42.0).abs() < f64::EPSILON);
        Ok(())
    });
}

/// Round-trip: `Array` → `Object` → `Array`; verify length preserved.
#[test]
fn array_object_roundtrip() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let arr_val = ctx.eval_raw(lock, "[10, 20, 30]").unwrap();
        assert!(arr_val.is_array());
        let arr = v8::Local::<v8::Array>::try_from(arr_val).unwrap();
        assert_eq!(arr.len(), 3);
        let obj: v8::Local<v8::Object> = arr.into();
        // Verify length via Object property access
        let prop = obj.get(lock, "length").unwrap();
        let len: jsg::Number = <jsg::Number as jsg::FromJS>::from_js(lock, prop)?;
        assert!((len.value() - 3.0).abs() < f64::EPSILON);
        // Roundtrip back and verify Array-specific methods work
        let back: v8::Local<v8::Array> = obj.try_into().unwrap();
        assert_eq!(back.len(), 3);
        assert!(!back.is_empty());
        Ok(())
    });
}

/// A `DataView` is an `ArrayBufferView` but not a `TypedArray`; the downcast
/// `TryFrom<Local<Value>> for Local<TypedArray>` rejects it.
#[test]
fn data_view_cannot_cast_to_typed_array() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let val = ctx
            .eval_raw(lock, "new DataView(new ArrayBuffer(8))")
            .unwrap();
        assert!(val.is_array_buffer_view());
        assert!(!val.is_typed_array());

        let error = v8::Local::<v8::TypedArray>::try_from(val)
            .expect_err("DataView must not cast to Local<TypedArray>");
        assert_eq!(
            error.to_string(),
            "expected type `jsg::v8::TypedArray`, got `jsg::v8::Value`"
        );

        let view: v8::Local<v8::ArrayBufferView> = val.try_into().unwrap();
        assert_eq!(view.byte_length(), 8);

        Ok(())
    });
}

/// Downcasts check the value whatever supertype they start from, not just from `Value`.
#[test]
fn downcasts_from_other_supertypes_check_the_value() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let object: v8::Local<v8::Object> = ctx.eval_raw(lock, "({})").unwrap().try_into().unwrap();
        assert!(v8::Local::<v8::Function>::try_from(object).is_err());
        assert!(v8::Local::<v8::Array>::try_from(object).is_err());

        let typed: v8::Local<v8::TypedArray> = ctx
            .eval_raw(lock, "new Uint8Array(4)")
            .unwrap()
            .try_into()
            .unwrap();
        assert!(v8::Local::<v8::Float32Array>::try_from(typed).is_err());
        assert!(v8::Local::<v8::Uint8Array>::try_from(typed).is_ok());

        let view: v8::Local<v8::ArrayBufferView> = typed.into();
        assert!(v8::Local::<v8::Uint16Array>::try_from(view).is_err());

        let name: v8::Local<v8::Name> = ctx.eval_raw(lock, "Symbol()").unwrap().try_into().unwrap();
        assert!(v8::Local::<v8::String>::try_from(name).is_err());
        assert!(v8::Local::<v8::Symbol>::try_from(name).is_ok());
        Ok(())
    });
}

/// V8 returns the buffer of a view over a `SharedArrayBuffer` as a `Local<ArrayBuffer>`,
/// although `IsArrayBuffer()` is false for it, so upcasting it must not check the value.
#[test]
fn shared_buffer_of_a_view_upcasts() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let view: v8::Local<v8::ArrayBufferView> = ctx
            .eval_raw(lock, "new Uint8Array(new SharedArrayBuffer(4))")
            .unwrap()
            .try_into()
            .unwrap();
        let buffer = view.buffer(lock);
        let value: v8::Local<v8::Value> = buffer.into();
        assert!(value.is_shared_array_buffer());
        let object: v8::Local<v8::Object> = buffer.into();
        assert!(object.has(lock, "byteLength").unwrap());
        Ok(())
    });
}
