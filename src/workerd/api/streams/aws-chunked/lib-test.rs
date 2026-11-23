// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

fn options(drain_on_error: bool) -> ffi::DecoderOptions {
    ffi::DecoderOptions {
        has_decoded_length: false,
        decoded_length: 0.0,
        has_allowed_trailers: false,
        allowed_trailers: Vec::new(),
        has_max_trailer_bytes: false,
        max_trailer_bytes: 0.0,
        drain_on_error,
    }
}

#[test]
fn draining_defers_the_error_to_the_end() {
    let mut decoder = new_decoder(options(true)).unwrap();
    decoder.push(b"5\r\nhello\r\nzz").unwrap();
    assert_eq!(decoder.take_output(), b"hello");
    decoder.push(b"more input").unwrap();
    assert!(decoder.take_output().is_empty());
    let error = decoder.end().unwrap_err();
    assert_eq!(
        error.description(),
        "jsg.TypeError: Malformed aws-chunked chunk size."
    );
}

#[test]
fn without_draining_the_push_fails() {
    let mut decoder = new_decoder(options(false)).unwrap();
    let error = decoder.push(b"5\r\nhello\r\nzz").unwrap_err();
    assert_eq!(
        error.description(),
        "jsg.TypeError: Malformed aws-chunked chunk size."
    );
    assert_eq!(decoder.take_output(), b"hello");
}

#[test]
fn lengths_are_validated() {
    for (decoded_length, ok) in [
        (0.0, true),
        (5.0, true),
        (-1.0, false),
        (0.5, false),
        (1e300, false),
    ] {
        let result = new_decoder(ffi::DecoderOptions {
            has_decoded_length: true,
            decoded_length,
            ..options(true)
        });
        assert_eq!(result.is_ok(), ok, "{decoded_length}");
    }
    let error = new_decoder(ffi::DecoderOptions {
        has_max_trailer_bytes: true,
        max_trailer_bytes: f64::NAN,
        ..options(true)
    })
    .err()
    .unwrap();
    assert!(
        error
            .description()
            .starts_with("jsg.RangeError: maxTrailerBytes must be")
    );
}

#[test]
fn trailers_are_returned() {
    let mut decoder = new_decoder(ffi::DecoderOptions {
        has_decoded_length: true,
        decoded_length: 2.0,
        ..options(true)
    })
    .unwrap();
    assert_eq!(Option::<u64>::from(decoder.decoded_length()), Some(2));
    decoder.push(b"2\r\nhi\r\n0\r\nx-a: b\r\n\r\n").unwrap();
    let trailers = decoder.end().unwrap();
    assert_eq!(trailers.len(), 1);
    assert_eq!(
        (trailers[0].name.as_str(), trailers[0].value.as_str()),
        ("x-a", "b")
    );
}
