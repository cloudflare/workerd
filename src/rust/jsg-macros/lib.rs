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
//! | `#[jsg_wildcard_property]` | fn inside impl | Answer reads of any property the object does not have          |
//! | `#[jsg_callable]`        | fn inside impl   | Make instances callable as functions                           |
//! | `#[jsg_struct]`          | struct           | Expose a Rust struct as a plain JavaScript object              |
//! | `#[jsg_oneof]`           | enum             | Accept one of several JavaScript types (`kj::OneOf`)           |
//!
//! See [`jsg/README.md`](../jsg/README.md) for full usage documentation.

#![forbid(unsafe_code)]

mod memory_info;
mod oneof;
mod resource;
mod trace;
mod utils;
mod wildcard;

use proc_macro::TokenStream;
use quote::quote;
use resource::generate_resource_impl;
use resource::generate_resource_struct;
use syn::DeriveInput;
use syn::FnArg;
use syn::ItemFn;
use syn::ItemImpl;
use syn::parse_macro_input;
use trace::generate_trace_statements;
use utils::arguments_element;
use utils::elide_named_lifetimes;
use utils::extract_name_attribute;
use utils::extract_named_fields;
use utils::fast_api_signature;
use utils::has_flag;
use utils::is_attr;
use utils::is_lock_ref;
use utils::is_named_this;
use utils::is_null_or_undefined_type;
use utils::is_option_type;
use utils::is_promise_type;
use utils::is_rc_self;
use utils::is_result_type;
use utils::snake_to_camel;
use utils::this_param;

// =============================================================================
// #[jsg_struct]
// =============================================================================

