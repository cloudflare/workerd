use proc_macro2::Ident;
use proc_macro2::Span;
use proc_macro2::TokenStream;
use quote::ToTokens;
use quote::quote_spanned;
use syn::Token;
use syn::spanned::Spanned;
use syn::token;

use crate::Array;
use crate::Atom;
use crate::Derive;
use crate::Enum;
use crate::EnumRepr;
use crate::ExternFn;
use crate::ExternType;
use crate::Future;
use crate::Impl;
use crate::Lifetimes;
use crate::NamedType;
use crate::Ptr;
use crate::Ref;
use crate::Signature;
use crate::SliceRef;
use crate::Struct;
use crate::Ty1;
use crate::Type;
use crate::TypeAlias;
use crate::Var;
use crate::atom::Atom::*;

impl ToTokens for Type {
    fn to_tokens(&self, tokens: &mut TokenStream) {
        match self {
            Self::Ident(ident) => {
                if ident.rust == Char {
                    let span = ident.rust.span();
                    tokens.extend(quote_spanned!(span=> ::cxx::core::ffi::));
                } else if ident.rust == Char16 {
                    // Unlike c_char, there is no core::ffi equivalent of
                    // char16_t; the cxx crate defines c_char16 itself.
                    let span = ident.rust.span();
                    tokens.extend(quote_spanned!(span=> ::cxx::));
                } else if ident.rust == CxxString {
                    let span = ident.rust.span();
                    tokens.extend(quote_spanned!(span=> ::cxx::));
                } else if ident.rust == RustString {
                    let span = ident.rust.span();
                    tokens.extend(quote_spanned!(span=> ::cxx::alloc::string::));
                }
                ident.to_tokens(tokens);
            }
            Self::RustBox(ty)
            | Self::UniquePtr(ty)
            | Self::KjOwn(ty)
            | Self::KjRc(ty)
            | Self::KjArc(ty)
            | Self::SharedPtr(ty)
            | Self::WeakPtr(ty)
            | Self::CxxVector(ty)
            | Self::KjMaybe(ty)
            | Self::RustVec(ty) => ty.to_tokens(tokens),
            Self::Ref(r) | Self::Str(r) => r.to_tokens(tokens),
            Self::Ptr(p) => p.to_tokens(tokens),
            Self::Array(a) => a.to_tokens(tokens),
            Self::Fn(f) => f.to_tokens(tokens),
            Self::Void(span) => tokens.extend(quote_spanned!(*span=> ())),
            Self::KjDate(span) => tokens.extend(quote_spanned!(*span=> ::kj_rs::KjDate)),
            Self::SliceRef(r) => r.to_tokens(tokens),
            Self::Future(f) => f.to_tokens(tokens),
        }
    }
}

impl ToTokens for Var {
    fn to_tokens(&self, tokens: &mut TokenStream) {
        let Self {
            cfg: _,
            doc: _,
            attrs: _,
            visibility: _,
            name,
            colon_token: _,
            ty,
        } = self;
        name.rust.to_tokens(tokens);
        Token![:](name.rust.span()).to_tokens(tokens);
        ty.to_tokens(tokens);
    }
}

impl ToTokens for Ty1 {
    fn to_tokens(&self, tokens: &mut TokenStream) {
        let Self {
            name,
            langle,
            inner,
            rangle,
        } = self;
        let span = name.span();
        match name.to_string().as_str() {
            "UniquePtr" | "SharedPtr" | "WeakPtr" | "CxxVector" => {
                tokens.extend(quote_spanned!(span=> ::cxx::));
            }
            "KjOwn" => {
                tokens.extend(quote_spanned!(span => ::kj_rs::repr::));
            }
            "KjRc" => {
                tokens.extend(quote_spanned!(span => ::kj_rs::repr::));
            }
            "KjArc" => {
                tokens.extend(quote_spanned!(span => ::kj_rs::repr::));
            }
            "Box" => {
                tokens.extend(quote_spanned!(span=> ::cxx::alloc::boxed::));
            }
            "KjMaybe" => {
                tokens.extend(quote_spanned!(span=> ::kj_rs::repr::));
            }
            "Vec" => {
                tokens.extend(quote_spanned!(span=> ::cxx::alloc::vec::));
            }
            _ => {}
        }
        name.to_tokens(tokens);
        langle.to_tokens(tokens);
        inner.to_tokens(tokens);
        rangle.to_tokens(tokens);
    }
}

impl ToTokens for Ref {
    fn to_tokens(&self, tokens: &mut TokenStream) {
        let Self {
            pinned: _,
            ampersand,
            lifetime,
            mutable: _,
            inner,
            pin_tokens,
            mutability,
        } = self;
        if let Some((pin, langle, _rangle)) = pin_tokens {
            tokens.extend(quote_spanned!(pin.span=> ::cxx::core::pin::Pin));
            langle.to_tokens(tokens);
        }
        ampersand.to_tokens(tokens);
        lifetime.to_tokens(tokens);
        mutability.to_tokens(tokens);
        inner.to_tokens(tokens);
        if let Some((_pin, _langle, rangle)) = pin_tokens {
            rangle.to_tokens(tokens);
        }
    }
}

impl ToTokens for Ptr {
    fn to_tokens(&self, tokens: &mut TokenStream) {
        let Self {
            star,
            mutable: _,
            inner,
            mutability,
            constness,
        } = self;
        star.to_tokens(tokens);
        mutability.to_tokens(tokens);
        constness.to_tokens(tokens);
        inner.to_tokens(tokens);
    }
}

