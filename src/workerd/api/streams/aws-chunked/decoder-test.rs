// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

fn options() -> Options {
    Options {
        decoded_length: None,
        allowed_trailers: None,
        max_trailer_bytes: 16 * 1024,
    }
}

/// Decodes `input` in pieces of `piece` bytes.
fn decode_with(
    options: Options,
    input: &[u8],
    piece: usize,
) -> Result<(Vec<u8>, Vec<Field>), Malformed> {
    let mut decoder = Decoder::new(options);
    let mut output = Vec::new();
    for chunk in input.chunks(piece.max(1)) {
        decoder.push(chunk, &mut output)?;
    }
    Ok((output, decoder.end()?))
}

fn decode(input: &[u8]) -> Result<(Vec<u8>, Vec<Field>), Malformed> {
    decode_with(options(), input, input.len())
}

fn error(input: &[u8]) -> String {
    decode(input).unwrap_err().0
}

const SIGNATURE: &str = "ad80c730a21e5b8d04586a2213dd63b9a0e99e0e2307b0ade35a65485a288648";

#[test]
fn decodes_every_split() {
    let input = b"6\r\nhello \r\n5\r\nworld\r\n0\r\nx-amz-checksum-crc32:DUoRhQ==\r\n\r\n";
    for piece in 1..=input.len() {
        let (output, trailers) = decode_with(options(), input, piece).unwrap();
        assert_eq!(output, b"hello world", "piece {piece}");
        assert_eq!(
            trailers,
            vec![Field {
                name: "x-amz-checksum-crc32".to_owned(),
                value: "DUoRhQ==".to_owned(),
            }]
        );
    }
}

#[test]
fn accepts_an_lf_before_a_trailer_crlf() {
    let (_, trailers) = decode(b"0\r\nx-amz-checksum-crc32:DUoRhQ==\n\r\nx-b: c \r\n\r\n").unwrap();
    assert_eq!(trailers.len(), 2);
    assert_eq!(trailers[0].value, "DUoRhQ==");
    assert_eq!(trailers[1].value, "c");
}

#[test]
fn signed_chunks() {
    let input = format!(
        "b;chunk-signature={SIGNATURE}\r\nhello world\r\n0;chunk-signature={SIGNATURE}\r\n\r\n"
    );
    assert_eq!(decode(input.as_bytes()).unwrap().0, b"hello world");
}

#[test]
fn extension_grammar() {
    for extension in [
        ";a",
        ";a=b",
        ";a;b=c",
        " ;a",
        "\t; a = b ;c",
        ";a=\"quoted ; value\"",
        ";a=\"escaped \\\" quote\"",
        ";a=\"\"",
    ] {
        let input = format!("5{extension}\r\nhello\r\n0{extension}\r\n\r\n");
        assert_eq!(
            decode(input.as_bytes()).unwrap().0,
            b"hello",
            "{extension:?}"
        );
    }
    for extension in [
        ";",
        ";;a",
        ";a=",
        ";a b",
        ";a=b c",
        ";a=\"open",
        ";a=\"x\"y",
        ";a=b ",
        " ",
        ";a ",
        ";@",
        ";a=\"\x01\"",
    ] {
        let input = format!("5{extension}\r\nhello\r\n0\r\n\r\n");
        assert_eq!(
            error(input.as_bytes()),
            "Malformed aws-chunked chunk extension.",
            "{extension:?}"
        );
    }
    let long = format!(
        "5;{}\r\nhello\r\n0\r\n\r\n",
        "e".repeat(MAX_EXTENSION_BYTES)
    );
    assert!(error(long.as_bytes()).contains("chunk extension is longer"));
}

#[test]
fn malformed_framing() {
    assert_eq!(error(b"zz\r\n"), "Malformed aws-chunked chunk size.");
    assert_eq!(error(b"\r\n"), "Malformed aws-chunked chunk size.");
    assert_eq!(
        error(b"5\r\rhello"),
        "Malformed aws-chunked chunk size line."
    );
    assert!(error(&[b'1'; 17]).contains("more than 16 digits"));
    assert!(error(b"5\r\nhelloX\r\n").contains("longer than its declared size"));
    assert_eq!(
        error(b"5\r\nhello\rX"),
        "Malformed aws-chunked chunk ending."
    );
    assert!(error(b"5\r\nhel").contains("ended before its final chunk"));
    assert!(error(b"0\r\n\r\nx").contains("continues past its trailer"));
    assert_eq!(
        error(b"0\r\nno colon\r\n\r\n"),
        "Malformed aws-chunked trailer field."
    );
    assert_eq!(
        error(b"0\r\nbad name: x\r\n\r\n"),
        "Malformed aws-chunked trailer field name."
    );
    assert_eq!(
        error(b"0\r\nx: a\x01\r\n\r\n"),
        "Malformed aws-chunked trailer field value."
    );
}

#[test]
fn output_before_a_failure_is_kept() {
    let mut decoder = Decoder::new(options());
    let mut output = Vec::new();
    assert!(decoder.push(b"5\r\nhello\r\nzz", &mut output).is_err());
    assert_eq!(output, b"hello");
}

#[test]
fn decoded_length() {
    let exact = Options {
        decoded_length: Some(5),
        ..options()
    };
    assert!(decode_with(exact, b"5\r\nhello\r\n0\r\n\r\n", 3).is_ok());
    let short = Options {
        decoded_length: Some(4),
        ..options()
    };
    assert!(
        decode_with(short, b"5\r\nhello\r\n0\r\n\r\n", 3)
            .unwrap_err()
            .0
            .contains("more than its decoded length (4 bytes)")
    );
    let long = Options {
        decoded_length: Some(6),
        ..options()
    };
    assert!(
        decode_with(long, b"5\r\nhello\r\n0\r\n\r\n", 3)
            .unwrap_err()
            .0
            .contains("decoded to 5 bytes, but its decoded length is 6")
    );
}

#[test]
fn trailer_limits_and_filtering() {
    let limited = Options {
        max_trailer_bytes: 64,
        ..options()
    };
    // The section, with its final CRLF, is exactly 64 bytes.
    let at_limit = format!("0\r\nx-a:{}\r\n\r\n", "v".repeat(56));
    assert!(decode_with(limited.clone(), at_limit.as_bytes(), 3).is_ok());
    let over = format!("0\r\nx-a:{}\r\n\r\n", "v".repeat(57));
    assert!(
        decode_with(limited, over.as_bytes(), 3)
            .unwrap_err()
            .0
            .contains("trailer is longer than 64 bytes")
    );

    let filtered = Options {
        allowed_trailers: Some(vec!["X-Amz-Checksum-CRC32".to_owned()]),
        ..options()
    };
    let input =
        format!("0\r\nx-amz-checksum-crc32:a\r\nx-amz-trailer-signature:{SIGNATURE}\r\n\r\n");
    let (_, trailers) = decode_with(filtered, input.as_bytes(), 7).unwrap();
    assert_eq!(trailers.len(), 1);
    assert_eq!(trailers[0].name, "x-amz-checksum-crc32");
}
