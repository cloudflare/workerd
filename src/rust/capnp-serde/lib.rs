// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Runtime support for the Rust types that `capnpc-rust-serde` generates from Cap'n Proto
//! schemas.
//!
//! The generated types serialize to and deserialize from the JSON of Cap'n Proto's C++
//! `capnp::JsonCodec` with `handleByAnnotation()`, the codec workerd uses to talk JSON to services
//! such as R2. Where the codec's JSON differs from what serde derives, a field names one of the
//! markers below in `#[serde(with = "::capnp_serde::As::<Marker>")]`:
//!
//! | Cap'n Proto                      | Rust          | Marker    | JSON                            |
//! | -------------------------------- | ------------- | --------- | ------------------------------- |
//! | `Int8` .. `Int32`, `UInt8` .. `UInt32` | `i8` .. `u32` | [`Integer`] | number                     |
//! | `Int64`, `UInt64`                | `i64`, `u64`  | [`Int64`] | decimal string                  |
//! | `Float32`, `Float64`             | `f32`, `f64`  | [`Float`] | number, `"Infinity"`, `"NaN"`   |
//! | `Data`                           | `Vec<u8>`     | [`Bytes`] | array of numbers                |
//! | `Data $Json.base64`              | `Vec<u8>`     | [`Base64`]| base64 string                   |
//! | `Data $Json.hex`                 | `Vec<u8>`     | [`Hex`]   | hex string                      |
//!
//! Markers compose through `Option<Marker>` and `Vec<Marker>`, so a `List(UInt64)` pointer field
//! is an `Option<Vec<u64>>` with `As::<Option<Vec<Int64>>>`. Like the C++ codec, deserialization
//! accepts more than serialization produces: integers as numbers or strings, floats as `null`
//! (NaN), and enums as names or ordinals.
//!
//! Cap'n Proto enums implement [`Enum`], and their `Serialize` and `Deserialize` impls call
//! [`serialize_enum`] and [`deserialize_enum`].
//!
//! Structs and unions derive `Serialize`, but their `Deserialize` impls are generated: they
//! implement [`Fields`] and call [`deserialize_fields`], which decode flattened fields and unions
//! as they come, rather than buffering the object as serde's derived impls would.
//!
//! Fields equal to their schema default are left out of the JSON, as with
//! `JsonCodec::setHasMode(HasMode::NON_DEFAULT)`, through `skip_serializing_if` and the predicates
//! [`is_default`], `eq_*`, `f32_bits_eq`, `f64_bits_eq` and [`enum_ordinal_eq`].
//!
//! JSON in bytes decodes faster through [`from_slice`] than through `serde_json::from_slice()`,
//! with the same results.
//!
//! # Differences from the C++ codec
//!
//! JSON that the C++ codec writes decodes to the same values in Rust, and JSON that Rust writes
//! decodes to the same message in C++; `capnp-serde-test` checks both for every construct the
//! plugin supports. The one exception is a message holding an enum value the schema does not
//! know (below). The other known differences are in JSON that only other writers produce, and in
//! how values are represented.
//!
//! Rust rejects JSON that C++ accepts:
//!
//! - TODO(conform): A union with no member in the JSON, which C++ decodes as its first member:
//!   an externally tagged union with none of its members' keys (`"plain": {}`, or no key of a
//!   flattened union), and an internally tagged union without its tag (`{"x": 5}` for
//!   `{"kind": "x", "x": 5}`), which C++ decodes by the member's key. A missing key of a union
//!   that is not flattened does take its default.
//! - TODO(conform): An internally tagged union's tag that names no member, which C++ ignores.
//! - TODO(conform): `null` for a union member of a pointer type, which C++ takes as a null
//!   pointer. Union members are not `Option`s, so they cannot be null.
//! - TODO(conform): Any value but `null` for a `Void` field or union member; C++ ignores the
//!   value.
//! - TODO(conform): An enum ordinal that names no enumerant. C++ keeps the number, and writes it
//!   back as a number; a Rust enum has no value to hold it.
//! - TODO(conform): Integers in strings that `strtoll()` accepts but Rust's `parse()` does not,
//!   such as hex (`"0x10"`) and leading whitespace (`" 5"`).
//! - TODO(conform): Malformed base64 and hex, which `kj::decodeBase64()` and `kj::decodeHex()`
//!   decode leniently: base64 with whitespace or other stray characters, hex of odd length or
//!   with non-hex digits.
//! - TODO(conform): Strings with unpaired UTF-16 surrogate escapes (`"\ud800"`), which C++
//!   decodes to invalid UTF-8 in a `Text`. A Rust `String` must be valid UTF-8, and `serde_json`
//!   rejects them.
//!
//! Rust accepts JSON that C++ rejects:
//!
//! - TODO(conform): Strings for the elements of a `Data` array (`["1", 2]`). C++ requires
//!   numbers there, though it accepts strings for integer fields.
//! - TODO(conform): Integers above 2^53 in JSON numbers, which Rust reads exactly. C++ reads
//!   every number as a double, so it rounds them (`9007199254740993` becomes
//!   `9007199254740992`) and rejects `18446744073709551615` as out of range for `UInt64`.
//! - TODO(conform): Values nested 65 to 128 levels deep. `JsonCodec` rejects more than 64 levels
//!   (`setMaxNestingDepth()`), `serde_json` more than 128.
//!
//! Rust decodes some JSON to different values:
//!
//! - TODO(conform): A member's value before an internally tagged union's tag
//!   (`{"foo": 4, "type": "foo"}`). Rust decodes it into the member the tag names; C++ resets
//!   the member when it reads the tag, so it has its default.
//! - TODO(conform): A flattened struct field is always `Some` after decoding, holding the
//!   struct's defaults when none of its fields is in the JSON. C++ leaves the pointer null. Both
//!   encode either as no fields at all.
//! - TODO(conform): A pointer field with a schema default (`foo @0 :Text = "x"`) is `None` when
//!   it is missing, where C++'s getter returns the default. Rust code has to apply the default.
//!
//! Rust encodes differently:
//!
//! - TODO(conform): Rust always writes as `HasMode::NON_DEFAULT` does, leaving out fields that
//!   hold their defaults. `JsonCodec`'s default, `HasMode::NON_NULL`, writes every field of a
//!   primitive type. Either decodes to the same values.
//! - TODO(conform): A `Float32` is written with the shortest digits that read back as the same
//!   `f32` (`0.1`), C++ with those of the `f64` it converts to (`0.10000000149011612`). Both
//!   decode to the same value.
//!
//! The plugin rejects these schemas, or parts of them:
//!
//! - TODO(someday): `$Json.flatten(prefix = ...)`, which has no serde equivalent and needs
//!   hand-written impls.
//! - TODO(someday): Generic structs, and fields of interface or `AnyPointer` type. C++ needs a
//!   registered `JsonCodec::Handler` for the latter two, and handlers have no Rust equivalent.
//! - TODO(someday): A struct flattened into itself, through flattened fields, groups and union
//!   members, whose list of JSON keys and [`Fields::Builder`] would be infinite.
//!
//! A flattened union member without `$Json.discriminator`, and `$Json.discriminator(valueName)`
//! without a discriminator name, are rejected too, but C++ cannot decode the JSON it writes for
//! them either.
//!
//! # Other TODOs
//!
//! - TODO(someday): Constants of struct and list types are not generated, nor are `using`
//!   aliases (such as `R2GetResponse`), which a `CodeGeneratorRequest` does not describe.
//! - TODO(someday): A choice of `HasMode` for encoding, if JSON that has every field is ever
//!   needed.
//! - TODO(cleanup): `wd_rust_serde_capnp_library()` names the crates of its `deps` from their
//!   target names, since a macro cannot read a dep's `crate_name`; so such deps must keep the
//!   default crate name.

