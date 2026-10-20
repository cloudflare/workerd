// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Tests for compatibility-dependent resource template shape and constructor call checks.

use std::cell::RefCell;

use jsg::Member;
use jsg::Number;
use jsg::PropertyPlacement;
use jsg::Resource;
use jsg::feature_flags::compatibility_flags;
use jsg_macros::jsg_constructor;
use jsg_macros::jsg_inspect_property;
use jsg_macros::jsg_method;
use jsg_macros::jsg_property;
use jsg_macros::jsg_resource;
use jsg_macros::jsg_static_constant;

use crate::Harness;

type OptionalNumber = Option<Number>;

struct OptionalValue;

impl<'s> jsg::FromJS<'s> for OptionalValue {
    type ResultType = Self;

    fn from_js(
        _lock: &mut jsg::Lock<'s>,
        _value: jsg::v8::Local<'s, jsg::v8::Value>,
    ) -> jsg::Result<Self> {
        Ok(Self)
    }

    fn is_optional() -> bool {
        true
    }
}

#[jsg_resource]
struct Sample {
    label: RefCell<String>,
}

#[jsg_resource(custom_members)]
impl Sample {
    #[jsg_static_constant]
    const VALUE: f64 = 1.0;

    #[jsg_constructor]
    fn constructor(this: String, _extra: OptionalNumber) -> Self {
        Self {
            label: RefCell::new(this),
        }
    }

    #[jsg_method]
    fn two(&self, a: Number, b: Number, _c: OptionalNumber) -> Number {
        Number::new(a.value() + b.value())
    }

    #[jsg_method]
    fn with_lock(&self, _lock: &mut jsg::Lock, a: Number) -> Number {
        a
    }

    #[jsg_method]
    fn add(a: Number, b: Number) -> Number {
        Number::new(a.value() + b.value())
    }

    #[jsg_method]
    fn custom_optional<'s>(
        &self,
        _value: jsg::v8::Local<'s, jsg::v8::Value>,
        _optional: OptionalValue,
        _after: jsg::Lenient<Number>,
    ) -> bool {
        true
    }

    #[jsg_property]
    pub fn get_label(&self) -> String {
        self.label.borrow().clone()
    }

    #[jsg_property]
    pub fn set_label(&self, value: String) -> Result<(), jsg::Error> {
        if value == "reject" {
            return Err(jsg::Error::new_type_error("invalid label"));
        }
        *self.label.borrow_mut() = value;
        Ok(())
    }

    #[jsg_property(readonly)]
    pub fn get_kind(&self) -> String {
        "sample".to_owned()
    }

    #[jsg_property(name = "cacheMode", readonly)]
    pub fn get_cache(&self) -> String {
        "no-store".to_owned()
    }

    #[jsg_method(name = "format")]
    pub fn format_label(&self, prefix: String) -> String {
        format!("{prefix}{}", self.label.borrow())
    }

    #[jsg_method]
    pub fn describe(prefix: String) -> String {
        format!("{prefix}sample")
    }

    #[jsg_inspect_property(name = "debugLabel")]
    pub fn debug_label(&self) -> String {
        self.label.borrow().clone()
    }

    #[jsg_static_constant(name = "EXTRA")]
    const EXTRA_VALUE: f64 = 2.0;
}

impl Resource for Sample {
    fn members(flags: compatibility_flags::Reader<'_>) -> Vec<Member> {
        let placement = if flags.get_jsg_property_on_prototype_template() {
            PropertyPlacement::Prototype
        } else {
            PropertyPlacement::Instance
        };
        let mut members = vec![
            Self::constructor_member(),
            Self::two_member(),
            Self::with_lock_member(),
            Self::add_member(),
            Self::custom_optional_member(),
            Self::value_member(),
            Self::get_kind_member(placement),
            Self::get_label_member(placement),
        ];
        if flags.get_cache_option_enabled() && flags.get_jsg_property_on_prototype_template() {
            members.extend([
                Self::get_cache_member(placement),
                Self::format_label_member(),
                Self::describe_member(),
                Self::debug_label_member(),
                Self::extra_value_member(),
            ]);
        }
        members
    }
}

fn eval_string(harness: &Harness, code: &str) -> String {
    let mut out = String::new();
    harness.run_in_context(|lock, ctx| {
        let constructor = jsg::resource::function_template_of::<Sample>(lock);
        ctx.set_global(lock, "Sample", constructor);
        out = ctx.eval(lock, code).unwrap();
        Ok(())
    });
    out
}

