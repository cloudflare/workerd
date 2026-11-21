// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

/// All four transcodable encodings, for exhaustively testing every pair.
const ENCODINGS: [Encoding; 4] = [
    Encoding::Ascii,
    Encoding::Latin1,
    Encoding::Utf8,
    Encoding::Utf16Le,
];

/// Runs both transcode steps the way [`crate::transcode`] does -- size,
/// allocate, convert, narrow to the written length -- against a plain
/// `Vec` rather than a V8 backing store, and returns the written bytes.
fn transcode(source: &[u8], from: Encoding, to: Encoding) -> Result<Vec<u8>, TranscodeError> {
    let transcoder = Transcoder::new(source, from, to)?;
    let mut dest = vec![0u8; transcoder.dest_len()];
    let written = transcoder.transcode_into(&mut dest)?;
    assert!(
        written <= dest.len(),
        "{from:?} -> {to:?} wrote {written} bytes into a {} byte buffer",
        dest.len()
    );
    dest.truncate(written);
    Ok(dest)
}

#[test]
fn every_pair_round_trips_ascii_text() {
    for &from in &ENCODINGS {
        // UTF16LE source bytes must have even length; every other encoding
        // is happy with plain ASCII bytes.
        let source: &[u8] = if from == Encoding::Utf16Le {
            &[0x48, 0x00, 0x69, 0x00] // "Hi" as UTF-16LE code units.
        } else {
            b"Hi"
        };
        for &to in &ENCODINGS {
            let result = transcode(source, from, to);
            assert!(result.is_ok(), "{from:?} -> {to:?} failed: {result:?}");
        }
    }
}

#[test]
fn every_pair_empty_input_is_empty_output() {
    for &from in &ENCODINGS {
        for &to in &ENCODINGS {
            let result = transcode(&[], from, to);
            assert_eq!(
                result.as_deref(),
                Ok([].as_slice()),
                "{from:?} -> {to:?} on empty input should be empty, got {result:?}"
            );
        }
    }
}

#[test]
fn identity_pairs_round_trip() {
    for &encoding in &ENCODINGS {
        // UTF16LE source bytes must be well-formed UTF-16LE to pass through
        // unchanged.
        let source: &[u8] = if encoding == Encoding::Utf16Le {
            b"H\0i\0"
        } else {
            b"identity"
        };
        assert_eq!(
            transcode(source, encoding, encoding).unwrap(),
            source,
            "{encoding:?} identity"
        );
    }
}

#[test]
fn identity_pairs_replace_ill_formed_input() {
    const FFFD: &[u8] = "\u{fffd}".as_bytes();
    for (source, expected) in [
        (&[0xe2, 0x82][..], FFFD.to_vec()),
        (&[0xed, 0xa0, 0x80], FFFD.repeat(3)),
        (&[0x41, 0xff, 0x42], [b"A", FFFD, b"B"].concat()),
    ] {
        assert_eq!(
            transcode(source, Encoding::Utf8, Encoding::Utf8).unwrap(),
            expected,
            "{source:x?}"
        );
    }
    for (source, expected) in [
        (&[0x61, 0x00, 0x62][..], &[0x61, 0x00, 0xfd, 0xff][..]),
        (&[0x00, 0xde, 0x3d, 0xd8], &[0xfd, 0xff, 0xfd, 0xff]),
        (&[0x3d, 0xd8, 0x00, 0xde], &[0x3d, 0xd8, 0x00, 0xde]),
        (&[0x61], &[0xfd, 0xff]),
        (&[0x41, 0x00, 0x00, 0xd8, 0x42], &[0x41, 0x00, 0xfd, 0xff]),
        (&[0x00, 0xdc, 0x42], &[0xfd, 0xff, 0xfd, 0xff]),
    ] {
        assert_eq!(
            transcode(source, Encoding::Utf16Le, Encoding::Utf16Le).unwrap(),
            expected,
            "{source:x?}"
        );
    }
}

