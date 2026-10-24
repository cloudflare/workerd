// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use quote::ToTokens;
use syn::parse_quote;

use super::*;

#[test]
fn validate_constructor_valid() {
    // A valid constructor: static (no self), returns Self.
    let method: syn::ImplItemFn = parse_quote! {
        fn constructor(name: String) -> Self { todo!() }
    };
    assert!(validate_constructor(&method).is_none());
}

#[test]
fn validate_constructor_rejects_self_receiver() {
    // Instance method — must not have &self.
    let method: syn::ImplItemFn = parse_quote! {
        fn constructor(&self) -> Self { todo!() }
    };
    assert!(validate_constructor(&method).is_some());
}

#[test]
fn validate_constructor_rejects_non_self_return() {
    // Returns String, not Self.
    let method: syn::ImplItemFn = parse_quote! {
        fn constructor() -> String { todo!() }
    };
    assert!(validate_constructor(&method).is_some());
}

#[test]
fn validate_constructor_accepts_result_of_self() {
    for method in [
        parse_quote! { fn constructor() -> Result<Self, jsg::Error> { todo!() } },
        parse_quote! { fn constructor() -> jsg::Result<Self> { todo!() } },
        parse_quote! { fn constructor() -> std::result::Result<Self, MyError> { todo!() } },
    ] {
        assert!(validate_constructor(&method).is_none());
        assert_eq!(constructor_return(&method), ConstructorReturn::Fallible);
    }
    let method: syn::ImplItemFn = parse_quote! { fn constructor() -> Self { todo!() } };
    assert_eq!(constructor_return(&method), ConstructorReturn::Infallible);
}

#[test]
fn validate_constructor_rejects_result_of_other_type() {
    for method in [
        parse_quote! { fn constructor() -> Result<String, jsg::Error> { todo!() } },
        parse_quote! { fn constructor() -> Option<Self> { todo!() } },
        parse_quote! { fn constructor() -> Result { todo!() } },
        parse_quote! { fn constructor() {} },
    ] {
        assert_eq!(constructor_return(&method), ConstructorReturn::Invalid);
        assert!(validate_constructor(&method).is_some());
    }
}

#[test]
fn extract_constructor_params_no_lock() {
    // Plain constructor — no Lock param, two JS args.
    let method: syn::ImplItemFn = parse_quote! {
        fn constructor(name: String, value: u32) -> Self { todo!() }
    };
    let (has_lock, unwraps, arg_exprs) = extract_constructor_params(&method);
    assert!(!has_lock);
    assert_eq!(unwraps.len(), 2);
    assert_eq!(arg_exprs.len(), 2);
}

#[test]
fn extract_constructor_params_with_lock() {
    // First param is `&mut jsg::Lock` — skipped from JS args.
    let method: syn::ImplItemFn = parse_quote! {
        fn constructor(lock: &mut jsg::Lock, name: String) -> Self { todo!() }
    };
    let (has_lock, unwraps, arg_exprs) = extract_constructor_params(&method);
    assert!(has_lock);
    // Only one JS arg (name); lock is not counted.
    assert_eq!(unwraps.len(), 1);
    assert_eq!(arg_exprs.len(), 1);
}

#[test]
fn extract_constructor_params_no_args() {
    let method: syn::ImplItemFn = parse_quote! {
        fn constructor() -> Self { todo!() }
    };
    let (has_lock, unwraps, arg_exprs) = extract_constructor_params(&method);
    assert!(!has_lock);
    assert!(unwraps.is_empty());
    assert!(arg_exprs.is_empty());
}

