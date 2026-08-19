use proc_macro2::TokenStream;
use quote::ToTokens;
use syn::Lifetime;
use syn::Token;
use syntax::Impl;
use syntax::Lifetimes;
use syntax::instantiate::NamedImplKey;
use syntax::resolve::Resolution;

pub struct ImplGenerics<'a> {
    explicit_impl: Option<&'a Impl>,
    resolve: Resolution<'a>,
}

pub struct TyGenerics<'a> {
    key: NamedImplKey<'a>,
    explicit_impl: Option<&'a Impl>,
    resolve: Resolution<'a>,
}

pub fn split_for_impl<'a>(
    key: NamedImplKey<'a>,
    explicit_impl: Option<&'a Impl>,
    resolve: Resolution<'a>,
) -> (ImplGenerics<'a>, TyGenerics<'a>) {
    let impl_generics = ImplGenerics {
        explicit_impl,
        resolve,
    };
    let ty_generics = TyGenerics {
        key,
        explicit_impl,
        resolve,
    };
    (impl_generics, ty_generics)
}

impl ToTokens for ImplGenerics<'_> {
    fn to_tokens(&self, tokens: &mut TokenStream) {
        if let Some(imp) = self.explicit_impl {
            imp.impl_generics.to_tokens(tokens);
        } else {
            self.resolve.generics.to_tokens(tokens);
        }
    }
}

impl ToTokens for TyGenerics<'_> {
    fn to_tokens(&self, tokens: &mut TokenStream) {
        if let Some(imp) = self.explicit_impl {
            imp.ty_generics.to_tokens(tokens);
        } else if !self.resolve.generics.lifetimes.is_empty() {
            let span = self.key.rust.span();
            self.key
                .lt_token
                .unwrap_or_else(|| Token![<](span))
                .to_tokens(tokens);
            self.resolve.generics.lifetimes.to_tokens(tokens);
            self.key
                .gt_token
                .unwrap_or_else(|| Token![>](span))
                .to_tokens(tokens);
        }
    }
}

pub struct UnderscoreLifetimes<'a> {
    generics: &'a Lifetimes,
}

pub fn to_underscore_lifetimes(generics: &Lifetimes) -> UnderscoreLifetimes<'_> {
    UnderscoreLifetimes { generics }
}

impl ToTokens for UnderscoreLifetimes<'_> {
    fn to_tokens(&self, tokens: &mut TokenStream) {
        let Lifetimes {
            lt_token,
            lifetimes,
            gt_token,
        } = self.generics;
        lt_token.to_tokens(tokens);
        for pair in lifetimes.pairs() {
            let (lifetime, punct) = pair.into_tuple();
            let lifetime = Lifetime::new("'_", lifetime.span());
            lifetime.to_tokens(tokens);
            punct.to_tokens(tokens);
        }
        gt_token.to_tokens(tokens);
    }
}
