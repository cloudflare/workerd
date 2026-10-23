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