#[test]
fn to_string_tag_follows_set_to_string_tag() {
    let code = "Object.prototype.toString.call(new Sample('x'))";
    assert_eq!(eval_string(&Harness::new(), code), "[object Object]");

    let harness = Harness::with_flags(|mut flags| flags.set_set_to_string_tag(true));
    assert_eq!(eval_string(&harness, code), "[object Sample]");
}

#[test]
fn prototype_is_read_only_with_set_immutable_prototype() {
    let code = r"
        const original = Sample.prototype;
        const { writable } = Object.getOwnPropertyDescriptor(Sample, 'prototype');
        Sample.prototype = {};
        JSON.stringify({ writable, unchanged: Sample.prototype === original });
    ";

    assert_eq!(
        eval_string(&Harness::new(), code),
        r#"{"writable":true,"unchanged":false}"#
    );

    {
        let harness =
            Harness::with_flags(|mut flags| flags.set_should_set_immutable_prototype(true));
        assert_eq!(
            eval_string(&harness, code),
            r#"{"writable":false,"unchanged":true}"#
        );
    }

    let harness = Harness::with_flags(|mut flags| flags.set_python_workers(true));
    assert_eq!(
        eval_string(&harness, code),
        r#"{"writable":false,"unchanged":true}"#
    );
}

#[test]
fn constant_descriptor_follows_spec_compliant_property_attributes() {
    let code = "JSON.stringify(Object.getOwnPropertyDescriptor(Sample.prototype, 'VALUE'))";
    assert_eq!(
        eval_string(&Harness::new(), code),
        r#"{"value":1,"writable":false,"enumerable":true,"configurable":true}"#
    );

    let harness =
        Harness::with_flags(|mut flags| flags.set_spec_compliant_property_attributes(true));
    assert_eq!(
        eval_string(&harness, code),
        r#"{"value":1,"writable":false,"enumerable":true,"configurable":false}"#
    );
}

#[test]
fn constructor_called_without_new_throws_type_error() {
    let harness = Harness::new();
    harness.run_in_context(|lock, ctx| {
        let constructor = jsg::resource::function_template_of::<Sample>(lock);
        ctx.set_global(lock, "Sample", constructor);

        for code in ["Sample('x')", "Sample.call({}, 'x')"] {
            let err = ctx
                .eval::<Number>(lock, code)
                .unwrap_err()
                .unwrap_jsg_err(lock);
            assert_eq!(err.name, jsg::ExceptionType::TypeError, "{code}");
            assert_eq!(
                err.message,
                "Failed to construct 'Sample': Please use the 'new' operator, this object \
                 constructor cannot be called as a function.",
                "{code}"
            );
        }

        // A receiver passed to a failed call is left unwrapped.
        let result: String = ctx
            .eval(
                lock,
                "const receiver = {}; try { Sample.call(receiver, 'x'); } catch {} \
                 String(Object.getOwnPropertyNames(receiver).length)",
            )
            .unwrap();
        assert_eq!(result, "0");
        Ok(())
    });
}

#[test]
fn custom_property_is_on_instance_when_flag_is_unset() {
    let placement = eval_string(
        &Harness::new(),
        "const s = new Sample('x'); \
         [Object.hasOwn(Sample.prototype, 'label'), Object.hasOwn(s, 'label'), s.label].join()",
    );
    assert_eq!(placement, "false,true,x");
}

#[test]
fn custom_property_is_on_prototype_when_flag_is_set() {
    let harness =
        Harness::with_flags(|mut flags| flags.set_jsg_property_on_prototype_template(true));
    let placement = eval_string(
        &harness,
        "const s = new Sample('x'); \
         [Object.hasOwn(Sample.prototype, 'label'), Object.hasOwn(s, 'label'), s.label].join()",
    );
    assert_eq!(placement, "true,false,x");
}

#[test]
fn custom_members_control_presence_with_compound_conditions() {
    for prototype in [false, true] {
        for cache in [false, true] {
            let harness = Harness::with_flags(|mut flags| {
                flags.set_jsg_property_on_prototype_template(prototype);
                flags.set_cache_option_enabled(cache);
            });
            let surface = eval_string(
                &harness,
                r"
                    const s = new Sample('x');
                    const inspectKey = Object.getOwnPropertySymbols(Sample.prototype)
                        .find(key => key.description === 'kResourceTypeInspect');
                    const inspect = Sample.prototype[inspectKey];
                    JSON.stringify([
                        'cacheMode' in s, 'format' in s, 'describe' in Sample,
                        'debugLabel' in inspect, 'EXTRA' in Sample, 'EXTRA' in s
                    ])
                ",
            );
            if prototype && cache {
                assert_eq!(surface, "[true,true,true,true,true,true]");
                assert_eq!(
                    eval_string(
                        &harness,
                        r"
                            const s = new Sample('x');
                            const inspectKey = Object.getOwnPropertySymbols(Sample.prototype)
                                .find(key => key.description === 'kResourceTypeInspect');
                            const inspect = Sample.prototype[inspectKey];
                            [s.cacheMode, s.format('hello '), Sample.describe('a '),
                             s[inspect.debugLabel], Sample.EXTRA, s.EXTRA].join()
                        "
                    ),
                    "no-store,hello x,a sample,x,2,2"
                );
            } else {
                assert_eq!(surface, "[false,false,false,false,false,false]");
            }
        }
    }
}

