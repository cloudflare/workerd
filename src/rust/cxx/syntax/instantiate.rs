use std::hash::Hash;
use std::hash::Hasher;

use proc_macro2::Ident;
use proc_macro2::Span;
use syn::Token;

use crate::NamedType;
use crate::Ty1;
use crate::Type;

#[derive(Copy, Clone, PartialEq, Eq, Hash)]
pub enum ImplKey<'a> {
    RustBox(NamedImplKey<'a>),
    RustVec(NamedImplKey<'a>),
    UniquePtr(NamedImplKey<'a>),
    Own(NamedImplKey<'a>),
    Maybe(NamedImplKey<'a>),
    KjRc(NamedImplKey<'a>),
    KjArc(NamedImplKey<'a>),
    SharedPtr(NamedImplKey<'a>),
    WeakPtr(NamedImplKey<'a>),
    CxxVector(NamedImplKey<'a>),
}

#[derive(Copy, Clone)]
pub struct NamedImplKey<'a> {
    pub begin_span: Span,
    pub rust: &'a Ident,
    pub lt_token: Option<Token![<]>,
    pub gt_token: Option<Token![>]>,
    pub end_span: Span,
}

impl Type {
    pub fn impl_key(&self) -> Option<ImplKey<'_>> {
        if let Self::RustBox(ty) = self {
            if let Self::Ident(ident) = &ty.inner {
                return Some(ImplKey::RustBox(NamedImplKey::new(ty, ident)));
            }
        } else if let Self::RustVec(ty) = self {
            if let Self::Ident(ident) = &ty.inner {
                return Some(ImplKey::RustVec(NamedImplKey::new(ty, ident)));
            }
        } else if let Self::UniquePtr(ty) = self {
            if let Self::Ident(ident) = &ty.inner {
                return Some(ImplKey::UniquePtr(NamedImplKey::new(ty, ident)));
            }
        } else if let Self::KjOwn(ty) = self {
            if let Self::Ident(ident) = &ty.inner {
                return Some(ImplKey::Own(NamedImplKey::new(ty, ident)));
            }
        } else if let Self::KjMaybe(ty) = self {
            if let Self::Ident(ident) = &ty.inner {
                return Some(ImplKey::Maybe(NamedImplKey::new(ty, ident)));
            }
        } else if let Self::KjRc(ty) = self {
            if let Self::Ident(ident) = &ty.inner {
                return Some(ImplKey::KjRc(NamedImplKey::new(ty, ident)));
            }
        } else if let Self::KjArc(ty) = self {
            if let Self::Ident(ident) = &ty.inner {
                return Some(ImplKey::KjArc(NamedImplKey::new(ty, ident)));
            }
        } else if let Self::SharedPtr(ty) = self {
            if let Self::Ident(ident) = &ty.inner {
                return Some(ImplKey::SharedPtr(NamedImplKey::new(ty, ident)));
            }
        } else if let Self::WeakPtr(ty) = self {
            if let Self::Ident(ident) = &ty.inner {
                return Some(ImplKey::WeakPtr(NamedImplKey::new(ty, ident)));
            }
        } else if let Self::CxxVector(ty) = self
            && let Self::Ident(ident) = &ty.inner
        {
            return Some(ImplKey::CxxVector(NamedImplKey::new(ty, ident)));
        }
        None
    }
}

impl PartialEq for NamedImplKey<'_> {
    fn eq(&self, other: &Self) -> bool {
        PartialEq::eq(self.rust, other.rust)
    }
}

impl Eq for NamedImplKey<'_> {}

impl Hash for NamedImplKey<'_> {
    fn hash<H: Hasher>(&self, hasher: &mut H) {
        self.rust.hash(hasher);
    }
}

impl<'a> NamedImplKey<'a> {
    fn new(outer: &Ty1, inner: &'a NamedType) -> Self {
        NamedImplKey {
            begin_span: outer.name.span(),
            rust: &inner.rust,
            lt_token: inner.generics.lt_token,
            gt_token: inner.generics.gt_token,
            end_span: outer.rangle.span,
        }
    }
}
