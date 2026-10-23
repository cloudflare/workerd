// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

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
            nested_types_error(attr).starts_with("expected `nested_types(...)`"),
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