#[test]
fn custom_property_setters_convert_arguments_and_propagate_errors() {
    for prototype in [false, true] {
        let harness = Harness::with_flags(|mut flags| {
            flags.set_jsg_property_on_prototype_template(prototype);
        });
        assert_eq!(
            eval_string(
                &harness,
                r"
                    const s = new Sample('x');
                    s.label = 42;
                    let error;
                    try { s.label = 'reject'; } catch (e) { error = e.toString(); }
                    [s.label, error].join()
                "
            ),
            "42,TypeError: invalid label"
        );
    }
}

#[test]
fn custom_member_order_controls_instance_key_order() {
    // V8 enumerates native data properties in reverse registration order, as in C++ JSG.
    assert_eq!(
        eval_string(&Harness::new(), "Object.keys(new Sample('x')).join()"),
        "label,kind"
    );
}

#[test]
fn custom_templates_are_cached_per_isolate_for_both_wrapping_paths() {
    let old = Harness::new();
    let new = Harness::with_flags(|mut flags| {
        flags.set_jsg_property_on_prototype_template(true);
    });
    for harness in [&old, &new, &old, &new] {
        harness.run_in_context(|lock, ctx| {
            // Wrap a Rust-created resource before exposing its constructor.
            ctx.set_global(
                lock,
                "wrapped",
                jsg::Rc::new(Sample::constructor("x".to_owned(), None)),
            );
            let constructor = jsg::resource::function_template_of::<Sample>(lock);
            ctx.set_global(lock, "Sample", constructor);
            let prototype = lock
                .feature_flags()
                .get_jsg_property_on_prototype_template();
            let code = if prototype {
                "!Object.hasOwn(wrapped, 'label') && !Object.hasOwn(new Sample('y'), 'label') && \
                 Object.getPrototypeOf(wrapped) === Sample.prototype"
            } else {
                "Object.hasOwn(wrapped, 'label') && Object.hasOwn(new Sample('y'), 'label') && \
                 Object.getPrototypeOf(wrapped) === Sample.prototype"
            };
            let correct: bool = ctx.eval(lock, code).unwrap();
            assert!(correct);
            Ok(())
        });
    }
}

#[test]
fn constructor_length_counts_argument_named_this() {
    for (enabled, expected) in [(false, "0,value"), (true, "1,value")] {
        let harness = Harness::with_flags(|mut flags| {
            flags.set_spec_compliant_property_attributes(enabled);
        });
        assert_eq!(
            eval_string(
                &harness,
                "const s = new Sample('value'); [Sample.length, s.label].join()",
            ),
            expected,
        );
    }
}

#[test]
fn function_lengths_are_zero_without_spec_compliant_property_attributes() {
    let lengths = eval_string(
        &Harness::new(),
        "[Sample.length, Sample.prototype.two.length, Sample.prototype.withLock.length, \
         Sample.add.length, Sample.prototype.customOptional.length].join()",
    );
    assert_eq!(lengths, "0,0,0,0,0");
}

/// Web IDL `.length` counts leading required arguments using `FromJS`, including aliases
/// and custom optional types. A `&mut Lock` is not a JavaScript argument.
#[test]
fn function_lengths_follow_web_idl_with_spec_compliant_property_attributes() {
    let harness =
        Harness::with_flags(|mut flags| flags.set_spec_compliant_property_attributes(true));
    let lengths = eval_string(
        &harness,
        "[Sample.length, Sample.prototype.two.length, Sample.prototype.withLock.length, \
         Sample.add.length, Sample.prototype.customOptional.length].join()",
    );
    assert_eq!(lengths, "1,2,1,2,1");
    assert_eq!(
        eval_string(
            &harness,
            "const s = new Sample('x'); [s.two(1, 2), s.customOptional('x')].join()",
        ),
        "3,true",
    );
}
