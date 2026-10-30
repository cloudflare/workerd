// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! `#[jsg_oneof]` variants of the types that a `kj::OneOf` can hold beyond strings, numbers,
//! booleans, resources, structs and sequences: integers, `i64`, `SystemTime`, `Nullable<T>`,
//! `Dict<V>` and V8 handles. Each converts in the pass that C++ JSG's `OneOfWrapper` gives its C++
//! counterpart (`int`, `int64_t`, `kj::Date`, `kj::Maybe<T>`, `jsg::Dict<V>` and `v8::Local<T>`).
//! What a union may not contain is checked at compile time, by the snippets in
//! `src/rust/jsg-compile-tests/oneof-*.rs`.

use std::time::Duration;
use std::time::SystemTime;

use jsg::Dict;
use jsg::ExceptionType;
use jsg::FromJS;
use jsg::Nullable;
use jsg::ToJS;
use jsg::v8;
use jsg_macros::jsg_method;
use jsg_macros::jsg_oneof;
use jsg_macros::jsg_resource;
use jsg_macros::jsg_struct;

#[jsg_struct]
#[derive(Debug, Clone, PartialEq)]
struct OptionalPoint {
    pub x: Option<u32>,
}

/// `kj::OneOf<uint32_t, kj::String>`.
#[jsg_oneof]
#[derive(Debug, Clone, PartialEq)]
enum U32OrString {
    U32(u32),
    String(String),
}

/// `kj::OneOf<int64_t, kj::String>`.
#[jsg_oneof]
#[derive(Debug, Clone, PartialEq)]
enum I64OrString {
    I64(i64),
    String(String),
}

/// `kj::OneOf<kj::Date, double>`.
#[jsg_oneof]
#[derive(Debug, Clone, PartialEq)]
enum DateOrNumber {
    Date(SystemTime),
    Number(f64),
}

/// `kj::OneOf<kj::Maybe<kj::String>, double>`.
#[jsg_oneof]
#[derive(Debug, Clone, PartialEq)]
enum NullableStringOrNumber {
    String(Nullable<String>),
    Number(f64),
}

/// `kj::OneOf<kj::Maybe<uint32_t>, kj::String>`.
#[jsg_oneof]
#[derive(Debug, Clone, PartialEq)]
enum NullableU32OrString {
    U32(Nullable<u32>),
    String(String),
}

/// `kj::OneOf<jsg::Dict<double>, kj::Array<kj::String>, kj::String>`.
#[jsg_oneof]
#[derive(Debug, Clone, PartialEq)]
enum DictSequenceOrString {
    Dict(Dict<f64>),
    Sequence(Vec<String>),
    String(String),
}

/// `kj::OneOf<v8::Local<v8::String>, double>`.
#[jsg_oneof]
enum StringHandleOrNumber<'s> {
    String(v8::Local<'s, v8::String>),
    Number(f64),
}

/// `kj::OneOf<v8::Local<v8::Object>, kj::String>`, with the lifetime named otherwise.
#[jsg_oneof]
enum ObjectOrString<'a> {
    Object(v8::Local<'a, v8::Object>),
    String(String),
}

/// `kj::OneOf<v8::Local<v8::Function>, double>`.
#[jsg_oneof]
enum FunctionOrNumber<'s> {
    Function(v8::Local<'s, v8::Function>),
    Number(f64),
}

/// `kj::OneOf<v8::Local<v8::BigInt>, kj::String>`.
#[jsg_oneof]
enum BigIntOrString<'s> {
    BigInt(v8::Local<'s, v8::BigInt>),
    String(String),
}

/// `kj::OneOf<v8::Local<v8::Uint8Array>, kj::String>`.
#[jsg_oneof]
enum Uint8ArrayOrString<'s> {
    Bytes(v8::Local<'s, v8::Uint8Array>),
    String(String),
}

/// `kj::OneOf<v8::Local<v8::Uint8ClampedArray>, kj::String>`.
#[jsg_oneof]
enum Uint8ClampedArrayOrString<'s> {
    Clamped(v8::Local<'s, v8::Uint8ClampedArray>),
    String(String),
}

/// A dictionary and a value of any type, in both orders: both are tried in the same pass, in
/// declaration order.
#[jsg_oneof]
enum PointOrValue<'s> {
    Point(OptionalPoint),
    Value(v8::Local<'s, v8::Value>),
}