use std::fmt;
use std::marker::PhantomData;
use std::str::FromStr;

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

/// Adapts a marker type to `#[serde(with = "...")]`: `#[serde(with = "::capnp_serde::As::<M>")]`
/// serializes and deserializes a field through `M`'s [`SerializeAs`] and [`DeserializeAs`] impls.
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

/// Marker for `Int8` through `UInt32`: a JSON number. Deserialization also accepts a decimal
/// string and a float with an integral value.
pub struct Integer;

/// Marker for `Int64` and `UInt64`: a decimal string, since a JSON number (an IEEE double in most
/// parsers) cannot hold every 64-bit integer. Deserialization also accepts a number.
pub struct Int64;

/// Deserializes an integer of type `T` from a number or a decimal string.
struct IntegerVisitor<T>(PhantomData<T>);

impl<T> de::Visitor<'_> for IntegerVisitor<T>
where
    T: TryFrom<i64> + TryFrom<u64> + FromStr,
{
    type Value = T;

    fn expecting(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
        write!(
            formatter,
            "an integer of type {}",
            std::any::type_name::<T>()
        )
    }

    fn visit_i64<E: de::Error>(self, value: i64) -> Result<T, E> {
        T::try_from(value).map_err(|_| E::invalid_value(de::Unexpected::Signed(value), &self))
    }

    fn visit_u64<E: de::Error>(self, value: u64) -> Result<T, E> {
        T::try_from(value).map_err(|_| E::invalid_value(de::Unexpected::Unsigned(value), &self))
    }

    #[expect(
        clippy::cast_possible_truncation,
        clippy::cast_precision_loss,
        clippy::cast_sign_loss,
        clippy::float_cmp,
        reason = "the casts are checked to round-trip exactly"
    )]
    fn visit_f64<E: de::Error>(self, value: f64) -> Result<T, E> {
        // Like the C++ codec, accept a float that converts to the integer type exactly.
        let unexpected = || E::invalid_value(de::Unexpected::Float(value), &self);
        if value.fract() != 0.0 || !value.is_finite() {
            return Err(unexpected());
        }
        if value < 0.0 {
            let int = value as i64;
            if int as f64 != value {
                return Err(unexpected());
            }
            T::try_from(int).map_err(|_| unexpected())
        } else {
            let int = value as u64;
            if int as f64 != value {
                return Err(unexpected());
            }
            T::try_from(int).map_err(|_| unexpected())
        }
    }

    fn visit_str<E: de::Error>(self, value: &str) -> Result<T, E> {
        value
            .parse()
            .map_err(|_| E::invalid_value(de::Unexpected::Str(value), &self))
    }
}

