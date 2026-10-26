// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

struct Node;

impl TypeInfo for Node {
    fn describe(cx: &mut Context) -> Type {
        cx.structure::<Self>("Node", true, |cx| {
            vec![Member::Property {
                name: "next",
                ty: Type::Maybe {
                    value: Box::new(Self::describe(cx)),
                    allows_null: false,
                    allows_undefined: true,
                },
                readonly: true,
                prototype: true,
                lazy: false,
            }]
        })
    }
}

struct FirstModule;
struct SecondModule;

fn describe_module<T: 'static>(cx: &mut Context, name: &'static str) -> Type {
    cx.structure::<T>(name, false, |cx| {
        vec![Member::Property {
            name: "node",
            ty: Node::describe(cx),
            readonly: true,
            prototype: false,
            lazy: false,
        }]
    })
}

impl TypeInfo for FirstModule {
    fn describe(cx: &mut Context) -> Type {
        describe_module::<Self>(cx, "FirstModule")
    }
}

impl TypeInfo for SecondModule {
    fn describe(cx: &mut Context) -> Type {
        describe_module::<Self>(cx, "SecondModule")
    }
}

fn read_context(cx: &Context) -> capnp::message::Reader<capnp::serialize::OwnedSegments> {
    let bytes = cx.encode("test");
    capnp::serialize::read_message(&mut bytes.as_slice(), capnp::message::ReaderOptions::new())
        .unwrap()
}

fn property(member: rtti_capnp::member::Reader<'_>) -> rtti_capnp::property::Reader<'_> {
    match member.which().unwrap() {
        rtti_capnp::member::Property(value) => value.unwrap(),
        _ => panic!("expected a property"),
    }
}

fn structure_reference(
    ty: rtti_capnp::type_::Reader<'_>,
) -> rtti_capnp::structure_type::Reader<'_> {
    match ty.which().unwrap() {
        rtti_capnp::type_::Structure(value) => value.unwrap(),
        _ => panic!("expected a structure reference"),
    }
}

fn module_identity(module: rtti_capnp::module::Reader<'_>) -> capnp::text::Reader<'_> {
    match module.which().unwrap() {
        rtti_capnp::module::StructureName(value) => value.unwrap(),
        rtti_capnp::module::TsDeclarations(_) => panic!("expected a module structure name"),
    }
}

#[test]
fn recursive_types_are_interned_before_visiting_members() {
    let mut cx = Context::default();
    Node::describe(&mut cx);
    Node::describe(&mut cx);
    let message = read_context(&cx);
    let root = message
        .get_root::<rtti_capnp::structure_groups::Reader>()
        .unwrap();
    let structures = root.get_groups().unwrap().get(0).get_structures().unwrap();
    assert_eq!(structures.len(), 1);
    let node = structures.get(0);
    assert!(node.get_ts_root());
    assert_eq!(node.get_ts_name().unwrap(), "Node");
    let next = property(node.get_members().unwrap().get(0));
    assert_eq!(next.get_name().unwrap(), "next");
    assert!(next.get_readonly());
    assert!(next.get_prototype());
    let rtti_capnp::type_::Maybe(maybe) = next.get_type().unwrap().which().unwrap() else {
        panic!("expected an optional type");
    };
    let maybe = maybe.unwrap();
    assert!(!maybe.get_allows_null());
    assert!(maybe.get_allows_undefined());
    let reference = structure_reference(maybe.get_value().unwrap());
    assert_eq!(
        reference.get_fully_qualified_name().unwrap(),
        type_name::<Node>()
    );
}

#[test]
fn module_roots_share_the_encoded_type_graph() {
    let mut cx = Context::default();
    cx.add_module::<FirstModule>("test:first");
    cx.add_module::<SecondModule>("test:second");
    let message = read_context(&cx);
    let root = message
        .get_root::<rtti_capnp::structure_groups::Reader>()
        .unwrap();
    let structures = root.get_groups().unwrap().get(0).get_structures().unwrap();
    assert_eq!(structures.len(), 3);
    assert_eq!(
        structures
            .iter()
            .filter(|s| s.get_ts_name().unwrap() == "Node")
            .count(),
        1
    );
    for module in [structures.get(0), structures.get(2)] {
        assert!(!module.get_ts_root());
        let property = property(module.get_members().unwrap().get(0));
        let reference = structure_reference(property.get_type().unwrap());
        assert_eq!(
            reference.get_fully_qualified_name().unwrap(),
            type_name::<Node>()
        );
    }
    let modules = root.get_modules().unwrap();
    assert_eq!(modules.len(), 2);
    assert_eq!(modules.get(0).get_specifier().unwrap(), "test:first");
    assert_eq!(module_identity(modules.get(0)), type_name::<FirstModule>());
    assert_eq!(modules.get(1).get_specifier().unwrap(), "test:second");
    assert_eq!(module_identity(modules.get(1)), type_name::<SecondModule>());
}

struct Flagged;

impl crate::Type for Flagged {
    fn class_name() -> &'static str {
        "Flagged"
    }

    fn is_exact(_value: v8::Local<'_, v8::Value>) -> bool {
        false
    }
}

impl crate::Traced for Flagged {}

impl crate::GarbageCollected for Flagged {
    fn memory_name(&self) -> &'static std::ffi::CStr {
        c"Flagged"
    }
}

