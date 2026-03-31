// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Trace code generation for `#[jsg_resource]` structs.
//!
//! We intentionally do not inspect field types here. Every field is assumed to
//! implement `jsg::Traced`, and the generated body simply delegates to
//! `jsg::Traced::trace(&self.field, visitor)` for each named field.

/// Generates one trace delegation statement per named field.
/// All fields are traced for safety by default, based on enforcing they implement
/// the `Traced` trait, which all `GarbageCollected` types implement, and all non-GC
/// types implement as a no-op.
pub fn generate_trace_statements(
    fields: &syn::punctuated::Punctuated<syn::Field, syn::token::Comma>,
) -> Vec<quote::__private::TokenStream> {
    use quote::quote;

    fields
        .iter()
        .filter_map(|field| {
            let field_name = field.ident.as_ref()?;
            Some(quote! {
                jsg::Traced::trace(&self.#field_name, visitor);
            })
        })
        .collect()
}