macro_rules! impl_integer {
    ($($ty:ty),*) => {$(
        impl SerializeAs<$ty> for Integer {
            fn serialize_as<S: Serializer>(value: &$ty, serializer: S) -> Result<S::Ok, S::Error> {
                value.serialize(serializer)
            }
        }

        impl<'de> DeserializeAs<'de, $ty> for Integer {
            fn deserialize_as<D: Deserializer<'de>>(deserializer: D) -> Result<$ty, D::Error> {
                deserializer.deserialize_any(IntegerVisitor::<$ty>(PhantomData))
            }
        }
    )*};
}

impl_integer!(i8, i16, i32, u8, u16, u32);

macro_rules! impl_int64 {
    ($($ty:ty),*) => {$(
        impl SerializeAs<$ty> for Int64 {
            fn serialize_as<S: Serializer>(value: &$ty, serializer: S) -> Result<S::Ok, S::Error> {
                serializer.collect_str(value)
            }
        }

        impl<'de> DeserializeAs<'de, $ty> for Int64 {
            fn deserialize_as<D: Deserializer<'de>>(deserializer: D) -> Result<$ty, D::Error> {
                deserializer.deserialize_any(IntegerVisitor::<$ty>(PhantomData))
            }
        }
    )*};
}

impl_int64!(i64, u64);

/// Marker for `Float32` and `Float64`: a JSON number, or a string for what JSON numbers cannot express.
///
/// The strings are `"Infinity"`, `"-Infinity"` and `"NaN"`. Deserialization also accepts `null` as
/// NaN and any number as a string.
pub struct Float;

/// Deserializes a float of type `T` from a number, a string or `null`.
struct FloatVisitor<T>(PhantomData<T>);

/// The float types [`Float`] handles.
trait FloatType: Copy + FromStr {
    const NAN: Self;
    fn from_f64(value: f64) -> Self;
    fn to_f64(self) -> f64;
}

impl FloatType for f32 {
    const NAN: Self = Self::NAN;

    #[expect(
        clippy::cast_possible_truncation,
        reason = "JSON numbers are doubles, which a Float32 field rounds as C++ does"
    )]
    fn from_f64(value: f64) -> Self {
        value as Self
    }

    fn to_f64(self) -> f64 {
        f64::from(self)
    }
}

impl FloatType for f64 {
    const NAN: Self = Self::NAN;

