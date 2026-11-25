// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The Rust half of fallback-client-test.c++, which checks the crate's requests and its reading of
//! responses against capnp's `JsonCodec` and `kj::Url`, which the C++ client used.

#![allow(unsafe_code, reason = "the cxx bridge expands to unsafe FFI glue")]

#[cxx::bridge(namespace = "workerd::fallback::test")]
mod bridge {
    /// A request as the crate encodes it.
    struct Encoded {
        target: String,
        body: Vec<u8>,
        expected_name: Vec<u8>,
    }

    /// A body as the crate decodes it: the module message if `ok`.
    struct Decoded {
        ok: bool,
        module: Vec<u8>,
    }

    extern "Rust" {
        /// An empty `raw_specifier` or `attribute_type` is absent.
        fn encode_request(
            v2: bool,
            specifier: &[u8],
            raw_specifier: &[u8],
            referrer: &[u8],
            attribute_type: &[u8],
        ) -> Encoded;

        fn decode_module(body: &[u8], expected_name: &[u8]) -> Decoded;
    }
}

fn encode_request(
    v2: bool,
    specifier: &[u8],
    raw_specifier: &[u8],
    referrer: &[u8],
    attribute_type: &[u8],
) -> bridge::Encoded {
    let request = fallback_client::Request {
        version: if v2 {
            fallback_client::Version::V2
        } else {
            fallback_client::Version::V1
        },
        import_type: fallback_client::ImportType::Import,
        specifier: specifier.to_vec(),
        raw_specifier: (!raw_specifier.is_empty()).then(|| raw_specifier.to_vec()),
        referrer: referrer.to_vec(),
        attributes: if attribute_type.is_empty() {
            Vec::new()
        } else {
            vec![(b"type".to_vec(), attribute_type.to_vec())]
        },
    };
    let encoded = fallback_client::encode(&request);
    bridge::Encoded {
        target: encoded.target,
        body: encoded.body.unwrap_or_default(),
        expected_name: encoded.expected_name,
    }
}

fn decode_module(body: &[u8], expected_name: &[u8]) -> bridge::Decoded {
    match fallback_client::decode_module(body, expected_name) {
        Ok(module) => bridge::Decoded { ok: true, module },
        Err(_) => bridge::Decoded {
            ok: false,
            module: Vec::new(),
        },
    }
}
