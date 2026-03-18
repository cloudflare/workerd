// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use jsg::Number;
use jsg::v8::ToLocalValue;

#[test]
fn v8_is_string_returns_true_for_strings() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let str_val = "hello".to_local(lock);
        assert!(str_val.is_string());
        Ok(())
    });
}

#[test]
fn v8_is_boolean_returns_true_for_booleans() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let bool_true = true.to_local(lock);
        assert!(bool_true.is_boolean());

        let bool_false = false.to_local(lock);
        assert!(bool_false.is_boolean());
        Ok(())
    });
}

#[test]
fn v8_is_number_returns_true_for_numbers() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let float_val = Number::new(2.5).to_local(lock);
        assert!(float_val.is_number());

        let int_val = 42u32.to_local(lock);
        assert!(int_val.is_number());

        let num_u8 = 255u8.to_local(lock);
        assert!(num_u8.is_number());
        Ok(())
    });
}

#[test]
fn v8_type_checks_are_mutually_exclusive() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        // String is only string
        let s = "test".to_local(lock);
        assert!(s.is_string() && !s.is_boolean() && !s.is_number());

        // Boolean is only boolean
        let b = true.to_local(lock);
        assert!(!b.is_string() && b.is_boolean() && !b.is_number());

        // Number is only number
        let n = Number::new(42.0).to_local(lock);
        assert!(!n.is_string() && !n.is_boolean() && n.is_number());
        Ok(())
    });
}

#[test]
fn v8_unwrap_boolean_returns_correct_values() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let bool_true = true.to_local(lock);
        // SAFETY: The isolate is locked and the value is a valid V8 local handle.
        let unwrapped_true =
            unsafe { jsg::v8::ffi::unwrap_boolean(lock.isolate().as_ffi(), bool_true.into_ffi()) };
        assert!(unwrapped_true);

        let bool_false = false.to_local(lock);
        // SAFETY: The isolate is locked and the value is a valid V8 local handle.
        let unwrapped_false =
            unsafe { jsg::v8::ffi::unwrap_boolean(lock.isolate().as_ffi(), bool_false.into_ffi()) };
        assert!(!unwrapped_false);
        Ok(())
    });
}

#[test]
fn v8_unwrap_number_returns_correct_values() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let num = Number::new(2.5).to_local(lock);
        // SAFETY: The isolate is locked and the value is a valid V8 local handle.
        let unwrapped =
            unsafe { jsg::v8::ffi::unwrap_number(lock.isolate().as_ffi(), num.into_ffi()) };
        assert!((unwrapped - 2.5).abs() < f64::EPSILON);

        let zero = Number::new(0.0).to_local(lock);
        // SAFETY: The isolate is locked and the value is a valid V8 local handle.
        let unwrapped_zero =
            unsafe { jsg::v8::ffi::unwrap_number(lock.isolate().as_ffi(), zero.into_ffi()) };
        assert!(unwrapped_zero.abs() < f64::EPSILON);

        let negative = Number::new(-42.5).to_local(lock);
        // SAFETY: The isolate is locked and the value is a valid V8 local handle.
        let unwrapped_neg =
            unsafe { jsg::v8::ffi::unwrap_number(lock.isolate().as_ffi(), negative.into_ffi()) };
        assert!((unwrapped_neg - (-42.5)).abs() < f64::EPSILON);
        Ok(())
    });
}

#[test]
fn v8_unwrap_string_returns_correct_values() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let s = "hello world".to_local(lock);
        // SAFETY: The isolate is locked and the value is a valid V8 local handle.
        let unwrapped =
            unsafe { jsg::v8::ffi::unwrap_string(lock.isolate().as_ffi(), s.into_ffi()) };
        assert_eq!(unwrapped.as_str(), "hello world");

        let empty = "".to_local(lock);
        // SAFETY: The isolate is locked and the value is a valid V8 local handle.
        let unwrapped_empty =
            unsafe { jsg::v8::ffi::unwrap_string(lock.isolate().as_ffi(), empty.into_ffi()) };
        assert_eq!(unwrapped_empty.as_str(), "");

        let unicode = "こんにちは".to_local(lock);
        // SAFETY: The isolate is locked and the value is a valid V8 local handle.
        let unwrapped_unicode =
            unsafe { jsg::v8::ffi::unwrap_string(lock.isolate().as_ffi(), unicode.into_ffi()) };
        assert_eq!(unwrapped_unicode.as_str(), "こんにちは");
        Ok(())
    });
}

/// Tests that `unwrap_resource` rejects a C++ JSG object (tagged with `WORKERD_WRAPPABLE_TAG`).
///
/// Rust wrappables use `WORKERD_RUST_WRAPPABLE_TAG` (0xeb05), while C++ JSG objects use
/// `WORKERD_WRAPPABLE_TAG` (0xeb04). Attempting to unwrap a C++ object through the Rust path
/// must return nullptr to prevent reading garbage from non-existent `data[2]` fields.
#[test]
fn unwrap_resource_rejects_cpp_tagged_object() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let cpp_obj = crate::Harness::create_cpp_tagged_object(lock);

        // unwrap_resource returns nullptr because the object has the C++ tag, not the Rust tag.
        let result =
            // SAFETY: isolate is valid and locked, value is a valid Local.
            unsafe { jsg::v8::ffi::unwrap_resource(lock.isolate().as_ffi(), cpp_obj.into_ffi()) };
        assert!(
            result.get().is_null(),
            "unwrap_resource should return null for a C++ tagged object"
        );

        Ok(())
    });
}

/// Tests that `unwrap_resource` rejects a plain JS object with no internal fields.
///
/// A plain `{}` object has no wrappable tag at all. This verifies we don't crash on
/// objects that were never wrapped by either the C++ or Rust JSG layer.
#[test]
fn unwrap_resource_rejects_plain_js_object() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let plain_obj = ctx.eval_raw("({})").unwrap();

        let result =
            // SAFETY: isolate is valid and locked, value is a valid Local.
            unsafe { jsg::v8::ffi::unwrap_resource(lock.isolate().as_ffi(), plain_obj.into_ffi()) };
        assert!(
            result.get().is_null(),
            "unwrap_resource should return null for a plain JS object"
        );

        Ok(())
    });
}
