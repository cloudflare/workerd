// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use proc_macro::TokenStream;
use quote::quote;
use quote::quote_spanned;
use syn::spanned::Spanned;

/// Expands `#[jsg_traced]` on a struct or an enum. Unlike `#[jsg_struct]`'s, the field bounds are
/// not higher-ranked: a trace that visits nothing is wrong for a field that holds a GC reference,
/// so such a field must fail to compile, at the field, rather than leave the type without the
/// marker. An enum's bounds cover the fields of every variant.
pub fn generate_traced_impl(item: TokenStream) -> TokenStream {
    let input = syn::parse_macro_input!(item as syn::DeriveInput);
    let fields: Vec<&syn::Field> = match &input.data {
        syn::Data::Struct(data) => data.fields.iter().collect(),
        syn::Data::Enum(data) => data.variants.iter().flat_map(|v| &v.fields).collect(),
        syn::Data::Union(_) => {
            return syn::Error::new(input.span(), "#[jsg_traced] takes a struct or an enum")
                .to_compile_error()
                .into();
        }
    };
    let name = &input.ident;
    let (impl_generics, type_generics, where_clause) = input.generics.split_for_impl();
    let predicates = where_clause
        .into_iter()
        .flat_map(|clause| &clause.predicates);
    let bounds = fields.iter().map(|field| {
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
