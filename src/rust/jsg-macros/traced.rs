// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use proc_macro::TokenStream;
use quote::quote;
use quote::quote_spanned;
use syn::spanned::Spanned;

/// Expands `#[jsg_traced]`. Unlike `#[jsg_struct]`'s, the field bounds are not higher-ranked: a
/// trace that visits nothing is wrong for a field that holds a GC reference, so such a field must
/// fail to compile, at the field, rather than leave the struct without the marker.
pub fn generate_traced_impl(item: TokenStream) -> TokenStream {
    let input = syn::parse_macro_input!(item as syn::ItemStruct);
    let name = &input.ident;
    let (impl_generics, type_generics, where_clause) = input.generics.split_for_impl();
    let predicates = where_clause
        .into_iter()
        .flat_map(|clause| &clause.predicates);
    let bounds = input.fields.iter().map(|field| {
        let ty = &field.ty;
        quote_spanned! {ty.span()=> #ty: jsg::HoldsNoGcReferences,}
    });
    let bounds = quote! { #(#predicates,)* #(#bounds)* };

    quote! {
        #input

        #[automatically_derived]
        impl #impl_generics jsg::Traced for #name #type_generics where #bounds {
            fn trace(&self, _visitor: &mut jsg::GcVisitor) {}
        }

        #[automatically_derived]
        impl #impl_generics jsg::HoldsNoGcReferences for #name #type_generics where #bounds {}
    }
    .into()
}
