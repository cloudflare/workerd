// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Code generation for `#[jsg_resource]` on structs and impl blocks.
//!
//! - On a **struct**: emits `jsg::Type`, `jsg::ToJS`, `jsg::FromJS`,
//!   `jsg::Traced`, and `jsg::GarbageCollected` implementations.
//! - On an **impl block**: emits member factories and, unless `custom_members` is set,
//!   the `jsg::Resource` trait with automatic member and nested type registration.

use std::collections::HashMap;

use proc_macro::TokenStream;
use quote::quote;
use syn::FnArg;
use syn::ItemImpl;

use crate::trace::generate_trace_statements;
use crate::utils::elide_named_lifetimes;
use crate::utils::extract_name_attribute;
use crate::utils::extract_named_fields;
use crate::utils::has_custom_trace_flag;
use crate::utils::is_attr;
use crate::utils::is_lock_ref;
use crate::utils::is_result_type;
use crate::utils::snake_to_camel;
use crate::utils::this_param;

// Compile-time mirror of `jsg::PropertyKind` used to group annotated methods
// and emit the correct token streams. Cannot reuse the runtime type directly
// because proc-macro crates cannot link against CXX-bridge runtime crates.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum PropertyKind {
    Prototype,
    Instance,
    LazyInstance,
    Inspect,
}

/// Entry point called from `lib.rs` for `#[jsg_resource]` on a struct.
pub fn generate_resource_struct(attr: TokenStream, input: &syn::DeriveInput) -> TokenStream {
    if let Some(err) = reject_nested_types_on_struct(attr.clone().into()) {
        return err.to_compile_error().into();
    }

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

            fn is_exact(value: jsg::v8::Local<'_, jsg::v8::Value>) -> bool {
                value.is_object()
            }
        }

        #[automatically_derived]
        impl<'s> jsg::ToJS<'s> for #name {
            fn to_js(
                self,
                lock: &mut jsg::Lock<'s>,
            ) -> ::std::result::Result<jsg::v8::Local<'s, jsg::v8::Value>, jsg::Error> {
                let r = jsg::Rc::new(self);
                jsg::ToJS::to_js(r, lock)
            }
        }

        #[automatically_derived]
        impl<'s> jsg::FromJS<'s> for #name {
            type ResultType = jsg::Rc<Self>;

            fn type_name() -> String {
                <Self as jsg::Type>::class_name().to_owned()
            }

            fn from_js(
                lock: &mut jsg::Lock<'s>,
                value: jsg::v8::Local<'s, jsg::v8::Value>,
            ) -> Result<Self::ResultType, jsg::Error> {
                <jsg::Rc<Self> as jsg::FromJS<'s>>::from_js(lock, value)
            }

            fn try_from_js(
                lock: &mut jsg::Lock<'s>,
                value: jsg::v8::Local<'s, jsg::v8::Value>,
            ) -> Result<Option<Self::ResultType>, jsg::Error> {
                <jsg::Rc<Self> as jsg::FromJS<'s>>::try_from_js(lock, value)
            }
        }

        #traced_impl
        #gc_impl
    }
    .into()
}

/// A generated member factory and its invocation for automatic registration.
struct MemberRegistration {
    source_name: syn::Ident,
    factory_name: syn::Ident,
    factory: proc_macro2::TokenStream,
    automatic: proc_macro2::TokenStream,
}

fn member_factory_name(rust_name: &syn::Ident) -> syn::Ident {
    syn::Ident::new(&format!("{rust_name}_member"), rust_name.span())
}

impl MemberRegistration {
    fn new(rust_name: &syn::Ident, member: &proc_macro2::TokenStream) -> Self {
        Self::with_factory_name(rust_name, member_factory_name(rust_name), member)
    }

    fn with_factory_name(
        rust_name: &syn::Ident,
        factory_name: syn::Ident,
        member: &proc_macro2::TokenStream,
    ) -> Self {
        Self {
            factory: quote! {
                #[automatically_derived]
                #[must_use]
                fn #factory_name() -> jsg::Member {
                    #member
                }
            },
            automatic: quote! { Self::#factory_name() },
            source_name: rust_name.clone(),
            factory_name,
        }
    }
}

