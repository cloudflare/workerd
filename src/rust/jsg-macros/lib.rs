// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Procedural macros for the JSG Rust bindings.
//!
//! # Macros
//!
//! | Macro                    | Apply to         | Purpose                                                        |
//! |--------------------------|------------------|----------------------------------------------------------------|
//! | `#[jsg_resource]`        | struct / impl    | Expose a Rust type to JavaScript as a GC resource              |
//! | `#[jsg_method]`          | fn inside impl   | Register a method (instance or static) on a resource           |
//! | `#[jsg_constructor]`     | fn inside impl   | Register `new MyResource(…)` JavaScript constructor            |
//! | `#[jsg_static_constant]` | const inside impl | Expose a numeric constant on both constructor and prototype  |
//! | `#[jsg_property]`        | fn inside impl   | Register a resource accessor property (getter/setter)          |
//! | `#[jsg_inspect_property]` | fn inside impl  | Register a debug-inspect-only symbol property                  |
//! | `#[jsg_struct]`          | struct           | Expose a Rust struct as a plain JavaScript object              |
//! | `#[jsg_oneof]`           | enum             | Accept one of several JavaScript types (`kj::OneOf`)           |
//!
//! See [`jsg/README.md`](../jsg/README.md) for full usage documentation.

#![forbid(unsafe_code)]

mod resource;
mod trace;
mod utils;

use proc_macro::TokenStream;
use quote::quote;
use resource::generate_resource_impl;
use resource::generate_resource_struct;
use syn::Data;
use syn::DeriveInput;
use syn::Fields;
use syn::FnArg;
use syn::ItemFn;
use syn::ItemImpl;
use syn::parse_macro_input;
use utils::elide_named_lifetimes;
use utils::error;
use utils::extract_name_attribute;
use utils::extract_named_fields;
use utils::fast_api_signature;
use utils::is_lock_ref;
use utils::is_option_type;
use utils::is_result_type;
use utils::snake_to_camel;

// =============================================================================
// #[jsg_struct]
// =============================================================================

