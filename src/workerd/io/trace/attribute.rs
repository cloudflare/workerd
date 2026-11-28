// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Span tag values and attributes: `Span::TagValue` (`tracing::Attribute::Value`) and
//! `tracing::Attribute`, after OpenTelemetry's attributes
//! (<https://opentelemetry.io/docs/concepts/signals/traces/#attributes>).

use trace_capnp::optional_bool;
use trace_capnp::optional_float64;
use trace_capnp::optional_string;
use trace_capnp::tag_value;
use worker_interface_capnp::trace::attribute;

/// A tag or attribute value. Strings are bytes, since a `kj::String` need not be UTF-8.
///
/// The arrays are OpenTelemetry's homogeneous primitive arrays, kept as one value, so that a
/// one-element array stays distinct from a scalar. A `None` element stands for a null or
/// undefined input.
#[derive(Clone, Debug, PartialEq)]
pub enum TagValue {
    String(Vec<u8>),
    Bool(bool),
    Int64(i64),
    Float64(f64),
    StringArray(Vec<Option<Vec<u8>>>),
    BoolArray(Vec<Option<bool>>),
    Float64Array(Vec<Option<f64>>),
}

impl TagValue {
    /// Reads a value as `deserializeTagValue()` does. An array element whose variant is unknown
    /// reads as `None`; an unknown variant of the value itself is an error.
    pub fn from_capnp(reader: tag_value::Reader<'_>) -> capnp::Result<Self> {
        Ok(match reader.which()? {
            tag_value::String(text) => Self::String(text?.as_bytes().to_vec()),
            tag_value::Bool(value) => Self::Bool(value),
            tag_value::Int64(value) => Self::Int64(value),
            tag_value::Float64(value) => Self::Float64(value),
            tag_value::StringArray(list) => Self::StringArray(
                list?
                    .iter()
                    .map(|element| match element.which() {
                        Ok(optional_string::Value(text)) => Ok(Some(text?.as_bytes().to_vec())),
                        Ok(optional_string::None(())) | Err(_) => Ok(None),
                    })
                    .collect::<capnp::Result<_>>()?,
            ),
            tag_value::BoolArray(list) => Self::BoolArray(
                list?
                    .iter()
                    .map(|element| match element.which() {
                        Ok(optional_bool::Value(value)) => Some(value),
                        Ok(optional_bool::None(())) | Err(_) => None,
                    })
                    .collect(),
            ),
            tag_value::Float64Array(list) => Self::Float64Array(
                list?
                    .iter()
                    .map(|element| match element.which() {
                        Ok(optional_float64::Value(value)) => Some(value),
                        Ok(optional_float64::None(())) | Err(_) => None,
                    })
                    .collect(),
            ),
        })
    }

    /// Writes the value as `serializeTagValue()` does.
    pub fn to_capnp(&self, mut builder: tag_value::Builder<'_>) {
        match self {
            Self::String(text) => builder.set_string(capnp::text::Reader::from(text.as_slice())),
            Self::Bool(value) => builder.set_bool(*value),
            Self::Int64(value) => builder.set_int64(*value),
            Self::Float64(value) => builder.set_float64(*value),
            Self::StringArray(values) => {
                let mut list = builder.init_string_array(list_len(values.len()));
                for (i, value) in (0..).zip(values) {
                    let mut element = list.reborrow().get(i);
                    match value {
                        Some(text) => {
                            element.set_value(capnp::text::Reader::from(text.as_slice()));
                        }
                        None => element.set_none(()),
                    }
                }
            }
            Self::BoolArray(values) => {
                let mut list = builder.init_bool_array(list_len(values.len()));
                for (i, value) in (0..).zip(values) {
                    let mut element = list.reborrow().get(i);
                    match value {
                        Some(value) => element.set_value(*value),
                        None => element.set_none(()),
                    }
                }
            }
            Self::Float64Array(values) => {
                let mut list = builder.init_float64_array(list_len(values.len()));
                for (i, value) in (0..).zip(values) {
                    let mut element = list.reborrow().get(i);
                    match value {
                        Some(value) => element.set_value(*value),
                        None => element.set_none(()),
                    }
                }
            }
        }
    }
}

/// The length of a capnp list of `len` elements. capnp lists hold fewer than 2^29 elements, and
/// the `init_*` that takes the length panics past that, so the saturation only keeps this total.
pub fn list_len(len: usize) -> u32 {
    u32::try_from(len).unwrap_or(u32::MAX)
}

/// A named attribute with one or more values, `tracing::Attribute`. The name is bytes, as a tag
/// value's strings are.
#[derive(Clone, Debug, PartialEq)]
pub struct Attribute {
    pub name: Vec<u8>,
    pub values: Vec<TagValue>,
}

impl Attribute {
    /// Reads an attribute. Without a value list it is an error, as C++ asserts.
    pub fn from_capnp(reader: attribute::Reader<'_>) -> capnp::Result<Self> {
        if !reader.has_value() {
            return Err(capnp::Error::failed("attribute has no value".into()));
        }
        Ok(Self {
            name: reader.get_name()?.as_bytes().to_vec(),
            values: reader
                .get_value()?
                .iter()
                .map(TagValue::from_capnp)
                .collect::<capnp::Result<_>>()?,
        })
    }

    pub fn to_capnp(&self, mut builder: attribute::Builder<'_>) {
        builder.set_name(capnp::text::Reader::from(self.name.as_slice()));
        let mut list = builder.init_value(list_len(self.values.len()));
        for (i, value) in (0..).zip(&self.values) {
            value.to_capnp(list.reborrow().get(i));
        }
    }
}

#[cfg(test)]
#[path = "attribute-test.rs"]
mod tests;
