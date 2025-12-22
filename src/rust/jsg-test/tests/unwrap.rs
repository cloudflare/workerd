use jsg::Lock;
use jsg::v8::ToLocalValue;

#[test]
fn v8_is_string_returns_true_for_strings() {
    let harness = crate::Harness::new();
    harness.run_in_context(|isolate, _ctx| unsafe {
        let mut lock = Lock::from_isolate_ptr(isolate);

        let str_val = "hello".to_local(&mut lock);
        assert!(str_val.is_string());
    });
}

#[test]
fn v8_is_boolean_returns_true_for_booleans() {
    let harness = crate::Harness::new();
    harness.run_in_context(|isolate, _ctx| unsafe {
        let mut lock = Lock::from_isolate_ptr(isolate);

        let bool_true = true.to_local(&mut lock);
        assert!(bool_true.is_boolean());

        let bool_false = false.to_local(&mut lock);
        assert!(bool_false.is_boolean());
    });
}

#[test]
fn v8_is_number_returns_true_for_numbers() {
    let harness = crate::Harness::new();
    harness.run_in_context(|isolate, _ctx| unsafe {
        let mut lock = Lock::from_isolate_ptr(isolate);

        let float_val = 2.5f64.to_local(&mut lock);
        assert!(float_val.is_number());

        let int_val = 42u32.to_local(&mut lock);
        assert!(int_val.is_number());

        let num_u8 = 255u8.to_local(&mut lock);
        assert!(num_u8.is_number());
    });
}

#[test]
fn v8_type_checks_are_mutually_exclusive() {
    let harness = crate::Harness::new();
    harness.run_in_context(|isolate, _ctx| unsafe {
        let mut lock = Lock::from_isolate_ptr(isolate);

        // String is only string
        let s = "test".to_local(&mut lock);
        assert!(s.is_string() && !s.is_boolean() && !s.is_number());

        // Boolean is only boolean
        let b = true.to_local(&mut lock);
        assert!(!b.is_string() && b.is_boolean() && !b.is_number());

        // Number is only number
        let n = 42.0f64.to_local(&mut lock);
        assert!(!n.is_string() && !n.is_boolean() && n.is_number());
    });
}

#[test]
fn v8_unwrap_boolean_returns_correct_values() {
    let harness = crate::Harness::new();
    harness.run_in_context(|isolate, _ctx| unsafe {
        let mut lock = Lock::from_isolate_ptr(isolate);

        let bool_true = true.to_local(&mut lock);
        let unwrapped_true =
            jsg::v8::ffi::unwrap_boolean(lock.isolate().as_ffi(), bool_true.into_ffi());
        assert!(unwrapped_true);

        let bool_false = false.to_local(&mut lock);
        let unwrapped_false =
            jsg::v8::ffi::unwrap_boolean(lock.isolate().as_ffi(), bool_false.into_ffi());
        assert!(!unwrapped_false);
    });
}

#[test]
fn v8_unwrap_number_returns_correct_values() {
    let harness = crate::Harness::new();
    harness.run_in_context(|isolate, _ctx| unsafe {
        let mut lock = Lock::from_isolate_ptr(isolate);

        let num = 2.5f64.to_local(&mut lock);
        let unwrapped = jsg::v8::ffi::unwrap_number(lock.isolate().as_ffi(), num.into_ffi());
        assert!((unwrapped - 2.5).abs() < f64::EPSILON);

        let zero = 0.0f64.to_local(&mut lock);
        let unwrapped_zero = jsg::v8::ffi::unwrap_number(lock.isolate().as_ffi(), zero.into_ffi());
        assert!(unwrapped_zero.abs() < f64::EPSILON);

        let negative = (-42.5f64).to_local(&mut lock);
        let unwrapped_neg =
            jsg::v8::ffi::unwrap_number(lock.isolate().as_ffi(), negative.into_ffi());
        assert!((unwrapped_neg - (-42.5)).abs() < f64::EPSILON);
    });
}

#[test]
fn v8_unwrap_string_returns_correct_values() {
    let harness = crate::Harness::new();
    harness.run_in_context(|isolate, _ctx| unsafe {
        let mut lock = Lock::from_isolate_ptr(isolate);

        let s = "hello world".to_local(&mut lock);
        let unwrapped = jsg::v8::ffi::unwrap_string(lock.isolate().as_ffi(), s.into_ffi());
        assert_eq!(unwrapped.as_str(), "hello world");

        let empty = "".to_local(&mut lock);
        let unwrapped_empty =
            jsg::v8::ffi::unwrap_string(lock.isolate().as_ffi(), empty.into_ffi());
        assert_eq!(unwrapped_empty.as_str(), "");

        let unicode = "こんにちは".to_local(&mut lock);
        let unwrapped_unicode =
            jsg::v8::ffi::unwrap_string(lock.isolate().as_ffi(), unicode.into_ffi());
        assert_eq!(unwrapped_unicode.as_str(), "こんにちは");
    });
}
