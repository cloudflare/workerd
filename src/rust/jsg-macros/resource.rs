// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Code generation for `#[jsg_resource]` on structs and impl blocks.
//!
//! - On a **struct**: emits `jsg::Type`, `jsg::ToJS`, `jsg::FromJS`,
//!   `jsg::Traced`, and `jsg::GarbageCollected` implementations.
//! - On an **impl block**: emits the `jsg::Resource` trait with method, static
//!   method, static constant, and constructor registrations.

use proc_macro::TokenStream;
use quote::quote;
use syn::FnArg;
use syn::ItemImpl;

use crate::trace::generate_trace_statements;
use crate::utils::error;
use crate::utils::extract_name_attribute;
use crate::utils::extract_named_fields;
use crate::utils::has_custom_trace_flag;
use crate::utils::is_attr;
use crate::utils::is_lock_ref;
use crate::utils::snake_to_camel;

// Compile-time mirror of `jsg::PropertyKind` used to group annotated methods
// and emit the correct token streams. Cannot reuse the runtime type directly
// because proc-macro crates cannot link against CXX-bridge runtime crates.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
enum PropertyKind {
    Prototype,
    Instance,
    Inspect,
}

/// Entry point called from `lib.rs` for `#[jsg_resource]` on a struct.
pub fn generate_resource_struct(attr: TokenStream, input: &syn::DeriveInput) -> TokenStream {
    // Check for `#[jsg_resource(custom_trace)]` before consuming `attr`.
    let custom_trace = has_custom_trace_flag(&attr);

    // Clone the name before mutating `input` so we can borrow freely later.
    let name = input.ident.clone();

    let class_name = if attr.is_empty() {
        name.to_string()
    } else {
        extract_name_attribute(attr).unwrap_or_else(|| name.to_string())
    };

    let fields = match extract_named_fields(input, "jsg_resource") {
        Ok(fields) => fields,
        Err(err) => return err,
    };

    let trace_statements = generate_trace_statements(&fields);
    let name_str = name.to_string();

    let traced_impl = if custom_trace {
        // `custom_trace` suppresses the generated `Traced` impl — the user will write their own.
        quote! {}
    } else {
        quote! {
            #[automatically_derived]
            impl jsg::Traced for #name {
                fn trace(&self, visitor: &mut jsg::GcVisitor) {
                    // Suppress unused warning when there are no traceable fields.
                    let _ = visitor;
                    #(#trace_statements)*
                }
            }
        }
    };

    let gc_impl = quote! {
        #[automatically_derived]
        impl jsg::GarbageCollected for #name {
            fn memory_name(&self) -> &'static ::std::ffi::CStr {
                // from_bytes_with_nul on a concat!(name, "\0") literal is a
                // compile-time constant expression — the compiler folds the
                // unwrap and emits a direct pointer into the read-only data
                // segment. The C++ side constructs a kj::StringPtr directly
                // from data()+size() with no allocation.
                ::std::ffi::CStr::from_bytes_with_nul(concat!(#name_str, "\0").as_bytes())
                    .unwrap()
            }
        }
    };

    quote! {
        #input

        #[automatically_derived]
        impl jsg::Type for #name {
            fn class_name() -> &'static str { #class_name }

            fn is_exact(value: &jsg::v8::Local<jsg::v8::Value>) -> bool {
                value.is_object()
            }
        }

        #[automatically_derived]
        impl jsg::ToJS for #name {
            fn to_js<'a, 'b>(self, lock: &'a mut jsg::Lock) -> jsg::v8::Local<'b, jsg::v8::Value>
            where
                'b: 'a,
            {
                let r = jsg::Rc::new(self);
                r.to_js(lock)
            }
        }

        #[automatically_derived]
        impl jsg::FromJS for #name {
            type ResultType = jsg::Rc<Self>;

            fn from_js(
                lock: &mut jsg::Lock,
                value: jsg::v8::Local<jsg::v8::Value>,
            ) -> Result<Self::ResultType, jsg::Error> {
                <jsg::Rc<Self> as jsg::FromJS>::from_js(lock, value)
            }
        }

        #traced_impl
        #gc_impl
    }
    .into()
}