/// Scans `impl_block` for `#[jsg_method]`-annotated functions.
fn collect_method_registrations(impl_block: &ItemImpl) -> Vec<MemberRegistration> {
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
                .map(|list| list.tokens.clone())
                .and_then(extract_name_attribute)
                .unwrap_or_else(|| snake_to_camel(&rust_method_name.to_string()));
            let callback_name =
                syn::Ident::new(&format!("{rust_method_name}_callback"), rust_method_name.span());

            let has_self = method
                .sig
                .inputs
                .iter()
                .any(|arg| matches!(arg, FnArg::Receiver(_)))
                || this_param(&method.sig).is_some();

            let fast_api =
                syn::Ident::new(&format!("{rust_method_name}_fast_api"), rust_method_name.span());
            let fast = quote! { Self::#fast_api() };

            let member = if has_self {
                quote! { jsg::Member::Method { name: #js_name.to_owned(), callback: Self::#callback_name(), fast: #fast } }
            } else {
                quote! { jsg::Member::StaticMethod { name: #js_name.to_owned(), callback: Self::#callback_name(), fast: #fast } }
            };
            Some(MemberRegistration::new(rust_method_name, &member))
        })
        .collect()
}

/// Scans `impl_block` for `#[jsg_wildcard_property]` methods. A resource type has at most one
/// wildcard property, so with automatic registration a second method is a compile error, which
/// is returned alongside the registrations so that the type is still registered and adds no
/// errors of its own. Custom registration may choose between several by compatibility flag;
/// registering more than one at once fails at runtime.
fn collect_wildcard_registrations(
    impl_block: &ItemImpl,
    mode: RegistrationMode,
) -> (Vec<MemberRegistration>, Option<proc_macro2::TokenStream>) {
    let methods: Vec<_> = impl_block
        .items
        .iter()
        .filter_map(|item| match item {
            syn::ImplItem::Fn(method)
                if method
                    .attrs
                    .iter()
                    .any(|a| is_attr(a, "jsg_wildcard_property")) =>
            {
                Some(method)
            }
            _ => None,
        })
        .collect();
    let error = methods
        .get(1)
        .filter(|_| mode == RegistrationMode::Automatic)
        .map(|second| {
            syn::Error::new_spanned(
                &second.sig.ident,
                "a resource type has at most one #[jsg_wildcard_property]; use \
                 `custom_members` to choose between several by compatibility flag",
            )
            .to_compile_error()
        });
    let registrations = methods
        .iter()
        .map(|method| {
            let rust_name = &method.sig.ident;
            let getter_callback = crate::wildcard::getter_callback_name(rust_name);
            let query_callback = crate::wildcard::query_callback_name(rust_name);
            let member = quote! {
                jsg::Member::WildcardProperty {
                    getter_callback: Self::#getter_callback(),
                    query_callback: Self::#query_callback(),
                }
            };
            MemberRegistration::new(rust_name, &member)
        })
        .collect();
    (registrations, error)
}

/// Scans `impl_block` for `#[jsg_static_constant]`-annotated consts.
fn collect_constant_registrations(impl_block: &ItemImpl) -> Vec<MemberRegistration> {
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
                .map(|list| list.tokens.clone())
                .and_then(extract_name_attribute)
                .unwrap_or_else(|| rust_name.to_string());

            let factory_base =
                syn::Ident::new(&rust_name.to_string().to_lowercase(), rust_name.span());
            Some(MemberRegistration::with_factory_name(
                rust_name,
                member_factory_name(&factory_base),
                &quote! {
                    jsg::Member::StaticConstant {
                        name: #js_name.to_owned(),
                        value: jsg::ConstantValue::from(Self::#rust_name),
                    }
                },
            ))
        })
        .collect()
}

/// Returns an error if the arguments of `#[jsg_resource]` on a struct include
/// `nested_types(...)`, which belongs on the impl block.
fn reject_nested_types_on_struct(attr: proc_macro2::TokenStream) -> Option<syn::Error> {
    use syn::punctuated::Punctuated;

    let metas: Punctuated<syn::Meta, syn::Token![,]> =
        syn::parse::Parser::parse2(Punctuated::parse_terminated, attr).ok()?;
    metas
        .iter()
        .find(|meta| meta.path().is_ident("nested_types"))
        .map(|meta| {
            syn::Error::new_spanned(
                meta.path(),
                "`nested_types(...)` belongs on the #[jsg_resource] impl block, not the struct",
            )
        })
}

/// One entry of `#[jsg_resource(nested_types(...))]`: `Type` or `Name = Type`.
struct NestedTypeArg {
    name: Option<syn::Ident>,
    ty: syn::Path,
}

impl syn::parse::Parse for NestedTypeArg {
    fn parse(input: syn::parse::ParseStream) -> syn::Result<Self> {
        if input.peek(syn::Ident) && input.peek2(syn::Token![=]) {
            let name = input.parse()?;
            input.parse::<syn::Token![=]>()?;
            Ok(Self {
                name: Some(name),
                ty: input.parse()?,
            })
        } else {
            Ok(Self {
                name: None,
                ty: input.parse()?,
            })
        }
    }
}

/// Returns a `Member::NestedType` token stream for a nested type.
fn nested_type_registration(arg: &NestedTypeArg) -> proc_macro2::TokenStream {
    use syn::ext::IdentExt;

    let ty = &arg.ty;
    let name = if let Some(name) = &arg.name {
        let name = name.unraw().to_string();
        quote! { #name.to_owned() }
    } else {
        quote! { <#ty as jsg::Type>::class_name().to_owned() }
    };
    quote! {
        jsg::Member::NestedType {
            name: #name,
            nested_type: jsg::NestedType::of::<#ty>(),
        }
    }
}

/// Check factory names against each other and the user's value-namespace associated items.
fn validate_factory_names(
    impl_block: &ItemImpl,
    registrations: &[MemberRegistration],
) -> syn::Result<()> {
    let declared_names: HashMap<_, _> = impl_block
        .items
        .iter()
        .filter_map(|item| match item {
            syn::ImplItem::Fn(method) => Some(&method.sig.ident),
            syn::ImplItem::Const(constant) => Some(&constant.ident),
            _ => None,
        })
        .map(|name| (name.to_string(), name))
        .collect();
    let mut factory_sources = HashMap::new();
    for registration in registrations {
        let name = &registration.factory_name;
        let source = &registration.source_name;
        let conflict = if let Some(other) = factory_sources.insert(name.to_string(), source) {
            Some((other, "the factory generated by"))
        } else {
            declared_names
                .get(&name.to_string())
                .map(|other| (*other, "associated item"))
        };
        if let Some((other, kind)) = conflict {
            let mut error = syn::Error::new_spanned(
                source,
                format!(
                    "member factory `{name}` generated by `{source}` conflicts with {kind} `{other}`"
                ),
            );
            error.combine(syn::Error::new_spanned(
                other,
                format!("`{other}` declared here"),
            ));
            return Err(error);
        }
    }
    Ok(())
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum RegistrationMode {
    Automatic,
    Custom,
}

/// The arguments of `#[jsg_resource]` on an impl block.
struct ImplArgs {
    mode: RegistrationMode,
    nested_types: Vec<NestedTypeArg>,
}

/// Parses the arguments of `#[jsg_resource]` on an impl block: `custom_members` and any
/// number of `nested_types(...)` lists.
///
/// A nested type listed twice without a name, or a name given twice, is an error. Nested
/// types without a name are named after their class names, which are only known when the
/// macro's output is compiled, so such a nested type can still clash with another nested
/// type or member. `nested_types(...)` cannot be combined with `custom_members`, which
/// leaves `Resource::members` to the author.
fn parse_impl_args(attr: proc_macro2::TokenStream) -> syn::Result<ImplArgs> {
    use syn::ext::IdentExt;
    use syn::parse::Parser as _;
    use syn::punctuated::Punctuated;

    const EXPECTED: &str = "expected `custom_members` or `nested_types(...)` on a \
                            #[jsg_resource] impl block (`name` and `custom_trace` belong on \
                            the struct)";

    let args = Punctuated::<syn::Meta, syn::Token![,]>::parse_terminated.parse2(attr)?;
    let mut custom_members = None;
    let mut nested_types = Vec::<NestedTypeArg>::new();
    let mut first_nested_types = None;
    for arg in args {
        match &arg {
            syn::Meta::Path(path) if path.is_ident("custom_members") => {
                if custom_members.is_some() {
                    return Err(syn::Error::new_spanned(arg, "duplicate `custom_members`"));
                }
                custom_members = Some(path.clone());
            }
            syn::Meta::List(list) if list.path.is_ident("nested_types") => {
                first_nested_types.get_or_insert_with(|| list.path.clone());
                let list_args = list.parse_args_with(
                    Punctuated::<NestedTypeArg, syn::Token![,]>::parse_terminated,
                )?;
                for nested in list_args {
                    let duplicate =
                        nested_types
                            .iter()
                            .any(|other| match (&nested.name, &other.name) {
                                (Some(name), Some(other)) => name.unraw() == other.unraw(),
                                (None, None) => {
                                    let (ty, other) = (&nested.ty, &other.ty);
                                    quote! { #ty }.to_string() == quote! { #other }.to_string()
                                }
                                _ => false,
                            });
                    if duplicate {
                        return Err(match &nested.name {
                            Some(name) => {
                                syn::Error::new_spanned(name, "duplicate nested type name")
                            }
                            None => syn::Error::new_spanned(&nested.ty, "duplicate nested type"),
                        });
                    }
                    nested_types.push(nested);
                }
            }
            _ => return Err(syn::Error::new_spanned(arg, EXPECTED)),
        }
    }
    let mode = if custom_members.is_some() {
        RegistrationMode::Custom
    } else {
        RegistrationMode::Automatic
    };
    if let (Some(custom_members), Some(nested)) = (custom_members, first_nested_types) {
        let mut error = syn::Error::new_spanned(
            nested,
            "`nested_types(...)` cannot be combined with `custom_members`; add \
             `jsg::Member::NestedType` entries to `Resource::members` instead",
        );
        error.combine(syn::Error::new_spanned(
            custom_members,
            "`custom_members` given here",
        ));
        return Err(error);
    }
    Ok(ImplArgs { mode, nested_types })
}

/// Entry point called from `lib.rs` for `#[jsg_resource]` on an impl block.
pub fn generate_resource_impl(attr: TokenStream, impl_block: &ItemImpl) -> TokenStream {
    expand_resource_impl(attr.into(), impl_block).into()
}

fn expand_resource_impl(
    attr: proc_macro2::TokenStream,
    impl_block: &ItemImpl,
) -> proc_macro2::TokenStream {
    let bindings = match generate_resource_bindings(attr, impl_block) {
        Ok(bindings) => bindings,
        Err(error) => error,
    };
    // Retain the user's methods even when registration is invalid, so references to them
    // do not produce missing-associated-item errors alongside the registration diagnostic.
    quote! {
        #impl_block
        #bindings
    }
}

fn generate_resource_bindings(
    attr: proc_macro2::TokenStream,
    impl_block: &ItemImpl,
) -> Result<proc_macro2::TokenStream, proc_macro2::TokenStream> {
    let ImplArgs { mode, nested_types } =
        parse_impl_args(attr).map_err(|err| err.to_compile_error())?;
    let self_ty = &impl_block.self_ty;

    if !matches!(&**self_ty, syn::Type::Path(_)) {
        return Err(syn::Error::new_spanned(
            self_ty,
            "#[jsg_resource] impl blocks must use a simple path type (e.g., `impl MyResource`)",
        )
        .to_compile_error());
    }

    let mut registrations = generate_constructor_registration(impl_block, self_ty)?
        .into_iter()
        .collect::<Vec<_>>();
    registrations.extend(collect_method_registrations(impl_block));
    registrations.extend(collect_property_registrations(impl_block)?);
    registrations.extend(collect_constant_registrations(impl_block));
    let (wildcard, wildcard_error) = collect_wildcard_registrations(impl_block, mode);
    registrations.extend(wildcard);
    validate_factory_names(impl_block, &registrations).map_err(|err| err.to_compile_error())?;
    let factories = registrations.iter().map(|r| &r.factory);
    let automatic = registrations.iter().map(|r| &r.automatic);
    let nested_type_registrations = nested_types.iter().map(nested_type_registration);

    let resource_impl = if mode == RegistrationMode::Automatic {
        quote! {
            #[automatically_derived]
            impl jsg::Resource for #self_ty {
                fn members(
                    _flags: jsg::feature_flags::compatibility_flags::Reader<'_>,
                ) -> Vec<jsg::Member>
                where
                    Self: Sized,
                {
                    vec![#(#automatic,)* #(#nested_type_registrations,)*]
                }
            }
        }
    } else {
        quote! {}
    };

    Ok(quote! {
        #wildcard_error

        #[automatically_derived]
        impl #self_ty {
            #(#factories)*
        }

        #resource_impl
    })
}

// ---------------------------------------------------------------------------
// Constructor helpers
// ---------------------------------------------------------------------------

/// Validates that a `#[jsg_constructor]` method has the right shape.
///
/// Returns a `compile_error!` token stream if the method has a `self` receiver
/// or does not return `Self`; returns `None` if the method is valid.
fn validate_constructor(method: &syn::ImplItemFn) -> Option<proc_macro2::TokenStream> {
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

    if constructor_return(method) == ConstructorReturn::Invalid {
        return Some(quote! {
            compile_error!("#[jsg_constructor] must return Self or Result<Self, E>");
        });
    }

    None
}

/// What a `#[jsg_constructor]` method returns.
#[derive(Debug, PartialEq, Eq)]
enum ConstructorReturn {
    /// `Self`.
    Infallible,
    /// `Result<Self, E>`, under any path ending in `Result`.
    Fallible,
    /// Anything else, which is not a valid constructor.
    Invalid,
}

fn constructor_return(method: &syn::ImplItemFn) -> ConstructorReturn {
    let syn::ReturnType::Type(_, ty) = &method.sig.output else {
        return ConstructorReturn::Invalid;
    };
    let is_self = |ty: &syn::Type| matches!(ty, syn::Type::Path(p) if p.path.is_ident("Self"));
    if is_self(ty) {
        return ConstructorReturn::Infallible;
    }
    if is_result_type(ty)
        && let syn::Type::Path(p) = &**ty
        && let Some(segment) = p.path.segments.last()
        && let syn::PathArguments::AngleBracketed(args) = &segment.arguments
        && let Some(syn::GenericArgument::Type(ok)) = args.args.first()
        && is_self(ok)
    {
        return ConstructorReturn::Fallible;
    }
    ConstructorReturn::Invalid
}

/// Extracts constructor argument unwrap statements and argument expressions.
fn extract_constructor_params(
    method: &syn::ImplItemFn,
) -> (
    bool,
    Vec<proc_macro2::TokenStream>,
    Vec<proc_macro2::TokenStream>,
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
            let elided_ty = elide_named_lifetimes(ty);
            let unwrap = quote! {
                let #var = match <#elided_ty as jsg::FromJS<'_>>::from_js(lock, args.get(#js_index)) {
                    Ok(v) => v,
                    Err(err) => {
                        lock.throw_exception(&err);
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
/// constructor member factory. Returns `None` if no constructor is defined.
/// Validates that a `#[jsg_constructor]` method has the right shape.
fn generate_constructor_registration(
    impl_block: &ItemImpl,
    self_ty: &syn::Type,
) -> Result<Option<MemberRegistration>, proc_macro2::TokenStream> {
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
        return Err(quote! {
            compile_error!("only one #[jsg_constructor] is allowed per impl block");
        });
    }

    let Some(method) = constructors.first() else {
        return Ok(None);
    };
    if let Some(err) = validate_constructor(method) {
        return Err(err);
    }

    let rust_method_name = &method.sig.ident;
    let call = quote! { #self_ty::#rust_method_name };

    let (has_lock_param, unwraps, arg_exprs) = extract_constructor_params(method);
    let lock_arg = if has_lock_param {
        quote! { &mut *lock, }
    } else {
        quote! {}
    };
    let construct = if constructor_return(method) == ConstructorReturn::Fallible {
        quote! {
            let resource = match #call(#lock_arg #(#arg_exprs),*) {
                Ok(resource) => resource,
                Err(err) => {
                    lock.throw_exception(&err.into());
                    return;
                }
            };
        }
    } else {
        quote! { let resource = #call(#lock_arg #(#arg_exprs),*); }
    };

    let member = quote! {
        jsg::Member::Constructor {
            callback: jsg::function_callback(|lock, args| {
                // Without `new`, `this` is an arbitrary receiver with no
                // internal fields to attach the resource to. The message
                // matches C++ `throwIfConstructorCalledAsFunction`.
                if !args.is_construct_call() {
                    lock.throw_exception(&jsg::Error::new_type_error(format!(
                        "Failed to construct '{}': Please use the 'new' operator, \
                         this object constructor cannot be called as a function.",
                        <#self_ty as jsg::Type>::class_name(),
                    )));
                    return;
                }

                #(#unwraps)*

                #construct
                let rc = jsg::Rc::new(resource);
                rc.attach_to_this(args);
            }),
        }
    };
    Ok(Some(MemberRegistration::new(rust_method_name, &member)))
}

// ---------------------------------------------------------------------------
// Property helpers
// ---------------------------------------------------------------------------

/// Emits a member factory for a property group, or an error if the group has no getter.
///
/// `is_readonly` only affects the lazy instance placement: other placements are read-only
/// exactly when there is no setter, but a lazy instance property never has a setter and is
/// writable unless `readonly` is given, like C++ `JSG_LAZY_INSTANCE_PROPERTY`.
fn emit_property_group(
    js_name: &str,
    kind: PropertyKind,
    getter: Option<syn::Ident>,
    setter: Option<syn::Ident>,
    is_readonly: bool,
) -> Result<MemberRegistration, proc_macro2::TokenStream> {
    let Some(getter_name) = getter else {
        return Err(quote! {
            compile_error!(concat!("no getter found for property \"", #js_name, "\""));
        });
    };

    let getter_cb = syn::Ident::new(&format!("{getter_name}_callback"), getter_name.span());
    let setter_tokens = if let Some(setter_name) = &setter {
        let setter_cb = syn::Ident::new(&format!("{setter_name}_callback"), setter_name.span());
        quote! { Some(Self::#setter_cb()) }
    } else {
        quote! { None }
    };

    let kind_tokens = if kind == PropertyKind::Inspect {
        quote! { jsg::PropertyKind::Inspect }
    } else {
        quote! { jsg::PropertyKind::Prototype }
    };
    let prototype_member = quote! {
        jsg::Member::Property {
            name: #js_name.to_owned(),
            kind: #kind_tokens,
            getter_callback: Self::#getter_cb(),
            setter_callback: #setter_tokens,
        }
    };
    if kind == PropertyKind::Inspect {
        return Ok(MemberRegistration::new(&getter_name, &prototype_member));
    }

    let instance_getter_cb = syn::Ident::new(
        &format!("{getter_name}_instance_callback"),
        getter_name.span(),
    );
    let has_setter = setter.is_some();
    let instance_setter = if let Some(setter_name) = setter {
        let setter_cb = syn::Ident::new(
            &format!("{setter_name}_instance_callback"),
            setter_name.span(),
        );
        quote! { Some(Self::#setter_cb()) }
    } else {
        quote! { None }
    };
    // A property with a setter cannot be lazy. Custom registration can still ask for the lazy
    // placement, so that arm registers a lazy-kind `Member::Property`, which template creation
    // rejects with a message naming the problem.
    let lazy_instance_member = if has_setter {
        quote! {
            jsg::Member::Property {
                name: #js_name.to_owned(),
                kind: jsg::PropertyKind::LazyInstance,
                getter_callback: Self::#getter_cb(),
                setter_callback: #setter_tokens,
            }
        }
    } else {
        quote! {
            jsg::Member::LazyInstanceProperty {
                name: #js_name.to_owned(),
                getter_callback: Self::#instance_getter_cb(),
                read_only: #is_readonly,
            }
        }
    };
    let factory_name = member_factory_name(&getter_name);
    let placement = match kind {
        PropertyKind::Instance => quote! { jsg::PropertyPlacement::Instance },
        PropertyKind::LazyInstance => quote! { jsg::PropertyPlacement::LazyInstance },
        PropertyKind::Prototype | PropertyKind::Inspect => {
            quote! { jsg::PropertyPlacement::Prototype }
        }
    };
    Ok(MemberRegistration {
        factory: quote! {
            #[automatically_derived]
            #[must_use]
            fn #factory_name(placement: jsg::PropertyPlacement) -> jsg::Member {
                match placement {
                    jsg::PropertyPlacement::Prototype => #prototype_member,
                    jsg::PropertyPlacement::Instance => jsg::Member::InstanceProperty {
                        name: #js_name.to_owned(),
                        getter_callback: Self::#instance_getter_cb(),
                        setter_callback: #instance_setter,
                    },
                    jsg::PropertyPlacement::LazyInstance => #lazy_instance_member,
                }
            }
        },
        automatic: quote! { Self::#factory_name(#placement) },
        source_name: getter_name,
        factory_name,
    })
}

/// Parses the argument list of `#[jsg_property([placement,] [name = "..."] [, readonly])]`.
///
/// `placement` is `prototype`, `instance` or `lazy_instance`; when omitted it defaults to
/// `Prototype`.
/// Returns `(PropertyKind, Option<js_name_override>, is_readonly)`.
pub fn parse_jsg_property_args(
    tokens: proc_macro2::TokenStream,
) -> Result<(PropertyKind, Option<String>, bool), proc_macro2::TokenStream> {
    use syn::parse::Parser as _;
    const CONFLICTING_PLACEMENT: &str =
        "conflicting placement: specify only one of `prototype`, `instance` or `lazy_instance`";

    let metas = syn::punctuated::Punctuated::<syn::Meta, syn::Token![,]>::parse_terminated
        .parse2(tokens)
        .map_err(|e| e.to_compile_error())?;

    let mut kind: Option<PropertyKind> = None;
    let mut name: Option<String> = None;
    let mut readonly = false;

    for meta in &metas {
        match meta {
            syn::Meta::Path(p) if p.is_ident("instance") => {
                if kind.is_some() {
                    return Err(
                        syn::Error::new_spanned(p, CONFLICTING_PLACEMENT).to_compile_error()
                    );
                }
                kind = Some(PropertyKind::Instance);
            }
            syn::Meta::Path(p) if p.is_ident("prototype") => {
                if kind.is_some() {
                    return Err(
                        syn::Error::new_spanned(p, CONFLICTING_PLACEMENT).to_compile_error()
                    );
                }
                kind = Some(PropertyKind::Prototype);
            }
            syn::Meta::Path(p) if p.is_ident("lazy_instance") => {
                if kind.is_some() {
                    return Err(
                        syn::Error::new_spanned(p, CONFLICTING_PLACEMENT).to_compile_error()
                    );
                }
                kind = Some(PropertyKind::LazyInstance);
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
                    "unknown argument; expected `prototype`, `instance`, `lazy_instance`, `readonly`, or `name = \"...\"`",
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
#[expect(clippy::expect_used, reason = "the entry was pushed on the line above")]
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
) -> Result<PropGroups, proc_macro2::TokenStream> {
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

            let tokens = attr
                .meta
                .require_list()
                .map(|list| list.tokens.clone())
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
            let attr_tokens = attr
                .meta
                .require_list()
                .ok()
                .map(|list| list.tokens.clone());
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

/// Phase 2 (per group): validate constraints and emit a member factory.
fn validate_and_emit_property(
    js_name: &str,
    kind: PropertyKind,
    methods: Vec<PropMethod>,
) -> Result<MemberRegistration, proc_macro2::TokenStream> {
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
    if kind == PropertyKind::LazyInstance
        && let Some(setter) = setters.first()
    {
        return Err(syn::Error::new(
            setter.rust_name.span(),
            "a `lazy_instance` property cannot have a setter; its value is computed once, \
             then stored on the instance (writable unless `readonly`)",
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
                    quote! { compile_error!(concat!("duplicate setter for property \"", #js_name, "\"")); },
                );
            }
        } else if getter.replace(m.rust_name).is_some() {
            return Err(
                quote! { compile_error!(concat!("duplicate getter for property \"", #js_name, "\"")); },
            );
        }
    }
    emit_property_group(js_name, kind, getter, setter, has_readonly_getter)
}

/// Scans an impl block for `#[jsg_property]` and `#[jsg_inspect_property]` annotations
/// and returns a member factory for each property group.
fn collect_property_registrations(
    impl_block: &ItemImpl,
) -> Result<Vec<MemberRegistration>, proc_macro2::TokenStream> {
    scan_property_annotations(impl_block)?
        .into_iter()
        .map(|((js_name, kind), methods)| validate_and_emit_property(&js_name, kind, methods))
        .collect()
}

#[cfg(test)]
#[path = "resource-test.rs"]
mod tests;
