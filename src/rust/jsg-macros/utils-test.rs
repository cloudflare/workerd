// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

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

#[test]
fn is_option_type_cases() {
    assert!(is_option_type(&parse_quote!(Option<String>)));
    assert!(is_option_type(&parse_quote!(std::option::Option<u32>)));
    assert!(!is_option_type(&parse_quote!(String)));
    assert!(!is_option_type(&parse_quote!(&Option<String>)));
}

#[test]
fn elide_named_lifetimes_cases() {
    let rewritten = |ty: syn::Type| elide_named_lifetimes(&ty).to_string();
    assert_eq!(
        rewritten(parse_quote!(jsg::v8::Local<'a, jsg::v8::Value>)),
        quote::quote!(jsg::v8::Local<'_, jsg::v8::Value>).to_string()
    );
    assert_eq!(
        rewritten(parse_quote!(&'b str)),
        quote::quote!(&'_ str).to_string()
    );
    // Lifetimes nested inside groups are rewritten too.
    assert_eq!(
        rewritten(parse_quote!(Option<(Local<'s, Value>, fn(&'t u8))>)),
        quote::quote!(Option<(Local<'_, Value>, fn(&'_ u8))>).to_string()
    );
    // `'static` and already-elided lifetimes are kept.
    assert_eq!(
        rewritten(parse_quote!(&'static str)),
        quote::quote!(&'static str).to_string()
    );
    assert_eq!(
        rewritten(parse_quote!(String)),
        quote::quote!(String).to_string()
    );
}

#[test]
fn fast_api_shape_cases() {
    let shape = |sig: syn::Signature| fast_api_shape(&sig).map(|s| (s.lock, s.args.len()));

    assert_eq!(
        shape(parse_quote!(fn f(&self, a: i32, s: String) -> Result<Foo, Bar>)),
        Ok((false, 2))
    );
    assert_eq!(shape(parse_quote!(fn f(a: other::Number))), Ok((false, 1)));
    assert_eq!(shape(parse_quote!(fn f(&self))), Ok((false, 0)));

    assert!(shape(parse_quote!(fn f(&mut self, a: i32))).is_err());
    assert!(shape(parse_quote!(fn f(self, a: i32))).is_err());
    assert!(shape(parse_quote!(fn f<T>(&self, a: i32))).is_err());
    assert!(shape(parse_quote!(async fn f(&self, a: i32))).is_err());
    assert!(shape(parse_quote!(fn f(&self, a: impl Into<i32>))).is_err());
}

#[test]
fn fast_api_shape_lock() {
    let shape = |sig: syn::Signature| fast_api_shape(&sig).map(|s| (s.lock, s.args.len()));

    assert_eq!(
        shape(parse_quote!(fn f(&self, lock: &mut Lock, a: i32))),
        Ok((true, 1))
    );
    assert_eq!(
        shape(parse_quote!(fn f(lock: &mut jsg::Lock<'_>) -> bool)),
        Ok((true, 0))
    );

    // Only a leading `&mut Lock` is the lock.
    assert_eq!(
        shape(parse_quote!(fn f(&self, a: i32, lock: &mut Lock))),
        Ok((false, 2))
    );
    assert_eq!(
        shape(parse_quote!(fn f(&self, lock: &Lock))),
        Ok((false, 1))
    );
}

#[test]
fn fast_mode_cases() {
    assert_eq!(fast_mode(quote::quote!()), Ok(FastMode::Auto));
    assert_eq!(fast_mode(quote::quote!(name = "x")), Ok(FastMode::Auto));
    assert_eq!(fast_mode(quote::quote!(fast)), Ok(FastMode::Fast));
    assert_eq!(
        fast_mode(quote::quote!(name = "x", nofast)),
        Ok(FastMode::NoFast)
    );
    assert!(fast_mode(quote::quote!(fast, nofast)).is_err());
    assert!(fast_mode(quote::quote!(fast, fast)).is_err());
    assert!(fast_mode(quote::quote!(nofast = true)).is_err());
    assert!(fast_mode(quote::quote!(nofsat)).is_err());
}
