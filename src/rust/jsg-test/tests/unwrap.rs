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
        let float_val = 2.5f64.to_local(lock);
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
        let n = 42.0f64.to_local(lock);
        assert!(!n.is_string() && !n.is_boolean() && n.is_number());
        Ok(())
    });
}

#[test]
fn v8_unwrap_boolean_returns_correct_values() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let bool_true = true.to_local(lock);
        let unwrapped_true =
            unsafe { jsg::v8::ffi::unwrap_boolean(lock.isolate().as_ffi(), bool_true.into_ffi()) };
        assert!(unwrapped_true);

        let bool_false = false.to_local(lock);
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
        let num = 2.5f64.to_local(lock);
        let unwrapped =
            unsafe { jsg::v8::ffi::unwrap_number(lock.isolate().as_ffi(), num.into_ffi()) };
        assert!((unwrapped - 2.5).abs() < f64::EPSILON);

        let zero = 0.0f64.to_local(lock);
        let unwrapped_zero =
            unsafe { jsg::v8::ffi::unwrap_number(lock.isolate().as_ffi(), zero.into_ffi()) };
        assert!(unwrapped_zero.abs() < f64::EPSILON);

        let negative = (-42.5f64).to_local(lock);
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
        let unwrapped =
            unsafe { jsg::v8::ffi::unwrap_string(lock.isolate().as_ffi(), s.into_ffi()) };
        assert_eq!(unwrapped.as_str(), "hello world");

        let empty = "".to_local(lock);
        let unwrapped_empty =
            unsafe { jsg::v8::ffi::unwrap_string(lock.isolate().as_ffi(), empty.into_ffi()) };
        assert_eq!(unwrapped_empty.as_str(), "");

        let unicode = "こんにちは".to_local(lock);
        let unwrapped_unicode =
            unsafe { jsg::v8::ffi::unwrap_string(lock.isolate().as_ffi(), unicode.into_ffi()) };
        assert_eq!(unwrapped_unicode.as_str(), "こんにちは");
        Ok(())
    });
}
