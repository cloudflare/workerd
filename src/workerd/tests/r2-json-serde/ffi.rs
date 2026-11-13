// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#![allow(unsafe_code, reason = "the cxx bridge expands to unsafe FFI glue")]

pub use bridge::Fixture;

use crate::fixtures::fixture;

#[cxx::bridge(namespace = "workerd::tests::r2_serde")]
mod bridge {
    /// The JSON documents of `fixtures.rs`.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum Fixture {
        HeadResponseMinimal,
        HeadResponseFull,
        ListResponseSmall,
        ListResponseLarge,
        CreateMultipartUploadResponse,
        UploadPartResponse,
        ErrorResponse,
        GetRequest,
        PutRequest,
        ListRequest,
        DeleteRequest,
        CompleteMultipartUploadRequest,
    }

    extern "Rust" {
        type R2HeadResponse;
        type R2ListResponse;
        type R2CreateMultipartUploadResponse;
        type R2UploadPartResponse;
        type R2ErrorResponse;
        type R2BindingRequest;

        fn fixture(fixture: Fixture) -> &'static str;

        // Each parses one JSON document into a new value, as JsonCodec::decode() does into a
        // new message.
        fn parse_head_response(json: &[u8]) -> Result<Box<R2HeadResponse>>;
        fn parse_list_response(json: &[u8]) -> Result<Box<R2ListResponse>>;
        fn parse_create_multipart_upload_response(
            json: &[u8],
        ) -> Result<Box<R2CreateMultipartUploadResponse>>;
        fn parse_upload_part_response(json: &[u8]) -> Result<Box<R2UploadPartResponse>>;
        fn parse_error_response(json: &[u8]) -> Result<Box<R2ErrorResponse>>;
        fn parse_binding_request(json: &[u8]) -> Result<Box<R2BindingRequest>>;
    }
}

/// Defines a bridge type that holds an `r2_api_serde` type, since an `extern "Rust"` type must be
/// local to the crate, and the function that parses one.
macro_rules! parsed {
    ($($name:ident = $parse:ident;)*) => {$(
        pub struct $name(pub r2_api_serde::$name);

        fn $parse(json: &[u8]) -> serde_json::Result<Box<$name>> {
            capnp_serde::from_slice(json).map(|value| Box::new($name(value)))
        }
    )*};
}

parsed! {
    R2HeadResponse = parse_head_response;
    R2ListResponse = parse_list_response;
    R2CreateMultipartUploadResponse = parse_create_multipart_upload_response;
    R2UploadPartResponse = parse_upload_part_response;
    R2ErrorResponse = parse_error_response;
    R2BindingRequest = parse_binding_request;
}
