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
