// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#[cxx::bridge(namespace = "workerd::rust::nbytes")]
mod ffi {
    unsafe extern "C++" {
        include!("workerd/rust/nbytes/ffi.h");

        #[cxx_name = "base64DecodedSize"]
        fn base64_decoded_size(input: &[u8]) -> usize;

        #[cxx_name = "base64DecodeInto"]
        fn base64_decode_into(output: &mut [u8], input: &[u8]) -> usize;

        #[cxx_name = "base64EncodedSize"]
        fn base64_encoded_size(input_size: usize, url: bool) -> usize;

        #[cxx_name = "base64EncodeInto"]
        fn base64_encode_into(output: &mut [u8], input: &[u8], url: bool) -> usize;
    }
}

/// Returns the maximum output size for Node's permissive Base64 decoder.
#[must_use]
pub fn base64_decoded_size(input: &[u8]) -> usize {
    ffi::base64_decoded_size(input)
}

/// Decodes Base64 into a bounded output slice using Node's permissive semantics.
///
/// Decoding stops once `output` is full and returns the number of bytes written.
#[must_use]
pub fn base64_decode_into(output: &mut [u8], input: &[u8]) -> usize {
    ffi::base64_decode_into(output, input)
}

/// Returns the exact output size for simdutf Base64 encoding.
#[must_use]
pub fn base64_encoded_size(input_size: usize, url: bool) -> usize {
    ffi::base64_encoded_size(input_size, url)
}

/// Encodes Base64 using simdutf, including Node's unpadded `Base64URL` variant.
#[must_use]
pub fn base64_encode(input: &[u8], url: bool) -> Vec<u8> {
    let mut output = vec![0; base64_encoded_size(input.len(), url)];
    if !output.is_empty() {
        let written = ffi::base64_encode_into(&mut output, input, url);
        debug_assert_eq!(written, output.len());
    }
    output
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn accepts_missing_padding() {
        let mut output = [0; 5];
        assert_eq!(base64_decode_into(&mut output, b"SGVsbG8"), 5);
        assert_eq!(&output, b"Hello");
    }

    #[test]
    fn ignores_garbage() {
        let mut output = [0; 5];
        assert_eq!(base64_decode_into(&mut output, b"S G\nVsbG8="), 5);
        assert_eq!(&output, b"Hello");
    }

    #[test]
    fn stops_at_output_capacity() {
        let mut output = [0; 2];
        assert_eq!(base64_decode_into(&mut output, b"SGVsbG8="), 2);
        assert_eq!(&output, b"He");
    }

    #[test]
    fn simd_base64_encoding_matches_node_variants() {
        assert_eq!(base64_encode(b"hello", false), b"aGVsbG8=");
        assert_eq!(base64_encode(&[0xfb, 0xff], true), b"-_8");
    }
}
