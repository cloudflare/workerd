// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Runtime support for the Rust types that `capnpc-rust-serde-json` generates from Cap'n Proto
//! schemas.
//!
//! The generated types derive `Serialize` and `Deserialize`, and their JSON is that of Cap'n
//! Proto's C++ `capnp::JsonCodec` with `handleByAnnotation()`, the codec workerd uses to talk
//! JSON to services such as R2. Where the codec's JSON differs from serde's, a field names one of
//! the markers below in `#[serde(with = "::capnp_serde_json::As::<Marker>")]`:
//!
//! | Cap'n Proto         | Rust         | Marker     | JSON                             |
//! | ------------------- | ------------ | ---------- | -------------------------------- |
//! | `Int64`, `UInt64`   | `i64`, `u64` | [`Int64`]  | decimal string                   |
//! | `Data $Json.base64` | `Vec<u8>`    | [`Base64`] | base64 string                    |
//! | `Data $Json.hex`    | `Vec<u8>`    | [`Hex`]    | hex string                       |
//!
//! Markers compose through `Option<Marker>` and `Vec<Marker>`, so a `List(UInt64)` pointer field
//! is an `Option<Vec<u64>>` with `As::<Option<Vec<Int64>>>`. `Bool` and the other integer types
//! are plain JSON values, as serde has them.
//!
//! Missing fields take their schema defaults, through `#[serde(default)]` on each struct and its
//! `Default` impl. Fields equal to their schema default are left out of the JSON, as with
//! `JsonCodec::setHasMode(HasMode::NON_DEFAULT)`, through `skip_serializing_if` and the
//! predicates [`is_default`], `eq_bool` and `eq_*`.
//!
//! JSON in bytes decodes faster through [`from_slice`] than through `serde_json::from_slice()`,
//! with the same results. [`from_codec_input`] also ends the input at a NUL byte, as the C++
//! codec does; the parse functions of the cxx bridges that `capnpc-rust-serde-json-ffi` generates
//! decode with it.
//!
//! # Differences from the C++ codec
//!
//! The types are idiomatic serde rather than a port of every behaviour of the C++ codec. JSON
//! that the C++ codec writes decodes to the same values in Rust, and JSON that Rust writes
//! decodes to the same message in C++. Rust always writes as `HasMode::NON_DEFAULT` does, leaving
//! out fields that hold their defaults, where `JsonCodec`'s default, `HasMode::NON_NULL`, writes
//! every field of a primitive type; both decode the same.
//!
//! A union member of a pointer type is never null in Rust: where C++ leaves the pointer null (the
//! default of a union whose first member is a pointer, or a member that a tag selects without a
//! value), Rust holds an empty value. Both read and write the same; only C++'s `hasX()` tells
//! them apart.
//!
//! JSON that only other writers produce may decode differently, since decoding follows serde
//! rather than the C++ codec: Rust is stricter about integers in strings, base64 and hex,
//! repeated keys and unions without a member or a tag, and accepts a few forms that C++ rejects.
//! The compatibility tables of this crate's `README.md` list every difference, and every schema
//! construct the plugin supports or rejects.
//!
//! `capnp-serde-json-ffi-test` checks, for every construct the plugin supports, that the
//! generated types decode the JSON the C++ codec writes as it does, and that the C++ codec with
//! `HasMode::NON_DEFAULT` writes what the Rust types write.
//!
//! # TODOs
//!
//! - TODO(cleanup): `wd_rust_capnp_serde_json_library()` names the crates of its `deps` from their
//!   target names, since a macro cannot read a dep's `crate_name`; so such deps must keep the
//!   default crate name.

use std::fmt;
use std::marker::PhantomData;

use serde::Deserialize;
use serde::Deserializer;
use serde::Serialize;
use serde::Serializer;
use serde::de;

/// Serializes a value of type `T` the way the marker type implementing this trait says.
pub trait SerializeAs<T: ?Sized> {
    /// Serializes `value` with `serializer`.
    ///
    /// # Errors
    ///
    /// Returns the serializer's error.
    fn serialize_as<S: Serializer>(value: &T, serializer: S) -> Result<S::Ok, S::Error>;
}