    fn from_f64(value: f64) -> Self {
        value
    }

    fn to_f64(self) -> f64 {
        self
    }
}

impl<T: FloatType> de::Visitor<'_> for FloatVisitor<T> {
    type Value = T;

    fn expecting(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
        formatter.write_str("a number, \"Infinity\", \"-Infinity\", \"NaN\" or null")
    }

    #[expect(
        clippy::cast_precision_loss,
        reason = "a JSON number is a double in the C++ codec too"
    )]
    fn visit_i64<E: de::Error>(self, value: i64) -> Result<T, E> {
        Ok(T::from_f64(value as f64))
    }

    #[expect(
        clippy::cast_precision_loss,
        reason = "a JSON number is a double in the C++ codec too"
    )]
    fn visit_u64<E: de::Error>(self, value: u64) -> Result<T, E> {
        Ok(T::from_f64(value as f64))
    }

    fn visit_f64<E: de::Error>(self, value: f64) -> Result<T, E> {
        Ok(T::from_f64(value))
    }

    fn visit_str<E: de::Error>(self, value: &str) -> Result<T, E> {
        // Rust's float parser accepts "Infinity", "-Infinity" and "NaN" in any case, as the C++
        // codec's strtod() does.
        value
            .parse()
            .map_err(|_| E::invalid_value(de::Unexpected::Str(value), &self))
    }

    fn visit_unit<E: de::Error>(self) -> Result<T, E> {
        Ok(T::NAN)
    }

    fn visit_none<E: de::Error>(self) -> Result<T, E> {
        Ok(T::NAN)
    }
}

fn serialize_float<T: FloatType + Serialize, S: Serializer>(
    value: T,
    serializer: S,
) -> Result<S::Ok, S::Error> {
    let double = value.to_f64();
    if double.is_nan() {
        serializer.serialize_str("NaN")
    } else if double == f64::INFINITY {
        serializer.serialize_str("Infinity")
    } else if double == f64::NEG_INFINITY {
        serializer.serialize_str("-Infinity")
    } else {
        value.serialize(serializer)
    }
}

impl SerializeAs<f32> for Float {
    fn serialize_as<S: Serializer>(value: &f32, serializer: S) -> Result<S::Ok, S::Error> {
        serialize_float(*value, serializer)
    }
}

impl SerializeAs<f64> for Float {
    fn serialize_as<S: Serializer>(value: &f64, serializer: S) -> Result<S::Ok, S::Error> {
        serialize_float(*value, serializer)
    }
}

impl<'de> DeserializeAs<'de, f32> for Float {
    fn deserialize_as<D: Deserializer<'de>>(deserializer: D) -> Result<f32, D::Error> {
        deserializer.deserialize_any(FloatVisitor::<f32>(PhantomData))
    }
}

impl<'de> DeserializeAs<'de, f64> for Float {
    fn deserialize_as<D: Deserializer<'de>>(deserializer: D) -> Result<f64, D::Error> {
        deserializer.deserialize_any(FloatVisitor::<f64>(PhantomData))
    }
}

/// Marker for `Data` without an encoding annotation: an array of byte values.
pub struct Bytes;

impl SerializeAs<Vec<u8>> for Bytes {
    fn serialize_as<S: Serializer>(value: &Vec<u8>, serializer: S) -> Result<S::Ok, S::Error> {
        serializer.collect_seq(value)
    }
}

impl<'de> DeserializeAs<'de, Vec<u8>> for Bytes {
    fn deserialize_as<D: Deserializer<'de>>(deserializer: D) -> Result<Vec<u8>, D::Error> {
        Vec::<Integer>::deserialize_as(deserializer)
    }
}

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
            decode: |text| {
                data_encoding::BASE64
                    .decode(text)
                    .or_else(|_| data_encoding::BASE64_NOPAD.decode(text))
                    .ok()
            },
            expected: "base64",
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
            decode: |text| data_encoding::HEXLOWER_PERMISSIVE.decode(text).ok(),
            expected: "hex digits",
        })
    }
}

