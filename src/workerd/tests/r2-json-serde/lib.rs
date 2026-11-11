// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! A `serde_json` parser of the R2 binding's JSON (`src/workerd/api/r2-api.capnp`), which
//! `bench-r2-json` measures capnp's `JsonCodec` against. The types are `r2_api_serde`'s, which
//! capnpc-rust-serde generates from the schema.

pub mod ffi;
pub mod fixtures;

/// Parses a JSON document into a new boxed value.
///
/// # Errors
///
/// When `json` is not a valid document of `T`.
pub fn parse<T: serde::de::DeserializeOwned>(json: &[u8]) -> serde_json::Result<Box<T>> {
    capnp_serde::from_slice(json)
}

#[cfg(test)]
#[path = "lib-test.rs"]
mod tests;
