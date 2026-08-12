use proc_macro2::TokenStream;
use quote::ToTokens;
use quote::quote_spanned;
use syn::Token;
use syntax::Receiver;

pub struct ReceiverType<'a>(pub &'a Receiver);
pub struct ReceiverTypeSelf<'a>(pub &'a Receiver);

impl ToTokens for ReceiverType<'_> {
    fn to_tokens(&self, tokens: &mut TokenStream) {
        let Receiver {
            pinned: _,
            ampersand,
            lifetime,
            mutable: _,
            var: _,
            colon_token: _,
            ty,
            shorthand: _,
            pin_tokens,
            mutability,
        } = &self.0;
        if let Some((pin, langle, _rangle)) = pin_tokens {
            tokens.extend(quote_spanned!(pin.span=> ::cxx::core::pin::Pin));
            langle.to_tokens(tokens);
        }
        ampersand.to_tokens(tokens);
        lifetime.to_tokens(tokens);
        mutability.to_tokens(tokens);
        ty.to_tokens(tokens);
        if let Some((_pin, _langle, rangle)) = pin_tokens {
            rangle.to_tokens(tokens);
        }
    }
}

impl ToTokens for ReceiverTypeSelf<'_> {
    fn to_tokens(&self, tokens: &mut TokenStream) {
        let Receiver {
            pinned: _,
            ampersand,
            lifetime,
            mutable: _,
            var: _,
            colon_token: _,
            ty,
            shorthand: _,
            pin_tokens,
            mutability,
        } = &self.0;
        if let Some((pin, langle, _rangle)) = pin_tokens {
            tokens.extend(quote_spanned!(pin.span=> ::cxx::core::pin::Pin));
            langle.to_tokens(tokens);
        }
        ampersand.to_tokens(tokens);
        lifetime.to_tokens(tokens);
        mutability.to_tokens(tokens);
        Token![Self](ty.rust.span()).to_tokens(tokens);
        if let Some((_pin, _langle, rangle)) = pin_tokens {
            rangle.to_tokens(tokens);
        }
    }
}