/// Deserializes a value of type `T` the way the marker type implementing this trait says.
pub trait DeserializeAs<'de, T>: Sized {
    /// Deserializes a `T` from `deserializer`.
    ///
    /// # Errors
    ///
    /// Returns the deserializer's error, or an error when the input does not encode a `T`.
    fn deserialize_as<D: Deserializer<'de>>(deserializer: D) -> Result<T, D::Error>;
}

/// Adapts a marker type to `#[serde(with = "...")]`:
/// `#[serde(with = "::capnp_serde_json::As::<M>")]` serializes and deserializes a field through
/// `M`'s [`SerializeAs`] and [`DeserializeAs`] impls.
pub struct As<M: ?Sized>(PhantomData<M>);

impl<M: ?Sized> As<M> {
    /// Serializes `value` through `M`.
    ///
    /// # Errors
    ///
    /// Returns the serializer's error.
    pub fn serialize<S: Serializer, T: ?Sized>(value: &T, serializer: S) -> Result<S::Ok, S::Error>
    where
        M: SerializeAs<T>,
    {
        M::serialize_as(value, serializer)
    }

    /// Deserializes a value through `M`.
    ///
    /// # Errors
    ///
    /// Returns the deserializer's error, or an error when the input does not encode a `T`.
    pub fn deserialize<'de, D: Deserializer<'de>, T>(deserializer: D) -> Result<T, D::Error>
    where
        M: DeserializeAs<'de, T>,
    {
        M::deserialize_as(deserializer)
    }
}

/// A `Serialize` value that serializes `T` through the marker `M`.
struct SerializeAsWrap<'a, T: ?Sized, M: ?Sized> {
    value: &'a T,
    marker: PhantomData<M>,
}

impl<'a, T: ?Sized, M: ?Sized> SerializeAsWrap<'a, T, M> {
    const fn new(value: &'a T) -> Self {
        Self {
            value,
            marker: PhantomData,
        }
    }
}

impl<T: ?Sized, M: SerializeAs<T> + ?Sized> Serialize for SerializeAsWrap<'_, T, M> {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        M::serialize_as(self.value, serializer)
    }
}

/// A `Deserialize` value that deserializes `T` through the marker `M`.
struct DeserializeAsWrap<T, M> {
    value: T,
    marker: PhantomData<M>,
}

impl<'de, T, M: DeserializeAs<'de, T>> Deserialize<'de> for DeserializeAsWrap<T, M> {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        Ok(Self {
            value: M::deserialize_as(deserializer)?,
            marker: PhantomData,
        })
    }
}

impl<T, M: SerializeAs<T>> SerializeAs<Option<T>> for Option<M> {
    fn serialize_as<S: Serializer>(value: &Option<T>, serializer: S) -> Result<S::Ok, S::Error> {
        match value {
            Some(value) => serializer.serialize_some(&SerializeAsWrap::<T, M>::new(value)),
            None => serializer.serialize_none(),
        }
    }
}

impl<'de, T, M: DeserializeAs<'de, T>> DeserializeAs<'de, Option<T>> for Option<M> {
    fn deserialize_as<D: Deserializer<'de>>(deserializer: D) -> Result<Option<T>, D::Error> {
        Ok(Option::<DeserializeAsWrap<T, M>>::deserialize(deserializer)?.map(|wrap| wrap.value))
    }
}

impl<T, M: SerializeAs<T>> SerializeAs<Vec<T>> for Vec<M> {
    fn serialize_as<S: Serializer>(value: &Vec<T>, serializer: S) -> Result<S::Ok, S::Error> {
        serializer.collect_seq(value.iter().map(SerializeAsWrap::<T, M>::new))
    }
}

impl<'de, T, M: DeserializeAs<'de, T>> DeserializeAs<'de, Vec<T>> for Vec<M> {
    fn deserialize_as<D: Deserializer<'de>>(deserializer: D) -> Result<Vec<T>, D::Error> {
        Ok(Vec::<DeserializeAsWrap<T, M>>::deserialize(deserializer)?
            .into_iter()
            .map(|wrap| wrap.value)
            .collect())
    }
}