#[jsg_oneof]
enum ValueOrPoint<'s> {
    Value(v8::Local<'s, v8::Value>),
    Point(OptionalPoint),
}

/// A union with a handle, nested in another; the inner union's variants are tried in the outer
/// union's passes.
#[jsg_oneof]
enum NumberOrNestedHandle<'s> {
    Number(f64),
    Nested(StringHandleOrBool<'s>),
}

#[jsg_oneof]
enum StringHandleOrBool<'s> {
    String(v8::Local<'s, v8::String>),
    Bool(bool),
}

#[jsg_resource]
struct VariantContext;

#[jsg_resource]
impl VariantContext {
    #[jsg_method]
    pub fn take_u32_or_string(&self, value: U32OrString) -> String {
        format!("{value:?}")
    }

    #[jsg_method]
    pub fn take_i64_or_string(&self, value: I64OrString) -> String {
        format!("{value:?}")
    }

    #[jsg_method]
    pub fn take_date_or_number(&self, value: DateOrNumber) -> String {
        match value {
            DateOrNumber::Date(date) => {
                let millis = date
                    .duration_since(SystemTime::UNIX_EPOCH)
                    .unwrap_or_default()
                    .as_millis();
                format!("date:{millis}")
            }
            DateOrNumber::Number(n) => format!("number:{n}"),
        }
    }

    #[jsg_method]
    pub fn return_date_or_number(&self, date: bool) -> DateOrNumber {
        if date {
            DateOrNumber::Date(SystemTime::UNIX_EPOCH + Duration::from_secs(1))
        } else {
            DateOrNumber::Number(1.0)
        }
    }

    #[jsg_method]
    pub fn take_nullable_string_or_number(&self, value: NullableStringOrNumber) -> String {
        format!("{value:?}")
    }

    #[jsg_method]
    pub fn take_nullable_u32_or_string(&self, value: NullableU32OrString) -> String {
        format!("{value:?}")
    }

    #[jsg_method]
    pub fn take_dict_sequence_or_string(&self, value: DictSequenceOrString) -> String {
        match value {
            DictSequenceOrString::Dict(dict) => {
                let entries: Vec<_> = dict
                    .into_iter()
                    .map(|(key, value)| format!("{key}={value}"))
                    .collect();
                format!("dict:{}", entries.join(","))
            }
            DictSequenceOrString::Sequence(items) => format!("sequence:{}", items.join(",")),
            DictSequenceOrString::String(s) => format!("string:{s}"),
        }
    }

    #[jsg_method]
    pub fn return_dict_or_sequence(&self, dict: bool) -> DictSequenceOrString {
        if dict {
            DictSequenceOrString::Dict(Dict::from(vec![("a".to_owned(), 1.0)]))
        } else {
            DictSequenceOrString::Sequence(vec!["x".to_owned()])
        }
    }

