// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The support code of `#[jsg_oneof]` unions, the Rust counterpart of C++ JSG's `kj::OneOf`.
//!
//! A union converts a value as C++ JSG's `OneOfWrapper::tryUnwrap()` (jsg/value.h) does, in the
//! passes that [`UnionMember`] describes, and checks at compile time that its variants follow
//! Web IDL's rules for unions, as C++ JSG's `webidl::UnionTypeValidator` (jsg/web-idl.h) does;
//! see [`UnionTypes`]. Everything here is used only by the code the macro generates.

use crate::Error;
use crate::Lenient;
use crate::Lock;
use crate::Type;
use crate::v8;

/// The kind of a `#[jsg_oneof]` variant, which decides the passes in which the
/// variant is tried.
///
/// A union converts a value as C++ JSG's `OneOfWrapper::tryUnwrap()`
/// (jsg/value.h) does, following Web IDL's union conversion: it makes a series
/// of passes over its variants and takes the first one that converts, trying
/// in order
///
/// 1. resource types,
/// 2. every type that is not a string, numeric or boolean type,
/// 3. boolean types, if the value is a boolean,
/// 4. numeric types, if the value is a number or a `BigInt`,
/// 5. string types,
/// 6. numeric types,
/// 7. boolean types.
///
/// Passes 5 to 7 coerce, so a value becomes a string only if no other variant
/// takes it, and becomes a number or boolean only if no string variant
/// converts it either. An error from a variant's conversion, such as a
/// `Symbol` that cannot become a string, ends the conversion with that error
/// rather than trying another variant.
#[doc(hidden)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum UnionMember {
    /// A resource type (`jsg::Rc<R>`).
    Resource,
    /// Any other type that is not a string, numeric or boolean type, such as
    /// an array, a typed array or a struct (C++ JSG's `IsFallibleType`).
    Fallible,
    /// A string type.
    String,
    /// A numeric type.
    Numeric,
    /// A boolean type.
    Boolean,
}

impl UnionMember {
    /// Whether a variant of this kind is tried in `pass`.
    pub(crate) const fn takes_part_in(self, pass: Self) -> bool {
        matches!(
            (self, pass),
            (Self::Resource, Self::Resource | Self::Fallible)
                | (Self::Fallible, Self::Fallible)
                | (Self::String, Self::String)
                | (Self::Numeric, Self::Numeric)
                | (Self::Boolean, Self::Boolean)
        )
    }
}

/// Converts `value` to a union by running `try_pass` for each pass of the
/// union conversion algorithm (see [`UnionMember`]) until one converts it.
#[doc(hidden)]
pub fn try_union_from_js<'s, R>(
    lock: &mut Lock<'s>,
    value: v8::Local<'s, v8::Value>,
    try_pass: impl Fn(&mut Lock<'s>, v8::Local<'s, v8::Value>, UnionMember) -> Result<Option<R>, Error>,
) -> Result<Option<R>, Error> {
    let passes = [
        (true, UnionMember::Resource),
        (true, UnionMember::Fallible),
        (value.is_boolean(), UnionMember::Boolean),
        (
            value.is_number() || value.is_big_int(),
            UnionMember::Numeric,
        ),
        (true, UnionMember::String),
        (true, UnionMember::Numeric),
        (true, UnionMember::Boolean),
    ];
    for (applies, pass) in passes {
        if applies && let Some(result) = try_pass(lock, value, pass)? {
            return Ok(Some(result));
        }
    }
    Ok(None)
}

/// How many types of each Web IDL category a union, or a type that may be one of its variants,
/// contains, as C++ JSG's `webidl::FlattenedTypeTraits` (jsg/web-idl.h) counts them.
///
/// Web IDL flattens unions: a variant that is itself a union, or a `Nullable<T>`, counts with the
/// types it contains, as a nested `kj::OneOf` or a `kj::Maybe<T>` does. [`UnionTypes::validate`]
/// then applies `webidl::UnionTypeValidator`'s rules to the sum. A type that falls in no category,
/// such as `jsg::v8::Local<'s, jsg::v8::Value>` or `SystemTime`, is [`UnionTypes::OTHER`]; C++ JSG
/// does not reject such types either. Rust has no symbol or callback function types in C++ JSG's
/// sense, so their rules never apply.
#[doc(hidden)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct UnionTypes {
    /// Dictionary types: `#[jsg_struct]`s (C++ JSG's `JSG_STRUCT`s).
    pub dictionary: u32,
    /// Boolean types.
    pub boolean: u32,
    /// Numeric types.
    pub numeric: u32,
    /// String types.
    pub string: u32,
    /// The object type, `jsg::v8::Local<'s, jsg::v8::Object>`.
    pub object: u32,
    /// Interface-like types: resources. C++ JSG's buffer sources, such as `kj::Array<kj::byte>`,
    /// have no counterpart: `Vec<u8>` is a sequence, and a typed array handle is in no category,
    /// as C++ JSG's `v8::Local<v8::Uint8Array>` is.
    pub interface_like: u32,
    /// Dictionary-like types: dictionaries and records (`jsg::Dict<V>`).
    pub dictionary_like: u32,
    /// Sequence-like types: `Vec<T>` and `&[T]`.
    pub sequence_like: u32,
    /// Nullable types: `Nullable<T>`.
    pub nullable: u32,
    /// Optional types, `Option<T>` and `Lenient<T>`, which may not be variants.
    pub optional: u32,
}

