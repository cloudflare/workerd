// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! `#[jsg_oneof]` unions convert values as C++ JSG's `kj::OneOf` does
//! (`OneOfWrapper` in `jsg/value.h`). The first tests mirror the `OneOf` test in
//! `src/workerd/jsg/value-test.c++`, except `returnOneOf(null, null, null)`,
//! since a Rust enum can't be empty. Later ones port the unions in other tests
//! there.

use std::sync::atomic::AtomicUsize;
use std::sync::atomic::Ordering;

use jsg::ExceptionType;
use jsg::NonCoercible;
use jsg::Nullable;
use jsg::ToJS;
use jsg_macros::jsg_method;
use jsg_macros::jsg_oneof;
use jsg_macros::jsg_property;
use jsg_macros::jsg_resource;
use jsg_macros::jsg_struct;

#[jsg_resource]
struct OneOfBox {
    value: f64,
}

#[jsg_resource]
impl OneOfBox {
    #[jsg_property(prototype, readonly)]
    pub fn get_value(&self) -> f64 {
        self.value
    }
}

#[jsg_struct]
#[derive(Debug, Clone, PartialEq)]
struct Point {
    pub x: u32,
}

#[jsg_struct]
#[derive(Debug, Clone, PartialEq)]
struct OptionalPoint {
    pub x: Option<u32>,
}

/// `kj::OneOf<double, kj::String, Ref<NumberBox>>`.
#[jsg_oneof]
#[derive(Debug, Clone)]
enum NumberStringOrBox {
    Number(f64),
    String(String),
    Box(jsg::Rc<OneOfBox>),
}

#[jsg_oneof]
#[derive(Debug, Clone, PartialEq)]
enum StringOrBool {
    String(String),
    Bool(bool),
}

#[jsg_oneof]
#[derive(Debug, Clone, PartialEq)]
enum NumberOrBool {
    Number(f64),
    Bool(bool),
}

#[jsg_oneof]
#[derive(Debug, Clone, PartialEq)]
enum StringOrNumber {
    String(String),
    Number(f64),
}

#[jsg_oneof]
#[derive(Debug, Clone, PartialEq)]
enum NumberOrString {
    Number(f64),
    String(String),
}

/// `kj::OneOf<double, kj::OneOf<kj::String, bool>>`.
#[jsg_oneof]
#[derive(Debug, Clone, PartialEq)]
enum NumberOrNested {
    Number(f64),
    Nested(StringOrBool),
}

#[jsg_oneof]
#[derive(Debug, Clone, PartialEq)]
enum StringOrBytes {
    String(String),
    Bytes(Vec<u8>),
}

#[jsg_oneof]
#[derive(Debug, Clone, PartialEq)]
enum StringOrOptionalPoint {
    String(String),
    Point(OptionalPoint),
}

#[jsg_oneof]
#[derive(Debug, Clone)]
enum PointOrBox {
    Point(OptionalPoint),
    Box(jsg::Rc<OneOfBox>),
}

#[jsg_oneof]
#[derive(Debug, Clone, PartialEq)]
enum PointOrBytes {
    Point(Point),
    Bytes(Vec<u8>),
}

#[jsg_oneof]
#[derive(Debug, Clone, PartialEq)]
enum StrictStringOrNumber {
    String(NonCoercible<String>),
    Number(f64),
}

/// `kj::OneOf<NonCoercible<bool>, NonCoercible<kj::String>>`.
#[jsg_oneof]
#[derive(Debug, Clone, PartialEq)]
enum StrictBoolOrString {
    Bool(NonCoercible<bool>),
    String(NonCoercible<String>),
}

/// `kj::OneOf<kj::String, Sequence<kj::String>>`.
#[jsg_oneof]
#[derive(Debug, Clone, PartialEq)]
enum StringOrStrings {
    String(String),
    Strings(Vec<String>),
}

/// `kj::OneOf<NonCoercible<kj::String>>`, taken as a `kj::Maybe`.
#[jsg_oneof]
#[derive(Debug, Clone, PartialEq)]
enum OnlyStrictString {
    String(NonCoercible<String>),
}