/// Generates `jsg::Struct`, `jsg::Type`, `jsg::ToJS`, `jsg::FromJS` and `jsg::Traced`
/// implementations for a plain data struct.
///
/// Only `pub` fields are projected into the JavaScript object. Each converts with its `ToJS`
/// impl, and a field whose conversion fails fails the struct's conversion with that error.
/// The generated `Traced` impl visits every field, so a field can be a `jsg::Rc<R>`.
/// Optional fields use `Option<T>` or `jsg::NullOrUndefined<T>`; prefer the latter to
/// `Option<jsg::Nullable<T>>` to distinguish `null` from `undefined`. A field that holds a
/// JavaScript value, as a C++ `jsg::JsRef<T>` field does, is a `jsg::v8::TracedGlobal<M>`;
/// a resource that moves such a struct out of a field keeps it in a `jsg::Cell` or
/// `jsg::RefCell`, which make the handle strong before it leaves.
/// Use `#[jsg_struct(name = "MyName")]` to override `Type::class_name()`
/// metadata and the TypeScript declaration name; it does not define a JS class.
#[proc_macro_attribute]
pub fn jsg_struct(attr: TokenStream, item: TokenStream) -> TokenStream {
    let input = parse_macro_input!(item as DeriveInput);
    let name = &input.ident;
    let class_name = extract_name_attribute(attr.clone()).unwrap_or_else(|| name.to_string());

    let named_fields = match extract_named_fields(&input, "jsg_struct") {
        Ok(fields) => fields,
        Err(err) => return err,
    };
    let rtti = describe_struct(name, &class_name, has_flag(&attr, "ts_root"), &named_fields);

    let mut to_js_fields = Vec::new();
    let mut field_extractions = Vec::new();
    let mut field_names = Vec::new();
    let mut all_optional = true;
    let custom_marker = has_flag(&attr, "custom_holds_no_gc_references");
    let traced_impl = struct_traced_impl(name, &named_fields, custom_marker);

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
        let present = if is_option_type(field_type) {
            Some(quote! { this.#field_name.is_some() })
        } else if is_null_or_undefined_type(field_type) {
            Some(quote! { !this.#field_name.is_undefined() })
        } else {
            None
        };
        all_optional &= present.is_some();

        to_js_fields.push((field_name, field_name_str.clone(), present));
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
        quote! { return Ok(Self { #(#field_names: Default::default()),* }); }
    } else {
        quote! {
            return Err(jsg::Error::new_type_error(format!(
                "Cannot initialize {} with required members from an undefined or null value.",
                #class_name,
            )));
        }
    };

    let to_js_body = to_js_fn_body(&to_js_fields);

    quote! {
        #input

        impl jsg::Type for #name {
            // A Web IDL dictionary type, like a C++ `JSG_STRUCT`.
            const UNION_TYPES: jsg::UnionTypes = jsg::UnionTypes::DICTIONARY;

            fn class_name() -> &'static str { #class_name }

            fn is_exact(value: jsg::v8::Local<'_, jsg::v8::Value>) -> bool {
                value.is_object()
            }
        }

        impl<'s> jsg::ToJS<'s> for #name {
            fn to_js(self, lock: &mut jsg::Lock<'s>) -> Result<jsg::v8::Local<'s, jsg::v8::Value>, jsg::Error> {
                jsg::ToJS::to_js_with_creator(self, lock, None)
            }

            // As C++ JSG's `JSG_STRUCT` does, passes `creator` on to the fields.
            fn to_js_with_creator(
                self,
                lock: &mut jsg::Lock<'s>,
                creator: Option<jsg::v8::Local<'s, jsg::v8::Object>>,
            ) -> Result<jsg::v8::Local<'s, jsg::v8::Value>, jsg::Error> {
                #to_js_body
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
                let Ok(obj) = jsg::v8::Local::<jsg::v8::Object>::try_from(value) else {
                    return Err(jsg::Error::new_type_error(
                        format!("Expected object but got {}", value.type_of(lock))
                    ));
                };
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

        #rtti

        #traced_impl
    }
    .into()
}

/// Generates the `Traced` impl of a `#[jsg_struct]`, which visits every field, and, unless
/// `custom_holds_no_gc_references` leaves it to the author, its `HoldsNoGcReferences` impl, which
/// applies when every field's type implements it.
fn struct_traced_impl(
    name: &syn::Ident,
    fields: &syn::punctuated::Punctuated<syn::Field, syn::token::Comma>,
    custom_holds_no_gc_references: bool,
) -> proc_macro2::TokenStream {
    let field_types = fields.iter().map(|field| &field.ty);
    let trace_statements = generate_trace_statements(fields);
    let marker_impl = (!custom_holds_no_gc_references).then(|| {
        quote! {
            // Higher-ranked, so that a field that holds a GC reference leaves the struct without the
            // impl instead of failing to compile as a trivially false bound.
            #[automatically_derived]
            impl jsg::HoldsNoGcReferences for #name
            where
                #(for<'__jsgl> #field_types: jsg::HoldsNoGcReferences,)*
            {
            }
        }
    });
    quote! {
        #[automatically_derived]
        impl jsg::Traced for #name {
            fn trace(&self, visitor: &mut jsg::GcVisitor) {
                let _ = visitor;
                #(#trace_statements)*
            }
        }

        #marker_impl
    }
}

/// The body of a `#[jsg_struct]`'s `ToJS::to_js`, given each public field's name, its
/// JavaScript name and the condition for its present state, if optional. Like C++ JSG's
/// `JSG_STRUCT`, it follows the `fast_jsg_struct` flag.
fn to_js_fn_body(
    fields: &[(&syn::Ident, String, Option<proc_macro2::TokenStream>)],
) -> proc_macro2::TokenStream {
    // A DictionaryTemplate instance has every field, so an absent optional field is undefined.
    let data_properties = fields.iter().map(|(field_name, js_name, _)| {
        quote! {
            properties.push((
                #js_name,
                jsg::ToJS::to_js_with_creator(this.#field_name, lock, creator)?,
            ));
        }
    });
    // Set() skips an absent optional field rather than setting it to undefined.
    let field_sets = fields.iter().map(|(field_name, js_name, present)| {
        let set = quote! {
            let value = jsg::ToJS::to_js_with_creator(value, lock, creator)?;
            obj.set(lock, #js_name, value)?;
        };
        if let Some(present) = present {
            quote! {
                if #present {
                    let value = this.#field_name;
                    #set
                }
            }
        } else {
            quote! { { let value = this.#field_name; #set } }
        }
    });
    quote! {
        let this = self;
        // Unused by a struct without public fields.
        let _ = creator;
        if lock.feature_flags().get_fast_jsg_struct() {
            // The fields are defined as data properties, as C++ JSG's DictionaryTemplate does, so
            // no setter a script put on Object.prototype runs.
            // TODO(soon): Use a precached template to create the object, as C++ JSG does, rather
            // than recreating its shape on every conversion.
            #[allow(unused_mut, reason = "a struct without public fields pushes nothing")]
            let mut properties = ::std::vec::Vec::new();
            #(#data_properties)*
            Ok(jsg::v8::Object::with_data_properties(lock, &properties)?.into())
        } else {
            // Without fast_jsg_struct, C++ JSG sets the fields one by one, which runs a setter on
            // Object.prototype; what it throws is passed on.
            let obj = lock.new_object();
            #(#field_sets)*
            Ok(obj.into())
        }
    }
}

fn describe_struct(
    name: &syn::Ident,
    class_name: &str,
    ts_root: bool,
    fields: &syn::punctuated::Punctuated<syn::Field, syn::token::Comma>,
) -> proc_macro2::TokenStream {
    let members = fields.iter().filter_map(|field| {
        if !matches!(field.vis, syn::Visibility::Public(_)) {
            return None;
        }
        let name = field.ident.as_ref()?.to_string();
        let ty = &field.ty;
        Some(quote! {
            jsg::rtti::Member::Property {
                name: #name,
                ty: <#ty as jsg::TypeInfo>::describe(cx),
                readonly: false,
                prototype: false,
                lazy: false,
            }
        })
    });
    quote! {
        #[automatically_derived]
        impl jsg::TypeInfo for #name {
            fn describe(cx: &mut jsg::rtti::Context) -> jsg::rtti::Type {
                cx.structure::<Self>(#class_name, #ts_root, |cx| {
                    let _ = &cx;
                    vec![#(#members),*]
                })
            }
        }
    }
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
/// Instance methods take `&self`, or `this: jsg::Rc<Self>` as their first parameter in
/// place of it, which receives a `jsg::Rc` of the resource the method was called on, like
/// C++ JSG's `JSG_THIS`. Any other receiver is a compile error, and so is a `this` parameter
/// anywhere else or of another type.
///
/// Use `#[jsg_method(name = "jsName")]` to override the default `camelCase`
/// conversion of the Rust function name.
#[proc_macro_attribute]
pub fn jsg_method(attr: TokenStream, item: TokenStream) -> TokenStream {
    let input_fn = parse_macro_input!(item as ItemFn);
    let fast = fast_callback(&input_fn.sig);
    let mut output = callback(
        quote! { #input_fn }.into(),
        Callback::Function,
        Some(attr),
        ReceiverCheck::Signature,
    );
    output.extend(TokenStream::from(fast));
    output
}

/// The V8 callback shape a generated callback takes.
#[derive(Clone, Copy)]
enum Callback {
    Function,
    /// `AccessorNameGetterCallback`, for instance and lazy instance properties.
    Getter,
    /// `AccessorNameSetterCallbackV2`, for instance properties.
    Setter,
}

/// How a `&self` callback finds its receiver's resource.
#[derive(Clone, Copy)]
enum ReceiverCheck {
    /// The callback is registered with the type's `v8::Signature`, which V8 checks before
    /// calling it.
    Signature,
    /// The callback has no signature, so the receiver can be any value.
    Marker,
}

/// `method_attr` is a method's `#[jsg_method]` arguments; argument errors for
/// methods match C++ JSG's.
fn callback(
    item: TokenStream,
    kind: Callback,
    method_attr: Option<TokenStream>,
    receiver_check: ReceiverCheck,
) -> TokenStream {
    callback_with_instance(item, kind, None, method_attr, receiver_check)
}

fn callback_with_instance(
    item: TokenStream,
    kind: Callback,
    instance_kind: Option<Callback>,
    method_attr: Option<TokenStream>,
    receiver_check: ReceiverCheck,
) -> TokenStream {
    let mut input_fn = parse_macro_input!(item as ItemFn);
    // A disposer's callback has the name this callback has. It is this one for a method that is
    // also a disposer, and the combination is an error for any other member, so the disposer's
    // attribute must not generate a second one if it comes later. `#[jsg_resource]` has already
    // read it.
    input_fn.attrs.retain(|a| !is_disposer_attr(a));
    let fn_name = &input_fn.sig.ident;
    let method_name = method_attr.map(|attr| {
        extract_name_attribute(attr).unwrap_or_else(|| snake_to_camel(&fn_name.to_string()))
    });
    let callbacks = match generate_callbacks(
        &input_fn,
        callback_names(fn_name, kind, instance_kind),
        method_name.as_deref(),
        receiver_check,
    ) {
        Ok(callbacks) => callbacks,
        Err(error) => return error.into(),
    };
    // The method keeps its other attributes, so that a `#[jsg_callable]` after the attribute
    // being expanded still expands.
    let method = reemit_method(&input_fn);
    quote! {
        #method
        #callbacks
    }
    .into()
}

/// Re-emits `input_fn` with its other attributes. The body is wrapped in braces spanned at the
/// macro call, as the methods have always been emitted, so that clippy treats the method as
/// macro output and does not lint the signature against its generated callbacks.
fn reemit_method(input_fn: &ItemFn) -> proc_macro2::TokenStream {
    let ItemFn {
        attrs,
        vis,
        sig,
        block,
    } = input_fn;
    quote! { #(#attrs)* #vis #sig { #block } }
}

/// Generates the callbacks named in `callback_kinds`, each calling `input_fn`, or returns the
/// method with an error and stubs of the callbacks (see [`reject`]). `method_name` is a method's
/// JS name; its errors match C++ JSG's.
fn generate_callbacks(
    input_fn: &ItemFn,
    callback_kinds: impl Iterator<Item = (Callback, syn::Ident)> + Clone,
    method_name: Option<&str>,
    receiver_check: ReceiverCheck,
) -> Result<proc_macro2::TokenStream, proc_macro2::TokenStream> {
    let fn_name = &input_fn.sig.ident;
    let fn_sig = &input_fn.sig;

    // Methods with a `&self` receiver, or a `this: jsg::Rc<Self>` parameter in its place, become
    // instance methods on the prototype. Methods with neither become static methods on the
    // constructor.
    if let Some(error) = receiver_error(input_fn, callback_kinds.clone()) {
        return Err(error);
    }
    if let Some(error) = this_error(input_fn, callback_kinds.clone()) {
        return Err(error);
    }
    if let Some(error) = rest_parameter_error(fn_sig) {
        return Err(reject(input_fn, callback_kinds, &error));
    }
    let has_self = fn_sig.receiver().is_some();
    let has_this = this_param(fn_sig).is_some();
    let receiver = if has_self {
        Receiver::SelfRef
    } else if has_this {
        Receiver::This
    } else {
        Receiver::None
    };

    let params: Vec<_> = fn_sig
        .inputs
        .iter()
        .filter_map(|arg| match arg {
            FnArg::Typed(pat_type) => Some(&pat_type.ty),
            FnArg::Receiver(_) => None,
        })
        .collect();

    // The parameters the callback fills in itself come first: `this`, then `&mut Lock` (like
    // C++ jsg::Lock&). Only the parameters after them are JavaScript arguments.
    let this_params = usize::from(has_this);
    let has_lock_param = params.get(this_params).is_some_and(|ty| is_lock_ref(ty));
    let injected_params = this_params + usize::from(has_lock_param);

    let (unwraps, arg_exprs): (Vec<_>, Vec<_>) = params
        .iter()
        .enumerate()
        .map(|(i, ty)| {
            if i == 0 && has_this {
                return (quote! {}, quote! { this });
            }
            if i == this_params && has_lock_param {
                return (quote! {}, quote! { &mut *lock });
            }

            let js_index = i - injected_params;
            let arg = syn::Ident::new(&format!("arg{js_index}"), fn_name.span());
            let unwrap = unwrap_argument(ty, &arg, js_index, method_name);
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

    let result_handling = result_handling(fn_sig, receiver);

    let invocation = invocation(fn_name, receiver, receiver_check, &arg_exprs);

    // Comments do not survive into the expansion, so the SAFETY comment on the `unsafe` block
    // that borrows the resource cannot satisfy the lint.
    let lint_attr = if has_self {
        quote! { #[expect(clippy::undocumented_unsafe_blocks)] }
    } else {
        quote! {}
    };
    let body = quote! {
        #(#unwraps)*
        #invocation
        #result_handling
    };
    // Like C++ JSG, a callback that returns a promise rejects it with what it throws, argument
    // conversion errors included, under `capture_async_api_throws`.
    let returns_promise =
        matches!(&fn_sig.output, syn::ReturnType::Type(_, ty) if is_promise_type(ty));
    let body = if returns_promise {
        quote! {
            jsg::capture_throws_as_rejection(
                lock,
                args,
                |lock, args| { #body },
                |args, promise| args.set_return_value(promise),
            );
        }
    } else {
        body
    };
    let callbacks = callback_kinds.map(|(kind, callback_name)| {
        let (raw_callback, make_callback) = callback_constructor(kind);
        quote! {
            #[automatically_derived]
            #lint_attr
            fn #callback_name() -> #raw_callback {
                #make_callback(|lock, args| {
                    #body
                })
            }
        }
    });

    Ok(quote! { #(#callbacks)* })
}

/// The statements that convert the call's `result` and return it to JavaScript, or throw what
/// the method or the conversion failed with.
fn result_handling(fn_sig: &syn::Signature, receiver: Receiver) -> proc_macro2::TokenStream {
    // Check if return type is Result<T, E>
    let is_result = matches!(&fn_sig.output, syn::ReturnType::Type(_, ty) if is_result_type(ty));

    // Like C++ JSG, an instance method or property passes its receiver as the `creator` of the
    // value it returns, which a promise keeps alive until it settles; a static one passes none.
    let creator = match receiver {
        Receiver::SelfRef | Receiver::This => {
            quote! { jsg::v8::Local::<jsg::v8::Object>::try_from(args.this()).ok() }
        }
        Receiver::None => quote! { None },
    };
    let conversion = if is_result {
        quote! {
            match result {
                Ok(value) => jsg::ToJS::to_js_with_creator(value, lock, creator),
                Err(err) => Err(err.into()),
            }
        }
    } else {
        quote! { jsg::ToJS::to_js_with_creator(result, lock, creator) }
    };
    quote! {
        let creator: Option<jsg::v8::Local<'_, jsg::v8::Object>> = #creator;
        let converted: ::std::result::Result<_, jsg::Error> = #conversion;
        match converted {
            Ok(value) => args.set_return_value(value),
            Err(err) => lock.throw_exception(&err),
        }
    }
}

/// The statements that call `fn_name` with `arg_exprs` and bind the call's `result`: on the
/// resource of `this` for a `&self` method, with a `jsg::Rc` of it for a `this` method, and as
/// an associated function otherwise.
fn invocation(
    fn_name: &syn::Ident,
    receiver: Receiver,
    receiver_check: ReceiverCheck,
    arg_exprs: &[proc_macro2::TokenStream],
) -> proc_macro2::TokenStream {
    // `resolve_method_receiver` is sound only for callbacks registered with a signature.
    let resolve = match receiver_check {
        ReceiverCheck::Signature => {
            quote! { unsafe { jsg::v8::WrappableRc::resolve_method_receiver::<Self>(lock, this) } }
        }
        ReceiverCheck::Marker => {
            quote! { jsg::v8::WrappableRc::resolve_receiver::<Self>(lock, this) }
        }
    };
    match receiver {
        Receiver::SelfRef => quote! {
            let this = args.this();
            // A callback without a signature may receive a wrong receiver. Throw V8's
            // "Illegal invocation" rather than panic, which would terminate execution.
            let resource = #resolve;
            let Some(resource) = resource else {
                lock.throw_exception(&jsg::Error::new_type_error("Illegal invocation"));
                return;
            };
            // SAFETY: `resource` points at the `Self` of `this`, whose wrapper keeps it alive
            // during the call. Resources are only ever borrowed shared, so this may alias the
            // borrow of a method that called into JavaScript, or of GC tracing.
            let self_: &Self = unsafe { resource.as_ref() };
            let result = self_.#fn_name(#(#arg_exprs),*);
        },
        Receiver::This => quote! {
            // Converts the JS `this` with `FromJS`, which only succeeds for a wrapper of this
            // resource type, so the `jsg::Rc` always points into jsg's own allocation. Any
            // other `this` throws the same `TypeError` as for `&self` methods.
            let this = match <jsg::Rc<Self> as jsg::FromJS<'_>>::try_from_js(lock, args.this()) {
                Ok(Some(this)) => this,
                Ok(None) => {
                    lock.throw_exception(&jsg::Error::new_type_error("Illegal invocation"));
                    return;
                }
                Err(err) => {
                    lock.throw_exception(&err);
                    return;
                }
            };
            let result = Self::#fn_name(#(#arg_exprs),*);
        },
        Receiver::None => quote! {
            let result = Self::#fn_name(#(#arg_exprs),*);
        },
    }
}

/// How a generated callback reaches the method's resource, if the method has one.
#[derive(Clone, Copy)]
enum Receiver {
    /// `&self`: the callback borrows the resource of `this`.
    SelfRef,
    /// `this: jsg::Rc<Self>`: the callback makes a `jsg::Rc` of the resource of `this`.
    This,
    /// A static method.
    None,
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

/// Unwraps JS argument `js_index` into `arg`, or throws and returns. A `jsg::Arguments<T>`
/// parameter takes the arguments from `js_index` on instead, each unwrapped as a `T`.
/// `method_name` is a method's JS name; its errors match C++ JSG's.
pub(crate) fn unwrap_argument(
    ty: &syn::Type,
    arg: &syn::Ident,
    js_index: usize,
    method_name: Option<&str>,
) -> proc_macro2::TokenStream {
    let Some(element) = arguments_element(ty) else {
        return unwrap_value(ty, arg, &quote! { #js_index }, js_index, method_name);
    };
    // Like C++ JSG (TypeWrapper::unwrap() in type-wrapper.h), each remaining argument is
    // unwrapped as a parameter at the rest parameter's position would be, so an error names that
    // position; there are none to miss.
    let value = syn::Ident::new("value", arg.span());
    let unwrap = unwrap_value(element, &value, &quote! { index }, js_index, method_name);
    quote! {
        let mut values = Vec::with_capacity(args.len().saturating_sub(#js_index));
        for index in #js_index..args.len() {
            #unwrap
            values.push(#value);
        }
        let #arg = jsg::Arguments::from(values);
    }
}

/// Unwraps the JS argument at `index`, an expression, into `arg` as a `ty`, or throws and
/// returns. `js_index` is the parameter's position, which errors name.
fn unwrap_value(
    ty: &syn::Type,
    arg: &syn::Ident,
    index: &proc_macro2::TokenStream,
    js_index: usize,
    method_name: Option<&str>,
) -> proc_macro2::TokenStream {
    let ty = elide_named_lifetimes(ty);
    let Some(method_name) = method_name else {
        return quote! {
            let #arg = match <#ty as jsg::FromJS<'_>>::from_js(lock, args.get(#index)) {
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
    // ...". FromJS::is_optional() determines whether an argument may be omitted.
    quote! {
        let not_of_type = || jsg::Error::new_type_error(format!(
            "Failed to execute '{}' on '{}': parameter {} is not of type '{}'.",
            #method_name,
            <Self as jsg::Type>::class_name(),
            #js_index + 1,
            <#ty as jsg::FromJS<'_>>::type_name(),
        ));
        if !<#ty as jsg::FromJS<'_>>::is_optional() && #index >= args.len() {
            lock.throw_exception(&not_of_type());
            return;
        }
        let #arg = match <#ty as jsg::FromJS<'_>>::try_from_js(lock, args.get(#index)) {
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

/// Returns an error if a `jsg::Arguments<T>` parameter of `sig` is not its last parameter: it
/// takes every argument from its position on, so a parameter after it would get none.
pub(crate) fn rest_parameter_error(sig: &syn::Signature) -> Option<syn::Error> {
    let mut params = sig.inputs.iter().filter_map(|arg| match arg {
        FnArg::Typed(param) => Some(param),
        FnArg::Receiver(_) => None,
    });
    let rest = params.find(|param| arguments_element(&param.ty).is_some())?;
    params.next().is_some().then(|| {
        syn::Error::new_spanned(
            rest,
            "a `jsg::Arguments` parameter takes the remaining arguments, so it is the last \
             parameter",
        )
    })
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
    );
    Some(reject(input_fn, callbacks, &error))
}

/// Rejects a `this` parameter that is not the first parameter of a method without `&self`, or
/// whose type is not `jsg::Rc<Self>`. The parameter is the resource the method was called on,
/// and a method has one receiver.
fn this_error(
    input_fn: &ItemFn,
    callbacks: impl Iterator<Item = (Callback, syn::Ident)>,
) -> Option<proc_macro2::TokenStream> {
    let named_this = input_fn.sig.inputs.iter().find(|arg| is_named_this(arg))?;
    if this_param(&input_fn.sig).is_some_and(|param| is_rc_self(&param.ty)) {
        return None;
    }
    let error = syn::Error::new_spanned(
        named_this,
        "`this` must be the first parameter, instead of `&self`, and have type `jsg::Rc<Self>`",
    );
    Some(reject(input_fn, callbacks, &error))
}

/// Emits `error` for a method the macro rejects, along with the method itself and stubs of its
/// callbacks, so that neither the method's callers nor its registration in `#[jsg_resource]`
/// add errors of their own.
fn reject(
    input_fn: &ItemFn,
    callbacks: impl Iterator<Item = (Callback, syn::Ident)>,
    error: &syn::Error,
) -> proc_macro2::TokenStream {
    let error = error.to_compile_error();
    let callbacks = callbacks.map(|(kind, callback_name)| {
        let (raw_callback, _) = callback_constructor(kind);
        quote! { fn #callback_name() -> #raw_callback { unreachable!() } }
    });
    quote! {
        #error
        #input_fn
        #(#callbacks)*
    }
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
/// With `memory_info`, it also emits `jsg::VisitForMemoryInfo`, reporting every named field
/// under its name except those marked `#[jsg_memory_info(skip)]`; with `custom_memory_info`,
/// the author implements it. Either makes `GarbageCollected::memory_info` call it.
///
/// **On an impl block** — emits `jsg::Resource::members(registry, flags)` registering every
/// `#[jsg_method]`, `#[jsg_property]`, `#[jsg_inspect_property]`, `#[jsg_wildcard_property]`,
/// `#[jsg_dispose]`, `#[jsg_async_dispose]`, `#[jsg_callable]`, `#[jsg_constructor]`, and
/// `#[jsg_static_constant]` item, plus member factories, which also describe their members for
/// RTTI.
/// With `custom_members`, emits only the factories; implement `jsg::Resource` yourself
/// to select members using ordinary Rust control flow and compatibility flags.
///
/// Use `#[jsg_resource(nested_types(Foo, Name = Bar))]` on the impl block to
/// expose other resource types' constructors on the prototype, like C++ JSG's
/// `JSG_NESTED_TYPE(Foo)` and `JSG_NESTED_TYPE_NAMED(Bar, Name)`. A nested type
/// without a name is named after its JavaScript class name. See
/// `src/rust/jsg/README.md` for how nested types behave.
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
/// The method must have no `self` receiver and must return `Self` or
/// `Result<Self, E>`, where `E: Into<jsg::Error>`; an `Err` is thrown to the
/// JavaScript caller. An optional first parameter of `&mut Lock` (or `&mut jsg::Lock`) receives
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
/// - `prototype`, `instance` or `lazy_instance` (optional, defaults to `prototype`)
/// - `name = "..."` for an explicit JS property name
/// - `readonly` to require no matching setter; on a `lazy_instance` property it
///   also makes the stored value read-only
///
/// Methods must start with `get_` (getter) or `set_` (setter). If `name` is
/// omitted, the prefix is stripped and the remainder is converted
/// `snake_case` -> `camelCase`.
///
/// Getters and setters take `&self`, or `this: jsg::Rc<Self>` as their first parameter in
/// place of it; any other receiver is a compile error.
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
    callback_with_instance(
        item,
        Callback::Function,
        Some(instance_kind),
        None,
        ReceiverCheck::Marker,
    )
}

/// Registers a method as `prototype[Symbol.dispose]` of a `#[jsg_resource]` type, which
/// `using` calls when the variable goes out of scope, like C++ JSG's `JSG_DISPOSE`.
///
/// The method takes `&self`, or `this: jsg::Rc<Self>` as its first parameter in place of it,
/// and is called like a `#[jsg_method]`. Combined with `#[jsg_method]` on the same function, it
/// is also exposed under its own name, like `JSG_METHOD(close); JSG_DISPOSE(close);`. A type has
/// at most one, and may also have a `#[jsg_async_dispose]`. See `src/rust/jsg/README.md` for
/// how to write one.
#[proc_macro_attribute]
pub fn jsg_dispose(attr: TokenStream, item: TokenStream) -> TokenStream {
    disposer("jsg_dispose", attr, item)
}

/// Registers a method as `prototype[Symbol.asyncDispose]` of a `#[jsg_resource]` type, like C++
/// JSG's `JSG_ASYNC_DISPOSE`.
///
/// `await using` calls it when the variable goes out of scope and awaits what it returns, which
/// should be a promise (`jsg::Promise<'s, T>` or `jsg::v8::Local<'s, jsg::v8::Promise>`). As
/// `#[jsg_dispose]` otherwise. Prefer `#[jsg_dispose]` unless disposal has to wait for I/O.
#[proc_macro_attribute]
pub fn jsg_async_dispose(attr: TokenStream, item: TokenStream) -> TokenStream {
    disposer("jsg_async_dispose", attr, item)
}

/// Whether `attr` is `#[jsg_dispose]` or `#[jsg_async_dispose]`.
fn is_disposer_attr(attr: &syn::Attribute) -> bool {
    is_attr(attr, "jsg_dispose") || is_attr(attr, "jsg_async_dispose")
}

/// Generates the callback of a `#[jsg_dispose]` or `#[jsg_async_dispose]` method, which
/// `#[jsg_resource]` registers. A method that is also a `#[jsg_method]` gets its callback from
/// that attribute instead: an attribute that generates a `<name>_callback` (`#[jsg_method]`,
/// `#[jsg_property]`, `#[jsg_inspect_property]`) drops the disposer attributes when it expands
/// first, and when it comes second it is still on the method here.
fn disposer(attr_name: &str, attr: TokenStream, item: TokenStream) -> TokenStream {
    if !attr.is_empty() {
        let mut output: TokenStream = syn::Error::new_spanned(
            proc_macro2::TokenStream::from(attr),
            format!("#[{attr_name}] takes no arguments"),
        )
        .to_compile_error()
        .into();
        output.extend(disposer(attr_name, TokenStream::new(), item));
        return output;
    }
    let mut input_fn = parse_macro_input!(item as ItemFn);
    // Another member attribute generates the callback, or the combination is an error that
    // `#[jsg_resource]` reports; either way this one generates none.
    let combined = input_fn.attrs.iter().any(|a| {
        is_attr(a, "jsg_method")
            || resource::NON_DISPOSER_MEMBERS
                .iter()
                .any(|name| is_attr(a, name))
    });
    if combined {
        return quote! { #input_fn }.into();
    }
    // A method that is both kinds of disposer is an error that `#[jsg_resource]` reports, so the
    // other disposer attribute must not generate the callback again.
    input_fn.attrs.retain(|a| !is_disposer_attr(a));
    callback(
        quote! { #input_fn }.into(),
        Callback::Function,
        Some(TokenStream::new()),
        ReceiverCheck::Signature,
    )
}

/// Registers a method as a debug-inspect-only property on a `#[jsg_resource]`
/// type. This maps to `jsg::PropertyKind::Inspect`.
///
/// Optional argument: `name = "..."` to set the symbol description.
///
/// Inspect properties are always read-only; setters are rejected at compile time.
/// The getter takes `&self`, or `this: jsg::Rc<Self>` as its first parameter in place of
/// it; any other receiver is a compile error.
#[proc_macro_attribute]
pub fn jsg_inspect_property(_attr: TokenStream, item: TokenStream) -> TokenStream {
    // Reuse jsg_method callback generation, without a signature: registration as
    // Member::Property is handled by #[jsg_resource] on the enclosing impl block.
    callback(
        item,
        Callback::Function,
        Some(TokenStream::new()),
        ReceiverCheck::Marker,
    )
}

/// Registers a method as the wildcard property of a `#[jsg_resource]` type.
///
/// Equivalent to C++ JSG's `JSG_WILDCARD_PROPERTY`: reading a string-named property that
/// neither the object nor its prototypes have calls the method with the name, and returns what
/// it returns.
///
/// The method takes `&self`, optionally `lock: &mut jsg::Lock`, and the name as a `String` or
/// `&str`, and returns `Option<T>` or `Result<Option<T>, E>` with `T: ToJS`. `None` means there
/// is no such property, so the read gives `undefined`. The method also answers `name in obj`
/// and assignment, so it must not have side effects. A type has at most one wildcard property.
/// See `src/rust/jsg/README.md` for how wildcard properties behave.
#[proc_macro_attribute]
pub fn jsg_wildcard_property(attr: TokenStream, item: TokenStream) -> TokenStream {
    // Registration as Member::WildcardProperty is handled by #[jsg_resource] on the enclosing
    // impl block.
    wildcard::expand(attr, item)
}

// =============================================================================
// #[jsg_callable]
// =============================================================================

/// The name that errors from converting the arguments of a `#[jsg_callable]` method give the
/// method, as C++ JSG's `registerCallable()` does.
const CALLED_AS_FUNCTION: &str = "(called as function)";

/// Makes the instances of a `#[jsg_resource]` type callable as functions, running the method.
///
/// Equivalent to C++ JSG's `JSG_CALLABLE`. The method takes `&self`, or `this: jsg::Rc<Self>` as
/// its first parameter in place of it, and otherwise has the shape of a `#[jsg_method]`: an
/// optional `&mut jsg::Lock`, then the JavaScript arguments. It runs whether the instance is
/// called with or without `new`, and always with the instance as `this`, so a callable taken off
/// the object it was read from still calls the instance. A type is callable at most one way.
/// The method may also be a `#[jsg_method]`, to be callable both ways.
/// See `src/rust/jsg/README.md` for how callable resources behave.
#[proc_macro_attribute]
pub fn jsg_callable(attr: TokenStream, item: TokenStream) -> TokenStream {
    // Registration as Member::Callable is handled by #[jsg_resource] on the enclosing impl block.
    let input_fn = parse_macro_input!(item as ItemFn);
    let fn_name = &input_fn.sig.ident;
    let callback_names = std::iter::once((Callback::Function, callable_callback_name(fn_name)));
    let error = if attr.is_empty() {
        let has_receiver =
            input_fn.sig.receiver().is_some() || input_fn.sig.inputs.iter().any(is_named_this);
        (!has_receiver).then(|| {
            syn::Error::new_spanned(
                fn_name,
                "a #[jsg_callable] method takes `&self`, or `this: jsg::Rc<Self>` in its place: \
                 it runs on the instance that is called",
            )
        })
    } else {
        Some(syn::Error::new_spanned(
            proc_macro2::TokenStream::from(attr),
            "#[jsg_callable] takes no arguments",
        ))
    };
    if let Some(error) = error {
        return reject(&input_fn, callback_names, &error).into();
    }
    // A call handler has no `v8::Signature`. V8 passes the instance that is called as `this`,
    // but the callback still checks the receiver, as an accessor's does.
    let callbacks = generate_callbacks(
        &input_fn,
        callback_names,
        Some(CALLED_AS_FUNCTION),
        ReceiverCheck::Marker,
    );
    // The method keeps its other attributes, so that a `#[jsg_method]` after this one expands.
    match callbacks {
        Ok(callbacks) => {
            let method = reemit_method(&input_fn);
            quote! {
                #method
                #callbacks
            }
        }
        Err(error) => error,
    }
    .into()
}

/// The name of the callback that `#[jsg_callable]` generates for `fn_name`. It differs from the
/// callback of a `#[jsg_method]`, so that a method can be both.
fn callable_callback_name(fn_name: &syn::Ident) -> syn::Ident {
    syn::Ident::new(&format!("{fn_name}_callable_callback"), fn_name.span())
}

// =============================================================================
// #[jsg_oneof]
// =============================================================================

/// Generates `jsg::Type`, `jsg::FromJS`, `jsg::Traced` and `jsg::ToJS` for a
/// union enum, equivalent to `kj::OneOf<…>` in C++ JSG.
///
/// Each variant must be a single-field tuple variant whose inner type converts
/// from JavaScript and is `jsg::Type`. A value converts as it
/// would to the equivalent `kj::OneOf`: the variants are tried in passes that
/// keep coercion for last (see "Union Types" in `src/rust/jsg/README.md`), and
/// in declaration order within a pass. A variant that is itself a union takes
/// part in each pass with its own variants. The union traces the variant it
/// holds, so it can be a resource field, unless it holds handles: such a union
/// has one lifetime parameter, their scope. `jsg::ToJS` is implemented when
/// every variant implements it. A union that breaks Web IDL's rules for unions,
/// as C++ JSG's `webidl::UnionTypeValidator` checks them, doesn't compile.
///
/// ```ignore
/// #[jsg_oneof]
/// #[derive(Debug, Clone)]
/// enum StringOrNumber {
///     String(String),
///     Number(f64),
/// }
/// ```
#[proc_macro_attribute]
pub fn jsg_oneof(attr: TokenStream, item: TokenStream) -> TokenStream {
    oneof::expand(&attr, item)
}
