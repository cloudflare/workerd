// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

fn no_signing() -> ffi::SigningOptions {
    ffi::SigningOptions {
        secret_access_key: String::new(),
        amz_date: String::new(),
        region: String::new(),
        service: String::new(),
        seed_signature: String::new(),
    }
}

fn options(decoded_length: u32) -> ffi::DecoderOptions {
    ffi::DecoderOptions {
        decoded_length: f64::from(decoded_length),
        has_allowed_trailers: false,
        allowed_trailers: Vec::new(),
        has_max_trailer_bytes: false,
        max_trailer_bytes: 0.0,
        drain_on_error: false,
        has_signing: false,
        signing: no_signing(),
    }
}

// The example in AWS's "Signature Calculations for the Authorization Header: Transferring
// Payload in Multiple Chunks": 66560 bytes of "a", sent as a 64 KiB chunk and a 1 KiB chunk.
const SECRET_ACCESS_KEY: &str = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";
const AMZ_DATE: &str = "20130524T000000Z";
const SEED_SIGNATURE: &str = "4f232c4386841ef735655705268965c44a0e4690baa4adea153f7db9fa80a0a9";
const EXAMPLE_SIGNATURES: [&str; 3] = [
    "ad80c730a21e5b8d04586a2213dd63b9a0e99e0e2307b0ade35a65485a288648",
    "0055627c9e194cb4542bae2aa5492e3c1575bbb81b612b7d234b86a503ef5497",
    "b6c6ea8a5354eaf15b3cb7646744f4275b71ea724fed81ceb9323e279d449df9",
];

// The same credentials signing "hello " and "world" as two chunks, then the final chunk, and a
// trailer section holding `x-amz-checksum-crc32:DUoRhQ==`.
const HELLO_WORLD_SIGNATURES: [&str; 4] = [
    "36a9919e5e855a54fc5e137c919e524764c71ade46792d228f7ebaade71602ab",
    "927d7061508e51b79d4e40eb55aed9033cd0c471859241fb0355ca3c9ff53a92",
    "ae13d294fc309f1858acb23b717c250dc572e78820dfd4bf5bae1dcb2055298f",
    "bf13d348abc27fe00099cbe7080e9a425fe4d33ec9a9225e6cc86a0a8bc2099a",
];

fn signing() -> ffi::SigningOptions {
    ffi::SigningOptions {
        secret_access_key: SECRET_ACCESS_KEY.to_owned(),
        amz_date: AMZ_DATE.to_owned(),
        region: "us-east-1".to_owned(),
        service: "s3".to_owned(),
        seed_signature: SEED_SIGNATURE.to_owned(),
    }
}

fn signed(decoded_length: u32) -> ffi::DecoderOptions {
    ffi::DecoderOptions {
        has_signing: true,
        signing: signing(),
        ..options(decoded_length)
    }
}

fn take_all(decoder: &mut Decoder, output: &mut Vec<u8>) {
    while decoder.has_output() {
        output.extend_from_slice(decoder.pop_output().data());
    }
}

type Decoded = (Vec<u8>, Vec<(String, String)>);

/// Decodes `input` in pieces of `piece` bytes, returning the output and the trailer fields, or
/// the error's description.
fn decode_with(
    options: ffi::DecoderOptions,
    input: &[u8],
    piece: usize,
) -> Result<Decoded, String> {
    let mut decoder = new_decoder(options).map_err(|e| e.description().to_owned())?;
    let mut output = Vec::new();
    for chunk in input.chunks(piece.max(1)) {
        let result = decoder.push(chunk);
        take_all(&mut decoder, &mut output);
        result.map_err(|e| e.description().to_owned())?;
    }
    let trailers = decoder.end().map_err(|e| e.description().to_owned())?;
    take_all(&mut decoder, &mut output);
    Ok((
        output,
        trailers
            .into_iter()
            .map(|field| (field.name, field.value))
            .collect(),
    ))
}

fn decode(decoded_length: u32, input: &[u8]) -> Result<Vec<u8>, String> {
    decode_with(options(decoded_length), input, input.len()).map(|(output, _)| output)
}

fn error(decoded_length: u32, input: &[u8]) -> String {
    decode(decoded_length, input).unwrap_err()
}

fn field(name: &str, value: &str) -> (String, String) {
    (name.to_owned(), value.to_owned())
}

const MALFORMED: &str = "jsg.TypeError: Malformed aws-chunked body.";
const MISMATCH: &str = "jsg.TypeError: An aws-chunked chunk or trailer signature does not match.";

