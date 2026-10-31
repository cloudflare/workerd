// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use quote::quote;

use super::*;

/// `tokens` with the lifetime `'s` replaced, as a string.
fn replaced(tokens: proc_macro2::TokenStream) -> String {
    let name = proc_macro2::Ident::new("s", proc_macro2::Span::call_site());
    replace_lifetime(tokens, &name).to_string()
}

#[test]
fn replace_lifetime_replaces_only_the_named_lifetime() {
    assert_eq!(
        replaced(quote! { jsg::v8::Local<'s, jsg::v8::Value> }),
        quote! { jsg::v8::Local<'static, jsg::v8::Value> }.to_string()
    );
    assert_eq!(
        replaced(quote! { Vec<(&'a str, Local<'s, Value>, Nested<'s>)> }),
        quote! { Vec<(&'a str, Local<'static, Value>, Nested<'static>)> }.to_string()
    );
    assert_eq!(replaced(quote! { s }), quote! { s }.to_string());
    assert_eq!(replaced(quote! { 'x' }), quote! { 'x' }.to_string());
}

/// `ty`, parsed, without the `Nullable<…>`s around it, as a string.
fn stripped(ty: &str) -> String {
    let ty: syn::Type = syn::parse_str(ty).unwrap();
    strip_nullable(&ty).to_token_stream().to_string()
}

#[test]
fn strip_nullable_strips_every_nullable_as_written() {
    assert_eq!(stripped("Nullable<String>"), "String");
    assert_eq!(
        stripped("jsg::Nullable<jsg::Rc<Thing>>"),
        "jsg :: Rc < Thing >"
    );
    assert_eq!(stripped("Nullable<Nullable<u32>>"), "u32");
    assert_eq!(stripped("Vec<Nullable<u32>>"), "Vec < Nullable < u32 > >");
    assert_eq!(stripped("Option<String>"), "Option < String >");
}