/// Deserializes `Data` from a string in a text encoding, decoding the string where the
/// deserializer holds it rather than from a copy.
struct DecodeVisitor {
    /// Decodes the string, or returns `None` when it is not in the encoding.
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

/// A Cap'n Proto enum.
pub trait Enum: Copy + 'static {
    /// The enumerants' JSON names (`$Json.name`, or else the schema name) and values, in ordinal
    /// order.
    const ENUMERANTS: &'static [(&'static str, Self)];

    /// The enumerant's ordinal.
    fn ordinal(self) -> u16;
}

/// Serializes an enumerant as its JSON name.
///
/// # Errors
///
/// Returns the serializer's error.
pub fn serialize_enum<E: Enum, S: Serializer>(value: E, serializer: S) -> Result<S::Ok, S::Error> {
    match E::ENUMERANTS.get(usize::from(value.ordinal())) {
        Some((name, _)) => serializer.serialize_str(name),
        None => serializer.serialize_u16(value.ordinal()),
    }
}

/// Deserializes an enumerant from its JSON name or its ordinal.
///
/// # Errors
///
/// Returns the deserializer's error, or an error for a name or ordinal that is not an enumerant.
pub fn deserialize_enum<'de, E: Enum, D: Deserializer<'de>>(
    deserializer: D,
) -> Result<E, D::Error> {
    deserializer.deserialize_any(EnumVisitor::<E>(PhantomData))
}

struct EnumVisitor<E>(PhantomData<E>);

impl<E: Enum> EnumVisitor<E> {
    fn enumerant<Err: de::Error>(self, ordinal: u64, unexpected: de::Unexpected) -> Result<E, Err> {
        usize::try_from(ordinal)
            .ok()
            .and_then(|index| E::ENUMERANTS.get(index))
            .map(|(_, value)| *value)
            .ok_or_else(|| Err::invalid_value(unexpected, &self))
    }
}

impl<E: Enum> de::Visitor<'_> for EnumVisitor<E> {
    type Value = E;

    fn expecting(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
        formatter.write_str("one of")?;
        for (index, (name, _)) in E::ENUMERANTS.iter().enumerate() {
            write!(
                formatter,
                "{} \"{name}\"",
                if index == 0 { "" } else { "," }
            )?;
        }
        formatter.write_str(" or an ordinal")
    }

    fn visit_str<Err: de::Error>(self, value: &str) -> Result<E, Err> {
        E::ENUMERANTS
            .iter()
            .find(|(name, _)| *name == value)
            .map(|(_, value)| *value)
            .ok_or_else(|| Err::invalid_value(de::Unexpected::Str(value), &self))
    }

    fn visit_u64<Err: de::Error>(self, value: u64) -> Result<E, Err> {
        self.enumerant(value, de::Unexpected::Unsigned(value))
    }

    fn visit_i64<Err: de::Error>(self, value: i64) -> Result<E, Err> {
        let unexpected = de::Unexpected::Signed(value);
        let ordinal = u64::try_from(value).map_err(|_| Err::invalid_value(unexpected, &self))?;
        self.enumerant(ordinal, unexpected)
    }

    #[expect(
        clippy::cast_possible_truncation,
        clippy::cast_sign_loss,
        clippy::cast_precision_loss,
        clippy::float_cmp,
        reason = "the cast is checked to round-trip exactly"
    )]
    fn visit_f64<Err: de::Error>(self, value: f64) -> Result<E, Err> {
        let unexpected = de::Unexpected::Float(value);
        let ordinal = value as u64;
        if ordinal as f64 != value {
            return Err(Err::invalid_value(unexpected, &self));
        }
        self.enumerant(ordinal, unexpected)
    }
}

/// A generated struct or union, decoded from the fields of a JSON object one at a time.
///
/// The generated `Deserialize` impls of structs and unions call [`deserialize_fields`], which
/// looks up each key of the object with [`Fields::key`] and hands its value to [`Fields::field`].
/// A struct offers a key to its own fields first, then to its flattened fields, groups and unions
/// in schema order; so a flattened value decodes straight from the input, where serde's derived
/// impls would first copy the whole object into a temporary tree. An internally tagged union
/// streams its member's fields once it has read the tag, which the C++ codec writes first; keys
/// before the tag are held in [`Pending`] until then.
///
/// Keys are indices into the value's list of JSON keys, which holds its own fields' keys first,
/// then those of its flattened fields, groups and union members. The generator knows every
/// type's list, so a struct passes a key on to a flattened field as an index into the field's
/// list, with no further string comparisons.
///
/// A key that appears more than once takes its last value, as in C++.
pub trait Fields: Sized {
    /// The state of a value while its fields are read.
    type Builder;

