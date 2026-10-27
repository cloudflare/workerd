// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! `#[jsg_wildcard_property]`: the getter and query callbacks of a wildcard property.

use proc_macro::TokenStream;
use quote::quote;
use syn::FnArg;
use syn::ItemFn;

use crate::utils::is_lock_ref;
use crate::utils::is_result_type;

/// The name of the getter callback that `#[jsg_wildcard_property]` generates for `fn_name`.
pub fn getter_callback_name(fn_name: &syn::Ident) -> syn::Ident {
    syn::Ident::new(
        &format!("{fn_name}_wildcard_getter_callback"),
        fn_name.span(),
    )
}

/// The name of the query callback that `#[jsg_wildcard_property]` generates for `fn_name`.
pub fn query_callback_name(fn_name: &syn::Ident) -> syn::Ident {
    syn::Ident::new(
        &format!("{fn_name}_wildcard_query_callback"),
        fn_name.span(),
    )
}

/// Expands `#[jsg_wildcard_property]` on `item`, a method taking `&self`, optionally a
/// `&mut jsg::Lock`, and the property name as a `String` or `&str`, and returning `Option<T>`
/// or `Result<Option<T>, E>`.
pub fn expand(attr: TokenStream, item: TokenStream) -> TokenStream {
    let input_fn = syn::parse_macro_input!(item as ItemFn);
    let fn_name = &input_fn.sig.ident;
    let getter_name = getter_callback_name(fn_name);
    let query_name = query_callback_name(fn_name);

    let signature = if attr.is_empty() {
        Signature::parse(&input_fn)
    } else {
        Err(syn::Error::new_spanned(
            proc_macro2::TokenStream::from(attr),
            "#[jsg_wildcard_property] takes no arguments",
        ))
    };
    let signature = match signature {
        Ok(signature) => signature,
        Err(error) => {
            // The method and its callbacks are still emitted, so that their callers and the
            // registration in `#[jsg_resource]` add no errors of their own.
            let error = error.to_compile_error();
            return quote! {
                #error
                #input_fn
                fn #getter_name() -> jsg::RawWildcardGetterCallback { unreachable!() }
                fn #query_name() -> jsg::RawWildcardQueryCallback { unreachable!() }
            }
            .into();
        }
    };

    let lookup = signature.lookup(fn_name);
    quote! {
        #input_fn

        #[automatically_derived]
        // Comments do not survive into the expansion, so the SAFETY comment on the `unsafe`
        // block that borrows the resource cannot satisfy the lint.
        #[expect(clippy::undocumented_unsafe_blocks)]
        fn #getter_name() -> jsg::RawWildcardGetterCallback {
            jsg::wildcard_getter_callback(|lock, name, args| {
                let this = args.this();
                #lookup
                // An error from converting the value is thrown as one the method returns.
                let converted = match result {
                    Ok(Some(value)) => jsg::ToJS::to_js(value, lock).map(Some),
                    Ok(None) => Ok(None),
                    Err(err) => Err(err),
                };
                match converted {
                    Ok(Some(value)) => {
                        args.set_return_value(value);
                        jsg::Intercepted::Yes
                    }
                    Ok(None) => jsg::Intercepted::No,
                    Err(err) => {
                        lock.throw_exception(&err);
                        jsg::Intercepted::Yes
                    }
                }
            })
        }

        #[automatically_derived]
        #[expect(clippy::undocumented_unsafe_blocks)]
        fn #query_name() -> jsg::RawWildcardQueryCallback {
            jsg::wildcard_query_callback(|lock, name, args| {
                let this = args.this();
                #lookup
                match result {
                    Ok(Some(_)) => jsg::Intercepted::Yes,
                    Ok(None) => jsg::Intercepted::No,
                    Err(err) => {
                        lock.throw_exception(&err);
                        jsg::Intercepted::Yes
                    }
                }
            })
        }
    }
    .into()
}

/// The parts of a `#[jsg_wildcard_property]` method's signature that its callbacks depend on.
struct Signature {
    has_lock_param: bool,
    name_is_ref: bool,
    returns_result: bool,
}

