use proc_macro2::Ident;

use self::ImproperCtype::*;
use crate::Type;
use crate::Types;
use crate::atom::Atom::*;
use crate::atom::Atom::{self};

pub enum ImproperCtype<'a> {
    Definite(bool),
    Depends(&'a Ident),
}

impl<'a> Types<'a> {
    // yes, no, maybe
    pub fn determine_improper_ctype(&self, ty: &Type) -> ImproperCtype<'a> {
        match ty {
            Type::Ident(ident) => {
                let ident = &ident.rust;
                if let Some(atom) = Atom::from(ident) {
                    Definite(atom == RustString)
                } else if let Some(strct) = self.structs.get(ident) {
                    Depends(&strct.name.rust) // iterate to fixed-point
                } else {
                    Definite(self.rust.contains(ident) || self.aliases.contains_key(ident))
                }
            }
            Type::RustBox(_)
            | Type::RustVec(_)
            | Type::Str(_)
            | Type::Fn(_)
            | Type::Void(_)
            | Type::KjDate(_)
            | Type::SliceRef(_) => Definite(true),
            Type::UniquePtr(_)
            | Type::KjOwn(_)
            | Type::KjRc(_)
            | Type::KjArc(_)
            | Type::SharedPtr(_)
            | Type::WeakPtr(_)
            | Type::CxxVector(_) => Definite(false),
            Type::Ref(ty) => self.determine_improper_ctype(&ty.inner),
            Type::Ptr(ty) => self.determine_improper_ctype(&ty.inner),
            Type::Array(ty) => self.determine_improper_ctype(&ty.inner),
            Type::KjMaybe(ty) => self.determine_improper_ctype(&ty.inner),
            Type::Future(_) => {
                todo!("file a workerd-cxx ticket")
            }
        }
    }
}