/// Generates `jsg::Struct`, `jsg::Type`, `jsg::ToJS`, and `jsg::FromJS`
/// implementations for a plain data struct.
///
/// Only `pub` fields are projected into the JavaScript object.
/// Use `#[jsg_struct(name = "MyName")]` to override `Type::class_name()`
/// metadata (used in diagnostics/type reporting), not to define a JS class.
#[proc_macro_attribute]
pub fn jsg_struct(attr: TokenStream, item: TokenStream) -> TokenStream {
    let input = parse_macro_input!(item as DeriveInput);
    let name = &input.ident;
    let class_name = extract_name_attribute(attr).unwrap_or_else(|| name.to_string());

    let named_fields = match extract_named_fields(&input, "jsg_struct") {
        Ok(fields) => fields,
        Err(err) => return err,
    };

    let mut field_assignments = Vec::new();
    let mut field_extractions = Vec::new();
    let mut field_names = Vec::new();
    let mut all_optional = true;

    for field in &named_fields {
        // Only public fields are projected into JavaScript objects.
        if !matches!(field.vis, syn::Visibility::Public(_)) {
            continue;
        }
        // Named fields always have an ident; guard is purely defensive.
        let Some(field_name) = field.ident.as_ref() else {
            continue;
        };
        let field_name_str = field_name.to_string();
        let field_type = &field.ty;
        let optional = is_option_type(field_type);
        all_optional &= optional;

        // Like C++ JSG, an absent optional field is omitted, not undefined.
        let set = quote! {
            let value = jsg::v8::ToLocalValue::to_local(value, lock);
            obj.set(lock, #field_name_str, value);
        };
        field_assignments.push(if optional {
            quote! { if let Some(value) = &this.#field_name { #set } }
        } else {
            quote! { { let value = &this.#field_name; #set } }
        });
        // Like C++ JSG, an absent property reads as undefined.
        field_extractions.push(quote! {
            let prop = obj.get(lock, #field_name_str)?;
            let #field_name = <#field_type as jsg::FromJS<'s>>::from_js(lock, prop)?;
        });
        field_names.push(field_name);
    }

    // Like C++ JSG, null or undefined is an empty dictionary, which is an
    // error if a field is required.
    let empty = if all_optional {
        quote! { return Ok(Self { #(#field_names: None),* }); }
    } else {
        quote! {
            return Err(jsg::Error::new_type_error(format!(
                "Cannot initialize {} with required members from an undefined or null value.",
                #class_name,
            )));
        }
    };

    quote! {
        #input

        impl jsg::Type for #name {
            fn class_name() -> &'static str { #class_name }

            fn is_exact(value: jsg::v8::Local<'_, jsg::v8::Value>) -> bool {
                value.is_object()
            }
        }

        impl<'s> jsg::ToJS<'s> for #name {
            fn to_js(self, lock: &mut jsg::Lock<'s>) -> jsg::v8::Local<'s, jsg::v8::Value> {
                // TODO(soon): Use a precached ObjectTemplate instance to create the object,
                // similar to how C++ JSG optimizes object creation. This would avoid recreating
                // the object shape on every wrap() call and improve performance.
                {
                    let this = self;
                    let obj = lock.new_object();
                    #(#field_assignments)*
                    obj.into()
                }
            }
        }

        impl<'s> jsg::FromJS<'s> for #name {
            type ResultType = Self;

            fn type_name() -> String {
                #class_name.to_owned()
            }

            fn from_js(lock: &mut jsg::Lock<'s>, value: jsg::v8::Local<'s, jsg::v8::Value>) -> Result<Self::ResultType, jsg::Error> {
                if value.is_null() || value.is_undefined() {
                    #empty
                }
                if !value.is_object() {
                    return Err(jsg::Error::new_type_error(
                        format!("Expected object but got {}", value.type_of(lock))
                    ));
                }
                let obj: jsg::v8::Local<'s, jsg::v8::Object> = value.into();
                #(#field_extractions)*
                Ok(Self { #(#field_names),* })
            }

            fn try_from_js(lock: &mut jsg::Lock<'s>, value: jsg::v8::Local<'s, jsg::v8::Value>) -> Result<Option<Self::ResultType>, jsg::Error> {
                if !value.is_object() && !value.is_null() && !value.is_undefined() {
                    return Ok(None);
                }
                Self::from_js(lock, value).map(Some)
            }
        }

        impl jsg::Struct for #name {}

        #[automatically_derived]
        impl jsg::Traced for #name {}
    }
    .into()
}

// =============================================================================
// #[jsg_method]
// =============================================================================

/// Generates a V8 `FunctionCallback` for a JSG resource method.
///
/// Parameters are extracted from JavaScript arguments via `jsg::FromJS`.
/// Return values are converted via `jsg::ToJS`.
/// `Result<T, E>` return types automatically throw exceptions on `Err`.
///
/// The first typed parameter may be `&mut Lock` (or `&mut jsg::Lock`) to receive
/// the isolate lock directly; it is not counted as a JavaScript argument.
///
/// Instance methods take `&self`; any other receiver is a compile error.
///
/// Use `#[jsg_method(name = "jsName")]` to override the default `camelCase`
/// conversion of the Rust function name.
#[proc_macro_attribute]
pub fn jsg_method(attr: TokenStream, item: TokenStream) -> TokenStream {
    let input_fn = parse_macro_input!(item as ItemFn);
    let fast = fast_callback(&input_fn.sig);
    let mut output = callback(quote! { #input_fn }.into(), Callback::Function, Some(attr));
    output.extend(TokenStream::from(fast));
    output
}

/// The V8 callback shape a generated callback takes.
#[derive(Clone, Copy)]
enum Callback {
    Function,
    /// `AccessorNameGetterCallback`, for instance properties.
    Getter,
    /// `AccessorNameSetterCallbackV2`, for instance properties.
    Setter,
}

/// `method_attr` is a method's `#[jsg_method]` arguments; argument errors for
/// methods match C++ JSG's.
fn callback(item: TokenStream, kind: Callback, method_attr: Option<TokenStream>) -> TokenStream {
    callback_with_instance(item, kind, None, method_attr)
}

fn callback_with_instance(
    item: TokenStream,
    kind: Callback,
    instance_kind: Option<Callback>,
    method_attr: Option<TokenStream>,
) -> TokenStream {
    let input_fn = parse_macro_input!(item as ItemFn);
    let fn_name = &input_fn.sig.ident;
    let method_name = method_attr.map(|attr| {
        extract_name_attribute(attr).unwrap_or_else(|| snake_to_camel(&fn_name.to_string()))
    });
    let fn_vis = &input_fn.vis;
    let fn_sig = &input_fn.sig;
    let fn_block = &input_fn.block;

    // Methods with a `&self` receiver become instance methods on the prototype.
    // Methods without a receiver become static methods on the constructor.
    let callback_kinds = callback_names(fn_name, kind, instance_kind);
    if let Some(error) = receiver_error(&input_fn, callback_kinds.clone()) {
        return error.into();
    }
    let has_self = fn_sig.receiver().is_some();

    let params: Vec<_> = fn_sig
        .inputs
        .iter()
        .filter_map(|arg| match arg {
            FnArg::Typed(pat_type) => Some(&pat_type.ty),
            FnArg::Receiver(_) => None,
        })
        .collect();

    // Check if the first typed parameter is `&mut Lock` — if so, pass `&mut lock`
    // directly instead of extracting it from JS args (like C++ jsg::Lock&).
    let has_lock_param = params.first().is_some_and(|ty| is_lock_ref(ty));

    let (unwraps, arg_exprs): (Vec<_>, Vec<_>) = params
        .iter()
        .enumerate()
        .map(|(i, ty)| {
            // First param is &mut Lock — pass the callback's lock directly.
            if i == 0 && has_lock_param {
                return (quote! {}, quote! { &mut *lock });
            }

            let js_index = i - usize::from(has_lock_param);
            let arg = syn::Ident::new(&format!("arg{js_index}"), fn_name.span());
            let unwrap = unwrap_argument(ty, &arg, js_index, method_name.as_deref());
            // For reference types (like &str), FromJS returns an owned type (String),
            // so we need to borrow it when passing to the function.
            let is_ref = matches!(ty.as_ref(), syn::Type::Reference(_));
            let arg_expr = if is_ref {
                quote! { &#arg }
            } else {
                quote! { #arg }
            };
            (unwrap, arg_expr)
        })
        .unzip();

    // Check if return type is Result<T, E>
    let is_result = matches!(&fn_sig.output, syn::ReturnType::Type(_, ty) if is_result_type(ty));

    let result_handling = if is_result {
        quote! {
            match result {
                Ok(value) => args.set_return_value(jsg::ToJS::to_js(value, lock)),
                Err(err) => lock.throw_exception(&err.into()),
            }
        }
    } else {
        quote! {
            args.set_return_value(jsg::ToJS::to_js(result, lock));
        }
    };

    let invocation = if has_self {
        quote! {
            let this = args.this();
            // For instance methods, `v8::Signature` (applied in
            // `create_resource_template`, ffi.c++) makes V8 throw
            // `TypeError: Illegal invocation` *before* this callback runs on a
            // wrong receiver, so the mismatch below is unreachable for them.
            // Property getters/setters (`#[jsg_property]` /
            // `#[jsg_inspect_property]`) reuse this same code but are registered
            // WITHOUT a signature, so a wrong receiver (e.g.
            // `Object.getOwnPropertyDescriptor(proto, p).get.call({})`) *can*
            // reach here. Surface that as a JS `TypeError` — matching V8's own
            // "Illegal invocation" — instead of panicking (a panic here is
            // caught by `catch_panic`, but as an *internal* error + execution
            // termination, which is the wrong shape for tenant-triggerable input).
            let Some(wrappable) = jsg::v8::WrappableRc::from_js(lock, this) else {
                lock.throw_exception(&jsg::Error::new_type_error("Illegal invocation"));
                return;
            };
            let Some(resource) = wrappable.resolve_resource::<Self>() else {
                lock.throw_exception(&jsg::Error::new_type_error("Illegal invocation"));
                return;
            };
            // SAFETY: `resource` points at the `Self` of `this`, whose wrapper keeps it alive
            // during the call. Resources are only ever borrowed shared, so this may alias the
            // borrow of a method that called into JavaScript, or of GC tracing.
            let self_: &Self = unsafe { resource.as_ref() };
            let result = self_.#fn_name(#(#arg_exprs),*);
        }
    } else {
        quote! {
            let result = Self::#fn_name(#(#arg_exprs),*);
        }
    };

    // Comments do not survive into the expansion, so the SAFETY comment on the `unsafe` block
    // that borrows the resource cannot satisfy the lint.
    let lint_attr = if has_self {
        quote! { #[expect(clippy::undocumented_unsafe_blocks)] }
    } else {
        quote! {}
    };
    let callbacks = callback_kinds.map(|(kind, callback_name)| {
        let (raw_callback, make_callback) = callback_constructor(kind);
        quote! {
            #[automatically_derived]
            #lint_attr
            fn #callback_name() -> #raw_callback {
                #make_callback(|lock, args| {
                    #(#unwraps)*
                    #invocation
                    #result_handling
                })
            }
        }
    });

    quote! {
        #fn_vis #fn_sig { #fn_block }
        #(#callbacks)*
    }
    .into()
}

/// The callbacks generated for `fn_name`: `{fn_name}_callback` of shape `kind`, and
/// `{fn_name}_instance_callback` of shape `instance_kind` if there is one.
fn callback_names(
    fn_name: &syn::Ident,
    kind: Callback,
    instance_kind: Option<Callback>,
) -> impl Iterator<Item = (Callback, syn::Ident)> + Clone + '_ {
    std::iter::once((kind, "callback"))
        .chain(instance_kind.map(|kind| (kind, "instance_callback")))
        .map(move |(kind, suffix)| {
            (
                kind,
                syn::Ident::new(&format!("{fn_name}_{suffix}"), fn_name.span()),
            )
        })
}

/// Unwraps JS argument `js_index` into `arg`, or throws and returns.
/// `method_name` is a method's JS name; its errors match C++ JSG's.
fn unwrap_argument(
    ty: &syn::Type,
    arg: &syn::Ident,
    js_index: usize,
    method_name: Option<&str>,
) -> proc_macro2::TokenStream {
    let ty = elide_named_lifetimes(ty);
    let Some(method_name) = method_name else {
        return quote! {
            let #arg = match <#ty as jsg::FromJS<'_>>::from_js(lock, args.get(#js_index)) {
                Ok(v) => v,
                Err(err) => {
                    lock.throw_exception(&err);
                    return;
                }
            };
        };
    };
    // Like C++ JSG (TypeWrapper::unwrap() in type-wrapper.h), a missing
    // required argument or a top-level type mismatch is "Failed to execute
    // ...". Only Option and Lenient arguments may be omitted.
    quote! {
        let not_of_type = || jsg::Error::new_type_error(format!(
            "Failed to execute '{}' on '{}': parameter {} is not of type '{}'.",
            #method_name,
            <Self as jsg::Type>::class_name(),
            #js_index + 1,
            <#ty as jsg::FromJS<'_>>::type_name(),
        ));
        if !<#ty as jsg::FromJS<'_>>::is_optional() && #js_index >= args.len() {
            lock.throw_exception(&not_of_type());
            return;
        }
        let #arg = match <#ty as jsg::FromJS<'_>>::try_from_js(lock, args.get(#js_index)) {
            Ok(Some(v)) => v,
            Ok(None) => {
                lock.throw_exception(&not_of_type());
                return;
            }
            Err(err) => {
                lock.throw_exception(&err);
                return;
            }
        };
    }
}

/// Generates `{name}_fast_api()`, which returns the V8 Fast API callback of a `#[jsg_method]`
/// if its signature allows one.
fn fast_callback(fn_sig: &syn::Signature) -> proc_macro2::TokenStream {
    let fn_name = &fn_sig.ident;
    let descriptor_name = syn::Ident::new(&format!("{fn_name}_fast_api"), fn_name.span());
    let descriptor = fast_api_signature(fn_sig).map_or_else(
        || quote! { None },
        |arg_types| {
            let args: Vec<_> = (0..arg_types.len())
                .map(|i| syn::Ident::new(&format!("a{i}"), fn_name.span()))
                .collect();
            let has_self = fn_sig
                .inputs
                .iter()
                .any(|arg| matches!(arg, FnArg::Receiver(_)));
            if has_self {
                quote! {
                    Some(jsg::fast_api::method(|this: &Self, #(#args: #arg_types),*| this.#fn_name(#(#args),*)))
                }
            } else {
                quote! {
                    Some(jsg::fast_api::static_method(|#(#args: #arg_types),*| Self::#fn_name(#(#args),*)))
                }
            }
        },
    );
    quote! {
        #[automatically_derived]
        #[doc(hidden)]
        fn #descriptor_name() -> Option<jsg::v8::ffi::FastApiDescriptor> {
            #descriptor
        }
    }
}

/// The type of a generated callback, and the `jsg` function that makes one from a closure.
fn callback_constructor(kind: Callback) -> (proc_macro2::TokenStream, proc_macro2::TokenStream) {
    match kind {
        Callback::Function => (
            quote! { jsg::RawFunctionCallback },
            quote! { jsg::function_callback },
        ),
        Callback::Getter => (
            quote! { jsg::RawGetterCallback },
            quote! { jsg::getter_callback },
        ),
        Callback::Setter => (
            quote! { jsg::RawSetterCallback },
            quote! { jsg::setter_callback },
        ),
    }
}

/// Rejects a method receiver other than `&self`. JavaScript can call back into the resource, and
/// GC can trace it, while a method runs, so a method never has exclusive access to its resource.
fn receiver_error(
    input_fn: &ItemFn,
    callbacks: impl Iterator<Item = (Callback, syn::Ident)>,
) -> Option<proc_macro2::TokenStream> {
    let receiver = input_fn.sig.receiver()?;
    // `&self` and `self: &Self` (with any lifetime) are the same receiver; nested forms such as
    // `self: &&Self` are not.
    if matches!(&*receiver.ty, syn::Type::Reference(r)
        if r.mutability.is_none()
            && matches!(&*r.elem, syn::Type::Path(p) if p.qself.is_none() && p.path.is_ident("Self")))
    {
        return None;
    }
    let error = syn::Error::new_spanned(
        receiver,
        "a resource method takes `&self`: JavaScript can call into the resource while the \
         method runs, so the method cannot have exclusive access. Keep state that methods \
         change in `Cell` or `RefCell` fields",
    )
    .to_compile_error();
    // The callbacks are still emitted so that their registration in `#[jsg_resource]` adds no
    // errors of its own, and so is the method, so that its callers add none.
    let callbacks = callbacks.map(|(kind, callback_name)| {
        let (raw_callback, _) = callback_constructor(kind);
        quote! { fn #callback_name() -> #raw_callback { unreachable!() } }
    });
    Some(quote! {
        #error
        #input_fn
        #(#callbacks)*
    })
}

// =============================================================================
// #[jsg_resource]
// =============================================================================

/// Generates JSG boilerplate for a resource type or its impl block.
///
/// **On a struct** — emits `jsg::Type`, `jsg::ToJS`, `jsg::FromJS`,
/// `jsg::Traced`, and `jsg::GarbageCollected`.
///
/// The generated `Traced::trace` body simply delegates to `Traced::trace()` on
/// every named field.
///
/// **On an impl block** — emits `jsg::Resource::members(flags)` registering every
/// `#[jsg_method]`, `#[jsg_property]`, `#[jsg_inspect_property]`,
/// `#[jsg_constructor]`, and `#[jsg_static_constant]` item, plus member factories.
/// With `custom_members`, emits only the factories; implement `jsg::Resource` yourself
/// to select members using ordinary Rust control flow and compatibility flags.
///
/// Use `#[jsg_resource(name = "JSName")]` on the struct to override the default
/// JavaScript class name.
#[proc_macro_attribute]
pub fn jsg_resource(attr: TokenStream, item: TokenStream) -> TokenStream {
    if let Ok(impl_block) = syn::parse::<ItemImpl>(item.clone()) {
        return generate_resource_impl(attr, &impl_block);
    }
    let input = parse_macro_input!(item as DeriveInput);
    generate_resource_struct(attr, &input)
}

// =============================================================================
// #[jsg_static_constant]  (marker only — processed by #[jsg_resource])
// =============================================================================

/// Marks a `const` item inside a `#[jsg_resource]` impl block as a static
/// constant exposed to JavaScript on both the constructor and its prototype.
///
/// The constant name is used as-is (no camelCase conversion), matching the
/// convention that constants are `UPPER_SNAKE_CASE` in both Rust and JavaScript.
/// Only numeric types (`i8`..`i64`, `u8`..`u64`, `f32`, `f64`) are supported.
///
/// ```ignore
/// #[jsg_resource]
/// impl WebSocket {
///     #[jsg_static_constant]
///     pub const CONNECTING: i32 = 0;
/// }
/// // JS: WebSocket.CONNECTING === 0  /  instance.CONNECTING === 0
/// ```
#[proc_macro_attribute]
pub fn jsg_static_constant(_attr: TokenStream, item: TokenStream) -> TokenStream {
    // Marker only — registration is handled by #[jsg_resource] on the impl block.
    item
}

// =============================================================================
// #[jsg_constructor]  (marker only — processed by #[jsg_resource])
// =============================================================================

/// Marks a static method as the JavaScript constructor for a `#[jsg_resource]`.
///
/// The method must have no `self` receiver and must return `Self`.
/// An optional first parameter of `&mut Lock` (or `&mut jsg::Lock`) receives
/// the isolate lock and is not exposed as a JavaScript argument.
///
/// Only one `#[jsg_constructor]` is allowed per impl block. Without it,
/// `new MyResource()` throws `Illegal constructor`, matching C++ JSG behaviour.
///
/// ```ignore
/// #[jsg_resource]
/// impl Greeting {
///     #[jsg_constructor]
///     fn constructor(message: String) -> Self {
///         Self { message }
///     }
/// }
/// // JS: let g = new Greeting("hello");
/// ```
#[proc_macro_attribute]
pub fn jsg_constructor(_attr: TokenStream, item: TokenStream) -> TokenStream {
    // Marker only — registration is handled by #[jsg_resource] on the impl block.
    item
}

/// Registers a method as a JavaScript property getter or setter on a
/// `#[jsg_resource]` type.
///
/// Supported arguments:
/// - `prototype` or `instance` (optional, defaults to `prototype`)
/// - `name = "..."` for an explicit JS property name
/// - `readonly` to require no matching setter
///
/// Methods must start with `get_` (getter) or `set_` (setter). If `name` is
/// omitted, the prefix is stripped and the remainder is converted
/// `snake_case` -> `camelCase`.
///
/// Getters and setters take `&self`; any other receiver is a compile error.
#[proc_macro_attribute]
pub fn jsg_property(_attr: TokenStream, item: TokenStream) -> TokenStream {
    // Registration as a Member is handled by #[jsg_resource] on the enclosing
    // impl block, which also reports malformed arguments.
    let setter = syn::parse::<ItemFn>(item.clone())
        .is_ok_and(|f| f.sig.ident.to_string().starts_with("set_"));
    let instance_kind = if setter {
        Callback::Setter
    } else {
        Callback::Getter
    };
    // Member factories select placement at template construction, so both V8 callback
    // signatures are available regardless of the automatic-registration placement.
    callback_with_instance(item, Callback::Function, Some(instance_kind), None)
}

/// Registers a method as a debug-inspect-only property on a `#[jsg_resource]`
/// type. This maps to `jsg::PropertyKind::Inspect`.
///
/// Optional argument: `name = "..."` to set the symbol description.
///
/// Inspect properties are always read-only; setters are rejected at compile time.
/// The getter takes `&self`; any other receiver is a compile error.
#[proc_macro_attribute]
pub fn jsg_inspect_property(_attr: TokenStream, item: TokenStream) -> TokenStream {
    // Reuse jsg_method callback generation; registration as Member::Property is
    // handled by #[jsg_resource] on the enclosing impl block.
    jsg_method(TokenStream::new(), item)
}

// =============================================================================
// #[jsg_oneof]
// =============================================================================

/// Generates `jsg::Type` and `jsg::FromJS` for a union enum, equivalent to
/// `kj::OneOf<…>` in C++ JSG.
///
/// Each variant must be a single-field tuple variant whose inner type implements
/// `jsg::Type` and `jsg::FromJS`. The macro tries each variant in declaration
/// order using exact-type matching and returns the first that succeeds.
///
/// ```ignore
/// #[jsg_oneof]
/// #[derive(Debug, Clone)]
/// enum StringOrNumber {
///     String(String),
///     Number(jsg::Number),
/// }
/// ```
#[proc_macro_attribute]
pub fn jsg_oneof(_attr: TokenStream, item: TokenStream) -> TokenStream {
    let input = parse_macro_input!(item as DeriveInput);
    let name = &input.ident;

    let Data::Enum(data) = &input.data else {
        return error(&input, "#[jsg_oneof] can only be applied to enums");
    };

    let mut variants = Vec::new();
    for variant in &data.variants {
        let variant_name = &variant.ident;
        let Fields::Unnamed(fields) = &variant.fields else {
            return error(
                variant,
                "#[jsg_oneof] variants must be tuple variants (e.g., `Variant(Type)`)",
            );
        };
        if fields.unnamed.len() != 1 {
            return error(variant, "#[jsg_oneof] variants must have exactly one field");
        }
        let inner_type = &fields.unnamed[0].ty;
        variants.push((variant_name, inner_type));
    }

    if variants.is_empty() {
        return error(&input, "#[jsg_oneof] requires at least one variant");
    }

    let type_checks: Vec<_> = variants
        .iter()
        .map(|(variant_name, inner_type)| {
            quote! {
                if let Some(result) = <#inner_type as jsg::FromJS<'s>>::try_from_js_exact(lock, value) {
                    return result.map(|value| Some(Self::#variant_name(value)));
                }
            }
        })
        .collect();

    let inner_types: Vec<_> = variants.iter().map(|(_, t)| t).collect();
    let type_names: Vec<_> = variants
        .iter()
        .map(|(_, inner_type)| quote! { <#inner_type as jsg::Type>::class_name() })
        .collect();

    let is_exact_checks: Vec<_> = variants
        .iter()
        .map(|(_, inner_type)| quote! { <#inner_type as jsg::Type>::is_exact(value) })
        .collect();

    let error_msg = quote! {
        let expected: Vec<&str> = vec![#(#type_names),*];
        format!(
            "Expected one of [{}] but got {}",
            expected.join(", "),
            type_name
        )
    };

    quote! {
        #input

        #[automatically_derived]
        impl jsg::Type for #name {
            fn class_name() -> &'static str {
                stringify!(#name)
            }

            fn is_exact(value: jsg::v8::Local<'_, jsg::v8::Value>) -> bool {
                #(#is_exact_checks)||*
            }
        }

        #[automatically_derived]
        impl<'s> jsg::FromJS<'s> for #name {
            type ResultType = Self;

            fn type_name() -> String {
                [#(<#inner_types as jsg::FromJS<'s>>::type_name()),*].join(" or ")
            }

            fn from_js(lock: &mut jsg::Lock<'s>, value: jsg::v8::Local<'s, jsg::v8::Value>) -> Result<Self::ResultType, jsg::Error> {
                let type_name = value.type_of(lock);
                Self::try_from_js(lock, value)?.ok_or_else(|| {
                    jsg::Error::new_type_error({ #error_msg })
                })
            }

            fn try_from_js(lock: &mut jsg::Lock<'s>, value: jsg::v8::Local<'s, jsg::v8::Value>) -> Result<Option<Self::ResultType>, jsg::Error> {
                #(#type_checks)*
                Ok(None)
            }
        }
    }
    .into()
}