impl Signature {
    fn parse(input_fn: &ItemFn) -> syn::Result<Self> {
        let sig = &input_fn.sig;
        let receiver_is_shared_self = sig.receiver().is_some_and(|receiver| {
            matches!(&*receiver.ty, syn::Type::Reference(r)
                if r.mutability.is_none()
                    && matches!(&*r.elem, syn::Type::Path(p)
                        if p.qself.is_none() && p.path.is_ident("Self")))
        });
        if !receiver_is_shared_self {
            let message = "a #[jsg_wildcard_property] method takes `&self`: JavaScript can call \
                           into the resource while the method runs, so the method cannot have \
                           exclusive access. Keep state that methods change in `Cell` or \
                           `RefCell` fields";
            return Err(match sig.receiver() {
                Some(receiver) => syn::Error::new_spanned(receiver, message),
                None => syn::Error::new_spanned(sig, message),
            });
        }

        let params: Vec<_> = sig
            .inputs
            .iter()
            .filter_map(|arg| match arg {
                FnArg::Typed(pat_type) => Some(&*pat_type.ty),
                FnArg::Receiver(_) => None,
            })
            .collect();
        let has_lock_param = params.first().is_some_and(|ty| is_lock_ref(ty));
        let name_is_ref = match &params[usize::from(has_lock_param)..] {
            [name] if is_string(name) => false,
            [syn::Type::Reference(r)] if r.mutability.is_none() && is_str(&r.elem) => true,
            _ => {
                return Err(syn::Error::new_spanned(
                    &sig.inputs,
                    "a #[jsg_wildcard_property] method takes `&self`, optionally \
                     `lock: &mut jsg::Lock`, and the property name as a `String` or `&str`",
                ));
            }
        };

        let returns_result = match &sig.output {
            syn::ReturnType::Type(_, ty) if is_result_type(ty) => true,
            syn::ReturnType::Type(_, ty) if last_segment_is(ty, "Option") => false,
            _ => {
                return Err(syn::Error::new_spanned(
                    &sig.output,
                    "a #[jsg_wildcard_property] method returns `Option<T>` or \
                     `Result<Option<T>, E>`: `None` if there is no such property",
                ));
            }
        };

        Ok(Self {
            has_lock_param,
            name_is_ref,
            returns_result,
        })
    }

    /// Statements that resolve the resource from `this`, convert `name`, call the method and
    /// bind `result: Result<Option<T>, jsg::Error>`, or throw and return `Intercepted::Yes`.
    fn lookup(&self, fn_name: &syn::Ident) -> proc_macro2::TokenStream {
        let lock_arg = if self.has_lock_param {
            quote! { &mut *lock, }
        } else {
            quote! {}
        };
        let name_arg = if self.name_is_ref {
            quote! { &name }
        } else {
            quote! { name }
        };
        let to_result = if self.returns_result {
            quote! { result.map_err(Into::into) }
        } else {
            quote! { Ok(result) }
        };
        quote! {
            // V8 calls an interceptor only on objects made from the resource's instance
            // template, so this is the resource's own wrapper; a mismatch is still a
            // TypeError rather than a panic.
            let Some(wrappable) = jsg::v8::WrappableRc::from_js(lock, this) else {
                lock.throw_exception(&jsg::Error::new_type_error("Illegal invocation"));
                return jsg::Intercepted::Yes;
            };
            let Some(resource) = wrappable.resolve_resource::<Self>() else {
                lock.throw_exception(&jsg::Error::new_type_error("Illegal invocation"));
                return jsg::Intercepted::Yes;
            };
            // V8 calls the interceptor only for string names (`kOnlyInterceptStrings`), whose
            // conversion runs no JavaScript.
            let name = match <String as jsg::FromJS<'_>>::from_js(lock, name.into()) {
                Ok(name) => name,
                Err(err) => {
                    lock.throw_exception(&err);
                    return jsg::Intercepted::Yes;
                }
            };
            // SAFETY: `resource` points at the `Self` of `this`, whose wrapper keeps it alive
            // during the call. Resources are only ever borrowed shared.
            let self_: &Self = unsafe { resource.as_ref() };
            let result = self_.#fn_name(#lock_arg #name_arg);
            let result: Result<_, jsg::Error> = #to_result;
        }
    }
}

fn last_segment_is(ty: &syn::Type, name: &str) -> bool {
    matches!(ty, syn::Type::Path(p)
        if p.qself.is_none() && p.path.segments.last().is_some_and(|s| s.ident == name))
}

fn is_string(ty: &syn::Type) -> bool {
    last_segment_is(ty, "String")
}

fn is_str(ty: &syn::Type) -> bool {
    matches!(ty, syn::Type::Path(p) if p.qself.is_none() && p.path.is_ident("str"))
}
