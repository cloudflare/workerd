// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use quote::quote;

use super::*;

fn parse(item: TokenStream) -> syn::DeriveInput {
    syn::parse2(item).unwrap()
}

#[test]
fn mode_follows_the_flags() {
    assert_eq!(Mode::from_attr(quote! {}).unwrap(), Mode::None);
    assert_eq!(
        Mode::from_attr(quote! { name = "Foo", custom_trace }).unwrap(),
        Mode::None
    );
    assert_eq!(
        Mode::from_attr(quote! { memory_info }).unwrap(),
        Mode::Generated
    );
    assert_eq!(
        Mode::from_attr(quote! { custom_trace, custom_memory_info }).unwrap(),
        Mode::Custom
    );
}

#[test]
fn memory_info_and_custom_memory_info_are_exclusive() {
    let err = Mode::from_attr(quote! { memory_info, custom_memory_info }).unwrap_err();
    assert!(err.to_string().contains("use one"), "{err}");
}

#[test]
fn generated_impl_reports_every_field_but_skipped_ones() {
    let mut input = parse(quote! {
        struct Foo {
            name: String,
            #[jsg_memory_info(skip)]
            scratch: Vec<u8>,
            r#type: u32,
        }
    });
    let generated = generate(Mode::Generated, &mut input).unwrap();
    let visit = generated.visit_impl.to_string();
    assert!(
        visit.contains("impl jsg :: VisitForMemoryInfo for Foo"),
        "{visit}"
    );
    assert!(visit.contains(r#"c"name""#), "{visit}");
    // A raw identifier is reported under its name.
    assert!(visit.contains(r#"c"type""#), "{visit}");
    assert!(!visit.contains("scratch"), "{visit}");
    assert!(
        generated
            .gc_fn
            .to_string()
            .contains("visit_for_memory_info")
    );

    // The attribute, which the compiler does not know, is gone.
    let output = quote! { #input }.to_string();
    assert!(!output.contains("jsg_memory_info"), "{output}");
    assert!(output.contains("scratch"), "{output}");
}

#[test]
fn custom_mode_generates_no_impl() {
    let mut input = parse(quote! { struct Foo { name: String } });
    let generated = generate(Mode::Custom, &mut input).unwrap();
    assert!(generated.visit_impl.is_empty());
    assert!(
        generated
            .gc_fn
            .to_string()
            .contains("visit_for_memory_info")
    );
}

#[test]
fn no_mode_generates_nothing() {
    let mut input = parse(quote! { struct Foo { name: String } });
    let generated = generate(Mode::None, &mut input).unwrap();
    assert!(generated.visit_impl.is_empty());
    assert!(generated.gc_fn.is_empty());
}

#[test]
fn skip_needs_generated_memory_info() {
    for mode in [Mode::None, Mode::Custom] {
        let mut input = parse(quote! {
            struct Foo {
                #[jsg_memory_info(skip)]
                name: String,
            }
        });
        let err = generate(mode, &mut input).err().expect("rejected");
        assert!(err.to_string().contains("needs `memory_info`"), "{err}");
    }
}

#[test]
fn field_attribute_takes_only_skip() {
    for attr in [
        quote! { #[jsg_memory_info] },
        quote! { #[jsg_memory_info(skipp)] },
        quote! { #[jsg_memory_info(skip, name = "x")] },
    ] {
        let mut input = parse(quote! {
            struct Foo {
                #attr
                name: String,
            }
        });
        assert!(generate(Mode::Generated, &mut input).is_err(), "{attr}");
    }
}

#[test]
fn duplicate_skip_is_rejected() {
    let mut input = parse(quote! {
        struct Foo {
            #[jsg_memory_info(skip)]
            #[jsg_memory_info(skip)]
            name: String,
        }
    });
    let err = generate(Mode::Generated, &mut input)
        .err()
        .expect("rejected");
    assert!(err.to_string().contains("duplicate"), "{err}");
}