impl ToTokens for SliceRef {
    fn to_tokens(&self, tokens: &mut TokenStream) {
        let Self {
            ampersand,
            lifetime,
            mutable: _,
            bracket,
            inner,
            mutability,
        } = self;
        ampersand.to_tokens(tokens);
        lifetime.to_tokens(tokens);
        mutability.to_tokens(tokens);
        bracket.surround(tokens, |tokens| {
            inner.to_tokens(tokens);
        });
    }
}

impl ToTokens for Array {
    fn to_tokens(&self, tokens: &mut TokenStream) {
        let Self {
            bracket,
            inner,
            semi_token,
            len: _,
            len_token,
        } = self;
        bracket.surround(tokens, |tokens| {
            inner.to_tokens(tokens);
            semi_token.to_tokens(tokens);
            len_token.to_tokens(tokens);
        });
    }
}

impl ToTokens for Atom {
    fn to_tokens(&self, tokens: &mut TokenStream) {
        Ident::new(self.as_ref(), Span::call_site()).to_tokens(tokens);
    }
}

impl ToTokens for Derive {
    fn to_tokens(&self, tokens: &mut TokenStream) {
        Ident::new(self.what.as_ref(), self.span).to_tokens(tokens);
    }
}

impl ToTokens for ExternType {
    fn to_tokens(&self, tokens: &mut TokenStream) {
        // Notional token range for error reporting purposes.
        self.type_token.to_tokens(tokens);
        self.name.rust.to_tokens(tokens);
        self.generics.to_tokens(tokens);
    }
}

impl ToTokens for TypeAlias {
    fn to_tokens(&self, tokens: &mut TokenStream) {
        // Notional token range for error reporting purposes.
        self.type_token.to_tokens(tokens);
        self.name.rust.to_tokens(tokens);
        self.generics.to_tokens(tokens);
    }
}

impl ToTokens for Struct {
    fn to_tokens(&self, tokens: &mut TokenStream) {
        // Notional token range for error reporting purposes.
        self.struct_token.to_tokens(tokens);
        self.name.rust.to_tokens(tokens);
        self.generics.to_tokens(tokens);
    }
}

impl ToTokens for Enum {
    fn to_tokens(&self, tokens: &mut TokenStream) {
        // Notional token range for error reporting purposes.
        self.enum_token.to_tokens(tokens);
        self.name.rust.to_tokens(tokens);
        self.generics.to_tokens(tokens);
    }
}

impl ToTokens for ExternFn {
    fn to_tokens(&self, tokens: &mut TokenStream) {
        // Notional token range for error reporting purposes.
        self.unsafety.to_tokens(tokens);
        self.sig.fn_token.to_tokens(tokens);
        self.semi_token.to_tokens(tokens);
    }
}

impl ToTokens for Future {
    fn to_tokens(&self, tokens: &mut TokenStream) {
        let output = &self.output;
        tokens.extend(
            quote_spanned!(self.output.span()=> impl ::std::future::Future<Output = ::std::result::Result<#output, ::cxx::KjException>>),
        );
    }
}

impl ToTokens for Impl {
    fn to_tokens(&self, tokens: &mut TokenStream) {
        let Self {
            cfg: _,
            impl_token,
            impl_generics,
            negative: _,
            ty,
            ty_generics: _,
            brace_token,
            negative_token,
        } = self;
        impl_token.to_tokens(tokens);
        impl_generics.to_tokens(tokens);
        negative_token.to_tokens(tokens);
        ty.to_tokens(tokens);
        brace_token.surround(tokens, |_tokens| {});
    }
}

impl ToTokens for Lifetimes {
    fn to_tokens(&self, tokens: &mut TokenStream) {
        let Self {
            lt_token,
            lifetimes,
            gt_token,
        } = self;
        lt_token.to_tokens(tokens);
        lifetimes.to_tokens(tokens);
        gt_token.to_tokens(tokens);
    }
}

impl ToTokens for Signature {
    fn to_tokens(&self, tokens: &mut TokenStream) {
        let Self {
            asyncness: _,
            unsafety: _,
            fn_token,
            generics: _,
            receiver: _,
            args,
            ret,
            throws: _,
            paren_token,
            throws_tokens,
        } = self;
        fn_token.to_tokens(tokens);
        paren_token.surround(tokens, |tokens| {
            args.to_tokens(tokens);
        });
        if let Some(ret) = ret {
            Token![->](paren_token.span.join()).to_tokens(tokens);
            if let Some((result, langle, rangle)) = throws_tokens {
                result.to_tokens(tokens);
                langle.to_tokens(tokens);
                ret.to_tokens(tokens);
                rangle.to_tokens(tokens);
            } else {
                ret.to_tokens(tokens);
            }
        } else if let Some((result, langle, rangle)) = throws_tokens {
            Token![->](paren_token.span.join()).to_tokens(tokens);
            result.to_tokens(tokens);
            langle.to_tokens(tokens);
            token::Paren(langle.span).surround(tokens, |_| ());
            rangle.to_tokens(tokens);
        }
    }
}

impl ToTokens for EnumRepr {
    fn to_tokens(&self, tokens: &mut TokenStream) {
        match self {
            Self::Native { atom, repr_type: _ } => atom.to_tokens(tokens),
            #[cfg(feature = "experimental-enum-variants-from-header")]
            EnumRepr::Foreign { rust_type } => rust_type.to_tokens(tokens),
        }
    }
}

impl ToTokens for NamedType {
    fn to_tokens(&self, tokens: &mut TokenStream) {
        let Self { rust, generics } = self;
        rust.to_tokens(tokens);
        generics.to_tokens(tokens);
    }
}