fn assert_validation_error_preserves_impl(
    attr: proc_macro2::TokenStream,
    input: &ItemImpl,
    message: &str,
) {
    let expanded = expand_resource_impl(attr, input);
    let file: syn::File = syn::parse2(expanded.clone()).unwrap();
    let Some(syn::Item::Impl(retained)) = file.items.first() else {
        panic!("validation error dropped the user's impl: {expanded}");
    };
    assert_eq!(quote!(#retained).to_string(), quote!(#input).to_string());
    assert!(file.items.iter().any(|item| {
        matches!(item, syn::Item::Macro(item) if item.mac.path.segments.last().unwrap().ident == "compile_error")
    }));
    assert!(expanded.to_string().contains(message), "{expanded}");
}

#[test]
fn invalid_registration_mode_preserves_impl() {
    assert_validation_error_preserves_impl(
        quote!(unknown),
        &parse_quote! {
            impl Sample {
                fn helper() -> Self { Self }
            }
        },
        "expected `custom_members`",
    );
}

#[test]
fn invalid_constructor_preserves_impl() {
    assert_validation_error_preserves_impl(
        quote!(),
        &parse_quote! {
            impl Sample {
                #[jsg_constructor]
                fn constructor() -> String { String::new() }
                fn helper() -> Self { Self }
            }
        },
        "must return Self",
    );
}

#[test]
fn invalid_properties_preserve_custom_impl() {
    assert_validation_error_preserves_impl(
        quote!(custom_members),
        &parse_quote! {
            impl Sample {
                #[jsg_property(readonly)]
                fn get_value(&self) -> u32 { 0 }
                #[jsg_property]
                fn set_value(&self, _value: u32) {}
            }
        },
        "read-only property cannot have a setter",
    );
    assert_validation_error_preserves_impl(
        quote!(custom_members),
        &parse_quote! {
            impl Sample {
                #[jsg_property]
                fn set_value(&self, _value: u32) {}
            }
        },
        "no getter found for property",
    );
}

#[test]
fn registration_modes_accept_default_custom_and_trailing_comma() {
    assert!(matches!(
        parse_registration_mode(quote!()).unwrap(),
        RegistrationMode::Automatic
    ));
    for attr in [quote!(custom_members), quote!(custom_members,)] {
        assert!(matches!(
            parse_registration_mode(attr).unwrap(),
            RegistrationMode::Custom
        ));
    }
}

#[test]
fn registration_modes_reject_duplicate_and_invalid_options() {
    for (attr, message) in [
        (
            quote!(custom_members, custom_members),
            "duplicate `custom_members`",
        ),
        (quote!(custom_member), "expected `custom_members`"),
        (quote!(custom_members = true), "expected `custom_members`"),
        (quote!(custom_members()), "expected `custom_members`"),
    ] {
        let error = parse_registration_mode(attr).err().unwrap();
        assert!(error.to_string().contains(message), "{error}");
    }
    assert!(parse_registration_mode(quote!(custom_members custom_members)).is_err());
}

#[test]
fn custom_registration_omits_resource_impl_but_retains_factories() {
    let input: ItemImpl = parse_quote! {
        impl Sample {
            #[jsg_method]
            fn foo(&self) {}
        }
    };
    let automatic: syn::File = syn::parse2(expand_resource_impl(quote!(), &input)).unwrap();
    let custom: syn::File =
        syn::parse2(expand_resource_impl(quote!(custom_members), &input)).unwrap();
    assert_eq!(automatic.items.len(), 3);
    assert_eq!(custom.items.len(), 2);
    assert_eq!(
        automatic.items[1].to_token_stream().to_string(),
        custom.items[1].to_token_stream().to_string()
    );
    let syn::Item::Impl(resource) = &automatic.items[2] else {
        panic!("missing automatic Resource implementation");
    };
    assert_eq!(
        resource
            .trait_
            .as_ref()
            .unwrap()
            .1
            .to_token_stream()
            .to_string(),
        quote!(jsg::Resource).to_string()
    );
}

#[test]
fn factory_collision_names_both_declarations_and_preserves_impl() {
    let input: ItemImpl = parse_quote! {
        impl Sample {
            #[jsg_static_constant]
            const FOO: u32 = 1;
            #[jsg_method]
            fn foo(&self) {}
        }
    };
    let mut registrations = collect_method_registrations(&input);
    registrations.extend(collect_constant_registrations(&input));
    let error = validate_factory_names(&input, &registrations).unwrap_err();
    let messages: Vec<_> = error.into_iter().map(|error| error.to_string()).collect();
    assert_eq!(
        messages,
        [
            "member factory `foo_member` generated by `FOO` conflicts with the factory generated by `foo`",
            "`foo` declared here",
        ]
    );
    for mode in [quote!(), quote!(custom_members)] {
        assert_validation_error_preserves_impl(mode, &input, &messages[0]);
    }
}

#[test]
fn factory_collision_with_user_associated_item_is_reported() {
    assert_validation_error_preserves_impl(
        quote!(),
        &parse_quote! {
            impl Sample {
                #[jsg_property]
                fn get_value(&self) -> u32 { 0 }
                fn get_value_member() {}
            }
        },
        "member factory `get_value_member` generated by `get_value` conflicts with associated item `get_value_member`",
    );
}

fn member_field<'a>(member: &'a syn::Expr, name: &str) -> &'a syn::Expr {
    let syn::Expr::Struct(member) = member else {
        panic!(
            "expected a Member constructor, got {}",
            member.to_token_stream()
        );
    };
    &member
        .fields
        .iter()
        .find(|field| matches!(&field.member, syn::Member::Named(ident) if ident == name))
        .unwrap()
        .expr
}