/// A string that converts from JavaScript but not to it.
#[derive(Debug, Clone, PartialEq)]
struct FromJsOnly(String);

jsg::jsg_traced!(FromJsOnly);

impl jsg::Type for FromJsOnly {
    fn class_name() -> &'static str {
        "FromJsOnly"
    }

    fn is_exact(value: jsg::v8::Local<'_, jsg::v8::Value>) -> bool {
        value.is_string()
    }
}

impl jsg::TypeInfo for FromJsOnly {
    fn describe(_cx: &mut jsg::rtti::Context) -> jsg::rtti::Type {
        jsg::rtti::Type::String
    }
}

impl<'s> jsg::FromJS<'s> for FromJsOnly {
    type ResultType = Self;

    fn from_js(
        lock: &mut jsg::Lock<'s>,
        value: jsg::v8::Local<'s, jsg::v8::Value>,
    ) -> Result<Self, jsg::Error> {
        <String as jsg::FromJS<'s>>::from_js(lock, value).map(Self)
    }

    fn try_from_js(
        lock: &mut jsg::Lock<'s>,
        value: jsg::v8::Local<'s, jsg::v8::Value>,
    ) -> Result<Option<Self>, jsg::Error> {
        if !value.is_string() {
            return Ok(None);
        }
        Self::from_js(lock, value).map(Some)
    }
}

/// A union that has no `ToJS`, since `FromJsOnly` has none.
#[jsg_oneof]
#[derive(Debug, Clone, PartialEq)]
enum FromJsOnlyOrNumber {
    Text(FromJsOnly),
    Number(f64),
}

#[jsg_resource]
struct OneOfContext;

#[jsg_resource]
impl OneOfContext {
    #[jsg_method]
    pub fn take_one_of(&self, value: NumberStringOrBox) -> String {
        match value {
            NumberStringOrBox::Number(n) => format!("number:{n}"),
            NumberStringOrBox::String(s) => format!("string:{s}"),
            NumberStringOrBox::Box(b) => format!("box:{}", b.value),
        }
    }

    #[jsg_method]
    pub fn return_one_of(&self, kind: String) -> NumberStringOrBox {
        match kind.as_str() {
            "number" => NumberStringOrBox::Number(123.0),
            "string" => NumberStringOrBox::String("foo".to_owned()),
            _ => NumberStringOrBox::Box(jsg::Rc::new(OneOfBox { value: 321.0 })),
        }
    }

    #[jsg_method]
    pub fn take_string_or_bool(&self, value: StringOrBool) -> String {
        describe_string_or_bool(&value)
    }

    #[jsg_method]
    pub fn take_number_or_bool(&self, value: NumberOrBool) -> String {
        match value {
            NumberOrBool::Number(n) => format!("number:{n}"),
            NumberOrBool::Bool(b) => format!("bool:{b}"),
        }
    }

    #[jsg_method]
    pub fn take_string_or_number(&self, value: StringOrNumber) -> String {
        match value {
            StringOrNumber::String(s) => format!("string:{s}"),
            StringOrNumber::Number(n) => format!("number:{n}"),
        }
    }

    #[jsg_method]
    pub fn take_string_or_number_ref(&self, value: &StringOrNumber) -> String {
        match value {
            StringOrNumber::String(s) => format!("ref_string:{s}"),
            StringOrNumber::Number(n) => format!("ref_number:{n}"),
        }
    }

    #[jsg_method]
    pub fn take_number_or_string(&self, value: NumberOrString) -> String {
        match value {
            NumberOrString::Number(n) => format!("number:{n}"),
            NumberOrString::String(s) => format!("string:{s}"),
        }
    }

    #[jsg_method]
    pub fn take_nested_one_of(&self, value: NumberOrNested) -> String {
        match value {
            NumberOrNested::Number(n) => format!("number:{n}"),
            NumberOrNested::Nested(nested) => describe_string_or_bool(&nested),
        }
    }