impl Resource for Flagged {
    fn members(registry: &mut impl MemberRegistry, flags: compatibility_flags::Reader<'_>) {
        let prototype = flags.get_jsg_property_on_prototype_template();
        registry.add(crate::described_member(
            || panic!("RTTI export built a member"),
            |_| Member::Method {
                name: "always",
                args: Vec::new(),
                result: Type::Void,
                is_static: false,
            },
        ));
        // A hand-built member has no description.
        registry.add(crate::Member::StaticConstant {
            name: "HIDDEN".to_owned(),
            value: crate::ConstantValue::Number(1.0),
        });
        if flags.get_cache_option_enabled() {
            registry.add(crate::described_member(
                || panic!("RTTI export built a member"),
                move |_| Member::Property {
                    name: "cache",
                    ty: Type::String,
                    readonly: true,
                    prototype,
                    lazy: false,
                },
            ));
        }
    }
}

impl TypeInfo for Flagged {
    fn describe(cx: &mut Context) -> Type {
        cx.structure::<Self>("Flagged", true, resource_members::<Self>)
    }
}

/// Describes `Flagged` and returns its members as `(name, prototype)`, where `prototype` is
/// `None` for anything but a property.
fn flagged_members(mut cx: Context) -> Vec<(String, Option<bool>)> {
    Flagged::describe(&mut cx);
    let message = read_context(&cx);
    let root = message
        .get_root::<rtti_capnp::structure_groups::Reader>()
        .unwrap();
    let structures = root.get_groups().unwrap().get(0).get_structures().unwrap();
    structures
        .get(0)
        .get_members()
        .unwrap()
        .iter()
        .map(|member| match member.which().unwrap() {
            rtti_capnp::member::Method(method) => (
                method.unwrap().get_name().unwrap().to_string().unwrap(),
                None,
            ),
            rtti_capnp::member::Property(property) => {
                let property = property.unwrap();
                (
                    property.get_name().unwrap().to_string().unwrap(),
                    Some(property.get_prototype()),
                )
            }
            _ => panic!("unexpected member"),
        })
        .collect()
}

#[test]
fn resource_members_follow_registration_under_the_context_flags() {
    assert_eq!(
        flagged_members(Context::default()),
        [("always".to_owned(), None)]
    );
    assert_eq!(
        flagged_members(Context::with_flags(|mut flags| {
            flags.set_cache_option_enabled(true);
        })),
        [
            ("always".to_owned(), None),
            ("cache".to_owned(), Some(false))
        ]
    );
    assert_eq!(
        flagged_members(Context::with_flags(|mut flags| {
            flags.set_cache_option_enabled(true);
            flags.set_jsg_property_on_prototype_template(true);
        })),
        [
            ("always".to_owned(), None),
            ("cache".to_owned(), Some(true))
        ]
    );
}

struct Outer;

impl crate::Type for Outer {
    fn class_name() -> &'static str {
        "Outer"
    }

    fn is_exact(_value: v8::Local<'_, v8::Value>) -> bool {
        false
    }
}

impl crate::Traced for Outer {}

impl crate::GarbageCollected for Outer {
    fn memory_name(&self) -> &'static std::ffi::CStr {
        c"Outer"
    }
}

impl Resource for Outer {
    fn members(registry: &mut impl MemberRegistry, _flags: compatibility_flags::Reader<'_>) {
        // A hand-built nested type is described through the nested type.
        registry.add(crate::Member::NestedType {
            name: "Renamed".to_owned(),
            nested_type: crate::NestedType::of::<Flagged>(),
        });
    }
}

impl TypeInfo for Outer {
    fn describe(cx: &mut Context) -> Type {
        cx.structure::<Self>("Outer", true, resource_members::<Self>)
    }
}

#[test]
fn nested_types_contain_their_whole_structure() {
    let mut cx = Context::default();
    Outer::describe(&mut cx);
    let message = read_context(&cx);
    let root = message
        .get_root::<rtti_capnp::structure_groups::Reader>()
        .unwrap();
    let structures = root.get_groups().unwrap().get(0).get_structures().unwrap();
    assert_eq!(structures.len(), 2);
    assert_eq!(structures.get(1).get_ts_name().unwrap(), "Flagged");
    let rtti_capnp::member::Nested(nested) = structures
        .get(0)
        .get_members()
        .unwrap()
        .get(0)
        .which()
        .unwrap()
    else {
        panic!("expected a nested type");
    };
    assert_eq!(nested.get_name().unwrap(), "Renamed");
    let structure = nested.get_structure().unwrap();
    assert_eq!(
        structure.get_fully_qualified_name().unwrap(),
        type_name::<Flagged>()
    );
    let members = structure.get_members().unwrap();
    assert_eq!(members.len(), 1);
    let rtti_capnp::member::Method(method) = members.get(0).which().unwrap() else {
        panic!("expected a method");
    };
    assert_eq!(method.unwrap().get_name().unwrap(), "always");
}

fn describe_named_after<T: 'static>(cx: &mut Context, _value: &T) {
    cx.structure::<T>("Closure", false, |_| Vec::new());
}

#[test]
#[should_panic(expected = "are both named")]
fn types_with_one_type_name_are_rejected() {
    // Two closures in one function have the same `type_name`.
    let (first, second) = (|| {}, || {});
    assert_eq!(type_name_of(&first), type_name_of(&second));
    let mut cx = Context::default();
    describe_named_after(&mut cx, &first);
    describe_named_after(&mut cx, &second);
}

fn type_name_of<T>(_value: &T) -> &'static str {
    type_name::<T>()
}