fn aws_example() -> Vec<u8> {
    let [first, second, last] = EXAMPLE_SIGNATURES;
    let mut input = Vec::new();
    input.extend_from_slice(format!("10000;chunk-signature={first}\r\n").as_bytes());
    input.resize(input.len() + 65536, b'a');
    input.extend_from_slice(format!("\r\n400;chunk-signature={second}\r\n").as_bytes());
    input.resize(input.len() + 1024, b'a');
    input.extend_from_slice(format!("\r\n0;chunk-signature={last}\r\n\r\n").as_bytes());
    input
}

fn hello_world_signed() -> String {
    let [first, second, last, trailer] = HELLO_WORLD_SIGNATURES;
    format!(
        "6;chunk-signature={first}\r\nhello \r\n5;chunk-signature={second}\r\nworld\r\n\
         0;chunk-signature={last}\r\nx-amz-checksum-crc32:DUoRhQ==\r\n\
         x-amz-trailer-signature:{trailer}\r\n\r\n"
    )
}

#[test]
fn decodes_every_split() {
    let input = b"6\r\nhello \r\n5\r\nworld\r\n0\r\nx-amz-checksum-crc32:DUoRhQ==\r\n\r\n";
    for piece in 1..=input.len() {
        let (output, trailers) = decode_with(options(11), input, piece).unwrap();
        assert_eq!(output, b"hello world", "piece {piece}");
        assert_eq!(trailers, vec![field("x-amz-checksum-crc32", "DUoRhQ==")]);
    }
}

#[test]
fn accepts_an_lf_before_a_trailer_crlf() {
    let (_, trailers) = decode_with(
        options(0),
        b"0\r\nx-amz-checksum-crc32:DUoRhQ==\n\r\nx-b: c \r\n\r\n",
        7,
    )
    .unwrap();
    assert_eq!(
        trailers,
        vec![field("x-amz-checksum-crc32", "DUoRhQ=="), field("x-b", "c")]
    );
}

#[test]
fn trailer_names_are_lowercased() {
    let (_, trailers) = decode_with(options(0), b"0\r\nX-Amz-Meta-A: 1\r\n\r\n", 4).unwrap();
    assert_eq!(trailers, vec![field("x-amz-meta-a", "1")]);
}

#[test]
fn a_body_may_end_after_its_final_chunk() {
    assert_eq!(decode(5, b"5\r\nhello\r\n0\r\n").unwrap(), b"hello");
    assert_eq!(decode(5, b"5\r\nhello\r\n0\r\n\r\n").unwrap(), b"hello");
}

#[test]
fn unsigned_data_is_produced_as_it_arrives() {
    let mut decoder = new_decoder(options(10)).unwrap();
    decoder.push(b"a\r\n01234").unwrap();
    assert_eq!(decoder.pop_output().data(), b"01234");
    assert!(!decoder.has_output());
    decoder.push(b"56789\r\n0\r\n\r\n").unwrap();
    assert_eq!(decoder.pop_output().data(), b"56789");
    assert!(decoder.pop_output().data().is_empty());
    assert!(decoder.end().unwrap().is_empty());
}

#[test]
fn verifies_the_aws_example() {
    let input = aws_example();
    for piece in [1000, 4096, 65536, input.len()] {
        let (output, trailers) = decode_with(signed(66560), &input, piece).unwrap();
        assert_eq!(output.len(), 66560, "piece {piece}");
        assert!(output.iter().all(|&c| c == b'a'));
        assert!(trailers.is_empty());
    }
}

#[test]
fn verifies_a_signed_trailer() {
    let input = hello_world_signed();
    for piece in 1..=input.len() {
        let (output, trailers) = decode_with(signed(11), input.as_bytes(), piece).unwrap();
        assert_eq!(output, b"hello world", "piece {piece}");
        assert_eq!(trailers, vec![field("x-amz-checksum-crc32", "DUoRhQ==")]);
    }
}

#[test]
fn rejects_a_wrong_signature() {
    let mut input = aws_example();
    // Change one byte of the second chunk's data.
    let index = input.len() - 200;
    input[index] = b'b';
    let mut decoder = new_decoder(signed(66560)).unwrap();
    assert_eq!(decoder.push(&input).unwrap_err().description(), MISMATCH);
    // The first chunk was verified, so it was produced; the second never is.
    let mut output = Vec::new();
    take_all(&mut decoder, &mut output);
    assert_eq!(output.len(), 65536);

    let wrong_trailer = hello_world_signed().replace(HELLO_WORLD_SIGNATURES[3], &"0".repeat(64));
    assert_eq!(
        decode_with(signed(11), wrong_trailer.as_bytes(), 9).unwrap_err(),
        MISMATCH
    );
    let other_key = ffi::DecoderOptions {
        signing: ffi::SigningOptions {
            secret_access_key: "another key".to_owned(),
            ..signing()
        },
        ..signed(66560)
    };
    assert_eq!(
        decode_with(other_key, &aws_example(), 4096).unwrap_err(),
        MISMATCH
    );
}