/// Scans `impl_block` for `#[jsg_method]`-annotated functions and returns a
/// `Member::Method` / `Member::StaticMethod` token stream for each.
fn collect_method_registrations(impl_block: &ItemImpl) -> Vec<quote::__private::TokenStream> {
    impl_block
        .items
        .iter()
        .filter_map(|item| {
            let syn::ImplItem::Fn(method) = item else {
                return None;
            };
            let attr = method.attrs.iter().find(|a| is_attr(a, "jsg_method"))?;

            let rust_method_name = &method.sig.ident;
            let js_name = attr
                .meta
                .require_list()
                .ok()
                .map(|list| list.tokens.clone().into())
                .and_then(extract_name_attribute)
                .unwrap_or_else(|| snake_to_camel(&rust_method_name.to_string()));
            let callback_name =
                syn::Ident::new(&format!("{rust_method_name}_callback"), rust_method_name.span());

            let has_self = method
                .sig
                .inputs
                .iter()
                .any(|arg| matches!(arg, FnArg::Receiver(_)));

            Some(if has_self {
                quote! { jsg::Member::Method { name: #js_name.to_owned(), callback: Self::#callback_name } }
            } else {
                quote! { jsg::Member::StaticMethod { name: #js_name.to_owned(), callback: Self::#callback_name } }
            })
        })
        .collect()
}

/// Scans `impl_block` for `#[jsg_static_constant]`-annotated consts and returns
/// a `Member::StaticConstant` token stream for each.
fn collect_constant_registrations(impl_block: &ItemImpl) -> Vec<quote::__private::TokenStream> {
    impl_block
        .items
        .iter()
        .filter_map(|item| {
            let syn::ImplItem::Const(constant) = item else {
                return None;
            };
            let attr = constant
                .attrs
                .iter()
                .find(|a| is_attr(a, "jsg_static_constant"))?;

            let rust_name = &constant.ident;
            let js_name = attr
                .meta
                .require_list()
                .ok()
                .map(|list| list.tokens.clone().into())
                .and_then(extract_name_attribute)
                .unwrap_or_else(|| rust_name.to_string());

            Some(quote! {
                jsg::Member::StaticConstant {
                    name: #js_name.to_owned(),
                    value: jsg::ConstantValue::from(Self::#rust_name),
                }
            })
        })
        .collect()
}

/// Entry point called from `lib.rs` for `#[jsg_resource]` on an impl block.
pub fn generate_resource_impl(impl_block: &ItemImpl) -> TokenStream {
    let self_ty = &impl_block.self_ty;

    if !matches!(&**self_ty, syn::Type::Path(_)) {
        return error(
            self_ty,
            "#[jsg_resource] impl blocks must use a simple path type (e.g., `impl MyResource`)",
        );
    }

    let method_registrations = collect_method_registrations(impl_block);
    let property_registrations = collect_property_registrations(impl_block);
    let constant_registrations = collect_constant_registrations(impl_block);

    let constructor_registration = generate_constructor_registration(impl_block, self_ty);
    let constructor_vec: Vec<_> = constructor_registration.into_iter().collect();

    quote! {
        #impl_block

        #[automatically_derived]
        impl jsg::Resource for #self_ty {
            fn members() -> Vec<jsg::Member>
            where
                Self: Sized,
            {
                vec![
                    #(#constructor_vec,)*
                    #(#method_registrations,)*
                    #(#property_registrations,)*
                    #(#constant_registrations,)*
                ]
            }
        }
    }
    .into()
}

// ---------------------------------------------------------------------------
// Constructor helpers
// ---------------------------------------------------------------------------

/// Validates that a `#[jsg_constructor]` method has the right shape.
///
/// Returns a `compile_error!` token stream if the method has a `self` receiver
/// or does not return `Self`; returns `None` if the method is valid.
fn validate_constructor(method: &syn::ImplItemFn) -> Option<quote::__private::TokenStream> {
    let has_self = method
        .sig
        .inputs
        .iter()
        .any(|arg| matches!(arg, FnArg::Receiver(_)));
    if has_self {
        return Some(quote! {
            compile_error!("#[jsg_constructor] must be a static method (no self receiver)");
        });
    }

    let returns_self = matches!(&method.sig.output,
        syn::ReturnType::Type(_, ty) if matches!(&**ty,
            syn::Type::Path(p) if p.path.is_ident("Self")
        )
    );
    if !returns_self {
        return Some(quote! {
            compile_error!("#[jsg_constructor] must return Self");
        });
    }

    None
}

/// Extracts constructor argument unwrap statements and argument expressions.
fn extract_constructor_params(
    method: &syn::ImplItemFn,
) -> (
    bool,
    Vec<quote::__private::TokenStream>,
    Vec<quote::__private::TokenStream>,
) {
    let params: Vec<_> = method
        .sig
        .inputs
        .iter()
        .filter_map(|arg| {
            if let FnArg::Typed(pat_type) = arg {
                Some((*pat_type.ty).clone())
            } else {
                None
            }
        })
        .collect();

    let has_lock_param = params.first().is_some_and(is_lock_ref);
    let js_arg_offset = usize::from(has_lock_param);

    let (unwraps, arg_exprs) = params
        .iter()
        .enumerate()
        .skip(js_arg_offset)
        .map(|(i, ty)| {
            let js_index = i - js_arg_offset;
            let var = syn::Ident::new(&format!("arg{js_index}"), method.sig.ident.span());
            let unwrap = quote! {
                let #var = match <#ty as jsg::FromJS>::from_js(&mut lock, args.get(#js_index)) {
                    Ok(v) => v,
                    Err(e) => {
                        lock.throw_exception(&e);
                        return;
                    }
                };
            };
            (unwrap, quote! { #var })
        })
        .unzip();

    (has_lock_param, unwraps, arg_exprs)
}

/// Scans an impl block for a `#[jsg_constructor]` attribute and generates the
/// constructor callback registration. Returns `None` if no constructor is defined.
/// Validates that a `#[jsg_constructor]` method has the right shape and returns
/// a compile-error token stream if it doesn't.
fn generate_constructor_registration(
    impl_block: &ItemImpl,
    self_ty: &syn::Type,
) -> Option<quote::__private::TokenStream> {
    let constructors: Vec<_> = impl_block
        .items
        .iter()
        .filter_map(|item| match item {
            syn::ImplItem::Fn(m) if m.attrs.iter().any(|a| is_attr(a, "jsg_constructor")) => {
                Some(m)
            }
            _ => None,
        })
        .collect();

    if constructors.len() > 1 {
        return Some(quote! {
            compile_error!("only one #[jsg_constructor] is allowed per impl block");
        });
    }

    constructors
        .into_iter()
        .map(|method| {
            if let Some(err) = validate_constructor(method) {
                return err;
            }

            let rust_method_name = &method.sig.ident;
            let callback_name = syn::Ident::new(
                &format!("{rust_method_name}_constructor_callback"),
                rust_method_name.span(),
            );

            let (has_lock_param, unwraps, arg_exprs) = extract_constructor_params(method);
            let lock_arg = if has_lock_param {
                quote! { &mut lock, }
            } else {
                quote! {}
            };

            quote! {
                jsg::Member::Constructor {
                    callback: {
                        unsafe extern "C" fn #callback_name(
                            info: *mut jsg::v8::ffi::FunctionCallbackInfo,
                        ) {
                            let mut lock = unsafe { jsg::Lock::from_args(info) };
                            jsg::catch_panic(&mut lock, || {
                                // SAFETY: info is a valid V8 FunctionCallbackInfo from the constructor call.
                                let mut args = unsafe { jsg::v8::FunctionCallbackInfo::from_ffi(info) };
                                let mut lock = unsafe { jsg::Lock::from_args(info) };

                                #(#unwraps)*

                                let resource = #self_ty::#rust_method_name(#lock_arg #(#arg_exprs),*);
                                let rc = jsg::Rc::new(resource);
                                rc.attach_to_this(&mut args);
                            });
                        }
                        #callback_name
                    },
                }
            }
        })
        .next()
}

// ---------------------------------------------------------------------------
// Property helpers
// ---------------------------------------------------------------------------

/// Emits one `Member::Property { .. }` token stream for a single property group,
/// or an `Err` compile-error stream if the group has no getter.
fn emit_property_group(
    js_name: &str,
    kind: PropertyKind,
    getter: Option<syn::Ident>,
    setter: Option<syn::Ident>,
) -> Result<quote::__private::TokenStream, quote::__private::TokenStream> {
    let Some(getter_name) = getter else {
        return Err(quote! {
            compile_error!(concat!("no getter found for property \"", #js_name, "\""))
        });
    };

    let getter_cb = syn::Ident::new(&format!("{getter_name}_callback"), getter_name.span());
    let kind_tokens = match kind {
        PropertyKind::Prototype => quote! { jsg::PropertyKind::Prototype },
        PropertyKind::Instance => quote! { jsg::PropertyKind::Instance },
        PropertyKind::Inspect => quote! { jsg::PropertyKind::Inspect },
    };
    let setter_tokens = if let Some(setter_name) = setter {
        let setter_cb = syn::Ident::new(&format!("{setter_name}_callback"), setter_name.span());
        quote! { Some(Self::#setter_cb) }
    } else {
        quote! { None }
    };

    Ok(quote! {
        jsg::Member::Property {
            name: #js_name.to_owned(),
            kind: #kind_tokens,
            getter_callback: Self::#getter_cb,
            setter_callback: #setter_tokens,
        }
    })
}

/// Parses the argument list of `#[jsg_property([placement,] [name = "..."] [, readonly])]`.
///
/// `placement` is optional; when omitted it defaults to `Prototype`.
/// Returns `(PropertyKind, Option<js_name_override>, is_readonly)`.
fn parse_jsg_property_args(
    tokens: TokenStream,
) -> Result<(PropertyKind, Option<String>, bool), quote::__private::TokenStream> {
    use syn::parse::Parser as _;
    let metas = syn::punctuated::Punctuated::<syn::Meta, syn::Token![,]>::parse_terminated
        .parse(tokens)
        .map_err(|e| e.to_compile_error())?;

    let mut kind: Option<PropertyKind> = None;
    let mut name: Option<String> = None;
    let mut readonly = false;

    for meta in &metas {
        match meta {
            syn::Meta::Path(p) if p.is_ident("instance") => {
                if kind.is_some() {
                    return Err(syn::Error::new_spanned(
                        p,
                        "conflicting placement: specify either `instance` or `prototype`, not both",
                    )
                    .to_compile_error());
                }
                kind = Some(PropertyKind::Instance);
            }
            syn::Meta::Path(p) if p.is_ident("prototype") => {
                if kind.is_some() {
                    return Err(syn::Error::new_spanned(
                        p,
                        "conflicting placement: specify either `instance` or `prototype`, not both",
                    )
                    .to_compile_error());
                }
                kind = Some(PropertyKind::Prototype);
            }
            syn::Meta::Path(p) if p.is_ident("readonly") => {
                readonly = true;
            }
            syn::Meta::NameValue(nv) if nv.path.is_ident("name") => {
                if let syn::Expr::Lit(syn::ExprLit {
                    lit: syn::Lit::Str(s),
                    ..
                }) = &nv.value
                {
                    name = Some(s.value());
                } else {
                    return Err(syn::Error::new_spanned(
                        &nv.value,
                        "expected a string literal for `name`",
                    )
                    .to_compile_error());
                }
            }
            _ => {
                return Err(syn::Error::new_spanned(
                    meta,
                    "unknown argument; expected `instance`, `prototype`, `readonly`, or `name = \"...\"`",
                )
                .to_compile_error());
            }
        }
    }

    // Default to `Prototype` when no explicit placement is given.
    let kind = kind.unwrap_or(PropertyKind::Prototype);

    Ok((kind, name, readonly))
}

struct PropMethod {
    rust_name: syn::Ident,
    is_setter: bool,
    is_readonly: bool,
}

/// Ordered list of property groups, preserving source-code declaration order.
/// Each entry is `((js_name, kind), methods)`.
type PropGroups = Vec<((String, PropertyKind), Vec<PropMethod>)>;

/// Derive the JS property name from a Rust method name: strip `get_`/`set_` prefix then
/// convert `snake_case` -> `camelCase`.
fn derive_js_name(rust_name: &str) -> String {
    let stripped = rust_name
        .strip_prefix("get_")
        .or_else(|| rust_name.strip_prefix("set_"))
        .unwrap_or(rust_name);
    snake_to_camel(stripped)
}

/// Find the group for `key` in `groups`, or append a new empty one and return it.
/// Preserves insertion order so that property registration matches source-code order.
fn prop_groups_find_or_insert(
    groups: &mut PropGroups,
    key: (String, PropertyKind),
) -> &mut Vec<PropMethod> {
    if let Some(pos) = groups.iter().position(|(k, _)| k == &key) {
        return &mut groups[pos].1;
    }
    groups.push((key, Vec::new()));
    &mut groups.last_mut().expect("just pushed").1
}

/// Phase 1: scan `impl_block` for `#[jsg_property]` / `#[jsg_inspect_property]` annotations
/// and group the annotated methods by `(js_name, PropertyKind)`.
fn scan_property_annotations(
    impl_block: &ItemImpl,
) -> Result<PropGroups, quote::__private::TokenStream> {
    let mut groups: PropGroups = Vec::new();

    for item in &impl_block.items {
        let syn::ImplItem::Fn(method) = item else {
            continue;
        };
        let rust_method_name = method.sig.ident.clone();
        let rust_name_str = rust_method_name.to_string();
        let is_setter = rust_name_str.starts_with("set_");

        if let Some(attr) = method.attrs.iter().find(|a| is_attr(a, "jsg_property")) {
            if !rust_name_str.starts_with("get_") && !rust_name_str.starts_with("set_") {
                return Err(syn::Error::new(
                    rust_method_name.span(),
                    "#[jsg_property] methods must be named with a `get_` prefix (getter) \
                     or `set_` prefix (setter)",
                )
                .to_compile_error());
            }

            let tokens: TokenStream = attr
                .meta
                .require_list()
                .map(|list| list.tokens.clone().into())
                .unwrap_or_default();
            let (kind, js_name_opt, is_readonly) = parse_jsg_property_args(tokens)?;
            let js_name = js_name_opt.unwrap_or_else(|| derive_js_name(&rust_name_str));
            prop_groups_find_or_insert(&mut groups, (js_name, kind)).push(PropMethod {
                rust_name: rust_method_name,
                is_setter,
                is_readonly,
            });
            continue;
        }

        if let Some(attr) = method
            .attrs
            .iter()
            .find(|a| is_attr(a, "jsg_inspect_property"))
        {
            let attr_tokens: Option<TokenStream> = attr
                .meta
                .require_list()
                .ok()
                .map(|list| list.tokens.clone().into());
            let js_name = attr_tokens
                .and_then(extract_name_attribute)
                .unwrap_or_else(|| derive_js_name(&rust_name_str));
            prop_groups_find_or_insert(&mut groups, (js_name, PropertyKind::Inspect)).push(
                PropMethod {
                    rust_name: rust_method_name,
                    is_setter,
                    is_readonly: false,
                },
            );
        }
    }
    Ok(groups)
}

/// Phase 2 (per group): validate constraints and emit one `Member::Property`.
fn validate_and_emit_property(
    js_name: &str,
    kind: PropertyKind,
    methods: Vec<PropMethod>,
) -> Result<quote::__private::TokenStream, quote::__private::TokenStream> {
    let has_readonly_getter = methods.iter().any(|m| m.is_readonly && !m.is_setter);
    for m in &methods {
        if m.is_readonly && m.is_setter {
            return Err(syn::Error::new(
                m.rust_name.span(),
                "`readonly` attribute cannot be used on a setter method",
            )
            .to_compile_error());
        }
    }
    let setters: Vec<_> = methods.iter().filter(|m| m.is_setter).collect();
    if has_readonly_getter && !setters.is_empty() {
        return Err(syn::Error::new(
            setters[0].rust_name.span(),
            "read-only property cannot have a setter; remove the setter or drop the `readonly` attribute",
        )
        .to_compile_error());
    }
    if kind == PropertyKind::Inspect {
        for m in &methods {
            if m.is_setter {
                return Err(syn::Error::new(
                    m.rust_name.span(),
                    "#[jsg_inspect_property] methods must be getters; inspect properties are always read-only",
                )
                .to_compile_error());
            }
        }
    }

    let mut getter: Option<syn::Ident> = None;
    let mut setter: Option<syn::Ident> = None;
    for m in methods {
        if m.is_setter {
            if setter.replace(m.rust_name).is_some() {
                return Err(
                    quote! { compile_error!(concat!("duplicate setter for property \"", #js_name, "\"")) },
                );
            }
        } else if getter.replace(m.rust_name).is_some() {
            return Err(
                quote! { compile_error!(concat!("duplicate getter for property \"", #js_name, "\"")) },
            );
        }
    }
    emit_property_group(js_name, kind, getter, setter)
}

/// Scans an impl block for `#[jsg_property]` and `#[jsg_inspect_property]` annotations
/// and returns a `Member::Property` token stream for each property group.
fn collect_property_registrations(impl_block: &ItemImpl) -> Vec<quote::__private::TokenStream> {
    let groups = match scan_property_annotations(impl_block) {
        Ok(g) => g,
        Err(e) => return vec![e],
    };
    let mut registrations = Vec::new();
    for ((js_name, kind), methods) in groups {
        match validate_and_emit_property(&js_name, kind, methods) {
            Ok(ts) => registrations.push(ts),
            Err(ts) => {
                registrations.push(ts);
                return registrations;
            }
        }
    }
    registrations
}

#[cfg(test)]
mod tests {
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
}