    /// A builder holding no fields yet.
    fn builder() -> Self::Builder;

    /// The index of `key` in the value's list of JSON keys, which includes those of its
    /// flattened fields and union members, whichever union member is set.
    fn key(key: &str) -> Option<usize>;

    /// Reads the value of the key with index `key` from `map`, if the value has a field under
    /// it. Returns whether it did.
    ///
    /// # Errors
    ///
    /// Returns the map's error, or an error when the input does not encode the field.
    fn field<'de, A: de::MapAccess<'de>>(
        builder: &mut Self::Builder,
        key: usize,
        map: &mut A,
    ) -> Result<bool, A::Error>;

    /// The value the builder holds.
    ///
    /// # Errors
    ///
    /// Returns an error when a union has no member.
    fn finish<E: de::Error>(builder: Self::Builder) -> Result<Self, E>;
}

/// The entry of a key map (see [`forward`]) for a key that a flattened value does not have.
pub const NO_KEY: u16 = u16::MAX;

/// Hands the key with index `key` to the flattened value `T`, as [`Fields::field`] does.
///
/// `builder` is `T`'s builder. `keys` maps the indices of the keys of the value `T` is flattened
/// into to `T`'s indices, or to [`NO_KEY`].
///
/// # Errors
///
/// Returns `T::field`'s errors.
pub fn forward<'de, T: Fields, A: de::MapAccess<'de>>(
    keys: &[u16],
    builder: &mut T::Builder,
    key: usize,
    map: &mut A,
) -> Result<bool, A::Error> {
    match keys.get(key) {
        Some(&key) if key != NO_KEY => T::field(builder, usize::from(key), map),
        _ => Ok(false),
    }
}

/// Deserializes a [`Fields`] value from an object.
///
/// # Errors
///
/// Returns the deserializer's error, or an error when the input does not encode a `T`.
pub fn deserialize_fields<'de, T: Fields, D: Deserializer<'de>>(
    deserializer: D,
) -> Result<T, D::Error> {
    deserializer.deserialize_map(FieldsVisitor::<T>(PhantomData))
}

struct FieldsVisitor<T>(PhantomData<T>);

impl<'de, T: Fields> de::Visitor<'de> for FieldsVisitor<T> {
    type Value = T;

    fn expecting(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
        write!(formatter, "an object for {}", std::any::type_name::<T>())
    }

    fn visit_map<A: de::MapAccess<'de>>(self, mut map: A) -> Result<T, A::Error> {
        let mut builder = T::builder();
        while let Some(key) = map.next_key_seed(KeySeed::<T>(PhantomData))? {
            let consumed = match key {
                Some(key) => T::field(&mut builder, key, &mut map)?,
                None => false,
            };
            if !consumed {
                map.next_value::<de::IgnoredAny>()?;
            }
        }
        T::finish(builder)
    }
}

/// Deserializes an object key to its [`Fields::key`] index, or `None` for a key that is not the
/// value's.
struct KeySeed<T>(PhantomData<T>);

impl<'de, T: Fields> de::DeserializeSeed<'de> for KeySeed<T> {
    type Value = Option<usize>;

    fn deserialize<D: Deserializer<'de>>(self, deserializer: D) -> Result<Self::Value, D::Error> {
        deserializer.deserialize_identifier(self)
    }
}

impl<T: Fields> de::Visitor<'_> for KeySeed<T> {
    type Value = Option<usize>;

    fn expecting(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
        formatter.write_str("an object key")
    }

    fn visit_str<E: de::Error>(self, value: &str) -> Result<Self::Value, E> {
        Ok(T::key(value))
    }
}

/// Reads the next value of `map` through the marker `M`, for a field with
/// `#[serde(with = "::capnp_serde::As::<M>")]`.
///
/// # Errors
///
/// Returns the map's error, or an error when the input does not encode a `T`.
pub fn next_value_as<'de, M: DeserializeAs<'de, T>, T, A: de::MapAccess<'de>>(
    map: &mut A,
) -> Result<T, A::Error> {
    Ok(map.next_value::<DeserializeAsWrap<T, M>>()?.value)
}

