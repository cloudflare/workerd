use proc_macro2::TokenStream;
use proc_macro2::TokenTree;
use quote::ToTokens;
use quote::format_ident;
use quote::quote;
use syn::ext::IdentExt;
use syntax::qualified::QualifiedName;

pub enum Crate {
    Cxx,
    DollarCrate(TokenTree),
}

impl ToTokens for Crate {
    fn to_tokens(&self, tokens: &mut TokenStream) {
        match self {
            Self::Cxx => tokens.extend(quote!(::cxx)),
            Self::DollarCrate(krate) => krate.to_tokens(tokens),
        }
    }
}

// "folly::File" => `(f, o, l, l, y, (), F, i, l, e)`
pub fn expand(krate: Crate, arg: QualifiedName) -> TokenStream {
    let mut ids = Vec::new();

    for word in arg.segments {
        if !ids.is_empty() {
            ids.push(quote!(()));
        }
        for ch in word.unraw().to_string().chars() {
            ids.push(match ch {
                'A'..='Z' | 'a'..='z' => {
                    let t = format_ident!("{}", ch);
                    quote!(#krate::#t)
                }
                '0'..='9' | '_' => {
                    let t = format_ident!("_{}", ch);
                    quote!(#krate::#t)
                }
                _ => quote!([(); #ch as _]),
            });
        }
    }

    quote! { (#(#ids,)*) }
}
