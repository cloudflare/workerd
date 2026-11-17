// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Heap snapshot code generation for `#[jsg_resource]` structs.
//!
//! - `memory_info` generates `jsg::VisitForMemoryInfo` from the named fields, reporting each
//!   under its name, as the generated `Traced` traces each field, so that a field added later
//!   is reported too. `#[jsg_memory_info(skip)]` leaves a field out.
//! - `custom_memory_info` leaves `jsg::VisitForMemoryInfo` to the author.
//!
//! Either makes the generated `GarbageCollected::memory_info` call `VisitForMemoryInfo`;
//! without them a resource reports no fields.

use proc_macro2::TokenStream;
use quote::quote;
use quote::quote_spanned;
use syn::ext::IdentExt;
use syn::spanned::Spanned;

use crate::utils::is_attr;

/// The field attribute that leaves a field out of the generated `VisitForMemoryInfo`.
const FIELD_ATTR: &str = "jsg_memory_info";

/// What `#[jsg_resource]` on a struct generates for heap snapshots.
pub struct MemoryInfo {
    /// The `GarbageCollected::memory_info` method, or nothing.
    pub gc_fn: TokenStream,
    /// The generated `jsg::VisitForMemoryInfo` impl, or nothing.
    pub visit_impl: TokenStream,
}

/// Which of the struct's flags ask for heap snapshot reporting.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Mode {
    /// Neither: the resource reports no fields.
    None,
    /// `memory_info`: a generated `VisitForMemoryInfo` reports the fields.
    Generated,
    /// `custom_memory_info`: the author's `VisitForMemoryInfo` reports the fields.
    Custom,
}

impl Mode {
    /// The mode that the arguments of `#[jsg_resource]` on a struct ask for.
    pub fn from_attr(attr: TokenStream) -> syn::Result<Self> {
        use syn::punctuated::Punctuated;

        let metas = syn::parse::Parser::parse2(
            Punctuated::<syn::Meta, syn::Token![,]>::parse_terminated,
            attr,
        )?;
        let flag = |name: &str| {
            metas
                .iter()
                .find(|meta| matches!(meta, syn::Meta::Path(path) if path.is_ident(name)))
        };
        match (flag("memory_info"), flag("custom_memory_info")) {
            (Some(_), Some(custom)) => Err(syn::Error::new_spanned(
                custom,
                "`memory_info` generates `jsg::VisitForMemoryInfo` and `custom_memory_info` \
                 leaves it to you; use one",
            )),
            (Some(_), None) => Ok(Self::Generated),
            (None, Some(_)) => Ok(Self::Custom),
            (None, None) => Ok(Self::None),
        }
    }
}

/// Generates what the arguments `attr` of `#[jsg_resource]` on a struct ask for, and returns the
/// struct without its `#[jsg_memory_info(...)]` attributes.
pub fn generate_for(
    attr: TokenStream,
    input: &syn::DeriveInput,
) -> syn::Result<(syn::DeriveInput, MemoryInfo)> {
    let mut input = input.clone();
    let memory_info = generate(Mode::from_attr(attr)?, &mut input)?;
    Ok((input, memory_info))
}

/// Generates what `mode` asks for, and takes the `#[jsg_memory_info(...)]` attributes off the
/// struct's fields, which the compiler does not know.
pub fn generate(mode: Mode, input: &mut syn::DeriveInput) -> syn::Result<MemoryInfo> {
    let generated = mode == Mode::Generated;
    let reported = take_reported_fields(input)?;
    if !generated && let Some(skipped) = reported.iter().find(|field| !field.reported) {
        return Err(syn::Error::new(
            skipped.attr_span,
            "`#[jsg_memory_info(skip)]` needs `memory_info` on the struct's `#[jsg_resource]`",
        ));
    }

    let gc_fn = if mode == Mode::None {
        quote! {}
    } else {
        quote! {
            fn memory_info(&self, tracker: &mut jsg::MemoryTracker<'_>) {
                <Self as jsg::VisitForMemoryInfo>::visit_for_memory_info(self, tracker);
            }
        }
    };

    let visit_impl = if generated {
        let name = &input.ident;
        let statements = reported.iter().filter(|field| field.reported).map(|field| {
            let ident = &field.ident;
            let edge_name = &field.edge_name;
            let ty = &field.ty;
            // Only the trait path takes the type's span, so that a type without
            // `jsg::MemoryInfo` is reported at the field; `self` and `tracker` keep the span of
            // the generated method, whose parameters they are, as the type's tokens may come from
            // another hygiene context, such as the caller of a `macro_rules!` macro.
            let track = quote_spanned! {ty.span()=>
                <#ty as jsg::MemoryInfo>::track_as_field
            };
            quote! {
                #track(&self.#ident, tracker, ::std::option::Option::Some(#edge_name));
            }
        });
        quote! {
            #[automatically_derived]
            impl jsg::VisitForMemoryInfo for #name {
                fn visit_for_memory_info(&self, tracker: &mut jsg::MemoryTracker<'_>) {
                    // For a struct with no reported fields.
                    let _ = &tracker;
                    #(#statements)*
                }
            }
        }
    } else {
        quote! {}
    };

    Ok(MemoryInfo { gc_fn, visit_impl })
}

/// A named field of the struct, and whether the generated `VisitForMemoryInfo` reports it.
struct Field {
    ident: syn::Ident,
    edge_name: syn::LitCStr,
    ty: syn::Type,
    reported: bool,
    /// The span of the `#[jsg_memory_info(skip)]` attribute, if any.
    attr_span: proc_macro2::Span,
}

/// Takes the `#[jsg_memory_info(skip)]` attributes off the struct's named fields, and returns
/// the fields.
fn take_reported_fields(input: &mut syn::DeriveInput) -> syn::Result<Vec<Field>> {
    let syn::Data::Struct(data) = &mut input.data else {
        return Ok(Vec::new());
    };
    let syn::Fields::Named(fields) = &mut data.fields else {
        return Ok(Vec::new());
    };
    let mut result = Vec::new();
    for field in &mut fields.named {
        let mut skip = None;
        let mut error = None;
        field.attrs.retain(|attr| {
            if !is_attr(attr, FIELD_ATTR) {
                return true;
            }
            let parsed = attr.parse_args::<syn::Ident>().and_then(|arg| {
                if arg == "skip" {
                    Ok(())
                } else {
                    Err(syn::Error::new_spanned(&arg, "expected `skip`"))
                }
            });
            match parsed {
                Ok(()) if skip.is_some() => {
                    error.get_or_insert_with(|| {
                        syn::Error::new_spanned(attr, "duplicate `#[jsg_memory_info(skip)]`")
                    });
                }
                Ok(()) => skip = Some(attr.span()),
                Err(err) => {
                    error.get_or_insert(err);
                }
            }
            false
        });
        if let Some(err) = error {
            return Err(err);
        }
        let Some(ident) = field.ident.clone() else {
            continue;
        };
        let Ok(edge_name) = std::ffi::CString::new(ident.unraw().to_string()) else {
            unreachable!("an identifier has no NUL")
        };
        result.push(Field {
            edge_name: syn::LitCStr::new(&edge_name, ident.span()),
            ident,
            ty: field.ty.clone(),
            reported: skip.is_none(),
            attr_span: skip.unwrap_or_else(proc_macro2::Span::call_site),
        });
    }
    Ok(result)
}

#[cfg(test)]
#[path = "memory_info-test.rs"]
mod tests;