    #[jsg_method]
    pub fn take_string_handle_or_number<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        value: StringHandleOrNumber<'s>,
    ) -> Result<String, jsg::Error> {
        Ok(match value {
            StringHandleOrNumber::String(s) => {
                format!("handle:{}", String::from_js(lock, s.into())?)
            }
            StringHandleOrNumber::Number(n) => format!("number:{n}"),
        })
    }

    #[jsg_method]
    pub fn take_object_or_string<'s>(&self, value: ObjectOrString<'s>) -> String {
        match value {
            ObjectOrString::Object(_) => "object".to_owned(),
            ObjectOrString::String(s) => format!("string:{s}"),
        }
    }

    /// Returns the object it is given, as the same union.
    #[jsg_method]
    pub fn echo_object_or_string<'s>(&self, value: ObjectOrString<'s>) -> ObjectOrString<'s> {
        value
    }

    #[jsg_method]
    pub fn take_function_or_number<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        value: FunctionOrNumber<'s>,
    ) -> Result<String, jsg::Error> {
        Ok(match value {
            FunctionOrNumber::Function(f) => {
                let result = f.call(lock, None, &[])?;
                let result = String::from_js(lock, result)?;
                format!("function:{result}")
            }
            FunctionOrNumber::Number(n) => format!("number:{n}"),
        })
    }

    #[jsg_method]
    pub fn take_big_int_or_string<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        value: BigIntOrString<'s>,
    ) -> Result<String, jsg::Error> {
        Ok(match value {
            BigIntOrString::BigInt(b) => format!("bigint:{}", i64::from_js(lock, b.into())?),
            BigIntOrString::String(s) => format!("string:{s}"),
        })
    }

    #[jsg_method]
    pub fn take_uint8_array_or_string<'s>(&self, value: Uint8ArrayOrString<'s>) -> String {
        match value {
            Uint8ArrayOrString::Bytes(bytes) => format!("bytes:{}", bytes.len()),
            Uint8ArrayOrString::String(s) => format!("string:{s}"),
        }
    }

    #[jsg_method]
    pub fn take_uint8_clamped_array_or_string<'s>(
        &self,
        value: Uint8ClampedArrayOrString<'s>,
    ) -> String {
        match value {
            Uint8ClampedArrayOrString::Clamped(bytes) => format!("clamped:{}", bytes.len()),
            Uint8ClampedArrayOrString::String(s) => format!("string:{s}"),
        }
    }

    #[jsg_method]
    pub fn take_point_or_value<'s>(&self, value: PointOrValue<'s>) -> String {
        match value {
            PointOrValue::Point(p) => format!("point:{:?}", p.x),
            PointOrValue::Value(_) => "value".to_owned(),
        }
    }

    #[jsg_method]
    pub fn take_value_or_point<'s>(&self, value: ValueOrPoint<'s>) -> String {
        match value {
            ValueOrPoint::Value(_) => "value".to_owned(),
            ValueOrPoint::Point(p) => format!("point:{:?}", p.x),
        }
    }

    #[jsg_method]
    pub fn take_number_or_nested_handle<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        value: NumberOrNestedHandle<'s>,
    ) -> Result<String, jsg::Error> {
        Ok(match value {
            NumberOrNestedHandle::Number(n) => format!("number:{n}"),
            NumberOrNestedHandle::Nested(nested) => describe_string_handle_or_bool(lock, &nested)?,
        })
    }

    #[jsg_method]
    pub fn take_string_handle_or_bool<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        value: StringHandleOrBool<'s>,
    ) -> Result<String, jsg::Error> {
        describe_string_handle_or_bool(lock, &value)
    }
}

fn describe_string_handle_or_bool<'s>(
    lock: &mut jsg::Lock<'s>,
    value: &StringHandleOrBool<'s>,
) -> Result<String, jsg::Error> {
    Ok(match value {
        StringHandleOrBool::String(s) => format!("handle:{}", String::from_js(lock, (*s).into())?),
        StringHandleOrBool::Bool(b) => format!("bool:{b}"),
    })
}

fn with_context(f: impl for<'s, 'c> FnOnce(&mut jsg::Lock<'s>, &crate::EvalContext<'c>)) {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let cx = jsg::Rc::new(VariantContext).to_js(lock)?;
        ctx.set_global(lock, "cx", cx);
        f(lock, ctx);
        Ok(())
    });
}

fn expect_eval(lock: &mut jsg::Lock<'_>, ctx: &crate::EvalContext<'_>, code: &str, expected: &str) {
    let result: String = ctx.eval(lock, code).unwrap();
    assert_eq!(result, expected, "{code}");
}

fn expect_type_error(lock: &mut jsg::Lock<'_>, ctx: &crate::EvalContext<'_>, code: &str) -> String {
    let err = ctx
        .eval::<String>(lock, code)
        .unwrap_err()
        .unwrap_jsg_err(lock);
    assert_eq!(err.name, ExceptionType::TypeError, "{code}");
    err.message
}

// =============================================================================
// Integers and i64
// =============================================================================

/// An integer is a numeric type: it takes a number in the exact-number pass, and anything else
/// only after the string pass. An error from converting a number ends the conversion, as
/// `JSG_REQUIRE` in C++ JSG's integer conversion does, rather than trying the string variant.
#[test]
fn integer_variant_is_numeric() {
    with_context(|lock, ctx| {
        expect_eval(lock, ctx, "cx.takeU32OrString(5)", "U32(5)");
        expect_eval(lock, ctx, "cx.takeU32OrString('5')", "String(\"5\")");
        expect_eval(lock, ctx, "cx.takeU32OrString(true)", "String(\"true\")");
        let message = expect_type_error(lock, ctx, "cx.takeU32OrString(-1)");
        assert!(message.contains("negative"), "{message}");
    });
}

