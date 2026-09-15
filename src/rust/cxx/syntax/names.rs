use std::fmt::Display;
use std::fmt::{self};
use std::iter;

use proc_macro2::Ident;
use proc_macro2::Span;
use syn::ext::IdentExt;
use syn::parse::Error;
use syn::parse::Parser;
use syn::parse::Result;
use syn::punctuated::Punctuated;

use crate::Lifetimes;
use crate::NamedType;
use crate::Pair;
use crate::Symbol;
use crate::symbol::Segment;

#[derive(Clone)]
pub struct ForeignName {
    text: String,
}

impl Pair {
    pub fn to_symbol(&self) -> Symbol {
        let segments = self
            .namespace
            .iter()
            .map(|ident| ident as &dyn Segment)
            .chain(iter::once(&self.cxx as &dyn Segment));
        Symbol::from_idents(segments)
    }

    pub fn to_fully_qualified(&self) -> String {
        // A fundamental type's name is a keyword, not something declared in a
        // scope, so it is spelled bare: `::char16_t` is ill-formed where
        // `::uint16_t` is fine. Reaching this with a non-empty namespace means
        // the name was not really a fundamental type, so leave it qualified
        // and let C++ report it.
        if self.namespace.is_empty() && self.cxx.is_fundamental() {
            return self.cxx.to_string();
        }

        let mut fully_qualified = String::new();
        for segment in &self.namespace {
            fully_qualified += "::";
            fully_qualified += &segment.to_string();
        }
        fully_qualified += "::";
        fully_qualified += &self.cxx.to_string();
        fully_qualified
    }
}

impl NamedType {
    pub fn new(rust: Ident) -> Self {
        let generics = Lifetimes {
            lt_token: None,
            lifetimes: Punctuated::new(),
            gt_token: None,
        };
        Self { rust, generics }
    }
}

impl ForeignName {
    pub fn from_ident(ident: &Ident) -> Self {
        Self {
            text: ident.to_string(),
        }
    }

    pub fn parse(text: &str, span: Span) -> Result<Self> {
        // TODO: support C++ names containing whitespace (`unsigned int`) or
        // non-alphanumeric characters (`operator++`).
        match Ident::parse_any.parse_str(text) {
            Ok(ident) => {
                let text = ident.to_string();
                Ok(Self { text })
            }
            Err(err) => Err(Error::new(span, err)),
        }
    }

    /// True if this names a C++ fundamental type.
    ///
    /// These are the only C++ type names that are keywords, and so the only
    /// ones that cannot be written with a `::` qualification. Fixed-width
    /// names like `uint16_t` are ordinary typedefs, not keywords, and are
    /// deliberately absent.
    ///
    /// Multi-token spellings such as `unsigned int` are absent because
    /// [`ForeignName::parse`] cannot represent a name containing whitespace.
    pub fn is_fundamental(&self) -> bool {
        matches!(
            self.text.as_str(),
            "bool"
                | "char"
                | "char8_t"
                | "char16_t"
                | "char32_t"
                | "double"
                | "float"
                | "int"
                | "long"
                | "short"
                | "signed"
                | "unsigned"
                | "void"
                | "wchar_t"
        )
    }
}

impl Display for ForeignName {
    fn fmt(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
        formatter.write_str(&self.text)
    }
}

impl PartialEq<str> for ForeignName {
    fn eq(&self, rhs: &str) -> bool {
        self.text == rhs
    }
}
