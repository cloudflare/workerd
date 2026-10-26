// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! `#[jsg_oneof]`: unions, the Rust counterpart of C++ JSG's `kj::OneOf`.

use proc_macro::TokenStream;
use proc_macro2::Spacing;
use proc_macro2::TokenTree;
use quote::ToTokens;
use quote::quote;
use quote::quote_spanned;
use syn::Data;
use syn::DeriveInput;
use syn::Fields;
use syn::GenericParam;
use syn::Lifetime;
use syn::spanned::Spanned;

use crate::utils::error;

/// Expands `#[jsg_oneof]` on `item`.
pub fn expand(item: TokenStream) -> TokenStream {
    let input = syn::parse_macro_input!(item as DeriveInput);
    match Union::parse(&input) {
        Ok(union) => union.generate(&input).into(),
        Err(err) => err,
    }
}

/// A `#[jsg_oneof]` enum.
struct Union<'a> {
    /// The enum's lifetime parameter, which its handle variants borrow, if it has one.
    lifetime: Option<&'a Lifetime>,
    /// The name of each variant.
    variant_names: Vec<&'a syn::Ident>,
    /// The type each variant holds.
    inner_types: Vec<&'a syn::Type>,
}

impl<'a> Union<'a> {
    /// Checks that `input` is an enum the macro supports, and collects its variants.
    fn parse(input: &'a DeriveInput) -> Result<Self, TokenStream> {
        let Data::Enum(data) = &input.data else {
            return Err(error(input, "#[jsg_oneof] can only be applied to enums"));
        };
        let lifetime = Self::parse_lifetime(input)?;

        let mut variant_names = Vec::new();
        let mut inner_types = Vec::new();
        for variant in &data.variants {
            let Fields::Unnamed(fields) = &variant.fields else {
                return Err(error(
                    variant,
                    "#[jsg_oneof] variants must be tuple variants (e.g., `Variant(Type)`)",
                ));
            };
            if fields.unnamed.len() != 1 {
                return Err(error(
                    variant,
                    "#[jsg_oneof] variants must have exactly one field",
                ));
            }
            variant_names.push(&variant.ident);
            inner_types.push(&fields.unnamed[0].ty);
        }
        if variant_names.is_empty() {
            return Err(error(input, "#[jsg_oneof] requires at least one variant"));
        }

        Ok(Self {
            lifetime,
            variant_names,
            inner_types,
        })
    }