/// `i64`, like `int64_t`, takes a `BigInt` in the exact-number pass too.
#[test]
fn i64_variant_takes_big_int() {
    with_context(|lock, ctx| {
        expect_eval(lock, ctx, "cx.takeI64OrString(5n)", "I64(5)");
        expect_eval(lock, ctx, "cx.takeI64OrString(5)", "I64(5)");
        expect_eval(lock, ctx, "cx.takeI64OrString('5')", "String(\"5\")");
    });
}

// =============================================================================
// SystemTime
// =============================================================================

/// `kj::Date` is in none of Web IDL's categories, so it is tried with the types that aren't
/// strings, numbers or booleans, before any numeric variant, and there takes a number of
/// milliseconds as well as a `Date`.
#[test]
fn date_variant_is_tried_before_numbers() {
    with_context(|lock, ctx| {
        expect_eval(
            lock,
            ctx,
            "cx.takeDateOrNumber(new Date(1000))",
            "date:1000",
        );
        expect_eval(lock, ctx, "cx.takeDateOrNumber(5)", "date:5");
        expect_eval(lock, ctx, "cx.takeDateOrNumber('5')", "number:5");
        expect_eval(
            lock,
            ctx,
            "String(cx.returnDateOrNumber(true) instanceof Date && \
             cx.returnDateOrNumber(true).getTime())",
            "1000",
        );
        expect_eval(lock, ctx, "typeof cx.returnDateOrNumber(false)", "number");
    });
}

// =============================================================================
// Nullable
// =============================================================================

/// A nullable variant takes `null` and `undefined`. Like `kj::Maybe<T>`, it is not itself a
/// string, number or boolean type, so it is tried before them, and converts any other value as
/// its inner type would, coercion included.
#[test]
fn nullable_variant_is_tried_before_primitives() {
    with_context(|lock, ctx| {
        expect_eval(
            lock,
            ctx,
            "cx.takeNullableStringOrNumber(null)",
            "String(Null)",
        );
        expect_eval(
            lock,
            ctx,
            "cx.takeNullableStringOrNumber(undefined)",
            "String(Null)",
        );
        expect_eval(
            lock,
            ctx,
            "cx.takeNullableStringOrNumber(5)",
            "String(Some(\"5\"))",
        );
        expect_eval(lock, ctx, "cx.takeNullableU32OrString(null)", "U32(Null)");
        expect_eval(lock, ctx, "cx.takeNullableU32OrString(5)", "U32(Some(5))");
    });
}

// =============================================================================
// Dict
// =============================================================================

/// A record takes an object other than an array, which a sequence takes.
#[test]
fn dict_variant_takes_objects_but_not_arrays() {
    with_context(|lock, ctx| {
        expect_eval(
            lock,
            ctx,
            "cx.takeDictSequenceOrString({ a: 1, b: 2 })",
            "dict:a=1,b=2",
        );
        expect_eval(
            lock,
            ctx,
            "cx.takeDictSequenceOrString(['x', 'y'])",
            "sequence:x,y",
        );
        expect_eval(lock, ctx, "cx.takeDictSequenceOrString('z')", "string:z");
        expect_eval(
            lock,
            ctx,
            "JSON.stringify(cx.returnDictOrSequence(true))",
            "{\"a\":1}",
        );
        expect_eval(
            lock,
            ctx,
            "JSON.stringify(cx.returnDictOrSequence(false))",
            "[\"x\"]",
        );
    });
}

// =============================================================================
// Handles
// =============================================================================

/// A string handle is a string type, but takes only a string, as C++ JSG's `v8::Local<v8::String>`
/// does, so a boolean is coerced by the numeric variant instead.
#[test]
fn string_handle_variant_takes_only_strings() {
    with_context(|lock, ctx| {
        expect_eval(lock, ctx, "cx.takeStringHandleOrNumber('a')", "handle:a");
        expect_eval(lock, ctx, "cx.takeStringHandleOrNumber(5)", "number:5");
        expect_eval(lock, ctx, "cx.takeStringHandleOrNumber(true)", "number:1");
    });
}

#[test]
fn object_handle_variant_takes_objects() {
    with_context(|lock, ctx| {
        expect_eval(lock, ctx, "cx.takeObjectOrString({})", "object");
        expect_eval(lock, ctx, "cx.takeObjectOrString([])", "object");
        expect_eval(lock, ctx, "cx.takeObjectOrString(5)", "string:5");
        expect_eval(
            lock,
            ctx,
            "const o = {}; String(cx.echoObjectOrString(o) === o)",
            "true",
        );
        expect_eval(lock, ctx, "cx.echoObjectOrString(5)", "5");
    });
}