#[test]
fn constant_factory_lowercases_rust_name_without_changing_js_name() {
    let input: ItemImpl = parse_quote! {
        impl Sample {
            #[jsg_static_constant(name = "maxPayload")]
            const MAX_BYTES: u32 = 1024;
        }
    };
    let registrations = collect_constant_registrations(&input);
    let registration = &registrations[0];
    let factory: syn::ImplItemFn = syn::parse2(registration.factory.clone()).unwrap();
    assert_eq!(factory.sig.ident, "max_bytes_member");
    assert_eq!(registration.source_name, "MAX_BYTES");
    assert_eq!(
        registration.automatic.to_string(),
        quote!(Self::max_bytes_member()).to_string()
    );
    let syn::Stmt::Expr(member, None) = &factory.block.stmts[0] else {
        panic!("expected the factory to return a Member");
    };
    assert_eq!(
        member_field(member, "name").to_token_stream().to_string(),
        quote!("maxPayload".to_owned()).to_string()
    );
    assert_eq!(
        member_field(member, "value").to_token_stream().to_string(),
        quote!(jsg::ConstantValue::from(Self::MAX_BYTES)).to_string()
    );
}

#[test]
fn property_factories_select_matching_getter_and_setter_callback_shapes() {
    for (kind, automatic_placement) in [
        (
            PropertyKind::Prototype,
            quote!(jsg::PropertyPlacement::Prototype),
        ),
        (
            PropertyKind::Instance,
            quote!(jsg::PropertyPlacement::Instance),
        ),
    ] {
        let registration = emit_property_group(
            "value",
            kind,
            Some(parse_quote!(get_value)),
            Some(parse_quote!(set_value)),
        )
        .unwrap();
        assert_eq!(
            registration.automatic.to_string(),
            quote!(Self::get_value_member(#automatic_placement)).to_string()
        );
        let factory: syn::ImplItemFn = syn::parse2(registration.factory).unwrap();
        assert_eq!(factory.sig.inputs.len(), 1);
        let syn::Stmt::Expr(syn::Expr::Match(placement), None) = &factory.block.stmts[0] else {
            panic!("expected a placement-dependent factory");
        };
        assert_eq!(placement.arms.len(), 2);
        for (arm, pattern, member_type, getter, setter) in [
            (
                &placement.arms[0],
                quote!(jsg::PropertyPlacement::Prototype),
                quote!(jsg::Member::Property),
                quote!(Self::get_value_callback()),
                quote!(Some(Self::set_value_callback())),
            ),
            (
                &placement.arms[1],
                quote!(jsg::PropertyPlacement::Instance),
                quote!(jsg::Member::InstanceProperty),
                quote!(Self::get_value_instance_callback()),
                quote!(Some(Self::set_value_instance_callback())),
            ),
        ] {
            assert_eq!(arm.pat.to_token_stream().to_string(), pattern.to_string());
            let syn::Expr::Struct(member) = arm.body.as_ref() else {
                panic!("expected a Member constructor");
            };
            assert_eq!(
                member.path.to_token_stream().to_string(),
                member_type.to_string()
            );
            assert_eq!(
                member_field(&arm.body, "getter_callback")
                    .to_token_stream()
                    .to_string(),
                getter.to_string()
            );
            assert_eq!(
                member_field(&arm.body, "setter_callback")
                    .to_token_stream()
                    .to_string(),
                setter.to_string()
            );
        }
    }
}

#[test]
fn readonly_property_factories_omit_setters_for_both_placements() {
    let registration = emit_property_group(
        "value",
        PropertyKind::Prototype,
        Some(parse_quote!(get_value)),
        None,
    )
    .unwrap();
    let factory: syn::ImplItemFn = syn::parse2(registration.factory).unwrap();
    let syn::Stmt::Expr(syn::Expr::Match(placement), None) = &factory.block.stmts[0] else {
        panic!("expected a placement-dependent factory");
    };
    for arm in &placement.arms {
        assert_eq!(
            member_field(&arm.body, "setter_callback")
                .to_token_stream()
                .to_string(),
            quote!(None).to_string()
        );
    }
}

fn parse_registration_mode(attr: proc_macro2::TokenStream) -> syn::Result<RegistrationMode> {
    parse_impl_args(attr).map(|args| args.mode)
}

fn parse_nested_types(attr: proc_macro2::TokenStream) -> syn::Result<Vec<NestedTypeArg>> {
    parse_impl_args(attr).map(|args| args.nested_types)
}

fn nested_type_names(attr: proc_macro2::TokenStream) -> Vec<(Option<String>, String)> {
    parse_nested_types(attr)
        .unwrap()
        .iter()
        .map(|arg| {
            let ty = &arg.ty;
            (
                arg.name.as_ref().map(ToString::to_string),
                quote! { #ty }.to_string().replace(' ', ""),
            )
        })
        .collect()
}

fn nested_types_error(attr: proc_macro2::TokenStream) -> String {
    parse_nested_types(attr).err().unwrap().to_string()
}

#[test]
fn parse_nested_types_empty() {
    assert!(nested_type_names(quote! {}).is_empty());
    assert!(nested_type_names(quote! { nested_types() }).is_empty());
}

#[test]
fn parse_nested_types_types_and_names() {
    assert_eq!(
        nested_type_names(quote! { nested_types(Foo, Name = Bar, crate::a::Baz, Other = ::x::Y,) }),
        [
            (None, "Foo".to_owned()),
            (Some("Name".to_owned()), "Bar".to_owned()),
            (None, "crate::a::Baz".to_owned()),
            (Some("Other".to_owned()), "::x::Y".to_owned()),
        ]
    );
}

#[test]
fn parse_nested_types_repeated_list() {
    assert_eq!(
        nested_type_names(quote! { nested_types(Foo), nested_types(Bar) }),
        [(None, "Foo".to_owned()), (None, "Bar".to_owned())]
    );
}

#[test]
fn parse_nested_types_registers_unraw_name() {
    let args = parse_nested_types(quote! { nested_types(r#type = Foo) }).unwrap();
    let registration = nested_type_registration(&args[0]).to_string();
    assert!(registration.contains("\"type\""), "{registration}");
}

#[test]
fn parse_nested_types_rejects_other_arguments() {
    for attr in [
        quote! { name = "Foo" },
        quote! { custom_trace },
        quote! { nested(Foo) },
    ] {
        assert!(
            nested_types_error(attr).contains("`nested_types(...)`"),
            "unexpected error"
        );
    }
}

#[test]
fn parse_nested_types_rejects_duplicates() {
    assert_eq!(
        nested_types_error(quote! { nested_types(Foo, a::Bar, a::Bar) }),
        "duplicate nested type"
    );
    assert_eq!(
        nested_types_error(quote! { nested_types(Name = Foo, r#Name = Bar) }),
        "duplicate nested type name"
    );
}

#[test]
fn parse_nested_types_allows_type_under_several_names() {
    assert_eq!(
        nested_type_names(quote! { nested_types(Foo, Name = Foo) }).len(),
        2
    );
}

#[test]
fn reject_nested_types_on_struct_only_rejects_nested_types() {
    assert!(reject_nested_types_on_struct(quote! { name = "Foo", custom_trace }).is_none());
    assert!(reject_nested_types_on_struct(quote! {}).is_none());
    assert!(reject_nested_types_on_struct(quote! { nested_types(Foo) }).is_some());
}

#[test]
fn nested_types_and_custom_members_together_are_rejected() {
    for attr in [
        quote! { custom_members, nested_types(Foo) },
        quote! { nested_types(Foo), custom_members },
    ] {
        assert!(
            nested_types_error(attr).starts_with("`nested_types(...)` cannot be combined"),
            "unexpected error"
        );
    }
}

#[test]
fn nested_types_are_registered_after_members() {
    let input: ItemImpl = parse_quote! {
        impl Sample {
            #[jsg_method]
            fn foo(&self) {}
        }
    };
    let expanded = expand_resource_impl(quote! { nested_types(Bar) }, &input).to_string();
    let method = expanded.find("Self :: foo_member ()").unwrap();
    let nested = expanded.find("jsg :: NestedType :: of :: < Bar >").unwrap();
    assert!(method < nested, "{expanded}");
}
