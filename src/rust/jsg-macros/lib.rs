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
use utils::error;
use utils::extract_name_attribute;
use utils::extract_named_fields;
use utils::is_lock_ref;
use utils::is_result_type;

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

        field_assignments.push(quote! {
            let #field_name = jsg::v8::ToLocalValue::to_local(&this.#field_name, lock);
            obj.set(lock, #field_name_str, #field_name);
        });
        field_extractions.push(quote! {
            let #field_name = {
                let prop = obj.get(lock, #field_name_str)
                    .ok_or_else(|| jsg::Error::new_type_error(
                        format!("Missing property '{}'", #field_name_str)
                    ))?;
                <#field_type as jsg::FromJS>::from_js(lock, prop)?
            };
        });
        field_names.push(field_name);
    }

    quote! {
        #input

        impl jsg::Type for #name {
            fn class_name() -> &'static str { #class_name }

            fn is_exact(value: &jsg::v8::Local<jsg::v8::Value>) -> bool {
                value.is_object()
            }
        }

        impl jsg::ToJS for #name {
            fn to_js<'a, 'b>(self, lock: &'a mut jsg::Lock) -> jsg::v8::Local<'b, jsg::v8::Value>
            where
                'b: 'a,
            {
                // TODO(soon): Use a precached ObjectTemplate instance to create the object,
                // similar to how C++ JSG optimizes object creation. This would avoid recreating
                // the object shape on every wrap() call and improve performance.
                {
                    let this = self;
                    let mut obj = lock.new_object();
                    #(#field_assignments)*
                    obj.into()
                }
            }
        }

        impl jsg::FromJS for #name {
            type ResultType = Self;

            fn from_js(lock: &mut jsg::Lock, value: jsg::v8::Local<jsg::v8::Value>) -> Result<Self::ResultType, jsg::Error> {
                if !value.is_object() {
                    return Err(jsg::Error::new_type_error(
                        format!("Expected object but got {}", value.type_of())
                    ));
                }
                let obj: jsg::v8::Local<'_, jsg::v8::Object> = value.into();
                #(#field_extractions)*
                Ok(Self { #(#field_names),* })
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
/// Use `#[jsg_method(name = "jsName")]` to override the default `camelCase`
/// conversion of the Rust function name.
#[proc_macro_attribute]
pub fn jsg_method(_attr: TokenStream, item: TokenStream) -> TokenStream {
    let input_fn = parse_macro_input!(item as ItemFn);
    let fn_name = &input_fn.sig.ident;
    let fn_vis = &input_fn.vis;
    let fn_sig = &input_fn.sig;
    let fn_block = &input_fn.block;
    let callback_name = syn::Ident::new(&format!("{fn_name}_callback"), fn_name.span());

    // Methods with a receiver (&self, &mut self) become instance methods on the prototype.
    // Methods without a receiver become static methods on the constructor.
    let has_self = fn_sig
        .inputs
        .iter()
        .any(|arg| matches!(arg, FnArg::Receiver(_)));

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
    let js_arg_offset = usize::from(has_lock_param);

    let (unwraps, arg_exprs): (Vec<_>, Vec<_>) = params
        .iter()
        .enumerate()
        .map(|(i, ty)| {
            // First param is &mut Lock — pass the callback's lock directly.
            if i == 0 && has_lock_param {
                return (quote! {}, quote! { &mut lock });
            }

            let js_index = i - js_arg_offset;
            let arg = syn::Ident::new(&format!("arg{js_index}"), fn_name.span());
            let unwrap = quote! {
                let #arg = match <#ty as jsg::FromJS>::from_js(&mut lock, args.get(#js_index)) {
                    Ok(v) => v,
                    Err(err) => {
                        lock.throw_exception(&err);
                        return;
                    }
                };
            };
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
                Ok(value) => args.set_return_value(jsg::ToJS::to_js(value, &mut lock)),
                Err(err) => lock.throw_exception(&err.into()),
            }
        }
    } else {
        quote! {
            args.set_return_value(jsg::ToJS::to_js(result, &mut lock));
        }
    };

    let invocation = if has_self {
        quote! {
            let this = args.this();
            // SAFETY: `v8::Signature` (passed to `FunctionTemplate::New` in
            // `create_resource_template`) enforces that V8 only dispatches this
            // callback when `this` is an instance of the resource's own
            // `FunctionTemplate`. If the caller destructures the method and calls
            // it with a wrong receiver (e.g. `const {abort} = ac; abort()`), V8
            // throws a `TypeError: Illegal invocation` *before* reaching this
            // code. Given that guarantee, `from_js` / `resolve_resource` perform
            // a belt-and-suspenders `TypeId` check; the `.expect` panics
            // (aborting the isolate) rather than triggering UB on a mismatch.
            // The `&mut` is sound because V8 is single-threaded and no other
            // Rust code can alias the resource during the callback.
            let self_: &mut Self = unsafe {
                let wrappable = jsg::v8::WrappableRc::from_js(lock.isolate(), this)
                    .expect("receiver is not a Rust-wrapped resource");
                &mut *wrappable.resolve_resource::<Self>()
                    .expect("type mismatch in resource callback")
                    .as_ptr()
            };
            let result = self_.#fn_name(#(#arg_exprs),*);
        }
    } else {
        quote! {
            let result = Self::#fn_name(#(#arg_exprs),*);
        }
    };

    quote! {
        #fn_vis #fn_sig { #fn_block }

        #[automatically_derived]
        #[expect(clippy::undocumented_unsafe_blocks)]
        extern "C" fn #callback_name(args: *mut jsg::v8::ffi::FunctionCallbackInfo) {
            let mut lock = unsafe { jsg::Lock::from_args(args) };
            jsg::catch_panic(&mut lock, || {
                let mut lock = unsafe { jsg::Lock::from_args(args) };
                let mut args = unsafe { jsg::v8::FunctionCallbackInfo::from_ffi(args) };
                #(#unwraps)*
                #invocation
                #result_handling
            });
        }
    }
    .into()
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
/// **On an impl block** — emits `jsg::Resource::members()` registering every
/// `#[jsg_method]`, `#[jsg_property]`, `#[jsg_inspect_property]`,
/// `#[jsg_constructor]`, and `#[jsg_static_constant]` item.
///
/// Use `#[jsg_resource(name = "JSName")]` on the struct to override the default
/// JavaScript class name.
#[proc_macro_attribute]
pub fn jsg_resource(attr: TokenStream, item: TokenStream) -> TokenStream {
    if let Ok(impl_block) = syn::parse::<ItemImpl>(item.clone()) {
        return generate_resource_impl(&impl_block);
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
#[proc_macro_attribute]
pub fn jsg_property(_attr: TokenStream, item: TokenStream) -> TokenStream {
    // Reuse jsg_method callback generation; registration as Member::Property is
    // handled by #[jsg_resource] on the enclosing impl block.
    jsg_method(TokenStream::new(), item)
}

/// Registers a method as a debug-inspect-only property on a `#[jsg_resource]`
/// type. This maps to `jsg::PropertyKind::Inspect`.
///
/// Optional argument: `name = "..."` to set the symbol description.
///
/// Inspect properties are always read-only; setters are rejected at compile time.
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
                if let Some(result) = <#inner_type as jsg::FromJS>::try_from_js_exact(lock, &value) {
                    return result.map(Self::#variant_name);
                }
            }
        })
        .collect();

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
        let msg = format!(
            "Expected one of [{}] but got {}",
            expected.join(", "),
            value.type_of()
        );
        Err(jsg::Error::new_type_error(msg))
    };

    quote! {
        #input

        #[automatically_derived]
        impl jsg::Type for #name {
            fn class_name() -> &'static str {
                stringify!(#name)
            }

            fn is_exact(value: &jsg::v8::Local<jsg::v8::Value>) -> bool {
                #(#is_exact_checks)||*
            }
        }

        #[automatically_derived]
        impl jsg::FromJS for #name {
            type ResultType = Self;

            fn from_js(lock: &mut jsg::Lock, value: jsg::v8::Local<jsg::v8::Value>) -> Result<Self::ResultType, jsg::Error> {
                #(#type_checks)*
                #error_msg
            }
        }
    }
    .into()
}
