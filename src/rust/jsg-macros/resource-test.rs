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
