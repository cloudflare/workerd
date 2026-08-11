use std::fmt::Display;
use std::fmt::{self};
use std::slice::Iter;

use quote::IdentFragment;
use syn::Expr;
use syn::Ident;
use syn::Lit;
use syn::Meta;
use syn::Token;
use syn::parse::Error;
use syn::parse::Parse;
use syn::parse::ParseStream;
use syn::parse::Result;

use crate::qualified::QualifiedName;

mod kw {
    syn::custom_keyword!(namespace);
}

#[derive(Clone, Default)]
pub struct Namespace {
    segments: Vec<Ident>,
}

impl Namespace {
    pub const ROOT: Self = Self {
        segments: Vec::new(),
    };

    pub fn iter(&self) -> Iter<'_, Ident> {
        self.segments.iter()
    }

    pub fn parse_bridge_attr_namespace(input: ParseStream) -> Result<Self> {
        if input.is_empty() {
            return Ok(Self::ROOT);
        }

        input.parse::<kw::namespace>()?;
        input.parse::<Token![=]>()?;
        let namespace = input.parse::<Self>()?;
        input.parse::<Option<Token![,]>>()?;
        Ok(namespace)
    }

    pub fn parse_meta(meta: &Meta) -> Result<Self> {
        if let Meta::NameValue(meta) = meta {
            match &meta.value {
                Expr::Lit(expr) => {
                    if let Lit::Str(lit) = &expr.lit {
                        let segments = QualifiedName::parse_quoted(lit)?.segments;
                        return Ok(Self { segments });
                    }
                }
                Expr::Path(expr)
                    if expr.qself.is_none()
                        && expr
                            .path
                            .segments
                            .iter()
                            .all(|segment| segment.arguments.is_none()) =>
                {
                    let segments = expr
                        .path
                        .segments
                        .iter()
                        .map(|segment| segment.ident.clone())
                        .collect();
                    return Ok(Self { segments });
                }
                _ => {}
            }
        }
        Err(Error::new_spanned(meta, "unsupported namespace attribute"))
    }
}

impl Default for &Namespace {
    fn default() -> Self {
        const ROOT: &Namespace = &Namespace::ROOT;
        ROOT
    }
}

impl Parse for Namespace {
    fn parse(input: ParseStream) -> Result<Self> {
        let segments = QualifiedName::parse_quoted_or_unquoted(input)?.segments;
        Ok(Self { segments })
    }
}

impl Display for Namespace {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        for segment in self {
            write!(f, "{}$", segment)?;
        }
        Ok(())
    }
}

impl IdentFragment for Namespace {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        Display::fmt(self, f)
    }
}

impl<'a> IntoIterator for &'a Namespace {
    type Item = &'a Ident;
    type IntoIter = Iter<'a, Ident>;
    fn into_iter(self) -> Self::IntoIter {
        self.iter()
    }
}

impl<'a> FromIterator<&'a Ident> for Namespace {
    fn from_iter<I>(idents: I) -> Self
    where
        I: IntoIterator<Item = &'a Ident>,
    {
        let segments = idents.into_iter().cloned().collect();
        Self { segments }
    }
}