impl UnionTypes {
    /// A type in no category.
    pub const OTHER: Self = Self {
        dictionary: 0,
        boolean: 0,
        numeric: 0,
        string: 0,
        object: 0,
        interface_like: 0,
        dictionary_like: 0,
        sequence_like: 0,
        nullable: 0,
        optional: 0,
    };
    /// A boolean type.
    pub const BOOLEAN: Self = Self {
        boolean: 1,
        ..Self::OTHER
    };
    /// A numeric type.
    pub const NUMERIC: Self = Self {
        numeric: 1,
        ..Self::OTHER
    };
    /// A string type.
    pub const STRING: Self = Self {
        string: 1,
        ..Self::OTHER
    };
    /// The object type.
    pub const OBJECT: Self = Self {
        object: 1,
        ..Self::OTHER
    };
    /// An interface-like type.
    pub const INTERFACE_LIKE: Self = Self {
        interface_like: 1,
        ..Self::OTHER
    };
    /// A dictionary type, which is also dictionary-like.
    pub const DICTIONARY: Self = Self {
        dictionary: 1,
        dictionary_like: 1,
        ..Self::OTHER
    };
    /// A record type, which is dictionary-like.
    pub const RECORD: Self = Self {
        dictionary_like: 1,
        ..Self::OTHER
    };
    /// A sequence-like type.
    pub const SEQUENCE_LIKE: Self = Self {
        sequence_like: 1,
        ..Self::OTHER
    };

    /// The types of a union of `self`'s and `other`'s types.
    #[must_use]
    pub const fn plus(self, other: Self) -> Self {
        Self {
            dictionary: self.dictionary + other.dictionary,
            boolean: self.boolean + other.boolean,
            numeric: self.numeric + other.numeric,
            string: self.string + other.string,
            object: self.object + other.object,
            interface_like: self.interface_like + other.interface_like,
            dictionary_like: self.dictionary_like + other.dictionary_like,
            sequence_like: self.sequence_like + other.sequence_like,
            nullable: self.nullable + other.nullable,
            optional: self.optional + other.optional,
        }
    }

    /// The types of a nullable type whose inner type has `self`'s types.
    #[must_use]
    pub const fn nullable(self) -> Self {
        Self {
            nullable: self.nullable + 1,
            ..self
        }
    }

    /// The types of an optional type whose inner type has `self`'s types.
    #[must_use]
    pub const fn optional(self) -> Self {
        Self {
            optional: self.optional + 1,
            ..self
        }
    }

    /// Panics, at compile time when evaluated in a constant, unless a union with these types
    /// follows Web IDL's rules for unions, as `webidl::UnionTypeValidator` checks them.
    ///
    /// # Panics
    ///
    /// Panics with the rule the union breaks.
    pub const fn validate(self) {
        assert!(
            self.nullable + self.dictionary <= 1,
            "A #[jsg_oneof] union may contain at most one nullable or dictionary type."
        );
        assert!(
            self.boolean <= 1,
            "A #[jsg_oneof] union may contain at most one boolean type."
        );
        assert!(
            self.numeric <= 1,
            "A #[jsg_oneof] union may contain at most one numeric type."
        );
        assert!(
            self.string <= 1,
            "A #[jsg_oneof] union may contain at most one string type."
        );
        assert!(
            self.object <= 1,
            "A #[jsg_oneof] union may contain at most one object type."
        );
        assert!(
            self.object == 0
                || self.interface_like + self.dictionary_like + self.sequence_like == 0,
            "A #[jsg_oneof] union may contain an object type only if it also contains no \
             interface-like, dictionary-like or sequence-like types."
        );
        assert!(
            self.dictionary_like <= 1,
            "A #[jsg_oneof] union may contain at most one dictionary-like type."
        );
        assert!(
            self.sequence_like <= 1,
            "A #[jsg_oneof] union may contain at most one sequence-like type."
        );
        assert!(
            self.optional == 0,
            "A #[jsg_oneof] union may not contain Option<T> or Lenient<T>, which mark optional \
             parameters and fields. Use Nullable<T> for a nullable type."
        );
    }
}

/// A type that can be a `#[jsg_oneof]` variant: every [`Type`], and the optional types, which
/// are not `Type`s and which a union rejects with an explanation rather than with a missing
/// impl.
#[doc(hidden)]
#[diagnostic::on_unimplemented(
    message = "`{Self}` cannot be a `#[jsg_oneof]` variant",
    note = "a variant must implement `jsg::Type`"
)]
pub trait UnionVariant {
    /// The variant's types, flattened; see [`UnionTypes`].
    const UNION_TYPES: UnionTypes;

    /// Whether `value` is exactly of the variant's type; see [`Type::is_exact`].
    fn is_exact(value: v8::Local<'_, v8::Value>) -> bool;
}

impl<T: Type> UnionVariant for T {
    const UNION_TYPES: UnionTypes = T::UNION_TYPES;

    fn is_exact(value: v8::Local<'_, v8::Value>) -> bool {
        T::is_exact(value)
    }
}

impl<T: UnionVariant> UnionVariant for Option<T> {
    const UNION_TYPES: UnionTypes = T::UNION_TYPES.optional();

    fn is_exact(value: v8::Local<'_, v8::Value>) -> bool {
        value.is_undefined() || T::is_exact(value)
    }
}

impl<T: UnionVariant> UnionVariant for Lenient<T> {
    const UNION_TYPES: UnionTypes = T::UNION_TYPES.optional();

    fn is_exact(value: v8::Local<'_, v8::Value>) -> bool {
        value.is_null_or_undefined() || T::is_exact(value)
    }
}

#[cfg(test)]
#[path = "oneof-test.rs"]
mod tests;
