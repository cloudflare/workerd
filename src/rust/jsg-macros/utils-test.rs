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
fn required_argument_count_uses_from_js_for_js_parameters() {
    let cases: Vec<(syn::Signature, Vec<syn::Type>)> = vec![
        (parse_quote!(fn f(&self)), vec![]),
        (
            parse_quote!(fn f(&self, a: String, b: u32)),
            vec![parse_quote!(String), parse_quote!(u32)],
        ),
        (
            parse_quote!(fn f(&self, lock: &mut jsg::Lock, a: String)),
            vec![parse_quote!(String)],
        ),
        (parse_quote!(fn f(lock: &mut Lock)), vec![]),
        (
            parse_quote!(fn constructor(this: String)),
            vec![parse_quote!(String)],
        ),
        (
            parse_quote!(fn constructor(lock: &mut Lock, this: String)),
            vec![parse_quote!(String)],
        ),
        (
            parse_quote!(fn constructor(mut this: String, extra: OptionalString)),
            vec![parse_quote!(String), parse_quote!(OptionalString)],
        ),
        (
            parse_quote!(fn constructor(this: OptionalString)),
            vec![parse_quote!(OptionalString)],
        ),
        (
            parse_quote!(fn f(a: String, b: jsg::Lenient<u32>, c: u32)),
            vec![
                parse_quote!(String),
                parse_quote!(jsg::Lenient<u32>),
                parse_quote!(u32),
            ],
        ),
        (
            parse_quote!(fn f(a: jsg::Nullable<String>)),
            vec![parse_quote!(jsg::Nullable<String>)],
        ),
        (
            parse_quote!(fn f<'s>(&self, lock: &mut Lock<'s>, value: jsg::v8::Local<'s, jsg::v8::Value>)),
            vec![parse_quote!(jsg::v8::Local<'_, jsg::v8::Value>)],
        ),
    ];
    for (sig, types) in cases {
        let expected = quote::quote! {{
            let optional: &[bool] = &[#(<#types as jsg::FromJS<'_>>::is_optional()),*];
            let mut length = 0u32;
            for &is_optional in optional {
                if is_optional {
                    break;
                }
                length += 1;
            }
            length
        }};
        assert_eq!(
            required_argument_count(sig.inputs.iter()).to_string(),
            expected.to_string(),
            "{sig:?}",
        );
    }
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
fn is_null_or_undefined_type_cases() {
    assert!(is_null_or_undefined_type(&parse_quote!(
        NullOrUndefined<String>
    )));
    assert!(is_null_or_undefined_type(&parse_quote!(
        jsg::NullOrUndefined<u32>
    )));
    assert!(!is_null_or_undefined_type(&parse_quote!(String)));
    assert!(!is_null_or_undefined_type(&parse_quote!(
        &NullOrUndefined<String>
    )));
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
fn fast_api_signature_cases() {
    let fast = |sig: syn::Signature| fast_api_signature(&sig).map(|args| args.len());

    assert_eq!(
        fast(parse_quote!(fn f(&self, a: i32, b: u32) -> bool)),
        Some(2)
    );
    assert_eq!(fast(parse_quote!(fn f(a: bool))), Some(1));
    assert_eq!(fast(parse_quote!(fn f(&self) -> ())), Some(0));

    assert_eq!(fast(parse_quote!(fn f(&mut self, a: i32))), None);
    assert_eq!(fast(parse_quote!(fn f(self, a: i32))), None);
    assert_eq!(fast(parse_quote!(fn f(&self, s: String))), None);
    assert_eq!(
        fast(parse_quote!(fn f(&self, lock: &mut Lock, a: i32))),
        None
    );
    assert_eq!(fast(parse_quote!(fn f(&self, x: f64) -> f64)), Some(1));
    assert_eq!(fast(parse_quote!(fn f(&self, x: f32))), None);
    assert_eq!(
        fast(parse_quote!(fn f(&self, x: ::core::primitive::f64))),
        None
    );
    assert_eq!(fast(parse_quote!(fn f(&self, x: Option<i32>))), None);
    assert_eq!(fast(parse_quote!(fn f(&self) -> i32)), Some(0));
    assert_eq!(
        fast(parse_quote!(fn f(&self, x: u8, y: i16) -> u16)),
        Some(2)
    );
    assert_eq!(
        fast(parse_quote!(fn f(&self, x: i8, y: u16) -> i8)),
        Some(2)
    );
    assert_eq!(fast(parse_quote!(fn f(&self) -> i64)), None);
    assert_eq!(fast(parse_quote!(fn f(&self, x: u64))), None);
    assert_eq!(fast(parse_quote!(fn f(&self) -> Option<u32>)), None);
    assert_eq!(
        fast(parse_quote!(fn f(&self) -> Result<bool, jsg::Error>)),
        None
    );
    assert_eq!(fast(parse_quote!(fn f<T>(&self, a: i32))), None);
    assert_eq!(fast(parse_quote!(async fn f(&self, a: i32))), None);
    assert_eq!(
        fast(parse_quote!(fn f(this: jsg::Rc<Self>, a: i32) -> bool)),
        None
    );
}

#[test]
fn is_rc_self_cases() {
    assert!(is_rc_self(&parse_quote!(jsg::Rc<Self>)));
    assert!(is_rc_self(&parse_quote!(Rc<Self>)));
    // Another module's `Rc`, a reference, another type argument, or no argument at all.
    assert!(!is_rc_self(&parse_quote!(std::rc::Rc<Self>)));
    assert!(!is_rc_self(&parse_quote!(&jsg::Rc<Self>)));
    assert!(!is_rc_self(&parse_quote!(jsg::Rc<Other>)));
    assert!(!is_rc_self(&parse_quote!(jsg::Rc<Self, Self>)));
    assert!(!is_rc_self(&parse_quote!(jsg::Rc)));
    assert!(!is_rc_self(&parse_quote!(jsg::Weak<Self>)));
}

#[test]
fn this_param_cases() {
    let this = |sig: syn::Signature| this_param(&sig).map(|param| param.ty.clone());

    assert_eq!(
        this(parse_quote!(fn f(this: jsg::Rc<Self>, a: i32))),
        Some(parse_quote!(jsg::Rc<Self>))
    );
    // The type is not checked here, and `mut this` is still `this`.
    assert_eq!(
        this(parse_quote!(fn f(mut this: String))),
        Some(parse_quote!(String))
    );
    // Only the first parameter counts, and a receiver comes first.
    assert_eq!(this(parse_quote!(fn f(&self, this: jsg::Rc<Self>))), None);
    assert_eq!(
        this(parse_quote!(fn f(lock: &mut Lock, this: jsg::Rc<Self>))),
        None
    );
    assert_eq!(this(parse_quote!(fn f(that: jsg::Rc<Self>))), None);
    assert_eq!(this(parse_quote!(fn f())), None);
}