    #[jsg_method]
    pub fn take_string_or_bytes(&self, value: StringOrBytes) -> String {
        match value {
            StringOrBytes::String(s) => format!("string:{s}"),
            StringOrBytes::Bytes(b) => format!("bytes:{b:?}"),
        }
    }

    #[jsg_method]
    pub fn take_string_or_optional_point(&self, value: StringOrOptionalPoint) -> String {
        match value {
            StringOrOptionalPoint::String(s) => format!("string:{s}"),
            StringOrOptionalPoint::Point(p) => format!("point:{:?}", p.x),
        }
    }

    #[jsg_method]
    pub fn take_point_or_box(&self, value: PointOrBox) -> String {
        match value {
            PointOrBox::Point(p) => format!("point:{:?}", p.x),
            PointOrBox::Box(b) => format!("box:{}", b.value),
        }
    }

    #[jsg_method]
    pub fn take_from_js_only_or_number(&self, value: FromJsOnlyOrNumber) -> String {
        match value {
            FromJsOnlyOrNumber::Text(t) => format!("text:{}", t.0),
            FromJsOnlyOrNumber::Number(n) => format!("number:{n}"),
        }
    }

    #[jsg_method]
    pub fn take_point_or_bytes(&self, value: PointOrBytes) -> String {
        match value {
            PointOrBytes::Point(p) => format!("point:{}", p.x),
            PointOrBytes::Bytes(b) => format!("bytes:{b:?}"),
        }
    }

    #[jsg_method]
    pub fn take_strict_string_or_number(&self, value: StrictStringOrNumber) -> String {
        match value {
            StrictStringOrNumber::String(s) => format!("string:{}", s.into_inner()),
            StrictStringOrNumber::Number(n) => format!("number:{n}"),
        }
    }

    #[jsg_method]
    pub fn take_strict_bool_or_string(&self, value: StrictBoolOrString) -> String {
        match value {
            StrictBoolOrString::Bool(b) => format!("bool:{}", b.into_inner()),
            StrictBoolOrString::String(s) => format!("string:{}", s.into_inner()),
        }
    }

    #[jsg_method]
    pub fn take_string_or_strings(&self, value: StringOrStrings) -> String {
        match value {
            StringOrStrings::String(s) => format!("string:{s}"),
            StringOrStrings::Strings(v) => format!("strings:{v:?}"),
        }
    }

    #[jsg_method]
    pub fn take_maybe_strict_string(&self, value: Nullable<OnlyStrictString>) -> String {
        match value {
            Nullable::Some(OnlyStrictString::String(s)) => format!("string:{}", s.into_inner()),
            Nullable::Null => "null".to_owned(),
        }
    }

    #[jsg_method]
    pub fn take_points_or_bytes(&self, values: Vec<PointOrBytes>) -> String {
        format!("{}", values.len())
    }
}

fn describe_string_or_bool(value: &StringOrBool) -> String {
    match value {
        StringOrBool::String(s) => format!("string:{s}"),
        StringOrBool::Bool(b) => format!("bool:{b}"),
    }
}