/// Marker for `Int64` and `UInt64`: a decimal string, as the C++ codec writes them, since a JSON
/// number (an IEEE double in most parsers) cannot hold every 64-bit integer.
///
/// Deserialization also accepts a JSON number that is an integer, as services written in other
/// languages write them.
pub struct Int64;

/// Deserializes a 64-bit integer of type `T` from an integer or a decimal string.
struct Int64Visitor<T>(PhantomData<T>);

impl<T: TryFrom<i64> + TryFrom<u64> + std::str::FromStr> de::Visitor<'_> for Int64Visitor<T> {
    type Value = T;

    fn expecting(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
        write!(
            formatter,
            "an integer of type {}, or a string of its decimal digits",
            std::any::type_name::<T>()
        )
    }

    fn visit_i64<E: de::Error>(self, value: i64) -> Result<T, E> {
        T::try_from(value).map_err(|_| E::invalid_value(de::Unexpected::Signed(value), &self))
    }

    fn visit_u64<E: de::Error>(self, value: u64) -> Result<T, E> {
        T::try_from(value).map_err(|_| E::invalid_value(de::Unexpected::Unsigned(value), &self))
    }

    fn visit_str<E: de::Error>(self, value: &str) -> Result<T, E> {
        value
            .parse()
            .map_err(|_| E::invalid_value(de::Unexpected::Str(value), &self))
    }
}

macro_rules! impl_int64 {
    ($($ty:ty),*) => {$(
        impl SerializeAs<$ty> for Int64 {
            fn serialize_as<S: Serializer>(value: &$ty, serializer: S) -> Result<S::Ok, S::Error> {
                serializer.collect_str(value)
            }
        }

        impl<'de> DeserializeAs<'de, $ty> for Int64 {
            fn deserialize_as<D: Deserializer<'de>>(deserializer: D) -> Result<$ty, D::Error> {
                deserializer.deserialize_any(Int64Visitor::<$ty>(PhantomData))
            }
        }
    )*};
}

impl_int64!(i64, u64);

/// Marker for `Data $Json.base64`: a string in standard, padded base64.
pub struct Base64;

impl SerializeAs<Vec<u8>> for Base64 {
    fn serialize_as<S: Serializer>(value: &Vec<u8>, serializer: S) -> Result<S::Ok, S::Error> {
        serializer.serialize_str(&data_encoding::BASE64.encode(value))
    }
}

impl<'de> DeserializeAs<'de, Vec<u8>> for Base64 {
    fn deserialize_as<D: Deserializer<'de>>(deserializer: D) -> Result<Vec<u8>, D::Error> {
        deserializer.deserialize_str(DecodeVisitor {
            decode: |text| data_encoding::BASE64.decode(text).ok(),
            expected: "standard, padded base64",
        })
    }
}

/// Marker for `Data $Json.hex`: a string of lowercase hex digits. Deserialization accepts either
/// case.
pub struct Hex;

impl SerializeAs<Vec<u8>> for Hex {
    fn serialize_as<S: Serializer>(value: &Vec<u8>, serializer: S) -> Result<S::Ok, S::Error> {
        serializer.serialize_str(&data_encoding::HEXLOWER.encode(value))
    }
}

impl<'de> DeserializeAs<'de, Vec<u8>> for Hex {
    fn deserialize_as<D: Deserializer<'de>>(deserializer: D) -> Result<Vec<u8>, D::Error> {
        deserializer.deserialize_str(DecodeVisitor {
            decode: decode_hex,
            expected: "hex digits",
        })
    }
}

/// The value of each hex digit, in either case, and 0xff for bytes that are not hex digits.
const HEX_DIGITS: [u8; 256] = {
    let mut digits = [0xff; 256];
    let mut byte = 0;
    while byte < 256 {
        // `byte` < 256, and a digit's value < 16, so the casts are exact.
        #[expect(clippy::cast_possible_truncation, reason = "see above")]
        if let Some(value) = (byte as u8 as char).to_digit(16) {
            digits[byte] = value as u8;
        }
        byte += 1;
    }
    digits
};

