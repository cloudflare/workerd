// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use jsg::ToJS;

#[test]
fn v8_is_string_returns_true_for_strings() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let str_val = "hello".to_js(lock)?;
        assert!(str_val.is_string());
        Ok(())
    });
}

#[test]
fn v8_is_boolean_returns_true_for_booleans() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let bool_true = true.to_js(lock)?;
        assert!(bool_true.is_boolean());

        let bool_false = false.to_js(lock)?;
        assert!(bool_false.is_boolean());
        Ok(())
    });
}

#[test]
fn v8_is_number_returns_true_for_numbers() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let float_val = 2.5_f64.to_js(lock)?;
        assert!(float_val.is_number());

        let int_val = 42u32.to_js(lock)?;
        assert!(int_val.is_number());

        let num_u8 = 255u8.to_js(lock)?;
        assert!(num_u8.is_number());
        Ok(())
    });
}

#[test]
fn v8_type_checks_are_mutually_exclusive() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        // String is only string
        let s = "test".to_js(lock)?;
        assert!(s.is_string() && !s.is_boolean() && !s.is_number());

        // Boolean is only boolean
        let b = true.to_js(lock)?;
        assert!(!b.is_string() && b.is_boolean() && !b.is_number());

        // Number is only number
        let n = 42_i32.to_js(lock)?;
        assert!(!n.is_string() && !n.is_boolean() && n.is_number());
        Ok(())
    });
}

#[test]
fn v8_unwrap_boolean_returns_correct_values() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let bool_true = true.to_js(lock)?;
        // SAFETY: The isolate is locked and the value is a valid V8 local handle.
        let unwrapped_true =
            unsafe { jsg::v8::ffi::unwrap_boolean(lock.isolate().as_ffi(), bool_true.into_ffi()) };
        assert!(unwrapped_true);

        let bool_false = false.to_js(lock)?;
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
        for expected in [2.5_f64, 0.0, -42.5] {
            let num = expected.to_js(lock)?;
            let mut unwrapped = f64::NAN;
            // SAFETY: The isolate is locked and the value is a valid V8 local handle.
            let ok = unsafe {
                jsg::v8::ffi::unwrap_number(lock.isolate().as_ffi(), num.into_ffi(), &mut unwrapped)
            };
            assert!(ok);
            assert!((unwrapped - expected).abs() < f64::EPSILON);
        }
        Ok(())
    });
}

#[test]
fn v8_unwrap_string_returns_correct_values() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        for expected in ["hello world", "", "こんにちは"] {
            let s = expected.to_js(lock)?;
            let mut unwrapped = String::new();
            // SAFETY: The isolate is locked and the value is a valid V8 local handle.
            let ok = unsafe {
                jsg::v8::ffi::unwrap_string(lock.isolate().as_ffi(), s.into_ffi(), &mut unwrapped)
            }
            .unwrap();
            assert!(ok);
            assert_eq!(unwrapped, expected);
        }
        Ok(())
    });
}

/// Tests that `unwrap_resource` rejects a C++ JSG object (tagged with `WORKERD_WRAPPABLE_TAG`).
///
/// Rust wrappables use `WORKERD_RUST_WRAPPABLE_TAG` (0xeb05), while C++ JSG objects use
/// `WORKERD_WRAPPABLE_TAG` (0xeb04). Attempting to unwrap a C++ object through the Rust path
/// must return None to prevent reading garbage from non-existent `data[2]` fields.
#[test]
fn unwrap_resource_rejects_cpp_tagged_object() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let cpp_obj = crate::Harness::create_cpp_tagged_object(lock);

        // unwrap_resource returns None because the object has the C++ tag, not the Rust tag.
        let result: Option<kj_rs::KjRc<jsg::v8::ffi::Wrappable>> =
            // SAFETY: isolate is valid and locked, value is a valid Local.
            unsafe { jsg::v8::ffi::unwrap_resource(lock.isolate().as_ffi(), cpp_obj.into_ffi()) }
                .into();
        assert!(
            result.is_none(),
            "unwrap_resource should return None for a C++ tagged object"
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
        let plain_obj = ctx.eval_raw(lock, "({})").unwrap();

        let result: Option<kj_rs::KjRc<jsg::v8::ffi::Wrappable>> =
            // SAFETY: isolate is valid and locked, value is a valid Local.
            unsafe { jsg::v8::ffi::unwrap_resource(lock.isolate().as_ffi(), plain_obj.into_ffi()) }
                .into();
        assert!(
            result.is_none(),
            "unwrap_resource should return None for a plain JS object"
        );

        Ok(())
    });
}
