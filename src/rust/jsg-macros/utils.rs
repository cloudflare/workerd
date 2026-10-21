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

/// Returns true if `ty` is `Option<T>`.
pub fn is_option_type(ty: &syn::Type) -> bool {
    matches!(ty, syn::Type::Path(p)
        if p.path.segments.last().is_some_and(|s| s.ident == "Option"))
}

/// Returns the tokens of `ty` with every named lifetime other than `'static`
/// replaced by `'_`.
///
/// Callbacks name a parameter's type in a position (`<T as jsg::FromJS<'_>>`) where
/// the method's own lifetime parameters are not in scope. Eliding them lets the
/// compiler infer them from the call, which ties them to the callback's scope.
pub fn elide_named_lifetimes(ty: &syn::Type) -> proc_macro2::TokenStream {
    fn rewrite(tokens: proc_macro2::TokenStream) -> proc_macro2::TokenStream {
        use proc_macro2::Spacing;
        use proc_macro2::TokenTree;

        let mut out = Vec::new();
        let mut after_lifetime_quote = false;
        for tree in tokens {
            let tree = match tree {
                TokenTree::Group(group) => {
                    let mut rewritten =
                        proc_macro2::Group::new(group.delimiter(), rewrite(group.stream()));
                    rewritten.set_span(group.span());
                    TokenTree::Group(rewritten)
                }
                TokenTree::Ident(ident) if after_lifetime_quote && ident != "static" => {
                    TokenTree::Ident(proc_macro2::Ident::new("_", ident.span()))
                }
                other => other,
            };
            after_lifetime_quote = matches!(
                &tree,
                TokenTree::Punct(punct) if punct.as_char() == '\'' && punct.spacing() == Spacing::Joint
            );
            out.push(tree);
        }
        out.into_iter().collect()
    }
    rewrite(ty.to_token_stream())
}

/// Types with a `FastApiArg` impl in `jsg/fast_api.rs`.
const FAST_API_ARG_TYPES: &[&str] = &["bool", "i32", "u32", "Number"];
/// Types with a `FastApiReturn` impl in `jsg/fast_api.rs`, besides `()`.
const FAST_API_RETURN_TYPES: &[&str] = &["bool", "Number"];

/// Returns the argument types of `sig` if V8 can call it through a fast callback.
///
/// Types are matched by name: an alias stays on the slow path, and another type named `Number`
/// fails to compile.
pub fn fast_api_signature(sig: &syn::Signature) -> Option<Vec<&syn::Type>> {
    if sig.asyncness.is_some() || !sig.generics.params.is_empty() || sig.variadic.is_some() {
        return None;
    }
    let mut args = Vec::new();
    for input in &sig.inputs {
        match input {
            syn::FnArg::Receiver(receiver) => {
                if receiver.reference.is_none() || receiver.mutability.is_some() {
                    return None;
                }
            }
            syn::FnArg::Typed(pat_type) => {
                if !is_named_type(&pat_type.ty, FAST_API_ARG_TYPES) {
                    return None;
                }
                args.push(pat_type.ty.as_ref());
            }
        }
    }
    let fast_return = match &sig.output {
        syn::ReturnType::Default => true,
        syn::ReturnType::Type(_, ty) => match ty.as_ref() {
            syn::Type::Tuple(tuple) => tuple.elems.is_empty(),
            ty => is_named_type(ty, FAST_API_RETURN_TYPES),
        },
    };
    fast_return.then_some(args)
}

/// Whether `ty` is one of `names` written bare, or `jsg::Number`.
fn is_named_type(ty: &syn::Type, names: &[&str]) -> bool {
    let syn::Type::Path(type_path) = ty else {
        return false;
    };
    if type_path.qself.is_some() {
        return false;
    }
    let segments: Vec<_> = type_path.path.segments.iter().collect();
    let last = match segments.as_slice() {
        [last] => last,
        [module, last] if module.ident == "jsg" && last.ident == "Number" => last,
        _ => return false,
    };
    last.arguments.is_none() && names.iter().any(|name| last.ident == name)
}

#[cfg(test)]
#[path = "utils-test.rs"]
mod tests;