/// Runs `f` with a `OneOfContext` installed as the global `cx` and a
/// `OneOfBox` holding 321 as the global `box321`.
fn with_context(f: impl for<'s, 'c> FnOnce(&mut jsg::Lock<'s>, &crate::EvalContext<'c>)) {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let cx = jsg::Rc::new(OneOfContext).to_js(lock)?;
        ctx.set_global(lock, "cx", cx);
        let boxed = jsg::Rc::new(OneOfBox { value: 321.0 }).to_js(lock)?;
        ctx.set_global(lock, "box321", boxed);
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
// value-test.c++ "OneOf"
// =============================================================================

#[test]
fn takes_one_of_number_string_or_resource() {
    with_context(|lock, ctx| {
        expect_eval(lock, ctx, "cx.takeOneOf(123)", "number:123");
        expect_eval(lock, ctx, "cx.takeOneOf('foo')", "string:foo");
        expect_eval(lock, ctx, "cx.takeOneOf(box321)", "box:321");
        expect_eval(lock, ctx, "cx.takeOneOf(undefined)", "string:undefined");
    });
}

#[test]
fn returns_one_of() {
    with_context(|lock, ctx| {
        expect_eval(lock, ctx, "typeof cx.returnOneOf('number')", "number");
        expect_eval(lock, ctx, "String(cx.returnOneOf('number'))", "123");
        expect_eval(lock, ctx, "typeof cx.returnOneOf('string')", "string");
        expect_eval(lock, ctx, "cx.returnOneOf('string')", "foo");
        expect_eval(
            lock,
            ctx,
            "String(Object.getPrototypeOf(cx.returnOneOf('box')) === \
             Object.getPrototypeOf(box321))",
            "true",
        );
        expect_eval(lock, ctx, "String(cx.returnOneOf('box').value)", "321");
    });
}

#[test]
fn string_or_bool_coerces_to_string() {
    with_context(|lock, ctx| {
        expect_eval(lock, ctx, "cx.takeStringOrBool(123)", "string:123");
        expect_eval(lock, ctx, "cx.takeStringOrBool('123')", "string:123");
        expect_eval(lock, ctx, "cx.takeStringOrBool(true)", "bool:true");
    });
}

#[test]
fn number_or_bool_coerces_to_number() {
    with_context(|lock, ctx| {
        expect_eval(lock, ctx, "cx.takeNumberOrBool(123)", "number:123");
        expect_eval(lock, ctx, "cx.takeNumberOrBool('123')", "number:123");
        expect_eval(lock, ctx, "cx.takeNumberOrBool(true)", "bool:true");
    });
}

#[test]
fn string_or_number_coerces_boolean_to_string() {
    with_context(|lock, ctx| {
        expect_eval(lock, ctx, "cx.takeStringOrNumber(123)", "number:123");
        expect_eval(lock, ctx, "cx.takeStringOrNumber('123')", "string:123");
        expect_eval(lock, ctx, "cx.takeStringOrNumber(true)", "string:true");
    });
}

#[test]
fn nested_one_of_is_flattened() {
    with_context(|lock, ctx| {
        expect_eval(lock, ctx, "cx.takeNestedOneOf(123)", "number:123");
        expect_eval(lock, ctx, "cx.takeNestedOneOf('123')", "string:123");
        expect_eval(lock, ctx, "cx.takeNestedOneOf(true)", "bool:true");
        expect_eval(
            lock,
            ctx,
            "cx.takeNestedOneOf(undefined)",
            "string:undefined",
        );
        expect_eval(lock, ctx, "cx.takeNestedOneOf(null)", "string:null");
        expect_eval(
            lock,
            ctx,
            "cx.takeNestedOneOf({})",
            "string:[object Object]",
        );
    });
}

// =============================================================================
// Unions in other value-test.c++ tests
// =============================================================================

// "NonCoercible Types".
#[test]
fn non_coercible_variants_take_only_their_own_type() {
    with_context(|lock, ctx| {
        expect_eval(lock, ctx, "cx.takeStrictBoolOrString(false)", "bool:false");
        expect_eval(lock, ctx, "cx.takeStrictBoolOrString('')", "string:");
        assert_eq!(
            expect_type_error(lock, ctx, "cx.takeStrictBoolOrString(new String(''))"),
            "Failed to execute 'takeStrictBoolOrString' on 'OneOfContext': parameter 1 is not \
             of type 'boolean or string'."
        );
    });
}

// "Sequence of types": a string is iterable, but goes to the string variant.
#[test]
fn string_is_not_taken_as_a_sequence() {
    with_context(|lock, ctx| {
        expect_eval(lock, ctx, "cx.takeStringOrStrings('aa')", "string:aa");
        expect_eval(
            lock,
            ctx,
            "cx.takeStringOrStrings(['b', 'b'])",
            r#"strings:["b", "b"]"#,
        );
    });
}

// "kj::Maybe noSubstituteNull", whose union C++ tests with `noSubstituteNull`, as
// Rust JSG always converts `Nullable`.
#[test]
fn nullable_union_rejects_a_value_no_variant_takes() {
    with_context(|lock, ctx| {
        assert_eq!(
            expect_type_error(lock, ctx, "cx.takeMaybeStrictString({})"),
            "Failed to execute 'takeMaybeStrictString' on 'OneOfContext': parameter 1 is not \
             of type 'string'."
        );
        expect_eval(lock, ctx, "cx.takeMaybeStrictString('a')", "string:a");
        expect_eval(lock, ctx, "cx.takeMaybeStrictString(null)", "null");
    });
}

// =============================================================================
// Pass order
// =============================================================================

#[test]
fn pass_order_beats_declaration_order() {
    with_context(|lock, ctx| {
        expect_eval(lock, ctx, "cx.takeStringOrNumber('42')", "string:42");
        expect_eval(lock, ctx, "cx.takeStringOrNumber(42)", "number:42");
        expect_eval(lock, ctx, "cx.takeNumberOrString('42')", "string:42");
        expect_eval(lock, ctx, "cx.takeNumberOrString(42)", "number:42");
    });
}

#[test]
fn resource_is_tried_before_dictionary() {
    with_context(|lock, ctx| {
        // A dictionary would take any object, the resource included.
        expect_eval(lock, ctx, "cx.takePointOrBox(box321)", "box:321");
        expect_eval(lock, ctx, "cx.takePointOrBox({ x: 1 })", "point:Some(1)");
    });
}

#[test]
fn sequence_is_tried_before_string_coercion() {
    with_context(|lock, ctx| {
        expect_eval(lock, ctx, "cx.takeStringOrBytes([1, 2])", "bytes:[1, 2]");
        expect_eval(lock, ctx, "cx.takeStringOrBytes(5)", "string:5");
    });
}

#[test]
fn dictionary_is_tried_before_string_coercion() {
    with_context(|lock, ctx| {
        expect_eval(
            lock,
            ctx,
            "cx.takeStringOrOptionalPoint({ x: 1 })",
            "point:Some(1)",
        );
        // Like a C++ dictionary, null and undefined are an empty dictionary.
        expect_eval(
            lock,
            ctx,
            "cx.takeStringOrOptionalPoint(null)",
            "point:None",
        );
        expect_eval(
            lock,
            ctx,
            "cx.takeStringOrOptionalPoint(undefined)",
            "point:None",
        );
        expect_eval(lock, ctx, "cx.takeStringOrOptionalPoint('a')", "string:a");
    });
}

#[test]
fn non_coercible_string_leaves_coercion_to_number() {
    with_context(|lock, ctx| {
        expect_eval(lock, ctx, "cx.takeStrictStringOrNumber('a')", "string:a");
        expect_eval(lock, ctx, "cx.takeStrictStringOrNumber(5)", "number:5");
        expect_eval(lock, ctx, "cx.takeStrictStringOrNumber(true)", "number:1");
    });
}

#[test]
fn big_int_goes_to_a_numeric_variant_first() {
    with_context(|lock, ctx| {
        // A number can't hold a BigInt, so the conversion throws rather than
        // falling through to the string variant.
        let message = expect_type_error(lock, ctx, "cx.takeStringOrNumber(1n)");
        assert!(message.contains("BigInt"), "{message}");
        // Without a numeric variant, a BigInt becomes a string.
        expect_eval(lock, ctx, "cx.takeStringOrBool(1n)", "string:1");
    });
}

#[test]
fn conversion_errors_are_kept() {
    with_context(|lock, ctx| {
        let message = expect_type_error(lock, ctx, "cx.takeStringOrNumber(Symbol())");
        assert!(message.contains("Symbol"), "{message}");
    });
}

#[test]
fn exception_from_coercion_stops_the_conversion() {
    with_context(|lock, ctx| {
        // The string pass throws, so the number pass never calls valueOf().
        let err = ctx
            .eval::<String>(
                lock,
                "globalThis.valueOfCalled = false; cx.takeStringOrNumber({ \
                   toString() { throw new RangeError('from toString'); }, \
                   valueOf() { globalThis.valueOfCalled = true; return 1; } })",
            )
            .unwrap_err()
            .unwrap_jsg_err(lock);
        assert_eq!(err.name, ExceptionType::RangeError);
        assert_eq!(err.message, "from toString");
        expect_eval(lock, ctx, "String(globalThis.valueOfCalled)", "false");
    });
}

#[test]
fn value_no_variant_takes_is_a_type_error() {
    with_context(|lock, ctx| {
        assert_eq!(
            expect_type_error(lock, ctx, "cx.takePointOrBytes(5)"),
            "Failed to execute 'takePointOrBytes' on 'OneOfContext': parameter 1 is not of \
             type 'Point or Array'."
        );
    });
}

#[test]
fn nested_value_no_variant_takes_names_the_union() {
    with_context(|lock, ctx| {
        let message = expect_type_error(lock, ctx, "cx.takePointsOrBytes([5])");
        assert!(
            message.contains("Expected Point or Array but got number"),
            "{message}"
        );
    });
}

// =============================================================================
// Rust-side behavior
// =============================================================================

static TRACED_BOX_DROPS: AtomicUsize = AtomicUsize::new(0);

#[jsg_resource]
struct TracedBox;

impl Drop for TracedBox {
    fn drop(&mut self) {
        TRACED_BOX_DROPS.fetch_add(1, Ordering::SeqCst);
    }
}

#[jsg_resource]
impl TracedBox {}

#[jsg_oneof]
#[derive(Clone)]
enum TracedBoxOrNumber {
    Box(jsg::Rc<TracedBox>),
    Number(f64),
}

#[jsg_resource]
struct HoldsUnion {
    pub held: TracedBoxOrNumber,
}

#[jsg_resource]
impl HoldsUnion {}

// The union traces its resource variant, so a resource holding the union keeps the
// variant's resource alive, and both are collected together.
#[test]
fn union_field_traces_its_resource_variant() {
    TRACED_BOX_DROPS.store(0, Ordering::SeqCst);
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let held = jsg::Rc::new(TracedBox);
        let parent = jsg::Rc::new(HoldsUnion {
            held: TracedBoxOrNumber::Box(held.clone()),
        });
        // Give the inner box a wrapper too, so that only tracing keeps it alive.
        let _wrapper = held.clone().to_js(lock)?;
        let wrapped = parent.clone().to_js(lock)?;
        ctx.set_global(lock, "parent", wrapped);
        std::mem::drop(held);
        std::mem::drop(parent);

        crate::Harness::request_gc(lock);
        assert_eq!(TRACED_BOX_DROPS.load(Ordering::SeqCst), 0);
        Ok(())
    });

    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(TRACED_BOX_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
}

#[test]
fn union_holds_gc_references_if_a_variant_does() {
    use jsg::Traced;
    assert!(TracedBoxOrNumber::holds_gc_references());
    assert!(!StringOrNumber::holds_gc_references());
}

#[test]
fn union_without_to_js_converts_from_js() {
    with_context(|lock, ctx| {
        expect_eval(lock, ctx, "cx.takeFromJsOnlyOrNumber('a')", "text:a");
        expect_eval(lock, ctx, "cx.takeFromJsOnlyOrNumber(5)", "number:5");
    });
}

#[test]
fn reference_parameter() {
    with_context(|lock, ctx| {
        expect_eval(
            lock,
            ctx,
            "cx.takeStringOrNumberRef('hello')",
            "ref_string:hello",
        );
        expect_eval(lock, ctx, "cx.takeStringOrNumberRef(42)", "ref_number:42");
    });
}

#[test]
fn derives_debug_and_clone() {
    let s = StringOrNumber::String("test".to_owned());
    let cloned = s.clone();
    assert_eq!(s, cloned);

    let debug_str = format!("{s:?}");
    assert!(debug_str.contains("String"));
    assert!(debug_str.contains("test"));
}