#[test]
fn identity_pairs_copy_the_well_formed_prefix() {
    let prefix = "\u{e9}".repeat(50);
    let source = [prefix.as_bytes(), &[0xe2, 0x82], b"z"].concat();
    let transcoder = Transcoder::new(&source, Encoding::Utf8, Encoding::Utf8).unwrap();
    // The prefix is copied, and only the three bytes after it are sized at
    // three bytes each.
    assert_eq!(transcoder.dest_len(), prefix.len() + 9);
    assert_eq!(
        transcode(&source, Encoding::Utf8, Encoding::Utf8).unwrap(),
        [prefix.as_bytes(), "\u{fffd}z".as_bytes()].concat()
    );

    let prefix: Vec<u8> = "\u{1f600}"
        .repeat(20)
        .encode_utf16()
        .flat_map(u16::to_le_bytes)
        .collect();
    let source = [&prefix[..], &[0x00, 0xd8, 0x42]].concat();
    assert_eq!(
        transcode(&source, Encoding::Utf16Le, Encoding::Utf16Le).unwrap(),
        [&prefix[..], &[0xfd, 0xff]].concat()
    );
}

#[test]
fn ascii_sources_are_copied_into_single_byte_and_utf8_targets() {
    let source = b"plain ascii";
    for (from, to) in [
        (Encoding::Ascii, Encoding::Ascii),
        (Encoding::Ascii, Encoding::Latin1),
        (Encoding::Ascii, Encoding::Utf8),
        (Encoding::Latin1, Encoding::Ascii),
        (Encoding::Latin1, Encoding::Utf8),
        (Encoding::Utf8, Encoding::Ascii),
        (Encoding::Utf8, Encoding::Latin1),
    ] {
        let transcoder = Transcoder::new(source, from, to).unwrap();
        assert_eq!(transcoder.dest_len(), source.len(), "{from:?} -> {to:?}");
        assert_eq!(
            transcode(source, from, to).unwrap(),
            source,
            "{from:?} -> {to:?}"
        );
    }
}

#[test]
fn text_after_an_ascii_prefix_is_still_converted() {
    // Long enough that the first non-ASCII byte is past simdutf's first
    // 64-byte block.
    let prefix = "a".repeat(100);
    let cases = [
        (
            Encoding::Ascii,
            Encoding::Utf8,
            vec![0xe9],
            "\u{fffd}".as_bytes(),
        ),
        (
            Encoding::Ascii,
            Encoding::Latin1,
            vec![0xe9],
            b"?".as_slice(),
        ),
        (Encoding::Latin1, Encoding::Ascii, vec![0xe9], b"?"),
        (Encoding::Utf8, Encoding::Latin1, "\u{e9}".into(), &[0xe9]),
        (Encoding::Utf8, Encoding::Ascii, "\u{20ac}".into(), b"?"),
        (Encoding::Utf8, Encoding::Latin1, vec![0xe2, 0x82], b"?"),
    ];
    for (from, to, tail, expected) in cases {
        let source = [prefix.as_bytes(), &tail, b"z"].concat();
        let expected = [prefix.as_bytes(), expected, b"z"].concat();
        assert_eq!(
            transcode(&source, from, to).unwrap(),
            expected,
            "{from:?} -> {to:?}"
        );
    }
}

#[test]
fn latin1_to_utf8_is_sized_exactly() {
    let source = [0x41, 0xe9, 0xff];
    let transcoder = Transcoder::new(&source, Encoding::Latin1, Encoding::Utf8).unwrap();
    assert_eq!(transcoder.dest_len(), 5);
    assert_eq!(
        transcode(&source, Encoding::Latin1, Encoding::Utf8).unwrap(),
        "A\u{e9}\u{ff}".as_bytes()
    );
}

#[test]
fn unmappable_characters_become_question_marks() {
    // '☕' (U+2615, HOT BEVERAGE) has no representation in ASCII or Latin-1.
    let utf8_source = "☕".as_bytes();
    let utf16le_source = [0x15, 0x26];
    for (source, from) in [
        (utf8_source, Encoding::Utf8),
        (utf16le_source.as_slice(), Encoding::Utf16Le),
    ] {
        for to in [Encoding::Ascii, Encoding::Latin1] {
            assert_eq!(
                transcode(source, from, to).unwrap(),
                b"?",
                "{from:?} -> {to:?}"
            );
        }
    }
}

