// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

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

#[jsg_struct]
#[derive(Debug, PartialEq)]
struct OptionalStruct {
    pub label: Option<String>,
    pub count: Option<u32>,
}

#[jsg_struct]
#[derive(Debug)]
struct RequiredStruct {
    pub count: u32,
}

#[test]
fn objects_can_be_wrapped_and_unwrapped() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let instance = TestStruct {
            str: "test".to_owned(),
        };
        let wrapped = instance.to_js(lock);
        let obj: v8::Local<'_, v8::Object> = wrapped.try_into().unwrap();
        assert!(obj.has(lock, "str").unwrap());
        let str_value = obj.get(lock, "str");
        assert!(str_value.unwrap().is_string());
        assert!(!obj.has(lock, "test").unwrap());
        let value = "value".to_local(lock);
        assert!(value.is_string());
        obj.set(lock, "test", value);
        assert!(obj.has(lock, "test").unwrap());
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
        let obj: v8::Local<'_, v8::Object> = wrapped.try_into().unwrap();

        assert!(obj.has(lock, "name").unwrap());
        assert!(obj.has(lock, "age").unwrap());
        assert!(obj.has(lock, "active").unwrap());

        let name_value = obj.get(lock, "name");
        assert!(name_value.is_ok());
        assert!(name_value.unwrap().is_string());

        let age_value = obj.get(lock, "age");
        assert!(age_value.is_ok());

        let active_value = obj.get(lock, "active");
        assert!(active_value.is_ok());
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
        assert!(byte_local.is_number());

        let int_val: u32 = 12345;
        let int_local = int_val.to_local(lock);
        assert!(int_local.is_number());
        Ok(())
    });
}

#[test]
fn empty_object_and_property_setting() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let obj = lock.new_object();

        assert!(!obj.has(lock, "nonexistent").unwrap());
        assert!(obj.get(lock, "nonexistent").unwrap().is_undefined());

        let str_value = "hello".to_local(lock);
        obj.set(lock, "key1", str_value);
        assert!(obj.has(lock, "key1").unwrap());

        let num_value = 100u32.to_local(lock);
        obj.set(lock, "key2", num_value);
        assert!(obj.has(lock, "key2").unwrap());

        let val1 = obj.get(lock, "key1");
        assert!(val1.is_ok());
        assert!(val1.unwrap().is_string());

        let val2 = obj.get(lock, "key2");
        assert!(val2.is_ok());
        Ok(())
    });
}

#[test]
fn global_handle_conversion() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let local_str = "global test".to_local(lock);
        assert!(local_str.is_string());

        let global_str = local_str.to_traced_global(lock);
        let local_again = global_str.as_local(lock);
        assert!(local_again.is_string());
        Ok(())
    });
}

#[test]
fn nested_object_properties() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let outer = lock.new_object();

        let inner_instance = NestedStruct {
            inner: "nested value".to_owned(),
        };
        let inner_wrapped = inner_instance.to_js(lock);
        outer.set(lock, "nested", inner_wrapped);

        assert!(outer.has(lock, "nested").unwrap());
        let nested_val = outer.get(lock, "nested");
        assert!(nested_val.is_ok());

        let nested_obj: v8::Local<'_, v8::Object> = nested_val.unwrap().try_into().unwrap();
        assert!(nested_obj.has(lock, "inner").unwrap());

        let inner_val = nested_obj.get(lock, "inner");
        assert!(inner_val.is_ok());
        assert!(inner_val.unwrap().is_string());
        Ok(())
    });
}

#[test]
fn error_creation_and_display() {
    let type_error = Error::new_type_error("Invalid type");
    assert_eq!(type_error.name, ExceptionType::TypeError);
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
    assert_eq!(error.name, ExceptionType::RangeError);
    assert!(error.message.contains("Failed to parse integer"));
}

#[test]
fn type_of_returns_correct_js_types() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let str_val = "hello".to_local(lock);
        assert_eq!(str_val.type_of(lock), "string");
        assert!(str_val.is_string());
        assert!(!str_val.is_null_or_undefined());

        let num_val = 42u32.to_local(lock);
        assert_eq!(num_val.type_of(lock), "number");
        assert!(num_val.is_number());
        assert!(!num_val.is_null_or_undefined());

        let bool_val = true.to_local(lock);
        assert_eq!(bool_val.type_of(lock), "boolean");
        assert!(bool_val.is_boolean());
        assert!(!bool_val.is_null_or_undefined());

        let obj_val = lock.new_object();
        assert_eq!(obj_val.type_of(lock), "object");
        assert!(!obj_val.is_null_or_undefined());
        Ok(())
    });
}

/// Converts the JS expression `expr` with `T`'s `FromJS`.
fn convert<'s, T: jsg::FromJS<'s>>(
    lock: &mut jsg::Lock<'s>,
    ctx: &crate::EvalContext<'_>,
    expr: &str,
) -> Result<T::ResultType, Error> {
    let value = ctx
        .eval_raw(lock, expr)
        .expect("expression should evaluate");
    T::from_js(lock, value)
}

/// Like C++ JSG, null or undefined is an empty dictionary when every field is
/// optional, and absent properties read as undefined.
#[test]
fn optional_struct_from_null_undefined_and_partial_objects() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let empty = OptionalStruct {
            label: None,
            count: None,
        };
        assert_eq!(convert::<OptionalStruct>(lock, ctx, "null")?, empty);
        assert_eq!(convert::<OptionalStruct>(lock, ctx, "undefined")?, empty);
        assert_eq!(convert::<OptionalStruct>(lock, ctx, "({})")?, empty);
        assert_eq!(
            convert::<OptionalStruct>(lock, ctx, "({ count: 3 })")?,
            OptionalStruct {
                label: None,
                count: Some(3),
            }
        );
        Ok(())
    });
}

/// A struct with a required field cannot come from null or undefined, and an
/// absent required property converts from undefined.
#[test]
fn required_struct_from_null_and_absent_property() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let err = convert::<RequiredStruct>(lock, ctx, "null").unwrap_err();
        assert_eq!(err.name, ExceptionType::TypeError);
        assert_eq!(
            err.message,
            "Cannot initialize RequiredStruct with required members from an undefined or null value."
        );
        let err = convert::<RequiredStruct>(lock, ctx, "({})").unwrap_err();
        assert_eq!(err.name, ExceptionType::TypeError);
        assert_eq!(
            err.message,
            "The value cannot be converted because it is not an integer."
        );
        Ok(())
    });
}

/// A throwing getter fails the conversion with its own error.
#[test]
fn struct_from_throwing_getter_propagates_error() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let err = convert::<OptionalStruct>(
            lock,
            ctx,
            "({ get label() { throw new RangeError('getter'); } })",
        )
        .unwrap_err();
        assert_eq!(err.name, ExceptionType::RangeError);
        assert!(err.message.contains("getter"), "got {:?}", err.message);
        Ok(())
    });
}

/// Like C++ JSG, an absent optional field is omitted from the JS object.
#[test]
fn struct_omits_absent_optional_fields() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let value = OptionalStruct {
            label: None,
            count: Some(1),
        }
        .to_js(lock);
        let obj: v8::Local<'_, v8::Object> = value.try_into().unwrap();
        assert!(!obj.has(lock, "label")?);
        assert!(obj.has(lock, "count")?);
        Ok(())
    });
}