/// Decodes hex digits in either case, two to a byte, or returns `None` if `text` is not such
/// digits. Faster than `data_encoding`'s decoder, which matters for the checksums in R2's
/// responses: the loop has no early exit, so the compiler vectorizes it.
fn decode_hex(text: &[u8]) -> Option<Vec<u8>> {
    let (pairs, rest) = text.as_chunks::<2>();
    if !rest.is_empty() {
        return None;
    }
    // A digit's value fits in the low four bits, so any high bit marks a byte that is not one.
    let mut invalid = 0;
    let bytes = pairs
        .iter()
        .map(|&[high, low]| {
            let high = HEX_DIGITS[usize::from(high)];
            let low = HEX_DIGITS[usize::from(low)];
            invalid |= high | low;
            high << 4 | low
        })
        .collect();
    (invalid & 0xf0 == 0).then_some(bytes)
}

/// Deserializes `Data` from a string in a text encoding, decoding the string where the
/// deserializer holds it rather than from a copy.
struct DecodeVisitor {
    /// Decodes the string.
    decode: fn(&[u8]) -> Option<Vec<u8>>,
    /// The encoding, for error messages.
    expected: &'static str,
}

impl de::Visitor<'_> for DecodeVisitor {
    type Value = Vec<u8>;

    fn expecting(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
        formatter.write_str(self.expected)
    }

    fn visit_str<E: de::Error>(self, value: &str) -> Result<Vec<u8>, E> {
        (self.decode)(value.as_bytes())
            .ok_or_else(|| E::invalid_value(de::Unexpected::Str(value), &self))
    }
}

/// Deserializes a `T` from the JSON document `json`, with the results of
/// `serde_json::from_slice()`.
///
/// `serde_json::from_slice()` checks that each string it decodes is valid UTF-8 as it reads it,
/// which takes longer than checking the whole document at once, for the short strings and keys
/// of Cap'n Proto messages. This function checks the document at once and decodes it as a
/// `str`. A document that is not valid UTF-8 goes to `serde_json::from_slice()`, which reports
/// where the invalid UTF-8 is, or accepts it in a value that `T` ignores.
///
/// # Errors
///
/// When `json` is not a valid document of `T`.
pub fn from_slice<'de, T: Deserialize<'de>>(json: &'de [u8]) -> serde_json::Result<T> {
    match std::str::from_utf8(json) {
        Ok(json) => serde_json::from_str(json),
        Err(_) => serde_json::from_slice(json),
    }
}

/// Deserializes a `T` from the JSON input `json` as `capnp::JsonCodec::decode()` does.
///
/// The C++ codec takes the input to end at its first NUL byte, so that a `kj::String`'s array,
/// with its terminator, is a valid input. The document up to there is decoded with
/// [`from_slice`].
///
/// # Errors
///
/// When the input up to its first NUL byte is not a valid document of `T`.
pub fn from_codec_input<'de, T: Deserialize<'de>>(json: &'de [u8]) -> serde_json::Result<T> {
    let end = memchr::memchr(0, json).unwrap_or(json.len());
    from_slice(&json[..end])
}

/// Whether `value` is its type's default: the `skip_serializing_if` predicate of a field whose
/// schema default is zero (`false` or `0`).
#[must_use]
pub fn is_default<T: Default + PartialEq>(value: &T) -> bool {
    *value == T::default()
}

/// Whether a `Bool` field holds its schema default `V`.
#[must_use]
pub const fn eq_bool<const V: bool>(value: &bool) -> bool {
    *value == V
}

macro_rules! eq_integer {
    ($($name:ident: $ty:ty),*) => {$(
        #[doc = concat!("Whether a `", stringify!($ty), "` field holds its schema default `V`.")]
        #[must_use]
        pub const fn $name<const V: $ty>(value: &$ty) -> bool {
            *value == V
        }
    )*};
}

eq_integer!(eq_i8: i8, eq_i16: i16, eq_i32: i32, eq_i64: i64, eq_u8: u8, eq_u16: u16, eq_u32: u32, eq_u64: u64);

#[cfg(test)]
#[path = "lib-test.rs"]
mod tests;