    /// The enum's lifetime parameter. A union takes at most one, for the scope of the handles
    /// it holds, and no type or const parameters, as `kj::OneOf` has none.
    fn parse_lifetime(input: &'a DeriveInput) -> Result<Option<&'a Lifetime>, TokenStream> {
        let generics = &input.generics;
        if let Some(where_clause) = &generics.where_clause {
            return Err(error(
                where_clause,
                "#[jsg_oneof] unions can't have a where clause",
            ));
        }
        let mut lifetime = None;
        for param in &generics.params {
            match param {
                GenericParam::Lifetime(param) if lifetime.is_none() && param.bounds.is_empty() => {
                    lifetime = Some(&param.lifetime);
                }
                _ => {
                    return Err(error(
                        param,
                        "#[jsg_oneof] unions can have only one lifetime parameter, with no \
                         bounds, for the handles they hold",
                    ));
                }
            }
        }
        Ok(lifetime)
    }

    /// Generates the enum and the union's trait implementations.
    fn generate(&self, input: &DeriveInput) -> proc_macro2::TokenStream {
        let checks = self.generate_checks(input);
        let type_impl = self.generate_type(input);
        let from_js_impl = self.generate_from_js(input);
        let traced_impl = self.generate_traced(input);
        let to_js_impl = self.generate_to_js(input);
        let type_info_impl = self.generate_type_info(input);
        quote! {
            #input
            #checks
            #type_impl
            #from_js_impl
            #traced_impl
            #to_js_impl
            #type_info_impl
        }
    }

    /// The enum's type, with its lifetime parameter if it has one.
    fn self_type(&self, input: &DeriveInput) -> proc_macro2::TokenStream {
        let name = &input.ident;
        let lifetime = self.lifetime.into_iter();
        quote! { #name<#(#lifetime)*> }
    }

    /// `ty` with the enum's lifetime replaced by `'static`, so that it can be named outside the
    /// enum's impls, in a constant.
    fn with_static_lifetime(&self, ty: impl ToTokens) -> proc_macro2::TokenStream {
        let tokens = ty.into_token_stream();
        if let Some(lifetime) = self.lifetime {
            replace_lifetime(tokens, &lifetime.ident)
        } else {
            tokens
        }
    }

    /// Compile-time checks that the union follows Web IDL's rules for unions, as C++ JSG's
    /// `webidl::UnionTypeValidator` checks a `kj::OneOf`. The rules on the variants' categories
    /// are checked through their types' `jsg::UnionVariant::UNION_TYPES`, which flatten nested
    /// unions and nullable types. A variant type that appears twice has two conflicting
    /// implementations of a local trait, whose error points at the variant; a `Nullable<T>`
    /// variant implements it for `T`, as `webidl::FlattenedTypeTraits` flattens `kj::Maybe<T>` to
    /// `T`. That can't see into a nested union, so the names in `jsg::Type::UNION_NAMES`, which
    /// flatten as the categories do, catch a resource, handle or `SystemTime` that appears in
    /// both a union and a union nested in it.
    fn generate_checks(&self, input: &DeriveInput) -> proc_macro2::TokenStream {
        let self_type = self.with_static_lifetime(self.self_type(input));
        // Spanned on the variant, so that a duplicate's error points at it.
        let distinct_impls = self.inner_types.iter().map(|ty| {
            let static_type = self.with_static_lifetime(strip_nullable(ty));
            quote_spanned! {ty.span()=>
                impl JsgOneofVariantsMustHaveDistinctTypes for #static_type {}
            }
        });
        quote! {
            const _: () = <#self_type as jsg::Type>::UNION_TYPES.validate();
            const _: () = jsg::validate_union_names(<#self_type as jsg::Type>::UNION_NAMES);

            const _: () = {
                /// A `#[jsg_oneof]` union may not contain duplicate types; a type that has two
                /// implementations of this trait is the type of two variants.
                trait JsgOneofVariantsMustHaveDistinctTypes {}
                #(#distinct_impls)*
            };
        }
    }

    /// `jsg::Type`, so that the union can be a variant of another union or a sequence's element.
    /// A union is not `jsg::Coercible`, so it can't be a `NonCoercible<T>`.
    fn generate_type(&self, input: &DeriveInput) -> proc_macro2::TokenStream {
        let name = &input.ident;
        let self_type = self.self_type(input);
        let lifetime = self.lifetime.into_iter();
        let inner_types = &self.inner_types;
        // A constant's type can't use the enum's lifetime, so the names are those of the
        // `'static` variant types; a lifetime doesn't change a type's name.
        let static_types: Vec<_> = inner_types
            .iter()
            .map(|ty| self.with_static_lifetime(ty))
            .collect();
        quote! {
            #[automatically_derived]
            impl<#(#lifetime)*> jsg::Type for #self_type {
                const UNION_TYPES: jsg::UnionTypes = jsg::UnionTypes::OTHER
                    #(.plus(<#inner_types as jsg::UnionVariant>::UNION_TYPES))*;

                const UNION_NAMES: &'static [&'static str] = {
                    const LEN: usize =
                        0 #(+ <#static_types as jsg::UnionVariant>::UNION_NAMES.len())*;
                    const NAMES: [&str; LEN] = jsg::concat_union_names(
                        &[#(<#static_types as jsg::UnionVariant>::UNION_NAMES),*],
                    );
                    &NAMES
                };

                fn class_name() -> &'static str {
                    stringify!(#name)
                }

                fn is_exact(value: jsg::v8::Local<'_, jsg::v8::Value>) -> bool {
                    #(<#inner_types as jsg::UnionVariant>::is_exact(value))||*
                }
            }
        }
    }

    /// `jsg::FromJS`, which tries the variants in the passes of `jsg::try_union_from_js`.
    fn generate_from_js(&self, input: &DeriveInput) -> proc_macro2::TokenStream {
        let self_type = self.self_type(input);
        let s = self.scope_lifetime();
        let impl_lifetime = &s;
        let variant_names = &self.variant_names;
        let inner_types = &self.inner_types;
        quote! {
            #[automatically_derived]
            impl<#impl_lifetime> jsg::FromJS<#s> for #self_type {
                type ResultType = Self;

                fn type_name() -> String {
                    [#(<#inner_types as jsg::FromJS<#s>>::type_name()),*].join(" or ")
                }

                fn from_js(lock: &mut jsg::Lock<#s>, value: jsg::v8::Local<#s, jsg::v8::Value>) -> Result<Self::ResultType, jsg::Error> {
                    let type_name = value.type_of(lock);
                    Self::try_from_js(lock, value)?.ok_or_else(|| {
                        jsg::Error::new_type_error(format!(
                            "Expected {} but got {}",
                            <Self as jsg::FromJS<#s>>::type_name(),
                            type_name
                        ))
                    })
                }

                fn try_from_js(lock: &mut jsg::Lock<#s>, value: jsg::v8::Local<#s, jsg::v8::Value>) -> Result<Option<Self::ResultType>, jsg::Error> {
                    jsg::try_union_from_js(lock, value, Self::try_from_js_in_union_pass)
                }

                fn try_from_js_in_union_pass(
                    lock: &mut jsg::Lock<#s>,
                    value: jsg::v8::Local<#s, jsg::v8::Value>,
                    pass: jsg::UnionMember,
                ) -> Result<Option<Self::ResultType>, jsg::Error> {
                    #(
                        if let Some(value) = <#inner_types as jsg::FromJS<#s>>::try_from_js_in_union_pass(lock, value, pass)? {
                            return Ok(Some(Self::#variant_names(value)));
                        }
                    )*
                    Ok(None)
                }
            }
        }
    }

    /// `jsg::Traced`: a union holds whatever its variant holds, so it can be a resource field.
    /// A union that holds handles can't, so its implementation requires what its variants can't
    /// provide, and it is never used.
    fn generate_traced(&self, input: &DeriveInput) -> proc_macro2::TokenStream {
        let self_type = self.self_type(input);
        let lifetime = self.lifetime.into_iter();
        let variant_names = &self.variant_names;
        let inner_types = &self.inner_types;
        let where_clause = self.lifetime.map(|_| {
            quote! { where #(#inner_types: jsg::Traced,)* }
        });
        quote! {
            #[automatically_derived]
            impl<#(#lifetime)*> jsg::Traced for #self_type #where_clause {
                fn holds_gc_references() -> bool {
                    #(<#inner_types as jsg::Traced>::holds_gc_references())||*
                }

                fn trace(&self, visitor: &mut jsg::v8::GcVisitor) {
                    match self {
                        #(Self::#variant_names(value) => jsg::Traced::trace(value, visitor),)*
                    }
                }
            }
        }
    }

    /// `jsg::ToJS`, when every variant converts to JavaScript.
    fn generate_to_js(&self, input: &DeriveInput) -> proc_macro2::TokenStream {
        let self_type = self.self_type(input);
        let s = self.scope_lifetime();
        let variant_names = &self.variant_names;
        let inner_types = &self.inner_types;
        quote! {
            #[automatically_derived]
            impl<#s> jsg::ToJS<#s> for #self_type
            where
                #(#inner_types: jsg::ToJS<#s>,)*
            {
                fn to_js(self, lock: &mut jsg::Lock<#s>) -> Result<jsg::v8::Local<#s, jsg::v8::Value>, jsg::Error> {
                    match self {
                        #(Self::#variant_names(value) => jsg::ToJS::to_js(value, lock),)*
                    }
                }
            }
        }
    }

    /// `jsg::TypeInfo`, which describes the union as its variants' types, as C++ RTTI describes
    /// a `kj::OneOf`.
    fn generate_type_info(&self, input: &DeriveInput) -> proc_macro2::TokenStream {
        let self_type = self.self_type(input);
        let lifetime = self.lifetime.into_iter();
        let inner_types = &self.inner_types;
        quote! {
            #[automatically_derived]
            impl<#(#lifetime)*> jsg::TypeInfo for #self_type {
                fn describe(cx: &mut jsg::rtti::Context) -> jsg::rtti::Type {
                    jsg::rtti::Type::OneOf(vec![
                        #(<#inner_types as jsg::TypeInfo>::describe(cx)),*
                    ])
                }
            }
        }
    }

    /// The lifetime of the lock in the union's `FromJS` and `ToJS` impls: the enum's lifetime
    /// parameter, which its handles borrow, or a fresh one.
    fn scope_lifetime(&self) -> Lifetime {
        self.lifetime
            .cloned()
            .unwrap_or_else(|| Lifetime::new("'s", proc_macro2::Span::call_site()))
    }
}

/// `ty` without the `Nullable<…>`s around it, as written: `T` for `Nullable<T>` or
/// `jsg::Nullable<Nullable<T>>`. A type alias of a `Nullable<T>` is not seen through.
fn strip_nullable(ty: &syn::Type) -> &syn::Type {
    let syn::Type::Path(path) = ty else {
        return ty;
    };
    if path.qself.is_some() {
        return ty;
    }
    let Some(segment) = path.path.segments.last() else {
        return ty;
    };
    if segment.ident != "Nullable" {
        return ty;
    }
    let syn::PathArguments::AngleBracketed(args) = &segment.arguments else {
        return ty;
    };
    match args.args.first() {
        Some(syn::GenericArgument::Type(inner)) if args.args.len() == 1 => strip_nullable(inner),
        _ => ty,
    }
}

/// `tokens` with every use of the lifetime named `name` replaced by `'static`.
fn replace_lifetime(
    tokens: proc_macro2::TokenStream,
    name: &proc_macro2::Ident,
) -> proc_macro2::TokenStream {
    let mut output = Vec::new();
    let mut tokens = tokens.into_iter().peekable();
    while let Some(token) = tokens.next() {
        match token {
            TokenTree::Punct(punct)
                if punct.as_char() == '\'' && punct.spacing() == Spacing::Joint =>
            {
                if let Some(TokenTree::Ident(ident)) = tokens.peek()
                    && ident == name
                {
                    tokens.next();
                    output.extend(quote! { 'static });
                } else {
                    output.push(TokenTree::Punct(punct));
                }
            }
            TokenTree::Group(group) => {
                let mut replaced = proc_macro2::Group::new(
                    group.delimiter(),
                    replace_lifetime(group.stream(), name),
                );
                replaced.set_span(group.span());
                output.push(TokenTree::Group(replaced));
            }
            token => output.push(token),
        }
    }
    output.into_iter().collect()
}

#[cfg(test)]
#[path = "oneof-test.rs"]
mod tests;