/// The [`Fields::Builder`] of an internally tagged union: the fields read before the tag, or the
/// builder of the member the tag names, a generated enum.
pub enum Tagged<M> {
    /// No tag yet.
    Pending(Pending),
    /// The tag named a member.
    Member(M),
}

impl<M> Default for Tagged<M> {
    fn default() -> Self {
        Self::Pending(Pending::default())
    }
}

/// Fields of an internally tagged union read before its tag, to be decoded once the tag says
/// which member they belong to.
#[derive(Default)]
pub struct Pending(Vec<(usize, serde_json::Value)>);

impl Pending {
    /// Reads the value of the key with index `key` from `map`.
    ///
    /// # Errors
    ///
    /// Returns the map's error.
    pub fn push<'de, A: de::MapAccess<'de>>(
        &mut self,
        key: usize,
        map: &mut A,
    ) -> Result<(), A::Error> {
        self.0.push((key, map.next_value()?));
        Ok(())
    }

    /// Hands each field, in input order, to `field`, which reads it as [`Fields::field`] does.
    ///
    /// # Errors
    ///
    /// Returns `field`'s errors.
    pub fn replay<E: de::Error>(
        self,
        mut field: impl FnMut(usize, &mut BufferedValue) -> serde_json::Result<bool>,
    ) -> Result<(), E> {
        for (key, value) in self.0 {
            field(key, &mut BufferedValue(Some(value))).map_err(E::custom)?;
        }
        Ok(())
    }
}

/// A [`MapAccess`](de::MapAccess) whose next value is a field [`Pending`] holds.
pub struct BufferedValue(Option<serde_json::Value>);

impl<'de> de::MapAccess<'de> for BufferedValue {
    type Error = serde_json::Error;

    fn next_key_seed<K: de::DeserializeSeed<'de>>(
        &mut self,
        _seed: K,
    ) -> serde_json::Result<Option<K::Value>> {
        Ok(None)
    }

    fn next_value_seed<V: de::DeserializeSeed<'de>>(
        &mut self,
        seed: V,
    ) -> serde_json::Result<V::Value> {
        let value = self
            .0
            .take()
            .ok_or_else(|| <serde_json::Error as de::Error>::custom("value already read"))?;
        seed.deserialize(value)
    }
}

/// Deserializes an internally tagged union's tag: a member's name, as an index into the names.
pub struct TagSeed(pub &'static [&'static str]);

impl<'de> de::DeserializeSeed<'de> for TagSeed {
    type Value = usize;

    fn deserialize<D: Deserializer<'de>>(self, deserializer: D) -> Result<usize, D::Error> {
        deserializer.deserialize_str(self)
    }
}

impl de::Visitor<'_> for TagSeed {
    type Value = usize;

    fn expecting(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
        formatter.write_str("the name of a union member")
    }

    fn visit_str<E: de::Error>(self, value: &str) -> Result<usize, E> {
        self.0
            .iter()
            .position(|name| *name == value)
            .ok_or_else(|| E::unknown_variant(value, self.0))
    }
}

/// The error for a union none of whose members is in the input.
#[must_use]
pub fn missing_member<E: de::Error>(union: &str) -> E {
    E::custom(format_args!("no member of union `{union}`"))
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

/// Whether `value` is its type's default: the `skip_serializing_if` predicate of a field whose
/// schema default is zero (`false`, `0`, or an enum's first enumerant).
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

/// Whether a `Float32` field holds its schema default, whose bits are `BITS`. Floats are compared
/// bit for bit, as the C++ codec does, so that `-0.0` and NaNs compare as their bits say.
#[must_use]
pub const fn f32_bits_eq<const BITS: u32>(value: &f32) -> bool {
    value.to_bits() == BITS
}

/// Whether a `Float64` field holds its schema default, whose bits are `BITS`. See
/// [`f32_bits_eq`].
#[must_use]
pub const fn f64_bits_eq<const BITS: u64>(value: &f64) -> bool {
    value.to_bits() == BITS
}

/// Whether an enum field holds its schema default, the enumerant with ordinal `N`.
#[must_use]
pub fn enum_ordinal_eq<E: Enum, const N: u16>(value: &E) -> bool {
    value.ordinal() == N
}

#[cfg(test)]
#[path = "lib-test.rs"]
mod tests;
