//! The bridge for `MimeType` in `workerd/util/mimetype.h`.
//!
//! Text crosses as bytes, since a `kj::String` need not be UTF-8. What the parser finds crosses
//! as slices of its input, and parameters go to a C++ `ParamSink` as they are found, so that the
//! only allocations are the `kj::String`s C++ keeps.

#![allow(
    unsafe_code,
    reason = "holds the cxx bridge, which expands to unsafe FFI glue"
)]

use std::pin::Pin;

use crate::parse as parser;
use crate::serialize as serializer;

#[cxx::bridge(namespace = "workerd::rust::mimetype")]
mod bridge {
    /// A type and subtype, as written in the input. Both are empty if the input is not a MIME
    /// type.
    struct Essence<'a> {
        type_: &'a [u8],
        subtype: &'a [u8],
    }

    /// A parameter of a `MimeType`: its name lowercased, its value without quotes or escapes.
    struct Param<'a> {
        name: &'a [u8],
        value: &'a [u8],
    }

    unsafe extern "C++" {
        include!("workerd/util/mimetype/ffi.h");

        /// Stores parameters as `MimeType` does: names lowercased, the first value of a name
        /// kept.
        type ParamSink;

        fn add(self: Pin<&mut ParamSink>, name: &[u8], value: &[u8]) -> Result<()>;
    }

    extern "Rust" {
        /// Parses a MIME type, passing its parameters to `params`.
        unsafe fn parse<'a>(input: &'a [u8], params: Pin<&mut ParamSink>) -> Result<Essence<'a>>;

        /// Parses a MIME type, ignoring its parameters.
        fn parse_essence(input: &[u8]) -> Essence<'_>;

        /// Extracts a MIME type from a `Content-Type` header value, passing its parameters to
        /// `params`.
        unsafe fn extract<'a>(input: &'a [u8], params: Pin<&mut ParamSink>) -> Result<Essence<'a>>;

        fn serialized_len(type_: &[u8], subtype: &[u8], params: &[Param<'_>]) -> usize;

        /// Writes to `out`, whose length is `serialized_len()` of the same arguments. Returns
        /// the length written, which C++ checks against that of `out`.
        fn serialize(type_: &[u8], subtype: &[u8], params: &[Param<'_>], out: &mut [u8]) -> usize;

        fn params_serialized_len(params: &[Param<'_>]) -> usize;

        /// Writes to `out`, whose length is `params_serialized_len()` of the same parameters.
        /// Returns the length written, which C++ checks against that of `out`.
        fn serialize_params(params: &[Param<'_>], out: &mut [u8]) -> usize;

        /// Whether `s` is a valid type, subtype or parameter name.
        fn is_token(s: &[u8]) -> bool;

        /// Whether `s` is a valid parameter value.
        fn is_param_value(s: &[u8]) -> bool;
    }
}

impl<'a> From<Option<parser::Essence<'a>>> for bridge::Essence<'a> {
    fn from(essence: Option<parser::Essence<'a>>) -> Self {
        let essence = essence.unwrap_or(parser::Essence {
            type_: &[],
            subtype: &[],
        });
        Self {
            type_: essence.type_,
            subtype: essence.subtype,
        }
    }
}

impl serializer::Param for bridge::Param<'_> {
    fn name(&self) -> &[u8] {
        self.name
    }

    fn value(&self) -> &[u8] {
        self.value
    }
}

fn parse<'a>(
    input: &'a [u8],
    mut params: Pin<&mut bridge::ParamSink>,
) -> Result<bridge::Essence<'a>, cxx::KjException> {
    let Some((essence, rest)) = parser::parse_essence(input) else {
        return Ok(None.into());
    };
    parser::parse_params(rest, &mut Vec::new(), |name, value| {
        params.as_mut().add(name, value)
    })?;
    Ok(Some(essence).into())
}

fn parse_essence(input: &[u8]) -> bridge::Essence<'_> {
    parser::parse_essence(input)
        .map(|(essence, _)| essence)
        .into()
}

fn extract<'a>(
    input: &'a [u8],
    mut params: Pin<&mut bridge::ParamSink>,
) -> Result<bridge::Essence<'a>, cxx::KjException> {
    let essence = parser::extract(input, &mut Vec::new(), |name, value| {
        params.as_mut().add(name, value)
    })?;
    Ok(essence.into())
}

fn serialized_len(type_: &[u8], subtype: &[u8], params: &[bridge::Param<'_>]) -> usize {
    serializer::serialized_len(type_, subtype, params)
}

fn serialize(type_: &[u8], subtype: &[u8], params: &[bridge::Param<'_>], out: &mut [u8]) -> usize {
    serializer::serialize(type_, subtype, params, out)
}

fn params_serialized_len(params: &[bridge::Param<'_>]) -> usize {
    serializer::params_serialized_len(params)
}

fn serialize_params(params: &[bridge::Param<'_>], out: &mut [u8]) -> usize {
    serializer::serialize_params(params, out)
}

fn is_token(s: &[u8]) -> bool {
    crate::is_token(s)
}

fn is_param_value(s: &[u8]) -> bool {
    crate::is_param_value(s)
}
