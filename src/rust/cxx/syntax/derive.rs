use std::fmt::Display;
use std::fmt::{self};

use proc_macro2::Ident;
use proc_macro2::Span;

#[derive(Copy, Clone)]
pub struct Derive {
    pub what: Trait,
    pub span: Span,
}

#[derive(Copy, Clone, PartialEq, Eq)]
pub enum Trait {
    Clone,
    Copy,
    Debug,
    Default,
    Eq,
    ExternType,
    Hash,
    Ord,
    PartialEq,
    PartialOrd,
    Serialize,
    Deserialize,
    JsgStruct,
}

impl Derive {
    pub fn from(ident: &Ident) -> Option<Self> {
        let what = match ident.to_string().as_str() {
            "Clone" => Trait::Clone,
            "Copy" => Trait::Copy,
            "Debug" => Trait::Debug,
            "Default" => Trait::Default,
            "Eq" => Trait::Eq,
            "ExternType" => Trait::ExternType,
            "Hash" => Trait::Hash,
            "Ord" => Trait::Ord,
            "PartialEq" => Trait::PartialEq,
            "PartialOrd" => Trait::PartialOrd,
            "Serialize" => Trait::Serialize,
            "Deserialize" => Trait::Deserialize,
            "JsgStruct" => Trait::JsgStruct,
            _ => return None,
        };
        let span = ident.span();
        Some(Self { what, span })
    }
}

impl PartialEq<Trait> for Derive {
    fn eq(&self, other: &Trait) -> bool {
        self.what == *other
    }
}

impl AsRef<str> for Trait {
    fn as_ref(&self) -> &str {
        match self {
            Self::Clone => "Clone",
            Self::Copy => "Copy",
            Self::Debug => "Debug",
            Self::Default => "Default",
            Self::Eq => "Eq",
            Self::ExternType => "ExternType",
            Self::Hash => "Hash",
            Self::Ord => "Ord",
            Self::PartialEq => "PartialEq",
            Self::PartialOrd => "PartialOrd",
            Self::Serialize => "Serialize",
            Self::Deserialize => "Deserialize",
            Self::JsgStruct => "JsgStruct",
        }
    }
}

impl Display for Derive {
    fn fmt(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
        formatter.write_str(self.what.as_ref())
    }
}

pub fn contains(derives: &[Derive], query: Trait) -> bool {
    derives.iter().any(|derive| derive.what == query)
}
