// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use jsg_test::Harness;

use super::*;

/// All four transcodable encodings, for exhaustively testing every pair.
const ENCODINGS: [Encoding; 4] = [
    Encoding::Ascii,
    Encoding::Latin1,
    Encoding::Utf8,
    Encoding::Utf16Le,
];

// `Harness::new()` initializes the V8 platform, which is what installs the
// embedded ICU data. ICU converter opens fail without it, even though these
// tests never create an isolate or run JavaScript.
fn init_icu() -> Harness {
    Harness::new()
}

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
    let _harness = init_icu();
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
    let _harness = init_icu();
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
    let _harness = init_icu();
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
fn unmappable_characters_become_question_marks() {
    let _harness = init_icu();
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
fn utf8_continuation_byte_only_input_yields_empty_utf16le() {
    // The UTF8 -> UTF16LE size estimate is zero for input consisting only of
    // continuation bytes, so the result is empty rather than an error, even
    // though the input is non-empty.
    let result = transcode(&[0x80], Encoding::Utf8, Encoding::Utf16Le).unwrap();
    assert!(result.is_empty());
}

#[test]
fn invalid_utf8_to_utf16le_is_unable_to_transcode() {
    let result = transcode(&[0x61, 0xc3], Encoding::Utf8, Encoding::Utf16Le);
    assert_eq!(result, Err(TranscodeError::UnableToTranscode));
}

#[test]
fn odd_length_utf16le_to_utf8_is_rejected() {
    let result = transcode(&[0x61], Encoding::Utf16Le, Encoding::Utf8);
    assert_eq!(result, Err(TranscodeError::OddUtf16leInput));
}

#[test]
fn odd_length_utf16le_to_latin1_is_rejected() {
    let _harness = init_icu();
    let result = transcode(&[0x61], Encoding::Utf16Le, Encoding::Latin1);
    assert_eq!(result, Err(TranscodeError::OddUtf16leInput));
}

#[test]
fn unpaired_surrogate_utf16le_to_utf8() {
    // U+D800, an unpaired high surrogate, encoded as UTF-16LE bytes.
    let source = [0x00, 0xd8];
    // `utf8_length_from_utf16le` does not validate, and reports a nonzero
    // length for the surrogate, but `convert_utf16le_to_utf8` rejects the input
    // and writes nothing -- so the mismatch check is what surfaces the
    // failure.
    let result = transcode(&source, Encoding::Utf16Le, Encoding::Utf8);
    assert_eq!(result, Err(TranscodeError::Utf8LengthMismatch));
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
fn destination_size_mismatch_is_rejected() {
    let transcoder = Transcoder::new(b"Hi", Encoding::Latin1, Encoding::Utf16Le).unwrap();
    let mut too_small = vec![0u8; transcoder.dest_len() - 1];
    assert_eq!(
        transcoder.transcode_into(&mut too_small),
        Err(TranscodeError::DestinationSizeMismatch)
    );
}
