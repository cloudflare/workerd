// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Shared utility helpers used across the jsg-macros crate.

use proc_macro::TokenStream;
use quote::ToTokens;
use syn::Data;
use syn::DeriveInput;
use syn::Fields;

/// Extracts named fields from a struct, returning an empty list for unit structs.
/// Returns `Err` with a compile error for tuple structs or non-struct data.
pub fn extract_named_fields(
    input: &DeriveInput,
    macro_name: &str,
) -> Result<syn::punctuated::Punctuated<syn::Field, syn::token::Comma>, TokenStream> {
    match &input.data {
        Data::Struct(data) => match &data.fields {
            Fields::Named(fields) => Ok(fields.named.clone()),
            Fields::Unit => Ok(syn::punctuated::Punctuated::new()),
            Fields::Unnamed(_) => Err(error(
                input,
                &format!("#[{macro_name}] does not support tuple structs"),
            )),
        },
        _ => Err(error(
            input,
            &format!("#[{macro_name}] can only be applied to structs or impl blocks"),
        )),
    }
}

/// Checks if an attribute matches a given name, handling both unqualified (`#[jsg_method]`)
/// and qualified (`#[jsg_macros::jsg_method]`) paths.
pub fn is_attr(attr: &syn::Attribute, name: &str) -> bool {
    attr.path().is_ident(name) || attr.path().segments.last().is_some_and(|s| s.ident == name)
}

/// Returns `true` if the `custom_trace` bare word is present in the attribute token stream.
///
/// Handles both bare `custom_trace` and combined forms like `name = "Foo", custom_trace`.
/// When set, `#[jsg_resource]` on a struct suppresses the auto-generated `Traced`
/// impl, letting the user write their own.
pub fn has_custom_trace_flag(attr: &TokenStream) -> bool {
    use syn::Meta;
    use syn::punctuated::Punctuated;

    let Ok(parsed) = syn::parse::Parser::parse(
        Punctuated::<Meta, syn::Token![,]>::parse_terminated,
        attr.clone(),
    ) else {
        return false;
    };

    parsed
        .iter()
        .any(|meta| matches!(meta, Meta::Path(path) if path.is_ident("custom_trace")))
}

/// Emits a `compile_error!` token stream anchored to `tokens` with message `msg`.
pub fn error(tokens: &impl ToTokens, msg: &str) -> TokenStream {
    syn::Error::new_spanned(tokens, msg)
        .to_compile_error()
        .into()
}

/// Extracts the `name = "..."` value from an attribute token stream.
///
/// Handles combined forms like `name = "Foo", custom_trace` by parsing the
/// token stream as comma-separated `syn::Meta` items and finding the first
/// `name = "..."` name-value pair.
pub fn extract_name_attribute(tokens: TokenStream) -> Option<String> {
    use syn::Meta;
    use syn::punctuated::Punctuated;

    let parsed: Punctuated<Meta, syn::Token![,]> =
        syn::parse::Parser::parse(Punctuated::parse_terminated, tokens).ok()?;

    for meta in &parsed {
        if let Meta::NameValue(nv) = meta
            && nv.path.is_ident("name")
            && let syn::Expr::Lit(syn::ExprLit {
                lit: syn::Lit::Str(s),
                ..
            }) = &nv.value
        {
            return Some(s.value());
        }
    }

    None
}

/// Converts a `snake_case` identifier to `camelCase`.
pub fn snake_to_camel(s: &str) -> String {
    let mut result = String::new();
    let mut cap_next = false;
    for (i, c) in s.chars().enumerate() {
        match c {
            '_' => cap_next = true,
            _ if i == 0 => result.push(c),
            _ if cap_next => {
                result.push(c.to_ascii_uppercase());
                cap_next = false;
            }
            _ => result.push(c),
        }
    }
    result
}

/// Checks if a type is `Result<T, E>`.
pub fn is_result_type(ty: &syn::Type) -> bool {
    if let syn::Type::Path(type_path) = ty
        && let Some(segment) = type_path.path.segments.last()
    {
        return segment.ident == "Result";
    }
    false
}

/// Returns true if the type is `&mut Lock` or `&mut jsg::Lock`.
///
/// When a method's first typed parameter matches this pattern, the macro passes the
/// callback's `lock` directly instead of extracting it from JavaScript arguments.
pub fn is_lock_ref(ty: &syn::Type) -> bool {
    let syn::Type::Reference(ref_type) = ty else {
        return false;
    };
    if ref_type.mutability.is_none() {
        return false;
    }
    let syn::Type::Path(type_path) = ref_type.elem.as_ref() else {
        return false;
    };
    let segments: Vec<_> = type_path.path.segments.iter().collect();
    match segments.len() {
        // `&mut Lock` — bare import (assumes `use jsg::Lock;`)
        1 => segments[0].ident == "Lock",
        // `&mut jsg::Lock` — fully qualified path
        2 => segments[0].ident == "jsg" && segments[1].ident == "Lock",
        _ => false,
    }
}

#[cfg(test)]
mod tests {
    use syn::parse_quote;

    use super::*;

    #[test]
    fn snake_to_camel_cases() {
        // First char is never uppercased; each `_` capitalises the next letter.
        assert_eq!(snake_to_camel(""), "");
        assert_eq!(snake_to_camel("hello"), "hello");
        assert_eq!(snake_to_camel("get_name"), "getName");
        assert_eq!(snake_to_camel("parse_caa_record"), "parseCaaRecord");
        assert_eq!(snake_to_camel("alreadyCamel"), "alreadyCamel");
        // A leading `_` sets cap_next; the next char is capitalised.
        assert_eq!(snake_to_camel("_private"), "Private");
        // Consecutive underscores — the second just re-sets cap_next.
        assert_eq!(snake_to_camel("a__b"), "aB");
    }

    #[test]
    fn is_result_type_cases() {
        assert!(is_result_type(&parse_quote!(Result<String, Error>)));
        // Qualified path: last segment is still `Result`.
        assert!(is_result_type(&parse_quote!(std::result::Result<(), ()>)));
        assert!(!is_result_type(&parse_quote!(Option<String>)));
        assert!(!is_result_type(&parse_quote!(String)));
    }

    #[test]
    fn is_lock_ref_cases() {
        assert!(is_lock_ref(&parse_quote!(&mut Lock)));
        assert!(is_lock_ref(&parse_quote!(&mut jsg::Lock)));
        // Immutable ref, wrong type, or not a ref at all must all return false.
        assert!(!is_lock_ref(&parse_quote!(&Lock)));
        assert!(!is_lock_ref(&parse_quote!(&mut String)));
        assert!(!is_lock_ref(&parse_quote!(Lock)));
    }

    #[test]
    fn is_attr_cases() {
        let simple: syn::ItemFn = parse_quote! { #[jsg_method] fn foo() {} };
        let qualified: syn::ItemFn = parse_quote! { #[jsg_macros::jsg_method] fn foo() {} };

        assert!(is_attr(&simple.attrs[0], "jsg_method"));
        assert!(!is_attr(&simple.attrs[0], "jsg_resource"));
        // Qualified path (`jsg_macros::jsg_method`) must also match by last segment.
        assert!(is_attr(&qualified.attrs[0], "jsg_method"));
    }
}