#[test]
fn signed_bodies_need_every_signature() {
    let [first, second, last, _] = HELLO_WORLD_SIGNATURES;
    let unsigned_chunk = format!("6\r\nhello \r\n0;chunk-signature={last}\r\n\r\n");
    assert_eq!(
        decode_with(signed(6), unsigned_chunk.as_bytes(), 5).unwrap_err(),
        MALFORMED
    );
    let no_trailer_signature = format!(
        "6;chunk-signature={first}\r\nhello \r\n5;chunk-signature={second}\r\nworld\r\n\
         0;chunk-signature={last}\r\nx-amz-checksum-crc32:DUoRhQ==\r\n\r\n"
    );
    assert_eq!(
        decode_with(signed(11), no_trailer_signature.as_bytes(), 5).unwrap_err(),
        MALFORMED
    );
}

#[test]
fn unsigned_bodies_reject_signatures() {
    let signature = EXAMPLE_SIGNATURES[0];
    let chunk = format!("5;chunk-signature={signature}\r\nhello\r\n0\r\n\r\n");
    assert_eq!(error(5, chunk.as_bytes()), MALFORMED);
    let trailer = format!("0\r\nx-a:1\r\nx-amz-trailer-signature:{signature}\r\n\r\n");
    assert_eq!(error(0, trailer.as_bytes()), MALFORMED);
}

#[test]
fn only_chunk_signatures_are_accepted_as_extensions() {
    for extension in [";a", ";a=b", " ;a", ";a=\"quoted\"", ";chunk-signature=abc"] {
        let input = format!("5{extension}\r\nhello\r\n0\r\n\r\n");
        assert_eq!(error(5, input.as_bytes()), MALFORMED, "{extension:?}");
    }
}

#[test]
fn malformed_framing() {
    let cases: [(u32, &[u8]); 9] = [
        (0, b"zz\r\n"),
        (0, b"\r\n"),
        (5, b"5\r\rhello\r\n"),
        (5, b"5\r\nhelloX\r\n"),
        (5, b"5\r\nhello\rX"),
        // A size of more than 32 bits.
        (0, b"100000000\r\n"),
        (0, b"0\r\nno colon\r\n\r\n"),
        (0, b"0\r\nbad name: x\r\n\r\n"),
        // Anything after the final chunk is its trailer section.
        (0, b"0\r\n\r\nx"),
    ];
    for (decoded_length, input) in cases {
        assert_eq!(
            decode(decoded_length, input),
            Err(MALFORMED.to_owned()),
            "{:?}",
            String::from_utf8_lossy(input)
        );
    }
    assert_eq!(
        error(0, b"0\r\nx: a\x01\r\n\r\n"),
        "jsg.TypeError: Malformed aws-chunked trailer field value."
    );
    let long = format!("{}5\r\nhello\r\n0\r\n\r\n", "0".repeat(1100));
    assert_eq!(
        error(5, long.as_bytes()),
        "jsg.TypeError: An aws-chunked chunk size line is longer than 1024 bytes."
    );
}

#[test]
fn incomplete_bodies() {
    assert_eq!(
        error(5, b"5\r\nhel"),
        "jsg.TypeError: The aws-chunked body ended after 3 of its 5 decoded bytes."
    );
    assert_eq!(
        error(5, b"5\r\nhello\r\n"),
        "jsg.TypeError: The aws-chunked body ended before its final chunk."
    );
    assert_eq!(
        error(0, b""),
        "jsg.TypeError: The aws-chunked body ended before its final chunk."
    );
}

#[test]
fn output_before_a_failure_is_kept() {
    let mut decoder = new_decoder(options(5)).unwrap();
    assert!(decoder.push(b"5\r\nhello\r\nzz\r\n").is_err());
    let mut output = Vec::new();
    take_all(&mut decoder, &mut output);
    assert_eq!(output, b"hello");
}

#[test]
fn decoded_length() {
    let input = b"5\r\nhello\r\n0\r\n\r\n";
    assert!(decode_with(options(5), input, 3).is_ok());
    assert_eq!(
        decode_with(options(4), input, 3).unwrap_err(),
        "jsg.TypeError: The aws-chunked body decodes to more than its decoded length (4 bytes)."
    );
    assert_eq!(
        decode_with(options(6), input, 3).unwrap_err(),
        "jsg.TypeError: The aws-chunked body ended after 5 of its 6 decoded bytes."
    );
    assert_eq!(new_decoder(options(2)).unwrap().decoded_length(), 2);
}