#[test]
fn function_handle_variant_takes_functions() {
    with_context(|lock, ctx| {
        expect_eval(
            lock,
            ctx,
            "cx.takeFunctionOrNumber(() => 'called')",
            "function:called",
        );
        expect_eval(lock, ctx, "cx.takeFunctionOrNumber('5')", "number:5");
    });
}

/// A `BigInt` handle is a numeric type, as C++ JSG's `v8::Local<v8::BigInt>` is, but takes only a
/// `BigInt`, so a number becomes a string.
#[test]
fn big_int_handle_variant_takes_only_big_ints() {
    with_context(|lock, ctx| {
        expect_eval(lock, ctx, "cx.takeBigIntOrString(5n)", "bigint:5");
        expect_eval(lock, ctx, "cx.takeBigIntOrString(5)", "string:5");
    });
}

#[test]
fn typed_array_handle_variant_takes_its_typed_array() {
    with_context(|lock, ctx| {
        expect_eval(
            lock,
            ctx,
            "cx.takeUint8ArrayOrString(new Uint8Array(3))",
            "bytes:3",
        );
        expect_eval(
            lock,
            ctx,
            "cx.takeUint8ArrayOrString(new Uint16Array(3))",
            "string:0,0,0",
        );
    });
}

/// A `Uint8ClampedArray` handle takes only a `Uint8ClampedArray`, not a `Uint8Array` with the same
/// elements, as C++ JSG's `v8::Local<v8::Uint8ClampedArray>` does.
#[test]
fn uint8_clamped_array_handle_variant_takes_only_its_typed_array() {
    with_context(|lock, ctx| {
        expect_eval(
            lock,
            ctx,
            "cx.takeUint8ClampedArrayOrString(new Uint8ClampedArray(3))",
            "clamped:3",
        );
        expect_eval(
            lock,
            ctx,
            "cx.takeUint8ClampedArrayOrString(new Uint8Array(3))",
            "string:0,0,0",
        );
    });
}

/// A value handle takes anything, in the pass for types that aren't strings, numbers or
/// booleans, so within that pass declaration order decides.
#[test]
fn declaration_order_decides_within_a_pass() {
    with_context(|lock, ctx| {
        expect_eval(lock, ctx, "cx.takePointOrValue({ x: 1 })", "point:Some(1)");
        expect_eval(lock, ctx, "cx.takePointOrValue(null)", "point:None");
        expect_eval(lock, ctx, "cx.takePointOrValue(5)", "value");
        expect_eval(lock, ctx, "cx.takeValueOrPoint({ x: 1 })", "value");
    });
}

#[test]
fn nested_union_with_handles_is_flattened() {
    with_context(|lock, ctx| {
        expect_eval(lock, ctx, "cx.takeNumberOrNestedHandle('a')", "handle:a");
        expect_eval(lock, ctx, "cx.takeNumberOrNestedHandle(true)", "bool:true");
        expect_eval(lock, ctx, "cx.takeNumberOrNestedHandle(5)", "number:5");
        expect_eval(lock, ctx, "cx.takeNumberOrNestedHandle({})", "number:NaN");
    });
}

/// A string handle takes only a string, so with no numeric variant a value that is neither a
/// string nor a boolean reaches the last pass, where the boolean variant coerces it, as in
/// `kj::OneOf<v8::Local<v8::String>, bool>`.
#[test]
fn boolean_variant_coerces_in_the_last_pass() {
    with_context(|lock, ctx| {
        expect_eval(lock, ctx, "cx.takeStringHandleOrBool('a')", "handle:a");
        expect_eval(lock, ctx, "cx.takeStringHandleOrBool(true)", "bool:true");
        expect_eval(lock, ctx, "cx.takeStringHandleOrBool(0)", "bool:false");
        expect_eval(lock, ctx, "cx.takeStringHandleOrBool(1)", "bool:true");
        expect_eval(lock, ctx, "cx.takeStringHandleOrBool({})", "bool:true");
        expect_eval(
            lock,
            ctx,
            "cx.takeStringHandleOrBool(undefined)",
            "bool:false",
        );
    });
}
