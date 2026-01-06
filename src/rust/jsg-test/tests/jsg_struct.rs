use jsg::Error;
use jsg::ExceptionType;
use jsg::ToJS;
use jsg::v8;
use jsg::v8::ToLocalValue;
use jsg_macros::jsg_struct;

#[jsg_struct]
struct TestStruct {
    pub str: String,
}

#[jsg_struct]
struct MultiPropertyStruct {
    pub name: String,
    pub age: u32,
    pub active: String,
}

#[jsg_struct]
struct NestedStruct {
    pub inner: String,
}

#[test]
fn objects_can_be_wrapped_and_unwrapped() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let instance = TestStruct {
            str: "test".to_owned(),
        };
        let wrapped = instance.to_js(lock);
        let mut obj: v8::Local<'_, v8::Object> = wrapped.into();
        assert!(obj.has(lock, "str"));
        let str_value = obj.get(lock, "str");
        assert!(str_value.unwrap().is_string());
        assert!(!obj.has(lock, "test"));
        let value = "value".to_local(lock);
        assert!(value.is_string());
        obj.set(lock, "test", value);
        assert!(obj.has(lock, "test"));
        Ok(())
    });
}

#[test]
fn struct_with_multiple_properties() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let instance = MultiPropertyStruct {
            name: "Alice".to_owned(),
            age: 30,
            active: "true".to_owned(),
        };
        let wrapped = instance.to_js(lock);
        let obj: v8::Local<'_, v8::Object> = wrapped.into();

        assert!(obj.has(lock, "name"));
        assert!(obj.has(lock, "age"));
        assert!(obj.has(lock, "active"));

        let name_value = obj.get(lock, "name");
        assert!(name_value.is_some());
        assert!(name_value.unwrap().is_string());

        let age_value = obj.get(lock, "age");
        assert!(age_value.is_some());

        let active_value = obj.get(lock, "active");
        assert!(active_value.is_some());
        assert!(active_value.unwrap().is_string());
        Ok(())
    });
}

#[test]
fn number_type_conversions() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let byte_val: u8 = 42;
        let byte_local = byte_val.to_local(lock);
        assert!(byte_local.has_value());

        let int_val: u32 = 12345;
        let int_local = int_val.to_local(lock);
        assert!(int_local.has_value());
        Ok(())
    });
}

#[test]
fn empty_object_and_property_setting() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let mut obj = lock.new_object();

        assert!(!obj.has(lock, "nonexistent"));
        assert!(obj.get(lock, "nonexistent").is_none());

        let str_value = "hello".to_local(lock);
        obj.set(lock, "key1", str_value);
        assert!(obj.has(lock, "key1"));

        let num_value = 100u32.to_local(lock);
        obj.set(lock, "key2", num_value);
        assert!(obj.has(lock, "key2"));

        let val1 = obj.get(lock, "key1");
        assert!(val1.is_some());
        assert!(val1.unwrap().is_string());

        let val2 = obj.get(lock, "key2");
        assert!(val2.is_some());
        Ok(())
    });
}

#[test]
fn global_handle_conversion() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let local_str = "global test".to_local(lock);
        assert!(local_str.has_value());

        let global_str = local_str.to_global(lock);
        let local_again = global_str.as_local(lock);
        assert!(local_again.has_value());
        Ok(())
    });
}

#[test]
fn nested_object_properties() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let mut outer = lock.new_object();

        let inner_instance = NestedStruct {
            inner: "nested value".to_owned(),
        };
        let inner_wrapped = inner_instance.to_js(lock);
        outer.set(lock, "nested", inner_wrapped);

        assert!(outer.has(lock, "nested"));
        let nested_val = outer.get(lock, "nested");
        assert!(nested_val.is_some());

        let nested_obj: v8::Local<'_, v8::Object> = nested_val.unwrap().into();
        assert!(nested_obj.has(lock, "inner"));

        let inner_val = nested_obj.get(lock, "inner");
        assert!(inner_val.is_some());
        assert!(inner_val.unwrap().is_string());
        Ok(())
    });
}

#[test]
fn error_creation_and_display() {
    let error = Error::default();
    assert_eq!(error.name, "Error");
    assert_eq!(error.message, "An unknown error occurred");

    let type_error = Error::new("TypeError", "Invalid type".to_owned());
    assert_eq!(type_error.name, "TypeError");
    assert_eq!(type_error.message, "Invalid type");

    // Test Display for all exception types
    assert_eq!(format!("{}", ExceptionType::RangeError), "RangeError");
    assert_eq!(
        format!("{}", ExceptionType::ReferenceError),
        "ReferenceError"
    );
    assert_eq!(format!("{}", ExceptionType::SyntaxError), "SyntaxError");
}

#[test]
fn error_from_parse_int_error() {
    let parse_result: Result<i32, _> = "not_a_number".parse();
    let error: Error = parse_result.unwrap_err().into();
    assert_eq!(error.name, "TypeError");
    assert!(error.message.contains("Failed to parse integer"));
}

#[test]
fn type_of_returns_correct_js_types() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let str_val = "hello".to_local(lock);
        assert_eq!(str_val.type_of(), "string");
        assert!(str_val.is_string());
        assert!(!str_val.is_null_or_undefined());

        let num_val = 42u32.to_local(lock);
        assert_eq!(num_val.type_of(), "number");
        assert!(num_val.is_number());
        assert!(!num_val.is_null_or_undefined());

        let bool_val = true.to_local(lock);
        assert_eq!(bool_val.type_of(), "boolean");
        assert!(bool_val.is_boolean());
        assert!(!bool_val.is_null_or_undefined());

        let obj_val = lock.new_object();
        assert_eq!(obj_val.type_of(), "object");
        assert!(!obj_val.is_null_or_undefined());
        Ok(())
    });
}