#[test]
fn ascii_to_utf16le_widens_high_bytes_via_latin1_path() {
    // ASCII -> UTF16LE takes the Latin-1 path, so a high byte is widened to
    // U+00FF rather than substituted with '?'.
    let result = transcode(&[0xff], Encoding::Ascii, Encoding::Utf16Le).unwrap();
    assert_eq!(result, vec![0xff, 0x00]);
}

#[test]
fn utf8_continuation_byte_only_input_to_utf16le_is_invalid() {
    // The UTF8 -> UTF16LE size estimate is zero for input consisting only of
    // continuation bytes; that input is as invalid as any other ill-formed
    // UTF-8 on this pair.
    for source in [&[0x80][..], &[0x80, 0xbf]] {
        let result = transcode(source, Encoding::Utf8, Encoding::Utf16Le);
        assert_eq!(result, Err(TranscodeError::InvalidChar), "{source:?}");
    }
}

#[test]
fn invalid_utf8_to_utf16le_is_invalid_char() {
    let result = transcode(&[0x61, 0xc3], Encoding::Utf8, Encoding::Utf16Le);
    assert_eq!(result, Err(TranscodeError::InvalidChar));
}

#[test]
fn odd_length_utf16le_ignores_the_trailing_byte() {
    // "ab" in UTF-16LE, then a stray byte.
    let source = [0x61, 0x00, 0x62, 0x00, 0x63];
    for to in [Encoding::Utf8, Encoding::Latin1, Encoding::Ascii] {
        assert_eq!(
            transcode(&source, Encoding::Utf16Le, to).unwrap(),
            b"ab",
            "{to:?}"
        );
    }
}

#[test]
fn odd_length_utf16le_identity_substitutes_the_trailing_byte() {
    let result = transcode(&[0x61, 0x00, 0x62], Encoding::Utf16Le, Encoding::Utf16Le).unwrap();
    assert_eq!(result, vec![0x61, 0x00, 0xfd, 0xff]);
}

#[test]
fn lone_utf16le_byte_is_invalid_to_utf8_but_empty_to_single_byte() {
    let result = transcode(&[0x61], Encoding::Utf16Le, Encoding::Utf8);
    assert_eq!(result, Err(TranscodeError::InvalidChar));
    for to in [Encoding::Latin1, Encoding::Ascii] {
        assert!(
            transcode(&[0x61], Encoding::Utf16Le, to)
                .unwrap()
                .is_empty(),
            "{to:?}"
        );
    }
}

#[test]
fn unpaired_surrogate_utf16le_to_utf8_is_invalid_char() {
    // U+D800, an unpaired high surrogate, encoded as UTF-16LE bytes.
    // `utf8_length_from_utf16le` does not validate and reports a nonzero
    // length, but `convert_utf16le_to_utf8` rejects the input and writes
    // nothing.
    for source in [&[0x00, 0xd8][..], &[0x41, 0x00, 0x00, 0xdc]] {
        let result = transcode(source, Encoding::Utf16Le, Encoding::Utf8);
        assert_eq!(result, Err(TranscodeError::InvalidChar), "{source:?}");
    }
}

#[test]
fn out_of_range_source_encoding_is_rejected() {
    let invalid = Encoding { repr: 4 };
    for &to in &ENCODINGS {
        assert_eq!(
            Transcoder::new(b"Hi", invalid, to).err(),
            Some(TranscodeError::InvalidEncoding),
            "{to:?}"
        );
    }
}

#[test]
fn out_of_range_destination_encoding_is_rejected() {
    let invalid = Encoding { repr: 4 };
    for &from in &ENCODINGS {
        assert_eq!(
            Transcoder::new(b"Hi", from, invalid).err(),
            Some(TranscodeError::InvalidEncoding),
            "{from:?}"
        );
    }
}

#[test]
fn destination_size_mismatch_is_rejected() {
    let transcoder = Transcoder::new(b"Hi", Encoding::Latin1, Encoding::Utf16Le).unwrap();
    let mut too_small = vec![0u8; transcoder.dest_len() - 1];
    assert_eq!(
        transcoder.transcode_into(&mut too_small),
        Err(TranscodeError::DestinationSizeMismatch)
    );
}