#[test]
fn trailer_limits_and_filtering() {
    let limited = || ffi::DecoderOptions {
        has_max_trailer_bytes: true,
        max_trailer_bytes: 64.0,
        ..options(0)
    };
    // The section, with its final CRLF, is exactly 64 bytes.
    let at_limit = format!("0\r\nx-a:{}\r\n\r\n", "v".repeat(56));
    assert!(decode_with(limited(), at_limit.as_bytes(), 3).is_ok());
    let over = format!("0\r\nx-a:{}\r\n\r\n", "v".repeat(57));
    assert_eq!(
        decode_with(limited(), over.as_bytes(), 3).unwrap_err(),
        "jsg.TypeError: The aws-chunked trailer is longer than 64 bytes."
    );

    let filtered = ffi::DecoderOptions {
        has_allowed_trailers: true,
        allowed_trailers: vec!["X-Amz-Checksum-CRC32".to_owned()],
        ..options(0)
    };
    let (_, trailers) = decode_with(
        filtered,
        b"0\r\nx-amz-checksum-crc32:a\r\nx-other:b\r\n\r\n",
        7,
    )
    .unwrap();
    assert_eq!(trailers, vec![field("x-amz-checksum-crc32", "a")]);
}

#[test]
fn draining_defers_the_error_to_the_end() {
    let mut decoder = new_decoder(ffi::DecoderOptions {
        drain_on_error: true,
        ..options(5)
    })
    .unwrap();
    decoder.push(b"5\r\nhello\r\nzz\r\n").unwrap();
    let mut output = Vec::new();
    take_all(&mut decoder, &mut output);
    assert_eq!(output, b"hello");
    decoder.push(b"more input").unwrap();
    assert!(!decoder.has_output());
    assert_eq!(decoder.end().unwrap_err().description(), MALFORMED);
}

#[test]
fn without_draining_the_push_fails() {
    let mut decoder = new_decoder(options(5)).unwrap();
    assert_eq!(
        decoder
            .push(b"5\r\nhello\r\nzz\r\n")
            .unwrap_err()
            .description(),
        MALFORMED
    );
    assert_eq!(decoder.push(b"x").unwrap_err().description(), MALFORMED);
    assert_eq!(decoder.end().unwrap_err().description(), MALFORMED);
}

#[test]
fn options_are_validated() {
    for (decoded_length, ok) in [
        (0.0, true),
        (5.0, true),
        (-1.0, false),
        (0.5, false),
        (f64::NAN, false),
        (1e300, false),
    ] {
        let result = new_decoder(ffi::DecoderOptions {
            decoded_length,
            ..options(0)
        });
        assert_eq!(result.is_ok(), ok, "{decoded_length}");
    }
    let error = new_decoder(ffi::DecoderOptions {
        has_max_trailer_bytes: true,
        max_trailer_bytes: f64::NAN,
        ..options(0)
    })
    .err()
    .unwrap();
    assert!(
        error
            .description()
            .starts_with("jsg.RangeError: maxTrailerBytes must be")
    );

    let bad_date = new_decoder(ffi::DecoderOptions {
        signing: ffi::SigningOptions {
            amz_date: "2013-05-24".to_owned(),
            ..signing()
        },
        ..signed(0)
    })
    .err()
    .unwrap();
    assert!(
        bad_date
            .description()
            .starts_with("jsg.TypeError: signing.amzDate must be")
    );
    let bad_seed = new_decoder(ffi::DecoderOptions {
        signing: ffi::SigningOptions {
            seed_signature: "abc".to_owned(),
            ..signing()
        },
        ..signed(0)
    })
    .err()
    .unwrap();
    assert_eq!(
        bad_seed.description(),
        "jsg.TypeError: signing.seedSignature must be 64 hex digits."
    );
}

#[test]
fn retained_bytes_cover_a_buffered_signed_chunk() {
    let input = aws_example();
    let mut decoder = new_decoder(signed(66560)).unwrap();
    // The first chunk is buffered until its signature can be checked.
    decoder.push(&input[..40_000]).unwrap();
    assert!(!decoder.has_output());
    assert_eq!(decoder.retained_bytes(), 40_000);
    decoder.push(&input[40_000..]).unwrap();
    assert!(decoder.has_output());
    assert_eq!(decoder.retained_bytes(), 0);
}
